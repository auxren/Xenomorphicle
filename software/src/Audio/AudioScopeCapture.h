#pragma once

#include <Arduino.h>
#include <AudioStream.h>

// ---------------------------------------------------------------------------
// The Scope's audio tap (apps/ScopeApp.h). Four mono inputs (audio in L/R,
// audio out L/R), wired permanently at the app's Init(). Every block is
// received and released, as the Audio Library requires; a tap that the
// Scope is showing also hands its full-rate samples to ScopeCapture, which
// decimates them into the tap's ring (ScopeRing.h). A tap nobody is showing
// costs a release() and one table lookup per block.
//
// Audio-ISR-hot: never FLASHMEM, no allocation, no blocking. Same placement
// discipline as AudioAnalyzeStrobe::update() (Audio/AudioAnalyzeStrobe.h)
// and AudioTweightyF32::update() (Audio/AudioTweightyF32.h).
// ---------------------------------------------------------------------------
#include "../ScopeCapture.h"

class AudioScopeCapture : public AudioStream {
public:
  static constexpr int kChannels = 4;  // in L, in R, out L, out R
  enum Tap {
    TAP_IN_L = 0,
    TAP_IN_R = 1,
    TAP_OUT_L = 2,
    TAP_OUT_R = 3,
  };
  AudioScopeCapture() : AudioStream(kChannels, inputQueueArray_) {}

  virtual void update(void);

private:
  audio_block_t *inputQueueArray_[kChannels];
};

// Audio-ISR context: never FLASHMEM (see the class comment above).
inline void AudioScopeCapture::update(void) {
  for (int ch = 0; ch < kChannels; ++ch) {
    audio_block_t *blk = receiveReadOnly(ch);
    if (!blk) continue;
    ScopeCapture::OnAudio(ch, blk->data, AUDIO_BLOCK_SAMPLES);
    release(blk);
  }
}
