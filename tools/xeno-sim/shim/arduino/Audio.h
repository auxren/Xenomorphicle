#ifndef XENOSIM_AUDIO_H_
#define XENOSIM_AUDIO_H_
// The Teensy Audio library, absent. The simulator carries no audio graph at
// all -- see README.md -- and the only thing the UI half asks it for is the
// CPU and memory usage the debug screen prints. Both read zero here, so that
// screen's audio numbers are meaningless and say nothing about the module's.
#include <stdint.h>
#define AUDIO_SAMPLE_RATE 44100.0f
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f
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
