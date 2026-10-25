#ifndef SCOPEFREQSERVICE_H_
#define SCOPEFREQSERVICE_H_
// ---------------------------------------------------------------------------
// The frequency counter as a running service: ONE static ScopeFreq::FreqCounter
// (about 450 bytes with its hand-off slot), fed from the capture path while the
// Scope is open, read from loop context. Pure apart from an interrupt mask in
// Select(): host-tested in test/test_scope_freq_service.cpp. Design:
// docs/specs/xeno-frequency-counter.md.
//
// LIFETIME. The counter is not in the Scope's lazily taken arena. An ISR
// pointer into that arena is the hazard the arena leak fix removed, and the
// counter has to keep working when the arena allocation fails. It runs only
// between Select(source) and Deselect(), which the Scope calls from loop
// context: Select when the Scope is open and not frozen, Deselect on suspend.
// With nothing selected the two ISR hooks cost one load and one compare.
//
// SOURCES. Audio in/out L/R are fed from the audio ISR, one 128-sample block
// at a time (ScopeCapture::OnAudio), at 48 kHz. CV in 1-8 are fed from the
// per-frame ADC hook (ScopeCapture::OnCvFrame), one sample per frame, at
// 37.35 kHz (kCvFsHz), DC-coupled: this is the route for LFOs and anything under the
// audio inputs' 1.6 Hz AC coupling. A CV input starts in the CV profile
// (10 Hz to 5 kHz, 0.1 s gate) and drops to CV_LOW (down to 0.5 Hz, 2 s
// gate) when the CV profile finds nothing countable; Poll() decides.
//
// GATE. FAST (0.1 s, what the screen uses) or SLOW (1 s, for calibration use).
// The error scales as T^1.5, so FAST reads about 30x coarser than SLOW; the
// result's own sigma says by how much.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include "ScopeFreq.h"
#include "ScopeFreqLine.h"
#include "ScopeSources.h"

namespace ScopeFreqSvc {

enum class Gate : uint8_t { FAST = 0, SLOW = 1 };
enum class Profile : uint8_t { NONE = 0, AUDIO, CV, CV_LOW };

constexpr double kAudioFsHz = 48000.0;
// The CV inputs' frame rate (one reading of each of the 8 inputs), derived from
// the timer setup in OC_ADC.cpp ADC33131D_Start():
//   comp1 = int(F_BUS_ACTUAL / 300000 / 2 + 0.5f) = 250;  TMR4 ch3 counts the
//   IPG clock (PCS(8): prescaler 1), LENGTH mode, OUTMODE(3): OFLAG toggles on
//   every compare; one reading is taken per OFLAG period (2 compares); the 8-way
//   mux makes 8 readings one frame.
//   F_BUS_ACTUAL is 150 MHz at 600 MHz (clockspeed.c: IPG = CPU/4, capped at 150 MHz).
// Ticks per compare: UNVERIFIED. The NXP SDK programs COMP1 = period - 1 for this
// counter mode, i.e. COMP1 + 1 ticks per compare, which gives 37,350.6 Hz
// (-3984 ppm against the nominal 37.5 kHz the comment in the driver implies). The
// reference manual is not in the repo and the hardware has not been asked.
// Everything below follows from these four lines; test_cv_rate_formula pins them.
// Time base: the ARM PLL (12 MHz * mult from the same 24 MHz crystal as the audio
// PLL), so crystal error is common with the audio sample rate; the audio PLL's own
// fractional-N error is not.
constexpr double kCvBusHz = 150000000.0;
constexpr int kCvTimerComp1 = 250;
constexpr int kCvTimerTicksPerCompare = kCvTimerComp1 + 1;   // UNVERIFIED (see above)
constexpr int kCvReadingsPerFrame = 8;
constexpr double kCvFsHz = kCvBusHz / (2.0 * kCvTimerTicksPerCompare * kCvReadingsPerFrame);
// Flip to true only after the bench procedure (docs/specs/xeno-frequency-counter.md,
// "CV frame rate") has agreed with kCvFsHz. While false, a CV reading carries the # mark.
constexpr bool kCvRateVerified = false;
// clock correction, in tenths of a ppm: +-1000 ppm
constexpr int kClockMaxPpm10 = 10000;
// An input that moves by more than this (LSB of the 0..32767 CV word, about 35 mV)
// without producing a countable signal is a slow one, not an idle one.
constexpr int kMovingLsb = 96;
// A CV frame word is 0..32767; the counter wants a signed centre.
constexpr int kCvOffset = 16384;

// Audio in/out L/R and CV in 1-8. Everything else (CV out, TR) is not counted.
bool Countable(ScopeSources::Source s);
const char *ProfileName(Profile p);

// The estimator settings for a profile and gate; clock_ppm10 as in SetClock().
ScopeFreq::Config MakeConfig(Profile p, Gate g, int clock_ppm10);

// CV profile selection from the results, kept apart from the ISR path.
struct AutoRange {
  uint8_t streak = 0;
  // The profile to use after result r while running `cur`. CV drops to CV_LOW
  // after two consecutive results that found nothing to count (NO SIGNAL or
  // TOO QUIET with the input moving by more than kMovingLsb, TOO LOW, SIGNAL
  // LOST); a flat or noise-floor input stays in CV, so a tone that appears
  // there is read within 0.2 s. CV_LOW returns to CV on silence, or on an OK
  // reading above 30 Hz.
  Profile Next(Profile cur, const ScopeFreq::Result &r);
};

// ---- loop context -------------------------------------------------------
// src is a ScopeSources::Source, or -1 to stop. Same source again: no-op (the
// Scope re-arms often and a reset would keep the gate from ever finishing).
// A source that is not Countable() stops the counter.
void Select(int src);
void Deselect();
int ActiveSource();         // -1 when stopped
Profile ActiveProfile();    // NONE when stopped
void SetGate(Gate g);       // takes effect now (restarts the gate)
Gate GetGate();
// The crystal correction: true_rate / nominal_rate - 1, in tenths of a ppm.
// False (and no change) outside +-kClockMaxPpm10. `calibrated` false clears.
bool SetClock(bool calibrated, int ppm10);
bool ClockCalibrated();
int ClockPpm10();
// The newest result, copied consistently. False until the first result after
// Select(). Safe against the ISR writing it at the same moment.
bool Read(ScopeFreq::Result *out);
// Loop context, every pass is fine: takes a new result when there is one, runs
// the CV auto-ranging, and feeds the Meter below.
void Poll();
// The screen's and the console's reading: what the Meter made of the results
// since Select(). Loop context only, like Poll().
const ScopeFreqLine::Reading &CurrentReading();
// Test hook: back to power-up state.
void ResetForTest();

// ---- ISR context --------------------------------------------------------
namespace detail {
extern ScopeFreq::FreqCounter g_counter;
// Both are stored plus one, so "nothing" is 0 and the variables are zero-initialised
// (.bss); a -1 initialiser would put them in RAM1's .data, where the linker's
// 4 KB alignment of what follows makes a few bytes cost kilobytes.
extern volatile uint8_t g_audio_tap1;  // 0, or 1 + AudioScopeCapture::Tap (0..3)
extern volatile uint8_t g_cv_index1;   // 0, or 1 + the frame's word index for the CV input
void Publish();                       // copies the counter's result to the hand-off slot
void PublishForTest(const ScopeFreq::Result &r);   // the same hand-off, any result
}  // namespace detail

inline void OnAudio(int tap, const int16_t *data, int n) {
  if (tap + 1 != (int)detail::g_audio_tap1) return;
  if (detail::g_counter.Push(data, n)) detail::Publish();
}

// One ADC frame (8 words). The word is 0..32767 where the input is plugged.
inline void OnCvFrame(const int16_t in[8]) {
  const int j1 = detail::g_cv_index1;
  if (j1 == 0) return;
  int32_t v = in[j1 - 1];
  if (v < 0) v = 0;
  if (detail::g_counter.Push((int16_t)(v - kCvOffset))) detail::Publish();
}

}  // namespace ScopeFreqSvc

#endif  // SCOPEFREQSERVICE_H_
