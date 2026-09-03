#pragma once

#include <Arduino.h>
#include <AudioStream.h>

// ---------------------------------------------------------------------------
// Lightweight audio-rate scope tap for apps/ScopeApp.h. Up to 4 mono inputs
// (audio-in L, audio-in R, audio-out L, audio-out R); does NOT store every
// raw sample -- 48kHz into a 128px-wide trace is far more data than a scope
// can show, and scanning/storing all of it would waste ISR time for no
// visible benefit. Instead, update() (called once per audio block, from the
// audio ISR) writes exactly ONE representative value per connected block
// into a small per-channel ring buffer sized to the OLED's 128px width --
// one slot per screen column.
//
// Representative value: peak-abs-with-sign (the loudest sample in the
// block, sign preserved), not the block's first sample. A first-sample pick
// is cheaper but can alias past short transients/clicks sitting between
// decimated samples entirely; at this level of decimation (48kHz/block-rate
// down to ~128 columns) exact waveform shape is already lost either way, so
// peak-abs buys "never miss a transient" -- more useful for a level/activity
// monitor -- at the same O(block size) cost as a full scan would need
// anyway if it wanted the true peak.
//
// Only the CURRENTLY SELECTED channel actually gets scanned/written each
// block (SetActiveChannel) -- the other (up to 3) connected inputs are
// still received and released every block, as the Audio Library requires,
// but cost is a plain release(), not a 128-sample scan. This mirrors
// ScopeApp's own "only the selected channel keeps a live ring buffer"
// design (see that file's class comment) rather than paying to keep all 4
// channels live at once.
//
// Audio-ISR-hot: never FLASHMEM, no allocation, no blocking. Same placement
// discipline as AudioAnalyzeStrobe::update() (Audio/AudioAnalyzeStrobe.h)
// and AudioTweightyF32::update() (Audio/AudioTweightyF32.h).
// ---------------------------------------------------------------------------
class AudioScopeCapture : public AudioStream {
public:
  static constexpr int kChannels = 4;  // in L, in R, out L, out R
  static constexpr int kRingSize = 128;

  enum Tap {
    TAP_IN_L = 0,
    TAP_IN_R = 1,
    TAP_OUT_L = 2,
    TAP_OUT_R = 3,
  };

  AudioScopeCapture() : AudioStream(kChannels, inputQueueArray_) {}

  // -1 disables capture on every tap (inputs still get received/released,
  // just never scanned); 0..kChannels-1 selects which one tap actively
  // writes into its ring buffer. Control-rate call, safe from any context.
  void SetActiveChannel(int ch) {
    active_channel_ = (ch >= 0 && ch < kChannels) ? static_cast<int8_t>(ch) : (int8_t)-1;
  }

  // Restarts tap ch's ring at column 0 -- called on channel switch so a
  // freshly-selected audio channel does not show stale data left over from
  // whatever was selected before.
  void ResetRing(int ch) {
    if (ch < 0 || ch >= kChannels) return;
    __disable_irq();
    head_[ch] = 0;
    __enable_irq();
  }

  int16_t RingValue(int ch, int idx) const {
    return ring_[ch][idx & (kRingSize - 1)];
  }
  uint8_t Head(int ch) const { return head_[ch]; }

  virtual void update(void);

private:
  audio_block_t *inputQueueArray_[kChannels];
  volatile int8_t active_channel_ = -1;
  volatile uint8_t head_[kChannels] = {0, 0, 0, 0};
  volatile int16_t ring_[kChannels][kRingSize] = {};
};

// Audio-ISR context: never FLASHMEM (see the class comment above).
inline void AudioScopeCapture::update(void) {
  for (int ch = 0; ch < kChannels; ++ch) {
    audio_block_t *blk = receiveReadOnly(ch);
    if (!blk) continue;
    if (ch == active_channel_) {
      int16_t peak = 0;
      int32_t peak_abs = -1;
      for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {
        const int16_t s = blk->data[n];
        const int32_t a = (s < 0) ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
        if (a > peak_abs) { peak_abs = a; peak = s; }
      }
      const uint8_t h = head_[ch];
      ring_[ch][h] = peak;
      head_[ch] = (h + 1) & (kRingSize - 1);
    }
    release(blk);
  }
}
