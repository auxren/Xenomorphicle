#ifndef XENOSIM_AUDIO_H_
#define XENOSIM_AUDIO_H_
// The Teensy Audio library, absent. The simulator carries no audio graph at
// all -- see README.md -- and the only thing the UI half asks it for is the
// CPU and memory usage the debug screen prints. Both read zero here, so that
// screen's audio numbers are meaningless and say nothing about the module's.
//
// AudioStream.h (a sibling shim, not this file) supplies the stock int16
// AudioStream/AudioConnection pair -- needed only so that
// software/src/extern/f32/AudioStream_F32.h (real firmware) has a base class
// to derive from and TweightyApp.h's raw AudioConnection* members type-check.
// See that file's own header comment for why it exists and what it does not
// attempt to model.
#include <stdint.h>
#include "AudioStream.h"
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 44100.0f
#endif
#ifndef AUDIO_SAMPLE_RATE_EXACT
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f
#endif
#ifndef AUDIO_BLOCK_SAMPLES
#define AUDIO_BLOCK_SAMPLES 128
#endif
static inline float AudioProcessorUsage() { return 0.f; }
static inline float AudioProcessorUsageMax() { return 0.f; }
static inline void AudioProcessorUsageMaxReset() {}
static inline unsigned AudioMemoryUsage() { return 0; }
static inline unsigned AudioMemoryUsageMax() { return 0; }
static inline void AudioMemoryUsageMaxReset() {}
static inline void AudioNoInterrupts() {}
static inline void AudioInterrupts() {}
#endif
