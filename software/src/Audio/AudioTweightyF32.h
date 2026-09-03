#pragma once

// ---------------------------------------------------------------------------
// TIME-mode audio engine for the Tweighty app: an AudioDelayExtF32-style,
// crossfade-smoothed 8-tap stereo delay, wrapped with a WRITE/RECIRC write
// gate (TweightyTransport.h) instead of a fixed feedback path.
//
// The trick that makes WRITE and RECIRC the same read machinery: taps always
// read "N seconds ago" from a buffer that is written every block, in both
// modes. What changes at a WRITE<->RECIRC edge is only how much of the fresh
// panel input is mixed into what gets written -- full in WRITE (so the taps
// hear live echoes), zero in RECIRC (so what continues to write, and
// therefore what the taps continue to hear, is only the captured window
// recirculating through feedback). The read side never has a seam, so the
// only discontinuity in the whole engine is that one write-gate scalar, and
// it is the only thing this class crossfades on a mode edge.
//
// PITCH mode (dual-head windowed reader) is NOT implemented -- 288r's own
// docs mark it as having an unresolved buffer-wrap glitch. TIME mode only
// for v1.
//
// Composition, not subclassing: AudioDelayExtF32 isn't designed to be
// subclassed (its crossfade/read internals are private to it), so the same
// equal-power-table-walk technique it uses for time-modulation smoothing is
// reimplemented here -- once for the tap-target crossfades (mirrors
// AudioDelayExtF32::cf_delay/ReadCrossfadeChunk almost exactly, just
// widened to read two channels off one shared phase so L/R never drift
// apart), and a second time for the write-gate crossfade that has no
// AudioDelayExtF32 analogue at all.
//
// Buffer sizing deliberately matches audio_applets/DelayApplet.h's own
// AudioDelayExtF32<9> usage (1024*512 samples/channel, ~12s, falling back to
// 1/32 that without PSRAM) rather than picking a new number: that is this
// codebase's only bench-proven point on the float32/one-pole slew range, and
// picking our own number would mean re-proving it.
// ---------------------------------------------------------------------------

#include "AudioBuffer.h"
#include "AudioParam.h"
#include "../dsputils.h"
#include "../util/util_macros.h"
#include "../extern/f32/AudioStream_F32.h"
#include "../TweightyTransport.h"
#include "../TweightyTapPhase.h"
#include <Audio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

extern "C" uint8_t external_psram_size;

class AudioTweightyF32 : public AudioStream_F32 {
public:
  static constexpr float MAX_LOOP_SECONDS = 12.0f;

  AudioTweightyF32()
    : AudioStream_F32(2, input_queue_array_),
      buffer_l_(external_psram_size ? kBufferSamples : kBufferSamples / 32),
      buffer_r_(external_psram_size ? kBufferSamples : kBufferSamples / 32) {
    delay_secs_.fill(OnePole(Interpolated(0.0f, AUDIO_BLOCK_SAMPLES), 0.0002f));
    tap_mix_.fill(Interpolated(1.0f, AUDIO_BLOCK_SAMPLES / kChunkSize));
    tap_feedback_.fill(Interpolated(1.0f, AUDIO_BLOCK_SAMPLES / kChunkSize));
    min_delay_secs_ = (kChunkSize + 2) / AUDIO_SAMPLE_RATE_EXACT;
    const size_t samples = external_psram_size ? kBufferSamples : kBufferSamples / 32;
    max_delay_secs_ = (samples - 1) / AUDIO_SAMPLE_RATE_EXACT;
  }

  // Acquire() is called once per session, from TweightyApp::ActivateOnce()
  // on Tweighty's first RESUME (control-rate); nothing in the app's ordinary
  // lifecycle calls Release() any more -- the engine stays connected and
  // processing for the rest of the session regardless of which app is
  // current (see ActivateOnce()'s comment). Release() stays defined and
  // correct anyway, as the hook a future explicit "close the looper" action
  // would call. update() runs from the audio ISR (software_isr(), via
  // AudioStream's per-block dispatch) on whatever streams are currently
  // AudioConnection_F32-"active" -- and that codebase-wide library never
  // resets `active` on disconnect() (only stock int16 AudioConnection does),
  // so update() can keep firing on this stream after a caller believes a
  // disconnect+Release() has stopped it. ready_ is the actual safety gate
  // update() trusts, independent of that: Release() clears it FIRST, before
  // freeing anything, so an update() that starts after this point sees
  // ready_==false and returns before touching a soon-freed pointer;
  // Acquire() sets it LAST, after every allocation is complete, so update()
  // never sees a partially-built engine. Whichever caller ever brackets a
  // connect/disconnect against this (ActivateOnce() today) additionally
  // wraps it in AudioNoInterrupts()/AudioInterrupts() to close the narrower
  // window where update() is already mid-flight -- ready_ alone only orders
  // visibility, it doesn't preempt an in-progress call.
  void Acquire() {
    buffer_l_.Acquire();
    buffer_r_.Acquire();
    // TODO xfade lut should be static (see AudioDelayExtF32::Acquire, same TODO)
    xfade_in_scalars_ = new float[kCrossfadeSamples];
    xfade_out_scalars_ = new float[kCrossfadeSamples];
    const float n = static_cast<float>(kCrossfadeSamples - 1);
    for (size_t i = 0; i < kCrossfadeSamples; i++) {
      EqualPowerFade(xfade_out_scalars_[i], xfade_in_scalars_[i], i / n);
    }
    ready_ = true;
  }

  void Release() {
    ready_ = false;
    delete[] xfade_in_scalars_;
    delete[] xfade_out_scalars_;
    xfade_in_scalars_ = nullptr;
    xfade_out_scalars_ = nullptr;
    buffer_l_.Release();
    buffer_r_.Release();
  }

  bool IsReady() const { return ready_ && buffer_l_.IsReady() && buffer_r_.IsReady(); }

  // --- App -> engine, control-rate (Controller()/Loop(), single writer) ----

  // Loop-window reference length -- drives the RECIRC wrap detector below,
  // not the taps directly (the App computes each tap's own target via
  // TweightyTapPhase and pushes it through SetTapTargetSecs).
  void SetTargetTimeSeconds(float secs) {
    CONSTRAIN(secs, min_delay_secs_, max_delay_secs_);
    loop_window_secs_ = secs;
  }

  void SetTapCount(int n) {
    if (n < 1) n = 1;
    if (n > static_cast<int>(kTaps)) n = static_cast<int>(kTaps);
    tap_count_ = static_cast<size_t>(n);
    active_tap_mask_ = static_cast<uint8_t>((1u << tap_count_) - 1u);
  }

  // One tap's target delay time, already resolved from TweightyTapPhase by
  // the caller. Crossfades to the new value exactly like
  // AudioDelayExtF32::cf_delay -- see ReadTapChunk() for the read side.
  void SetTapTargetSecs(int tap, float secs) {
    if (tap < 0 || tap >= static_cast<int>(kTaps)) return;
    CONSTRAIN(secs, min_delay_secs_, max_delay_secs_);
    auto &t = target_delay_[tap];
    if (t.phase >= kCrossfadeSamples && t.target != secs) {
      // Same noise floor AudioDelayExtF32::cf_delay documents: below it, a
      // "changed" TIME reading is clock jitter, not a real edit.
      if (t.target > 0.0f && std::abs(t.target - secs) / t.target < 0.001f)
        return;
      t.phase = 0;
      t.target = secs;
    }
  }

  // 288r precedent: each tap's own POT8-16 slider feeds only that tap's own
  // output jack, never a shared per-tap-count-normalized bus scalar -- see
  // TweightyApp.h's class comment for the hardware citation. tap_gain (the
  // EQUAL_POWER_EQUAL_MIX[tap_count_] scaling below) still applies on top,
  // same as before, so adding taps doesn't silently deepen the mix.
  void SetTapMix(int tap, float level) {
    if (tap < 0 || tap >= static_cast<int>(kTaps)) return;
    CONSTRAIN(level, 0.0f, 1.0f);
    tap_mix_[tap] = level;
  }

  // Not a 288r feature -- Tweighty's own extension, decoupled from
  // SetTapMix() so a tap can feed the recirculation path at a different
  // level than it feeds the output bus (silent taps can still drive the
  // loop, audible taps can stay out of it). Defaults to 1.0 so an
  // untouched tap reproduces the old uniform-gain feedback math exactly.
  void SetTapFeedback(int tap, float level) {
    if (tap < 0 || tap >= static_cast<int>(kTaps)) return;
    CONSTRAIN(level, 0.0f, 1.0f);
    tap_feedback_[tap] = level;
  }

  void SetFeedback(float fb) {
    CONSTRAIN(fb, 0.0f, 1.0f);
    feedback_ = fb;
  }

  void SetWetDry(float mix) {
    CONSTRAIN(mix, 0.0f, 1.0f);
    wet_dry_ = mix;
  }

  void RequestTransportToggle() { transport_request_toggle(transport_); }

  // --- engine -> App, audio-ISR-hot mirrors (App reads only) ---------------
  volatile uint8_t transport_state_ = XP_WRITE;
  volatile float meter_level_ = 0.0f;
  volatile bool crossfade_active_ = false;
  volatile uint8_t active_tap_mask_ = 0xFF;

  void update(void) override {
    if (!IsReady()) return;

    audio_block_f32_t *in_l = receiveWritable_f32(0);
    audio_block_f32_t *in_r = receiveWritable_f32(1);
    if (!in_l) {
      in_l = AudioStream_F32::allocate_f32();
      if (!in_l) {
        if (in_r) AudioStream_F32::release(in_r);
        return;
      }
      std::fill(in_l->data, in_l->data + AUDIO_BLOCK_SAMPLES, 0.0f);
    }
    if (!in_r) {
      in_r = AudioStream_F32::allocate_f32();
      if (!in_r) {
        AudioStream_F32::release(in_l);
        return;
      }
      std::fill(in_r->data, in_r->data + AUDIO_BLOCK_SAMPLES, 0.0f);
    }

    audio_block_f32_t *out_l = AudioStream_F32::allocate_f32();
    audio_block_f32_t *out_r = AudioStream_F32::allocate_f32();
    if (!out_l || !out_r) {
      if (out_l) AudioStream_F32::release(out_l);
      if (out_r) AudioStream_F32::release(out_r);
      AudioStream_F32::release(in_l);
      AudioStream_F32::release(in_r);
      return;
    }

    // A genuine WRITE<->RECIRC mode edge and a RECIRC loop-wrap share the
    // same resync_edge flag (the tap-read crossfade in ReadTapChunk() below
    // wants to know about both), but the write gate must react to ONLY the
    // former: a loop-wrap doesn't change transport_should_write()'s answer,
    // yet restarting write_gate_phase_ at 0 on every wrap re-reads
    // xfade_out_scalars_[0] == 1.0 -- an already-settled RECIRC gate (wg ==
    // 0.0) jumped back to full gain and ramped down again over ~43ms, every
    // ~loop_window_secs_, briefly re-admitting live input into what should
    // be a frozen loop. Comparing against the write gate's own last-known
    // mode (not just consuming the shared edge) tells the two events apart.
    // Denormal guard, same pattern as AudioEffectModalResonator::update()
    // (Audio/AudioEffectModalResonator.h): in RECIRC with 0 < feedback < 1
    // and a quiet captured window, buffer_l_/buffer_r_'s content decays
    // geometrically toward (not through) zero every pass and will cross
    // into float32 denormal range before it reaches exact zero. Cortex-M7
    // handles denormals in software microcode, not hardware, so without
    // this a quiet decaying tail is a real CPU-time/glitch risk, not just a
    // correctness nicety. FZ+DN (FPSCR bits 24,25) is restored before this
    // function returns so it never leaks into any other stream's update().
    uint32_t fpscr_save;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr_save));
    __asm__ volatile("vmsr fpscr, %0" :: "r"(fpscr_save | 0x03000000u));

    const bool edge = transport_consume_resync_edge(transport_);
    const bool want_write = transport_should_write(transport_);
    if (edge && want_write != write_gate_last_mode_) {
      write_gate_phase_ = 0;
      write_gate_fading_to_write_ = want_write;
    }
    write_gate_last_mode_ = want_write;
    transport_state_ = static_cast<uint8_t>(transport_.mode);

    float tap_l[kChunkSize], tap_r[kChunkSize];
    float tmp_l[kChunkSize], tmp_r[kChunkSize];
    float out_mix_l[kChunkSize], out_mix_r[kChunkSize];
    float fb_mix_l[kChunkSize], fb_mix_r[kChunkSize];
    float write_l[kChunkSize], write_r[kChunkSize];
    float gated_l[kChunkSize], gated_r[kChunkSize];

    const float fb = feedback_.ReadNext();
    float dry_gain, wet_gain;
    EqualPowerFade(dry_gain, wet_gain, wet_dry_.ReadNext());
    const float tap_gain = EQUAL_POWER_EQUAL_MIX[tap_count_];

    // tap_gain folds into both: it's the tap-count normalization keeping
    // total loop gain roughly invariant as tap_count_ changes, and applying
    // it to only one of mix/feedback would make adding taps a hidden
    // feedback-depth boost.
    float tap_mix_gain[kTaps];
    float tap_fb_gain[kTaps];
    for (size_t tap = 0; tap < tap_count_; tap++) {
      tap_mix_gain[tap] = tap_mix_[tap].ReadNext() * tap_gain;
      tap_fb_gain[tap] = tap_feedback_[tap].ReadNext() * tap_gain;
    }

    float peak = 0.0f;

    for (uint_fast16_t chunk_start = 0; chunk_start < AUDIO_BLOCK_SAMPLES;
         chunk_start += kChunkSize) {
      auto *chin_l = in_l->data + chunk_start;
      auto *chin_r = in_r->data + chunk_start;

      std::fill(out_mix_l, out_mix_l + kChunkSize, 0.0f);
      std::fill(out_mix_r, out_mix_r + kChunkSize, 0.0f);
      std::fill(fb_mix_l, fb_mix_l + kChunkSize, 0.0f);
      std::fill(fb_mix_r, fb_mix_r + kChunkSize, 0.0f);

      for (size_t tap = 0; tap < tap_count_; tap++) {
        ReadTapChunk(tap, tap_l, tap_r, tmp_l, tmp_r);

        arm_scale_f32(tap_l, tap_mix_gain[tap], tmp_l, kChunkSize);
        arm_scale_f32(tap_r, tap_mix_gain[tap], tmp_r, kChunkSize);
        arm_add_f32(out_mix_l, tmp_l, out_mix_l, kChunkSize);
        arm_add_f32(out_mix_r, tmp_r, out_mix_r, kChunkSize);

        arm_scale_f32(tap_l, tap_fb_gain[tap], tmp_l, kChunkSize);
        arm_scale_f32(tap_r, tap_fb_gain[tap], tmp_r, kChunkSize);
        arm_add_f32(fb_mix_l, tmp_l, fb_mix_l, kChunkSize);
        arm_add_f32(fb_mix_r, tmp_r, fb_mix_r, kChunkSize);
      }

      // Write gate: how much of this chunk's live input reaches the buffer,
      // equal-power-ramped across a mode toggle so WRITE<->RECIRC is never a
      // hard cut, however deep the feedback is set.
      float wg;
      if (write_gate_phase_ < kCrossfadeSamples) {
        wg = write_gate_fading_to_write_ ? xfade_in_scalars_[write_gate_phase_]
                                          : xfade_out_scalars_[write_gate_phase_];
        write_gate_phase_ += kChunkSize;
      } else {
        wg = write_gate_fading_to_write_ ? 1.0f : 0.0f;
      }
      crossfade_active_ = write_gate_phase_ < kCrossfadeSamples;

      arm_scale_f32(chin_l, wg, gated_l, kChunkSize);
      arm_scale_f32(chin_r, wg, gated_r, kChunkSize);
      arm_scale_f32(fb_mix_l, fb, write_l, kChunkSize);
      arm_scale_f32(fb_mix_r, fb, write_r, kChunkSize);
      arm_add_f32(write_l, gated_l, write_l, kChunkSize);
      arm_add_f32(write_r, gated_r, write_r, kChunkSize);

      buffer_l_.Write(write_l, kChunkSize);
      buffer_r_.Write(write_r, kChunkSize);

      auto *chout_l = out_l->data + chunk_start;
      auto *chout_r = out_r->data + chunk_start;
      arm_scale_f32(chin_l, dry_gain, chout_l, kChunkSize);
      arm_scale_f32(chin_r, dry_gain, chout_r, kChunkSize);
      arm_scale_f32(out_mix_l, wet_gain, tmp_l, kChunkSize);
      arm_scale_f32(out_mix_r, wet_gain, tmp_r, kChunkSize);
      arm_add_f32(chout_l, tmp_l, chout_l, kChunkSize);
      arm_add_f32(chout_r, tmp_r, chout_r, kChunkSize);

      for (size_t i = 0; i < kChunkSize; i++) {
        const float a = std::abs(chout_l[i]);
        if (a > peak) peak = a;
      }
    }

    meter_level_ = peak;

    // Block-granular loop-wrap bookkeeping is enough: the read side has no
    // seam to hide (see the class comment), so this only retriggers the
    // write-gate crossfade above and feeds the UI's tap-ring animation --
    // never gates audio continuity on its own.
    if (transport_should_write(transport_)) {
      loop_phase_samples_ = 0;
    } else {
      loop_phase_samples_ += AUDIO_BLOCK_SAMPLES;
      const uint32_t wrap_samples =
        static_cast<uint32_t>(loop_window_secs_ * AUDIO_SAMPLE_RATE_EXACT);
      if (wrap_samples > 0 && loop_phase_samples_ >= wrap_samples) {
        loop_phase_samples_ -= wrap_samples;
        transport_signal_loop_wrap(transport_);
      }
    }

    __asm__ volatile("vmsr fpscr, %0" :: "r"(fpscr_save));

    AudioStream_F32::release(in_l);
    AudioStream_F32::release(in_r);
    AudioStream_F32::transmit(out_l, 0);
    AudioStream_F32::transmit(out_r, 1);
    AudioStream_F32::release(out_l);
    AudioStream_F32::release(out_r);
  }

private:
  static constexpr size_t kChunkSize = 8;
  static constexpr size_t kCrossfadeSamples = 2048;
  static constexpr size_t kTaps = kTweightyTapCount;
  // 1024*512 samples/channel: matches AudioDelayExtF32<9>'s DELAY_LENGTH in
  // audio_applets/DelayApplet.h (~12s with PSRAM, /32 without -- see the
  // class comment for why this number is borrowed rather than re-derived).
  static constexpr size_t kBufferSamples = 1024 * 512;

  struct CrossfadeTarget {
    float target = 0.0f;
    uint16_t phase = kCrossfadeSamples;
  };

  audio_block_f32_t *input_queue_array_[2];
  // Written by Acquire()/Release() (control-rate), read by update() (audio
  // ISR) -- see the Acquire()/Release() comment for why this is the real
  // safety gate, not just buffer_l_/buffer_r_'s own IsReady().
  volatile bool ready_ = false;
  float *xfade_in_scalars_ = nullptr;
  float *xfade_out_scalars_ = nullptr;

  std::array<CrossfadeTarget, kTaps> target_delay_;
  std::array<OnePole<Interpolated>, kTaps> delay_secs_;
  std::array<Interpolated, kTaps> tap_mix_;
  std::array<Interpolated, kTaps> tap_feedback_;

  ExtAudioBuffer<float> buffer_l_;
  ExtAudioBuffer<float> buffer_r_;

  size_t tap_count_ = kTaps;
  float min_delay_secs_ = 0.0f;
  float max_delay_secs_ = MAX_LOOP_SECONDS;
  float loop_window_secs_ = 1.0f;
  uint32_t loop_phase_samples_ = 0;

  Interpolated feedback_{0.0f, AUDIO_BLOCK_SAMPLES / kChunkSize};
  Interpolated wet_dry_{0.5f, AUDIO_BLOCK_SAMPLES / kChunkSize};

  TweightyTransportState transport_;
  // Starts fully open (not fading, fading-to-write) so the initial WRITE
  // mode is live from the first block -- no click at Acquire().
  uint16_t write_gate_phase_ = kCrossfadeSamples;
  bool write_gate_fading_to_write_ = true;
  // The write gate's own last-observed mode, so a loop-wrap-only resync edge
  // (mode unchanged) can be told apart from a genuine WRITE<->RECIRC edge --
  // see the update() comment above. Matches transport_'s initial XP_WRITE.
  bool write_gate_last_mode_ = true;

  // Shared between channels because a tap's delay TIME is identical for L
  // and R -- only the buffer contents differ. Sharing keeps the two
  // channels' reads from a tap sample-for-sample in phase, and halves the
  // crossfade bookkeeping this would otherwise need per channel.
  void ReadTapChunk(size_t tap, float *out_l, float *out_r, float *tmp_l,
                     float *tmp_r) {
    auto &tap_delay = delay_secs_[tap];
    auto &target = target_delay_[tap];

    if (tap_delay.Done() || target.phase < kCrossfadeSamples) {
      buffer_l_.ReadFromSecondsAgo(tap_delay.Read(), out_l, kChunkSize);
      buffer_r_.ReadFromSecondsAgo(tap_delay.Read(), out_r, kChunkSize);
      if (target.phase < kCrossfadeSamples) {
        buffer_l_.ReadFromSecondsAgo(target.target, tmp_l, kChunkSize);
        buffer_r_.ReadFromSecondsAgo(target.target, tmp_r, kChunkSize);
        arm_mult_f32(tmp_l, xfade_in_scalars_ + target.phase, tmp_l, kChunkSize);
        arm_mult_f32(tmp_r, xfade_in_scalars_ + target.phase, tmp_r, kChunkSize);
        arm_mult_f32(out_l, xfade_out_scalars_ + target.phase, out_l, kChunkSize);
        arm_mult_f32(out_r, xfade_out_scalars_ + target.phase, out_r, kChunkSize);
        arm_add_f32(out_l, tmp_l, out_l, kChunkSize);
        arm_add_f32(out_r, tmp_r, out_r, kChunkSize);
        target.phase += kChunkSize;
        if (target.phase >= kCrossfadeSamples) {
          tap_delay = target.target;
          tap_delay.Reset();
        }
      }
    } else {
      for (size_t s = 0; s < kChunkSize; s++) {
        const float pos =
          tap_delay.ReadNext() * AUDIO_SAMPLE_RATE_EXACT - static_cast<float>(s);
        out_l[s] = buffer_l_.ReadInterp(pos);
        out_r[s] = buffer_r_.ReadInterp(pos);
      }
    }
  }
};
