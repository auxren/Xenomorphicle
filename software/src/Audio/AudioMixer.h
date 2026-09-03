#pragma once

#include "AudioParam.h"
#include <arm_math.h>
#include <Audio.h>

template <size_t NumChannels>
class AudioMixer : public AudioStream {
public:
  AudioMixer() : AudioStream(NumChannels, inputQueueArray) {}

  void gain(size_t channel, float gain) {
    if (channel >= NumChannels) return;
    gains[channel] = gain;
  }

  virtual void update(void) {
    float aux[AUDIO_BLOCK_SAMPLES];
    float out[AUDIO_BLOCK_SAMPLES];
    bool out_filled = false;

    // Originally I had this scaling and mixing all with q15 but that has much
    // less desirable saturation behavior with phase correlated material.
    for (size_t channel = 0; channel < NumChannels; channel++) {
      auto* in = receiveReadOnly(channel);
      if (in) {
        float gain = gains[channel];
        if (!out_filled) {
          arm_fill_f32(0.0f, out, AUDIO_BLOCK_SAMPLES);
          out_filled = true;
        }
        if (gain != 0.0f) {
          arm_q15_to_float(in->data, aux, AUDIO_BLOCK_SAMPLES);
          if (gain != 1.0f) {
            arm_scale_f32(aux, gain, aux, AUDIO_BLOCK_SAMPLES);
          }
          arm_add_f32(aux, out, out, AUDIO_BLOCK_SAMPLES);
        }
        release(in);
      }
    }

    if (out_filled) {
      audio_block_t* out_block = allocate();
      arm_float_to_q15(out, out_block->data, AUDIO_BLOCK_SAMPLES);
      transmit(out_block);
      release(out_block);
    }
  }

private:
  audio_block_t* inputQueueArray[NumChannels];
  std::array<float, NumChannels> gains = {0.0f};
};

// AudioSummingRoute<NumChannels, NumSources>: a shared destination that
// legitimately has more than one concurrently-live source feeding each
// output channel -- unlike AudioPassthrough<N> (Audio/AudioPassthrough.h),
// a pure per-channel relay where, if two sources are both connected to the
// same channel, whichever source's update() the audio ISR happens to run
// last for a given block silently wins and the other's audio never gets
// out. This actually sums them, using the same q15<->float scale/add
// approach as AudioMixer<N> above (see its comment: mixing directly in q15
// has much worse saturation behavior with phase-correlated material than
// summing in float and letting the final q15 conversion saturate cleanly).
//
// Input index layout is source-major: source `s`'s channel `c` lands at
// index `s * NumChannels + c`. A single pre-existing connection that targets
// index == channel (unaware any sharing is going on, e.g. dest_index ==
// `side` for a stereo pair) keeps landing on source slot 0 completely
// unchanged; only a *new* concurrent source has to know to claim a
// different slot by offsetting its own connections by `NumChannels` per
// slot. Gain is fixed at unity per source -- see the call site (AudioIO.cpp)
// for why that's a deliberate, not-yet-fully-solved gain-staging choice.
template <uint8_t NumChannels, uint8_t NumSources>
class AudioSummingRoute : public AudioStream {
public:
  AudioSummingRoute()
    : AudioStream(NumChannels * NumSources, inputQueueArray) {}

  virtual void update(void) {
    float aux[AUDIO_BLOCK_SAMPLES];
    float out[AUDIO_BLOCK_SAMPLES];

    for (uint8_t ch = 0; ch < NumChannels; ch++) {
      bool out_filled = false;
      for (uint8_t src = 0; src < NumSources; src++) {
        audio_block_t* in = receiveReadOnly(src * NumChannels + ch);
        if (in) {
          if (!out_filled) {
            arm_fill_f32(0.0f, out, AUDIO_BLOCK_SAMPLES);
            out_filled = true;
          }
          arm_q15_to_float(in->data, aux, AUDIO_BLOCK_SAMPLES);
          arm_add_f32(aux, out, out, AUDIO_BLOCK_SAMPLES);
          release(in);
        }
      }
      if (out_filled) {
        audio_block_t* out_block = allocate();
        if (out_block) {
          arm_float_to_q15(out, out_block->data, AUDIO_BLOCK_SAMPLES);
          transmit(out_block, ch);
          release(out_block);
        }
      }
    }
  }

private:
  audio_block_t* inputQueueArray[NumChannels * NumSources];
};
