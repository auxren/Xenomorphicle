// Pure maths for the calibration routine. See the header for why this is split out.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define CALROUTINE_MATH_CODE FLASHMEM
#else
#define CALROUTINE_MATH_CODE
#endif

#include "CalRoutineMath.h"

#include <algorithm>
#include <cmath>

namespace CalRoutineMath {

namespace CM = CvMeterMath;

static constexpr int kMaxReadings = 64;
static constexpr double kTrustSpreadMv = 0.5;
static constexpr double kStepTolerance = 0.15;

CALROUTINE_MATH_CODE
Reading Summarize(const double *mv, int n) {
  Reading r = {0.0, 0.0, 0};
  if (n <= 0) return r;
  if (n > kMaxReadings) n = kMaxReadings;
  double s[kMaxReadings];
  for (int i = 0; i < n; ++i) s[i] = mv[i];
  std::sort(s, s + n);
  r.n = n;
  r.median_mv = (n & 1) ? s[n / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
  r.spread_mv = s[n - 1] - s[0];
  return r;
}

CALROUTINE_MATH_CODE
bool ReadingTrusted(const Reading &r) { return r.n >= 3 && r.spread_mv <= kTrustSpreadMv; }

CALROUTINE_MATH_CODE
uint16_t NextCode(uint16_t code, double measured_mv, int32_t target_mv,
                  float codes_per_mv, bool *at_rail) {
  const double next = std::round(double(code) + (double(target_mv) - measured_mv) * double(codes_per_mv));
  *at_rail = false;
  if (!(next > 0.0)) {  // also catches NaN
    *at_rail = !(next == 0.0);
    return 0;
  }
  if (next > 65535.0) { *at_rail = true; return 65535; }
  return uint16_t(next);
}

CALROUTINE_MATH_CODE
float LocalSlope(const uint16_t table[kNodes], const double measured_mv[kNodes], int k) {
  if (k < 0) k = 0;
  if (k > kNodes - 1) k = kNodes - 1;
  int lo = k > 0 ? k - 1 : k;
  int hi = k < kNodes - 1 ? k + 1 : k;
  if (!std::isfinite(measured_mv[lo])) lo = k;
  if (!std::isfinite(measured_mv[hi])) hi = k;
  if (lo == hi || !std::isfinite(measured_mv[lo]) || !std::isfinite(measured_mv[hi]))
    return kNominalCodesPerMv;
  const double dmv = measured_mv[hi] - measured_mv[lo];
  const double dcode = double(table[hi]) - double(table[lo]);
  if (!(dmv > 0.0)) return kNominalCodesPerMv;
  const double slope = dcode / dmv;
  if (!(slope >= 0.5 * kNominalCodesPerMv && slope <= 2.0 * kNominalCodesPerMv))
    return kNominalCodesPerMv;
  return float(slope);
}

CALROUTINE_MATH_CODE
TableVerdict CheckTable(const uint16_t table[kNodes]) {
  for (int k = 1; k < kNodes; ++k)
    if (!(table[k] > table[k - 1])) return TABLE_NOT_MONOTONE;
  const double nominal = double(kNominalCodesPerMv) * kNodeMv;
  for (int k = 1; k < kNodes; ++k) {
    const double step = double(table[k]) - double(table[k - 1]);
    if (step < nominal * (1.0 - kStepTolerance) || step > nominal * (1.0 + kStepTolerance))
      return TABLE_SLOPE;
  }
  return TABLE_OK;
}

CALROUTINE_MATH_CODE
int SweepPoints(int32_t out_mv[], int cap) {
  int n = 0;
  for (int k = 0; k < kNodes - 1; ++k)
    for (int j = 1; j <= 4; ++j) {
      if (n >= cap) return n;
      out_mv[n++] = k * kNodeMv + j * (kNodeMv / 5);
    }
  return n;
}

// ---- input curve ---------------------------------------------------------------

// mV at `raw` by linear interpolation between the points (raw strictly
// decreasing in index order), extrapolating the end segments. Points are the
// subset selected by `first` and `step`.
static double InterpMv(const InputSweepPoint *pts, int first, int step, int count, double raw) {
  // Segment i joins points first+i*step and first+(i+1)*step. Raw falls with
  // index, so pick the first segment whose far end is at or below raw.
  int seg = count - 2;
  for (int i = 0; i < count - 1; ++i) {
    if (raw >= double(pts[first + (i + 1) * step].stats.mean_raw)) { seg = i; break; }
  }
  const InputSweepPoint &a = pts[first + seg * step];
  const InputSweepPoint &b = pts[first + (seg + 1) * step];
  const double dr = double(b.stats.mean_raw) - double(a.stats.mean_raw);
  return double(a.mv) + (raw - double(a.stats.mean_raw)) * double(b.mv - a.mv) / dr;
}

CALROUTINE_MATH_CODE
CM::Verdict BuildInputCurve(const InputSweepPoint *pts, int n, int16_t mv_x2_out[kCurvePoints],
                            CM::Fit *line, float *worst_dev_mv, float *holdout_mv) {
  line->gain = 0.f;
  line->offset_mv = 0.f;
  *worst_dev_mv = 0.f;
  *holdout_mv = 0.f;
  if (n < 4) return CM::VERDICT_NO_SAMPLES;
  for (int i = 0; i < n; ++i)
    if (!pts[i].stats.samples) return CM::VERDICT_NO_SAMPLES;
  for (int i = 0; i < n; ++i) {
    const float m = pts[i].stats.mean_raw;
    if (m <= CM::kRailMarginCounts || m >= float(CM::kRawMax) - CM::kRailMarginCounts)
      return CM::VERDICT_RAIL;
  }
  for (int i = 0; i < n; ++i)
    if (pts[i].stats.max_raw - pts[i].stats.min_raw > CM::kMaxNoisePpCounts)
      return CM::VERDICT_NOISY;

  float rmin = pts[0].stats.mean_raw, rmax = rmin;
  for (int i = 1; i < n; ++i) {
    rmin = std::min(rmin, pts[i].stats.mean_raw);
    rmax = std::max(rmax, pts[i].stats.mean_raw);
  }
  if (!(rmax - rmin >= CM::kMinFitSpanCounts)) return CM::VERDICT_FLAT;

  // Least-squares line mV = gain * raw + offset over all points.
  double sr = 0, sm = 0;
  for (int i = 0; i < n; ++i) { sr += pts[i].stats.mean_raw; sm += pts[i].mv; }
  const double mr = sr / n, mm = sm / n;
  double srr = 0, srm = 0;
  for (int i = 0; i < n; ++i) {
    const double dr = pts[i].stats.mean_raw - mr;
    srr += dr * dr;
    srm += dr * (pts[i].mv - mm);
  }
  line->gain = float(srm / srr);
  line->offset_mv = float(mm - double(line->gain) * mr);
  for (int i = 0; i < n; ++i)
    *worst_dev_mv = std::max(*worst_dev_mv,
                             std::fabs(CM::FitMillivolts(*line, pts[i].stats.mean_raw) - float(pts[i].mv)));

  if (line->gain >= 0.f) return CM::VERDICT_SIGN;
  if (!CM::GainPlausible(line->gain)) return CM::VERDICT_GAIN;
  for (int i = 1; i < n; ++i)
    if (!(pts[i].mv > pts[i - 1].mv && pts[i].stats.mean_raw < pts[i - 1].stats.mean_raw))
      return CM::VERDICT_MONOTONIC;

  const int ne = (n + 1) / 2;  // even-indexed points build the curve
  for (int i = 0; i < kCurvePoints; ++i) {
    const double mv = InterpMv(pts, 0, 2, ne, double(i << kCurveShift));
    mv_x2_out[i] = int16_t(std::max(-32768.0, std::min(32767.0, std::round(2.0 * mv))));
  }
  for (int i = 1; i < n; i += 2) {
    const double raw = pts[i].stats.mean_raw;
    const int seg = std::min(int(raw) >> kCurveShift, kCurvePoints - 2);
    const double f = raw / double(1 << kCurveShift) - seg;
    const double mv = 0.5 * (mv_x2_out[seg] + f * (mv_x2_out[seg + 1] - mv_x2_out[seg]));
    *holdout_mv = std::max(*holdout_mv, float(std::fabs(mv - double(pts[i].mv))));
  }
  return CM::VERDICT_OK;
}

CALROUTINE_MATH_CODE
void CurveToValueTable(const int16_t mv_x2[kCurvePoints], int32_t value_q8_out[kCurvePoints]) {
  // (x2 / 2) * 4096 / 12000 * 256, rounded.
  for (int i = 0; i < kCurvePoints; ++i)
    value_q8_out[i] = int32_t(std::lround(double(mv_x2[i]) * (4096.0 * 256.0 / 24000.0)));
}

CALROUTINE_MATH_CODE
void LineToValueTable(const CM::Fit &line, int32_t value_q8_out[kCurvePoints]) {
  for (int i = 0; i < kCurvePoints; ++i) {
    const double mv = double(line.gain) * double(i << kCurveShift) + double(line.offset_mv);
    value_q8_out[i] = int32_t(std::lround(mv * (4096.0 * 256.0 / 12000.0)));
  }
}

}  // namespace CalRoutineMath
