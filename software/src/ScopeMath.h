#ifndef SCOPEMATH_H_
#define SCOPEMATH_H_

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Pure logic for the Scope app (apps/ScopeApp.h): channel numbering/labels,
// gain-step table, ring-buffer index wraparound, and the raw-value -> pixel
// row mapping. No Arduino/Audio.h dependency -- host-tested by
// test/test_scope_math.cpp, same split as TweightyTapPhase.h/.cpp.
//
// Channel layout is a single flat 0..19 index so a persisted channel byte
// means the same thing across builds: 0-7 CV IN 1-8, 8-15 CV OUT 1-8, 16-19
// AUDIO IN L/R, AUDIO OUT L/R. The audio slice only makes sense on a build
// with AUDIO_INTERFACE (real hardware audio taps); ScopeApp is the one that
// keeps the encoder cursor off 16-19 when that's not available -- this
// table itself has no opinion on that, so it stays testable on the host
// with no #ifdef of its own.
// ---------------------------------------------------------------------------

namespace ScopeMath {

inline constexpr int kCvInCount = 8;
inline constexpr int kCvOutCount = 8;
inline constexpr int kAudioChannelCount = 4;  // in L, in R, out L, out R
inline constexpr int kChannelCount =
    kCvInCount + kCvOutCount + kAudioChannelCount;  // 20

inline constexpr int kCvChannelCount = kCvInCount + kCvOutCount;  // 16

enum ChannelKind {
  KIND_CV_IN = 0,
  KIND_CV_OUT = 1,
  KIND_AUDIO = 2,
};

// One ring slot per OLED column.
inline constexpr int kRingSize = 128;

// Discrete gain detents, 0.25x..4x -- a handful of musically-round steps
// rather than continuous float precision (encR moves one step per detent).
inline constexpr int kGainStepCount = 9;
inline constexpr float kGainSteps[kGainStepCount] = {
    0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f,
};
inline constexpr int kDefaultGainIndex = 3;  // kGainSteps[3] == 1.0x

// Nominal full-scale (maps to the top/bottom of the plot at 1.0x gain) per
// channel kind. CV IN: OC::ADC::value() is a 12-bit-resolution, calibrated,
// zero-centered reading (see OC_ADC.h) -- roughly +/-2048 at full swing.
// CV OUT: OC::DAC::value() is the raw unsigned DAC8565 code (0..65535,
// OC_DAC.h); ScopeApp re-centers it around kCvOutCenter before this table
// is applied, since the DAC has no notion of "zero" of its own. AUDIO: the
// int16 audio bus's own native range.
inline constexpr int32_t kCvInFullScale = 2048;
inline constexpr int32_t kCvOutCenter = 32768;
inline constexpr int32_t kCvOutFullScale = 32768;
inline constexpr int32_t kAudioFullScale = 32768;

// Wraps idx into [0, count) for any idx (including negative deltas from an
// encoder turn) -- count must be > 0.
int WrapIndex(int idx, int count);

// Clamps idx into [0, kGainStepCount).
int ClampGainIndex(int idx);

// Which kind of channel a flat 0..19 index refers to.
ChannelKind ChannelKindOf(int ch);

// Index within its own kind: CV_IN/CV_OUT -> 0..7, AUDIO -> 0..3 (the tap
// index AudioScopeCapture uses: 0=in L, 1=in R, 2=out L, 3=out R).
int ChannelSubIndex(int ch);

// Nominal full-scale for ch's kind (see the constants above).
int32_t ChannelFullScale(int ch);

// Writes ch's display label ("CV IN 3", "CV OUT 8", "AUDIO IN L", ...) into
// buf, NUL-terminated. buf must be at least 12 bytes.
void ChannelLabel(int ch, char *buf, size_t buflen);

// Ring-buffer read index for on-screen column `column` (0 = oldest/leftmost,
// kRingSize-1 = newest/rightmost), given `head` = the index the NEXT sample
// will be written to (i.e. one past the most recent write, wrapping).
int RingReadIndex(int head, int column, int ring_size = kRingSize);

// Maps a raw (signed, zero-centered) sample to a pixel row within a
// plot_h-tall region (row 0 = top = +full_scale, row plot_h-1 = bottom =
// -full_scale), after applying gain. Clamped to the plot at either end
// rather than wrapping or extrapolating off-screen.
int ValueToRow(int32_t raw, float gain, int32_t full_scale, int plot_h);

}  // namespace ScopeMath

#endif  // SCOPEMATH_H_
