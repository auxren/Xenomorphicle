#ifndef SAMPLERMATH_H_
#define SAMPLERMATH_H_

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Pure logic for the Sampler app (apps/SamplerApp.h): filename-from-number
// generation, parameter clamping, and the per-slot gate/trigger edge
// detector. No Arduino/Audio.h dependency -- host-tested by
// test/test_sampler_math.cpp, same split as ScopeMath.h/.cpp and
// TweightyTapPhase.h/.cpp.
//
// FILE CONVENTION: mirrors audio_applets/WAVPlayerApplet.h exactly -- SD
// root, "NNN.WAV" for NNN in 000..999. There is no directory browser
// anywhere in this codebase; a real Bitbox Micro's touchscreen file browser
// has no hardware-appropriate equivalent on a 128x64 1-bit panel with two
// encoders, so slots are assigned by number, the same convention the
// existing single-voice player already established.
//
// GATE/TRIGGER: this hardware has 8 CV inputs (ADC_CHANNEL_1..8) but only 4
// dedicated TR jacks (OC::DigitalInputs, already claimed one apiece by other
// apps, e.g. Tweighty's Digital-In-1). "One slot per CV/gate input" (8 slots
// for 8 CV jacks, matching Tweighty's 8 taps and Scope's 8 CV channels) means
// each slot's own CV input does double duty: its instantaneous level is a
// live pitch/rate modulation source (ComputeRateMultiplier), AND crossing
// kGateThresholdRaw is what a "gate" means on a jack with no separate
// comparator -- UpdateGate() turns the two most recent samples of one input
// into a rising/falling edge. Patching a slow modulation-only CV into a slot
// can therefore also (re)trigger it if it crosses the threshold -- documented
// behavior, not a bug, inherent to sharing one jack for both jobs.
// ---------------------------------------------------------------------------

namespace SamplerMath {

inline constexpr int kSlotCount = 8;

inline constexpr int kMinFileNum = 0;
inline constexpr int kMaxFileNum = 999;

// Percent of native playback speed, matching WAVPlayerApplet's own
// percent-based "Rate" convention (0.01f * playrate) rather than a
// semitone/pitch unit -- this engine varies sample-read speed, not a
// synth oscillator, so "speed" is the honest unit.
inline constexpr int kMinRatePercent = 25;    // 0.25x
inline constexpr int kMaxRatePercent = 400;   // 4.0x
inline constexpr int kDefaultRatePercent = 100;

// OC::ADC::value() full-scale for a CV input (see ScopeMath.h's own
// kCvInFullScale comment: a 12-bit-resolution, calibrated, zero-centered
// reading, roughly +/-2048 at full +/-5V swing).
inline constexpr int32_t kCvFullScale = 2048;

// 25% of full scale -- same fraction HSIOFrame::GATE_THRESHOLD uses of its
// own frame's full scale (15<<7 of HSAPPLICATION_5V(7680) == 0.25), just
// expressed in OC::ADC::value()'s raw +/-2048 scale instead of HSApplication's
// internal one, since this app is not an HSApplication (see class comment
// in apps/SamplerApp.h) and reads OC::ADC::value() directly like ScopeApp
// does.
inline constexpr int32_t kGateThresholdRaw = kCvFullScale / 4;  // 512, ~1.25V

// How far a slot's own CV can swing its playback rate: +/-100% of the base
// rate at full +/-5V CV, i.e. a fully negative CV can (with clamping below)
// nearly stop playback and a fully positive one can double it on top of
// whatever the base rate already is.
inline constexpr float kPitchCvDepth = 1.0f;

// Absolute floor/ceiling the *combined* (base * CV) multiplier is clamped
// to, independent of kMinRatePercent/kMaxRatePercent's base-only range --
// CV modulation can push the live rate outside the base knob's own range,
// but never outside what the engine can be trusted to resample cleanly.
inline constexpr float kMinRateMult = 0.1f;
inline constexpr float kMaxRateMult = 4.0f;

enum TriggerMode : uint8_t {
  TRIG_ONE_SHOT = 0,     // rising edge starts playback; gate ignored after
  TRIG_GATE_SUSTAIN = 1, // rising edge starts; falling edge stops
  kTriggerModeCount,
};

enum GateEdge : uint8_t {
  EDGE_NONE = 0,
  EDGE_RISING = 1,
  EDGE_FALLING = 2,
};

// Wraps a slot index (e.g. from an encL turn) into [0, kSlotCount).
int WrapSlotIndex(int idx);

// Clamps a raw file-select delta/value into [kMinFileNum, kMaxFileNum].
int ClampFileNum(int n);

// Clamps a raw rate-percent delta/value into [kMinRatePercent, kMaxRatePercent].
int ClampRatePercent(int pct);

// Clamps/validates a persisted or UI-cycled trigger-mode byte.
uint8_t ClampTriggerMode(int m);

// Writes "NNN.WAV\0" (8 bytes) for file_num (clamped first) into buf.
// buf must be at least kFilenameBufLen bytes; a shorter buffer is left
// empty (buf[0] = '\0') rather than overrun.
inline constexpr size_t kFilenameBufLen = 8;
void BuildFilename(int file_num, char *buf, size_t buflen);

// Turns the latest sample of one CV/gate input into an edge, given (and
// updating) that slot's own previous-sample state. Pure comparator logic --
// no debounce/hysteresis beyond the single threshold, matching how a real
// modular gate comparator behaves.
GateEdge UpdateGate(int32_t raw, bool &prev_high);

// Combines a slot's base rate-percent with its own live CV reading into the
// float multiplier AudioPlaySdResmp::setPlaybackRate() wants. cv_raw is the
// same OC::ADC::value() reading UpdateGate() was given.
float ComputeRateMultiplier(int rate_percent, int32_t cv_raw);

}  // namespace SamplerMath

#endif  // SAMPLERMATH_H_
