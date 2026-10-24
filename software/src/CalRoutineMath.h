#ifndef CALROUTINEMATH_H_
#define CALROUTINEMATH_H_

#include <stddef.h>
#include <stdint.h>

#include "CvMeterMath.h"

// ---------------------------------------------------------------------------
// Pure maths behind the calibration routine (design/cal-routine-design.md
// sections 6.3 and 6.4): setting each of an output's 11 DAC table points
// against a meter reading, checking the table, the verify-sweep targets, the
// 17-point input correction curve built from a multi-point sweep, and the
// integer apply used by the core ISR.
//
// Units are 1.2 V per octave, so 1 mV = 1 cent. Calibrated input values use
// the nominal model's 409.6 value counts per 1200 mV:
//   value = mV * 4096 / 12000,  pitch = value * 15360 >> 12.
// Raw counts are the ADC's 12-bit reading; volts rise as raw falls, so a
// curve/line gain is negative on this hardware (CvMeterMath's convention).
// No Arduino dependency -- host-tested by test/test_calroutine_math.cpp.
// ---------------------------------------------------------------------------

namespace CalRoutineMath {

constexpr int kNodes = 11;          // DAC table entries, k * 1200 mV, k = 0..10
constexpr int32_t kNodeMv = 1200;
constexpr int kCurvePoints = 17;    // breakpoints at raw 0, 256, ... 4096
constexpr int kCurveShift = 8;      // 256 raw counts per segment

constexpr float kNominalCodesPerMv = 65535.f / 12000.f;
constexpr float kTargetMv = 0.2f;   // the iteration aims here; one code is ~0.18 mV
constexpr float kPassMv = 1.0f;     // every sweep residual must be within this
constexpr int kMaxIterations = 6;

// ---- reading a meter -----------------------------------------------------------

struct Reading {
  double median_mv;
  double spread_mv;  // max - min
  int n;
};

// Median and spread of n meter readings (n is capped at 64; n <= 0 gives zeros).
Reading Summarize(const double *mv, int n);
// False when n < 3 or the spread exceeds 0.5 mV.
bool ReadingTrusted(const Reading &r);

// ---- DAC node iteration --------------------------------------------------------

// Next code to try: code + (target - measured) * codes_per_mv, rounded and
// clamped to 0..65535. *at_rail is set when the clamp was needed (the DAC
// cannot reach the target), cleared otherwise.
uint16_t NextCode(uint16_t code, double measured_mv, int32_t target_mv,
                  float codes_per_mv, bool *at_rail);

// Codes per mV around node k from its measured neighbours (measured_mv[i] is
// the meter reading at table[i]; NaN means "not measured yet"). Falls back to
// kNominalCodesPerMv when the neighbours are not measured or the result is
// implausible (outside half to twice nominal).
float LocalSlope(const uint16_t table[kNodes], const double measured_mv[kNodes], int k);

enum TableVerdict { TABLE_OK, TABLE_NOT_MONOTONE, TABLE_SLOPE };

// Codes strictly increasing, and each step's codes per 1200 mV within +/-15 %
// of nominal. A top entry clamped at 65535 passes as long as it is above the
// one before it and the step is not short by more than the tolerance.
TableVerdict CheckTable(const uint16_t table[kNodes]);

// Verify-sweep targets: 4 evenly spaced points between each pair of nodes
// (240 mV apart), 0..12000 mV, nodes excluded, ascending. Returns the count
// written (40 when cap >= 40).
int SweepPoints(int32_t out_mv[], int cap);

// ---- input curve ---------------------------------------------------------------

struct InputSweepPoint {
  int32_t mv;                 // what the DAC was set to (the reference)
  CvMeterMath::PointStats stats;
};

// pts: ascending mV. Even-indexed points build the curve, odd-indexed points
// give *holdout_mv (worst |curve - mv| over them). mv_x2_out: true mV x 2 at
// raw 0, 256, ... 4096, linear between measured points and linearly
// extrapolated at the ends. *line: least-squares fit over all points;
// *worst_dev_mv: the line's worst deviation over all points. Checks, in order:
// NO_SAMPLES (also n < 4), RAIL, NOISY, FLAT, SIGN, GAIN (on the line),
// MONOTONIC (raw strictly falling as mV rises). Outputs are filled once the
// line exists (from SIGN on); the curve only on OK.
CvMeterMath::Verdict BuildInputCurve(const InputSweepPoint *pts, int n,
                                     int16_t mv_x2_out[kCurvePoints],
                                     CvMeterMath::Fit *line, float *worst_dev_mv,
                                     float *holdout_mv);

// Value counts x 256 at each breakpoint: mV * 4096 / 12000 * 256.
void CurveToValueTable(const int16_t mv_x2[kCurvePoints], int32_t value_q8_out[kCurvePoints]);
// The same from the ICA1 line (fallback when no curve was built).
void LineToValueTable(const CvMeterMath::Fit &line, int32_t value_q8_out[kCurvePoints]);

// Calibrated value counts for a raw reading: integer only, for the ISR.
// raw12 is clamped to 4095, so the highest index read is 16.
//
// One rounding, to nearest (half up, also for negative values: >> is an
// arithmetic shift on every target here): the interpolation is carried in
// 16 fractional bits, q8 * 256 + diff * frac, and rounded once at the end.
// Flooring twice instead biased every calibrated input by -0.5..-1 count.
// No overflow while |q8| <= kMaxValueQ8: |q8| * 256 + 2 |q8| * 255 + 2^15
// < 2^31. The int16 mV x 2 curve stays below that (|q8| <= ~1.43e6, so
// ~3.7e8 + ~7.3e8), and InputCal rejects any table outside it.
constexpr int32_t kMaxValueQ8 = 2000000;
inline int32_t ApplyValueTable(const int32_t q8[kCurvePoints], uint32_t raw12) {
  if (raw12 > 4095u) raw12 = 4095u;
  const uint32_t seg = raw12 >> kCurveShift;
  const int32_t frac = int32_t(raw12 & 255u);
  return (q8[seg] * 256 + (q8[seg + 1] - q8[seg]) * frac + 32768) >> 16;
}

}  // namespace CalRoutineMath

#endif  // CALROUTINEMATH_H_
