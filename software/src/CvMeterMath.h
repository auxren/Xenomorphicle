#ifndef CVMETERMATH_H_
#define CVMETERMATH_H_

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Pure logic behind the bench CV meter (CvMeter.cpp, console 'v' 'd' 'Q' 'n'):
// the two volts conversions, the per-input two-point calibration and its
// acceptance checks, calibrated-millivolts -> DAC code, the frequency
// counter's note/cents arithmetic, and the exact text of every streamed line.
// No Arduino dependency -- host-tested by test/test_cvmeter_math.cpp, same
// split as ScopeSources.h/.cpp.
//
// Raw counts here are the ADC's own 12-bit reading (OC::ADC::raw_value()),
// either as a float or as the 12.8 fixed-point smoothed value the ADC keeps
// (OC::ADC::smoothed_raw_q8()). Volts go UP as raw goes DOWN on this
// hardware: OC::ADC::value() is offset - raw.
// ---------------------------------------------------------------------------

namespace CvMeterMath {

constexpr int kChannels = 8;
constexpr int32_t kRawMax = 4095;           // 12-bit window after the >> 4
constexpr float kPitchUnitsPerVolt = 1536.f;  // 12 semitones << 7

// ---- volts per "octave" ---------------------------------------------------
//
// The firmware's pitch octave, and the spacing of the DAC calibration table,
// is 1.0 V -- except on Northern Light (Buchla-format) hardware, the
// Xenomorpher, where it is 1.2 V: the calibration screens label DAC table
// step n as n * 1.2 V and ask for 1.2 V / 3.6 V at the ADC pitch steps
// (apps/SETTINGS.h), and gfxPrintVoltage() prints 1536 pitch units as 1.20 V
// (HSUtils.cpp). +/-10 V hardware doubles the DAC table step on top.
inline int32_t OctaveMv(bool northern_light) { return northern_light ? 1200 : 1000; }
inline int32_t DacStepMv(bool northern_light, bool dac_20vpp) {
  return OctaveMv(northern_light) * (dac_20vpp ? 2 : 1);
}

// The DAC calibration routine sets table entries 0 and 8 against a meter and
// interpolates the rest (apps/SETTINGS.h InterpolateChannel; the steps are
// DAC_x_VOLT_MIN / DAC_x_VOLT_HIGH in OC_calibration.cpp). The closed-loop
// input calibration keeps its DAC settings inside that metered span.
constexpr int kDacMeteredTopIndex = 8;

// ---- the shared ("S") path: what every app sees today ----------------------
//
// OC::ADC::value_to_pitch() is (value * pitch_cv_scale) >> 12 pitch units,
// 1536 to the octave, with value = offset[ch] - raw. One scale for all
// inputs (calibrated on CV1 only), one 0 V offset per input
// (OC::ADC::CalibrationData). octave_mv: OctaveMv().
inline float SharedMillivolts(int32_t offset, float raw, uint16_t scale, int32_t octave_mv) {
  return (float(offset) - raw) * float(scale) * (float(octave_mv) / (4096.f * kPitchUnitsPerVolt));
}

// mV per raw count on the shared path. Negative: raw falls as volts rise.
inline float SharedGain(uint16_t scale, int32_t octave_mv) {
  return -float(scale) * (float(octave_mv) / (4096.f * kPitchUnitsPerVolt));
}

// Volts at the two ADC rails on the shared path (raw 4095 and raw 0).
void SharedReadableRange(int32_t offset, uint16_t scale, int32_t octave_mv,
                         int32_t *lo_mv, int32_t *hi_mv);

// ---- the per-input ("C") path ------------------------------------------------
//
// Stored in its own EEPROM page next to the DAC/ADC calibration (see
// OC_config.h, EEPROM_INPUTCAL_*), NOT inside OC::CalibrationData: that page
// fills its 224-byte region exactly, and growing it would move the app-data
// page and invalidate every unit's stored settings and calibration. A unit
// that has never run the input calibration has no page here, loads
// valid_mask == 0, and converts exactly as before.
//
// Indexed by PHYSICAL ADC channel, like OC::ADC::CalibrationData::offset.
struct InputCalibration {
  // 'I','C','A',1 -- same packing as util_misc.h's FourCC<>
  static constexpr uint32_t FOURCC = (uint32_t('I') << 24) | (uint32_t('C') << 16) |
                                     (uint32_t('A') << 8) | 1u;
  static constexpr uint8_t kVersion = 1;

  uint8_t version;
  uint8_t valid_mask;             // bit n: physical input n has a fit
  uint8_t reserved0[2];
  float gain[kChannels];          // mV per raw count (negative on this hardware)
  float offset_mv[kChannels];     // mV at raw == 0
  int16_t verify_mv10[kChannels]; // verification residual at commit, 0.1 mV
  uint8_t source_output[kChannels]; // 1-based output used as the reference
  uint32_t reserved1;
};
static_assert(sizeof(InputCalibration) == 96, "InputCalibration layout changed: bump FOURCC and add a migration");

void InputCalReset(InputCalibration &cal);

// Called on every load. Version 1 is the only layout; an unknown version
// (written by a newer firmware with the same size) is treated as "no input
// calibration" in RAM, never rewritten. Any channel whose stored fit is not
// finite or fails GainPlausible() is dropped. Returns the channels dropped.
int InputCalSanitize(InputCalibration &cal);

bool InputCalValid(const InputCalibration &cal, int phys);

// Millivolts for one physical input: the per-input fit when it has one, else
// the shared path. *per_input says which one was used.
float InputMillivolts(const InputCalibration &cal, int phys, float raw,
                      int32_t shared_offset, uint16_t shared_scale, int32_t octave_mv,
                      bool *per_input);

// ---- fitting and acceptance --------------------------------------------------

struct Fit {
  float gain;       // mV per count
  float offset_mv;  // mV at raw == 0
};

// Points closer than this in raw counts cannot define a line (the input did
// not move: nothing patched, or the wrong jack).
constexpr float kMinFitSpanCounts = 50.f;
// Measured gain must be within this fraction of the shared path's gain.
// Component tolerance on the input divider is a few percent; 15 % still
// catches a cable in the wrong jack or a 2:1 attenuator in the path.
constexpr float kGainTolerance = 0.15f;
// Absolute sanity window for any stored gain, |mV per count|. Nominal is
// 2.44 (10 octaves of 1 V across 4096 counts), 2.93 on 1.2 V hardware.
constexpr float kGainAbsMin = 1.0f;
constexpr float kGainAbsMax = 6.0f;
// The verification point must land within this of where the DAC was set.
// About 4 LSB of the 12-bit path; the DAC reference itself is good to a mV
// or two after its own calibration.
constexpr float kMaxVerifyResidualMv = 10.f;
// Peak-to-peak spread of the readings at one point, in counts. Larger means
// a loose cable, a noisy source, or something else driving the input.
constexpr float kMaxNoisePpCounts = 24.f;
// A mean this close to either rail is (or may be) clipped.
constexpr float kRailMarginCounts = 8.f;

bool TwoPointFit(float raw1, float mv1, float raw2, float mv2, Fit *fit);

inline float FitMillivolts(const Fit &fit, float raw) { return fit.gain * raw + fit.offset_mv; }

bool GainPlausible(float gain);

struct PointStats {
  float mean_raw;
  float min_raw;
  float max_raw;
  uint32_t samples;
};

enum Verdict {
  VERDICT_OK,
  VERDICT_NO_SAMPLES,
  VERDICT_RAIL,       // a point sits on an ADC rail
  VERDICT_NOISY,      // spread at a point above kMaxNoisePpCounts
  VERDICT_FLAT,       // fit points too close: input not moving
  VERDICT_SIGN,       // raw moved the wrong way
  VERDICT_GAIN,       // gain outside tolerance / absolute window
  VERDICT_MONOTONIC,  // verify point not between the fit points in raw
  VERDICT_RESIDUAL,   // verify point too far from the line
};

const char *VerdictName(Verdict v);

// points[0], points[1]: the fit; points[2]: verification. mv[]: what the DAC
// was set to at each. Fills *fit and *residual_mv whenever a line could be
// drawn (from VERDICT_SIGN on), else leaves them at zero.
Verdict CheckCalibration(const PointStats points[3], const int32_t mv[3],
                         uint16_t shared_scale, int32_t octave_mv, Fit *fit,
                         float *residual_mv);

// Three DAC settings for the closed-loop calibration, inside [lo_mv, hi_mv]
// (the overlap of what the output can drive and the input can read): about
// 10 % and 90 % of the span for the fit, the middle for verification. Each is
// moved to the nearest multiple of node_mv (the DAC's own calibration
// points, where it is most accurate) when that keeps it at least 5 % of the
// span away from either end. False if the span is under 2 V.
bool ChooseCalPoints(int32_t lo_mv, int32_t hi_mv, int32_t node_mv, int32_t out[3]);

// ---- calibrated millivolts -> DAC code ---------------------------------------
//
// table: one output's OC::DAC::CalibrationData::calibrated_octaves row
// (points entries, ascending codes), table[octave_zero] is 0 V, one entry
// every mv_per_step (DacStepMv()). Linear between entries -- the same
// interpolation OC::DAC::PitchToScaledDAC() uses.
void DacRange(int points, int octave_zero, int32_t mv_per_step, int32_t *lo_mv, int32_t *hi_mv);
bool MillivoltsToDacCode(const uint16_t *table, int points, int octave_zero,
                         int32_t mv_per_step, int32_t mv, uint16_t *code);

// ---- frequency counter -------------------------------------------------------

// sum_counts: the summed periods of `periods` consecutive edges, in ticks of
// clock_hz. 0 if there is nothing to divide.
double PeriodsToHz(uint64_t sum_counts, uint32_t periods, double clock_hz);

// Nearest 12-TET note (MIDI number, 69 = A4 = a4_hz) and the offset from it
// in cents, -50..+50. False for hz <= 0.
bool NearestNote(double hz, double a4_hz, int *midi, double *cents);

// "A4", "C#5", "B-1". out must hold 5 bytes.
void NoteName(int midi, char *out);

// ---- line formatting ---------------------------------------------------------

// v / 10^decimals with an explicit '-' (no '+'), e.g. (-12345, 2) -> "-123.45".
// Returns the length written (snprintf semantics, truncated to n).
int FormatFixed(char *buf, size_t n, int64_t v, int decimals);

// Round half away from zero to an integer number of 10^-decimals units.
int64_t ScaleRound(double v, int decimals);

// Clip flag for a window: 'L' if any reading reached raw 0..1, 'H' if any
// reached 4094..4095, '.' otherwise.
char ClipFlag(uint32_t min_raw_q8, uint32_t max_raw_q8);

// One CV stream line, without the line ending:
//   CV,<ms>,<seq>,<dropped>,<raw1>..<raw8>,<mv1>..<mv8>,<cal>,<clip>
// raw: counts, 2 decimals; mv: millivolts, 1 decimal; cal: 8 chars of
// 'C' (per-input fit) / 'S' (shared scale + offset); clip: 8 chars of
// ClipFlag(). 22 comma-separated fields, inputs in panel order 1..8.
int FormatCvLine(char *buf, size_t n, uint32_t ms, uint32_t seq, uint32_t dropped,
                 const uint32_t raw_q8[kChannels], const int32_t mv10[kChannels],
                 const char cal[kChannels], const char clip[kChannels]);

// One frequency line, without the line ending:
//   FREQ,<ms>,<hz>,<periods>,<note>,<cents>
// hz: 4 decimals; cents: signed, 2 decimals, always with a sign.
int FormatFreqLine(char *buf, size_t n, uint32_t ms, double hz, uint32_t periods,
                   double a4_hz);

// Internal helpers, exposed only so they can carry the flash-section
// attribute (it is ignored on internal-linkage functions).
int32_t SnapToNode(int32_t v, int32_t node);
void Append(char *buf, size_t n, size_t *pos, const char *s);

// ---- console argument: signed millivolts ------------------------------------
//
// Fed one byte at a time after "d<ch>": an optional sign, 1..6 digits, then
// Enter or space. Anything else is BAD (the console's convention: a stray
// byte cancels a pending argument, never gets reinterpreted).
struct MvArg {
  enum Result { MORE, DONE, BAD };
  int32_t value;
  uint8_t digits;
  bool neg;
  bool signed_;

  void Reset() { value = 0; digits = 0; neg = false; signed_ = false; }
  Result Feed(char c);
  int32_t get() const { return neg ? -value : value; }
};

}  // namespace CvMeterMath

#endif  // CVMETERMATH_H_
