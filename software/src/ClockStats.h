#ifndef CLOCKSTATS_H_
#define CLOCKSTATS_H_
// ---------------------------------------------------------------------------
// Free-running time counters for measuring the crystal against a host clock.
//
// Console command `%t` (ScopeFreqConsole.h) answers one line
//
//   STIME,<cycles64>,<audio_samples64>,<cv_frames64>,<seq>
//
//   cycles64        ARM_DWT_CYCCNT extended to 64 bits (F_CPU_ACTUAL = 600 MHz, from
//                   the same 24 MHz crystal as the audio PLL and the ADC trigger timer)
//   audio_samples64 audio samples the graph has processed (blocks of 128, counted in the
//                   audio ISR by AudioScopeCapture::update, whether or not the Scope is open)
//   cv_frames64     CV ADC frames the core ISR has consumed (OC_ADC.cpp), also always on
//   seq             reply number, wraps at 2^32
//
// All three are read in one interrupts-off window, so they describe one instant (a
// few dozen cycles). The line is built with integer arithmetic only (the module's
// snprintf prints nothing for %f/%e/%g; see ScopeFmt.h).
//
// ISR cost: one load, add, store of a 32-bit word per audio block (every 2.667 ms)
// and per ADC pass that found frames. Everything else runs in the console handler.
//
// Wrap handling (state is touched from loop context only, never from an ISR):
//   cycles   the 32-bit counter wraps every 7.16 s. The extender also looks at millis()
//            and picks the number of wraps that agrees with the elapsed milliseconds, so
//            a loop stall (a 45 s flash format) or a host that polls every few minutes
//            still gets a monotonic 64-bit value. Needs the two reads to be within a
//            fraction of a wrap (3.5 s) of each other in cycles vs millis() skew: true.
//   audio    32-bit count of 128-sample blocks: wraps after 3.3 years. Extended by delta.
//   cv       32-bit count of frames: wraps after 32 h at 37.35 kHz, so a host must ask
//            at least once per 32 h (the extender adds the modular delta).
// A reading is "since boot" only approximately (the counters start at the first
// `%t`); only differences between replies are meaningful.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace ClockStats {

constexpr uint32_t kBlockSamples = 128;   // AUDIO_BLOCK_SAMPLES

// Written by ISRs (single writer each), read by Capture() with interrupts off.
inline volatile uint32_t g_audio_blocks = 0;
inline volatile uint32_t g_cv_frames = 0;

// Audio ISR: once per block.
inline void AudioBlock() { g_audio_blocks = g_audio_blocks + 1; }
// Core ISR (ADC): n frames consumed.
inline void CvFrames(uint32_t n) { g_cv_frames = g_cv_frames + n; }

// One instant: the raw 32-bit readings.
struct Raw {
  uint32_t cyc = 0;      // ARM_DWT_CYCCNT
  uint32_t ms = 0;       // millis() at the same instant
  uint32_t blocks = 0;   // g_audio_blocks
  uint32_t frames = 0;   // g_cv_frames
};

struct Stamp {
  uint64_t cycles = 0;
  uint64_t audio = 0;    // samples
  uint64_t cv = 0;       // frames
  uint32_t seq = 0;
};

// The 64-bit extension and the reply counter. Loop context only.
class Clock {
public:
  explicit constexpr Clock(uint32_t cycles_per_ms = 600000u) : cpm_(cycles_per_ms) {}
  Stamp Sample(const Raw &r);   // advances seq
  uint32_t seq() const { return seq_; }
private:
  uint32_t cpm_;
  uint32_t last_cyc_ = 0, last_ms_ = 0, last_blocks_ = 0, last_frames_ = 0;
  uint64_t cycles_ = 0, blocks_ = 0, frames_ = 0;
  uint32_t seq_ = 0;
};

// "STIME,<c>,<a>,<f>,<seq>" (no line ending), integer arithmetic only. Returns the
// length the full text has, like snprintf; always NUL-terminates when n > 0.
int FormatStime(char *buf, size_t n, const Stamp &s);

// Decimal text of v, no padding; returns the length (buf needs 21 bytes).
int U64ToDec(char *buf, uint64_t v);

#if defined(__IMXRT1062__)
// Reads the four counters in one interrupts-off window.
Raw Capture();
#endif

}  // namespace ClockStats

#endif  // CLOCKSTATS_H_
