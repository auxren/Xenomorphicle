#pragma once

// Float32 port of AudioDelayExt (see AudioDelayExt.h): same multi-tap,
// crossfade/stretch modulated delay, but the buffer and the feedback path stay
// float32 between blocks, so recirculating audio never truncates to int16.

#include <new>   // std::nothrow
#include "AudioBuffer.h"
#include "XfadeTable.h"
#include "AudioParam.h"
#include "../dsputils.h"
#include "../util/util_macros.h"
#include "../extern/f32/AudioStream_F32.h"
#include <Audio.h>
#include <cstdint>

template <
  size_t Taps = 1,
  // ChunkSize *must* evenly divide AUDIO_BLOCK_SAMPLES
  size_t ChunkSize = 8,
  // Crossfade time will be AUDIO_SAMPLE_RATE / CrossfadeSamples
  // 2048 give ~48ms, or ~22hz, just below human audio range
  size_t CrossfadeSamples = 2048>
class AudioDelayExtF32 : public AudioStream_F32 {
public:
  float MAX_DELAY_SECS, MIN_DELAY_SECS;

  AudioDelayExtF32(size_t BufferLength = static_cast<size_t>(AUDIO_SAMPLE_RATE))
    : AudioStream_F32(1, input_queue_array), buffer(BufferLength) {
    delay_secs.fill(OnePole(Interpolated(0.0f, AUDIO_BLOCK_SAMPLES), 0.0002f));
    fb.fill(Interpolated(0.0f, AUDIO_BLOCK_SAMPLES / ChunkSize));

    // -1 and +2 to ensure we have enough points for hermite interpolation
    MAX_DELAY_SECS = (BufferLength - 1) / AUDIO_SAMPLE_RATE_EXACT;
    MIN_DELAY_SECS = (ChunkSize + 2) / AUDIO_SAMPLE_RATE_EXACT;
  }

  void Acquire() {
    buffer.Acquire();
    // The crossfade tables are constexpr (XfadeTable.h): nothing to allocate,
    // so there is no failure path left here. They used to be two `new float[]`
    // per instance, 16 KB of RAM2 each; two of those faulted the module on the
    // bench when the Sampler held the rest of the heap (2026-09-18).
    //
    // Failing to get the buffer is not fatal to the instrument: IsReady() is
    // false and update() early-returns. With no PSRAM chip, extmem_calloc falls
    // back to calloc (RAM2), so the buffer is the reduced size if the heap can
    // supply it, and absent only when it cannot.
  }

  void Release() {
    buffer.Release();
  }

  void delay(size_t tap, float secs) {
    CONSTRAIN(secs, MIN_DELAY_SECS, MAX_DELAY_SECS);
    delay_secs[tap] = secs;
    if (delay_secs[tap].Read() == 0.0f) delay_secs[tap].Reset();
  }

  void cf_delay(size_t tap, float secs) {
    CONSTRAIN(secs, MIN_DELAY_SECS, MAX_DELAY_SECS);
    auto& t = target_delay[tap];
    if (t.phase >= CrossfadeSamples && t.target != secs) {
      // 0.0005f comes from variation
      // I observed a <0.0005 noise in detecting internal clock signal. External
      // I saw a bit larger with faster clocks. 0.001 seems reasonable, though
      // it means, with full delay, <12ms changes will be ignored.
      if (abs(t.target - secs) / t.target < 0.001f) {
        return;
      }
      t.phase = 0;
      t.target = secs;
    }
  }

  void taps(size_t num) {
    taps_ = num;
  }

  // Did Acquire() actually get the buffer? It comes from extmem_calloc, which
  // falls back to calloc in RAM2 when no PSRAM is fitted (core extmem.c), so
  // this is false only when neither pool could supply it; then update() below
  // produces nothing at all. update()'s own
  // `!buffer.IsReady()` guard keeps that from crashing; this lets an applet
  // or an app SAY so instead of going quietly silent.
  bool BufferReady() const {
    return buffer.IsReady();
  }

  void feedback(size_t tap, float fb) {
    this->fb[tap] = fb;
  }

  void update(void) override {
    if (!buffer.IsReady()) return;
    audio_block_f32_t* in_block = receiveWritable_f32();

    if (in_block == NULL) {
      // Even if we're not getting anything we need to process feedback.
      // e.g. a VCA will actually not output blocks when closed.
      in_block = AudioStream_F32::allocate_f32();
      if (in_block == NULL) return;
      std::fill(in_block->data, in_block->data + AUDIO_BLOCK_SAMPLES, 0.0f);
    }

    audio_block_f32_t* outs[Taps];
    bool alloc_failed = false;
    for (uint_fast8_t tap = 0; tap < taps_; tap += 1) {
      outs[tap] = AudioStream_F32::allocate_f32();
      if (outs[tap] == NULL) alloc_failed = true;
    }
    if (alloc_failed) {
      // Pool exhausted; drop this block rather than crashing.
      for (uint_fast8_t tap = 0; tap < taps_; tap++) {
        if (outs[tap]) AudioStream_F32::release(outs[tap]);
      }
      AudioStream_F32::release(in_block);
      return;
    }

    float temp_buff[ChunkSize];
    for (uint_fast8_t chunk_start = 0; chunk_start < AUDIO_BLOCK_SAMPLES;
         chunk_start += ChunkSize) {
      auto* in_chunk = in_block->data + chunk_start;
      for (uint_fast8_t tap = 0; tap < taps_; tap++) {
        auto& tap_delay = delay_secs[tap];
        auto& target = target_delay[tap];
        auto* chunk_out = outs[tap]->data + chunk_start;

        if (tap_delay.Done() || target.phase < CrossfadeSamples) {
          ReadCrossfadeChunk(delay_secs[tap], target, chunk_out, temp_buff);
        } else {
          ReadStretchChunk(tap_delay, chunk_out);
        }
        float f = fb[tap].ReadNext();
        arm_scale_f32(chunk_out, f, temp_buff, ChunkSize);
        arm_add_f32(temp_buff, in_chunk, in_chunk, ChunkSize);
      }
      buffer.Write(in_chunk, ChunkSize);
    }
    AudioStream_F32::release(in_block);
    for (uint_fast8_t tap = 0; tap < taps_; tap++) {
      AudioStream_F32::transmit(outs[tap], tap);
      AudioStream_F32::release(outs[tap]);
    }
  }

private:
  struct CrossfadeTarget {
    float target = 0.0f;
    uint16_t phase = CrossfadeSamples; // out of CrossfadeSamples
  };

  audio_block_f32_t* input_queue_array[1];
  const float* xfade_in_scalars = XfadeLut::kTable<CrossfadeSamples>.in.data();
  const float* xfade_out_scalars = XfadeLut::kTable<CrossfadeSamples>.out.data();
  std::array<CrossfadeTarget, Taps> target_delay;
  std::array<OnePole<Interpolated>, Taps> delay_secs;
  std::array<Interpolated, Taps> fb;
  ExtAudioBuffer<float> buffer;
  size_t taps_ = Taps;

  void ReadCrossfadeChunk(
    OnePole<Interpolated>& tap_delay,
    CrossfadeTarget& target,
    float* chunk_out,
    float* temp_buff
  ) {
    buffer.ReadFromSecondsAgo(tap_delay.Read(), chunk_out, ChunkSize);
    if (target.phase < CrossfadeSamples) {
      buffer.ReadFromSecondsAgo(target.target, temp_buff, ChunkSize);
      arm_mult_f32(
        temp_buff, xfade_in_scalars + target.phase, temp_buff, ChunkSize
      );
      arm_mult_f32(
        chunk_out, xfade_out_scalars + target.phase, chunk_out, ChunkSize
      );
      arm_add_f32(chunk_out, temp_buff, chunk_out, ChunkSize);
      target.phase += ChunkSize;
      if (target.phase >= CrossfadeSamples) {
        tap_delay = target.target;
        tap_delay.Reset();
      }
    }
  }

  void ReadStretchChunk(OnePole<Interpolated>& tap_delay, float* chunk_out) {
    for (uint_fast8_t sample = 0; sample < ChunkSize; sample++) {
      // No Clip16 here: float path saturates nowhere, which is the point.
      chunk_out[sample] = buffer.ReadInterp(
        tap_delay.ReadNext() * AUDIO_SAMPLE_RATE_EXACT - sample
      );
    }
  }
};
