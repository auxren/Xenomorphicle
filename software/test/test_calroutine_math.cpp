// Host tests for the calibration routine's pure maths (src/CalRoutineMath.h):
// DAC node iteration, table checks, verify-sweep targets, the multi-point
// input curve and its integer apply. Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_calroutine_math
//     test_calroutine_math.cpp ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "../src/CalRoutineMath.h"

using namespace CalRoutineMath;
namespace CM = CvMeterMath;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= double(tol))

typedef std::function<double(double)> DacModel;  // code -> output mV

static const double kPi = 3.14159265358979323846;

// Runs the node iteration for one node against a simulated DAC.
static double run_node(const DacModel &dac, int k, uint16_t *code_out, bool *rail, int *iters) {
  uint16_t code = uint16_t(std::lround(k * 1200 * double(kNominalCodesPerMv)));
  double err = 0;
  *rail = false;
  int i = 0;
  for (; i < kMaxIterations; ++i) {
    const double mv = dac(code);
    err = mv - k * 1200.0;
    if (std::fabs(err) <= kTargetMv) break;
    bool at_rail = false;
    const uint16_t next = NextCode(code, mv, k * 1200, kNominalCodesPerMv, &at_rail);
    if (at_rail) { *rail = true; code = next; err = dac(code) - k * 1200.0; break; }
    code = next;
  }
  *code_out = code;
  *iters = i;
  return err;
}

static void test_node_converges_linear() {
  DacModel dac = [](double c) { return c * 12000.0 / 65535.0 + 3.0; };
  uint16_t table[kNodes];
  for (int k = 0; k < kNodes; ++k) {
    bool rail; int it;
    const double err = run_node(dac, k, &table[k], &rail, &it);
    if (k == 0) {
      // A +3 mV offset puts 0 mV below code 0: the best possible is code 0,
      // and it must be flagged, not silently accepted.
      CHECK(rail && table[0] == 0);
    } else {
      CHECK(!rail);
      CHECK(std::fabs(err) <= kTargetMv);
      CHECK(it <= kMaxIterations);
    }
  }
  CHECK(CheckTable(table) == TABLE_OK);
}

static void test_node_converges_bowed() {
  // Smooth bow, 4 mV peak at mid scale, zero at both ends.
  DacModel dac = [](double c) {
    const double x = c / 65535.0;
    return x * 12000.0 + 4.0 * std::sin(kPi * x);
  };
  uint16_t table[kNodes];
  double mv_meas[kNodes];
  for (int k = 0; k < kNodes; ++k) {
    bool rail; int it;
    const double err = run_node(dac, k, &table[k], &rail, &it);
    CHECK(!rail);
    CHECK(std::fabs(err) <= kTargetMv);
    mv_meas[k] = dac(table[k]);
  }
  CHECK(CheckTable(table) == TABLE_OK);
  // Residual between the nodes, with the firmware's linear interpolation:
  // computed and reported, not asserted to be small -- the sweep exists to
  // measure it.
  int32_t sweep[48];
  const int n = SweepPoints(sweep, 48);
  double worst = 0;
  for (int i = 0; i < n; ++i) {
    const int k = sweep[i] / 1200;
    const double f = (sweep[i] - k * 1200) / 1200.0;
    const double code = table[k] + f * (table[k + 1] - table[k]);
    worst = std::fmax(worst, std::fabs(dac(code) - sweep[i]));
  }
  printf("  bowed DAC: worst sweep residual between nodes = %.3f mV (pass limit %.1f)\n",
         worst, double(kPassMv));
  CHECK(worst < 4.0);  // a 4 mV bow can never leave more than the bow itself
  // LocalSlope with all nodes known is close to nominal for this DAC.
  NEAR(LocalSlope(table, mv_meas, 5), kNominalCodesPerMv, 0.02 * kNominalCodesPerMv);
}

static void test_rail_entry_flagged_not_clipped() {
  DacModel dac = [](double c) { return std::fmin(c * 12000.0 / 65535.0, 11900.0); };
  uint16_t table[kNodes];
  bool rail[kNodes];
  for (int k = 0; k < kNodes; ++k) {
    int it;
    run_node(dac, k, &table[k], &rail[k], &it);
  }
  CHECK(table[10] == 65535 && rail[10]);
  CHECK(!rail[9]);
  CHECK(CheckTable(table) == TABLE_OK);
  // And NextCode clamps both ways.
  bool r = false;
  CHECK(NextCode(65000, 0.0, 12000, kNominalCodesPerMv, &r) == 65535 && r);
  r = false;
  CHECK(NextCode(100, 5000.0, 0, kNominalCodesPerMv, &r) == 0 && r);
  r = true;
  CHECK(NextCode(30000, 5499.0, 5500, kNominalCodesPerMv, &r) > 30000 && !r);
}

static void test_table_checks() {
  uint16_t t[kNodes];
  for (int k = 0; k < kNodes; ++k) t[k] = uint16_t(std::lround(k * 6553.5));
  CHECK(CheckTable(t) == TABLE_OK);
  uint16_t bad[kNodes]; memcpy(bad, t, sizeof t);
  bad[5] = bad[4];  // equal: not strictly increasing
  CHECK(CheckTable(bad) == TABLE_NOT_MONOTONE);
  memcpy(bad, t, sizeof t); bad[6] = bad[5] - 10;
  CHECK(CheckTable(bad) == TABLE_NOT_MONOTONE);
  memcpy(bad, t, sizeof t);
  bad[4] = uint16_t(t[3] + int(6553.5 * 1.30));  // step 3->4 is +30 %, step 4->5 shrinks
  CHECK(CheckTable(bad) == TABLE_SLOPE);
  // 10 % off is within tolerance.
  memcpy(bad, t, sizeof t);
  bad[4] = uint16_t(t[3] + int(6553.5 * 1.10));
  CHECK(CheckTable(bad) == TABLE_OK);
}

static void test_sweep_points() {
  int32_t p[64];
  CHECK(SweepPoints(p, 64) == 40);
  for (int i = 0; i < 40; ++i) {
    CHECK(p[i] > 0 && p[i] < 12000);
    CHECK(p[i] % 1200 != 0);
    if (i) CHECK(p[i] > p[i - 1]);
  }
  CHECK(SweepPoints(p, 10) == 10);  // capped, not overrun
}

static void test_summarize() {
  const double a[] = {1, 9, 2, 3, 2};
  Reading r = Summarize(a, 5);
  NEAR(r.median_mv, 2.0, 1e-9); NEAR(r.spread_mv, 8.0, 1e-9); CHECK(r.n == 5);
  const double b[] = {1, 2, 3, 4};
  NEAR(Summarize(b, 4).median_mv, 2.5, 1e-9);
  Reading t = {0, 0.6, 5}; CHECK(!ReadingTrusted(t));
  t.spread_mv = 0.3; CHECK(ReadingTrusted(t));
  t.n = 2; CHECK(!ReadingTrusted(t));
  CHECK(Summarize(a, 0).n == 0);
}

// ---- input curve ------------------------------------------------------------
// Hardware: raw falls as mV rises; 0 mV at raw 4095, about 2.93 mV per count.
static const double kGain = -1200.0 * 15360.0 / (4096.0 * 1536.0);  // -2.9297

typedef std::function<double(double)> InputModel;  // true mV -> raw

static int make_points(const InputModel &raw_of_mv, int n, CM::PointStats stats_template,
                       InputSweepPoint *out, int32_t lo = 300, int32_t hi = 11700) {
  for (int i = 0; i < n; ++i) {
    out[i].mv = lo + int32_t(std::lround((hi - lo) * double(i) / (n - 1)));
    CM::PointStats s = stats_template;
    s.mean_raw = float(raw_of_mv(out[i].mv));
    s.min_raw = s.mean_raw - 1.f;
    s.max_raw = s.mean_raw + 1.f;
    out[i].stats = s;
  }
  return n;
}

static CM::PointStats stats0() { CM::PointStats s = {0, 0, 0, 16}; return s; }

static double curve_mv_at(const int16_t c[kCurvePoints], double raw) {
  const int seg = std::min(int(raw / 256), kCurvePoints - 2);
  const double f = raw / 256.0 - seg;
  return (c[seg] + f * (c[seg + 1] - c[seg])) / 2.0;
}

static void test_input_curve_linear() {
  InputModel lin = [](double mv) { return 4095.0 + mv / kGain; };
  InputSweepPoint pts[17];
  const int n = make_points(lin, 17, stats0(), pts);
  int16_t c[kCurvePoints]; CM::Fit line; float worst = -1, hold = -1;
  CHECK(BuildInputCurve(pts, n, c, &line, &worst, &hold) == CM::VERDICT_OK);
  for (int i = 0; i < kCurvePoints; ++i) {
    const double truth = kGain * (256.0 * i) + 4095.0 * -kGain;
    NEAR(c[i] / 2.0, truth, 0.5);
    NEAR(CM::FitMillivolts(line, 256.f * i), truth, 0.5);
  }
  CHECK(hold <= 0.5f);
  CHECK(worst <= 0.5f);
}

static void test_input_curve_bent() {
  // 7.5 mV S-bend, zero at both ends of the swept span.
  InputModel bent = [](double mv) {
    const double x = (mv - 300.0) / 11400.0;
    const double mv_err = 7.5 * std::sin(2.0 * kPi * x);
    return 4095.0 + (mv + mv_err) / kGain;  // reading of a slightly wrong input
  };
  InputSweepPoint pts[17];
  const int n = make_points(bent, 17, stats0(), pts);
  int16_t c[kCurvePoints]; CM::Fit line; float worst = 0, hold = 0;
  CHECK(BuildInputCurve(pts, n, c, &line, &worst, &hold) == CM::VERDICT_OK);
  CHECK(worst >= 5.f);
  CHECK(hold <= 1.0f);
  // At each held-out point the curve maps the reading back to the DAC mV
  // (the reference) within 1 mV.
  for (int i = 1; i < n; i += 2)
    NEAR(curve_mv_at(c, pts[i].stats.mean_raw), pts[i].mv, 1.0);
}

static void test_input_curve_verdicts() {
  InputModel lin = [](double mv) { return 4095.0 + mv / kGain; };
  InputSweepPoint pts[17];
  int16_t c[kCurvePoints]; CM::Fit line; float w, h;

  int n = make_points(lin, 17, stats0(), pts);
  pts[8].stats.mean_raw = 3.f;  // on the low rail
  CHECK(BuildInputCurve(pts, n, c, &line, &w, &h) == CM::VERDICT_RAIL);

  n = make_points(lin, 17, stats0(), pts);
  pts[5].stats.max_raw = pts[5].stats.min_raw + 30.f;
  CHECK(BuildInputCurve(pts, n, c, &line, &w, &h) == CM::VERDICT_NOISY);

  n = make_points(lin, 17, stats0(), pts);
  for (int i = 0; i < n; ++i) { pts[i].stats.mean_raw = 2000.f; pts[i].stats.min_raw = 1999.f; pts[i].stats.max_raw = 2001.f; }
  CHECK(BuildInputCurve(pts, n, c, &line, &w, &h) == CM::VERDICT_FLAT);

  InputModel wrong = [](double mv) { return 150.0 - 0.8 * mv / kGain; };  // raw rises with mV
  n = make_points(wrong, 17, stats0(), pts);
  CHECK(BuildInputCurve(pts, n, c, &line, &w, &h) == CM::VERDICT_SIGN);

  InputModel half = [](double mv) { return 4095.0 + mv / (kGain * 3.0); };  // gain a third
  n = make_points(half, 17, stats0(), pts, 300, 11700);
  CHECK(BuildInputCurve(pts, n, c, &line, &w, &h) == CM::VERDICT_GAIN);

  n = make_points(lin, 17, stats0(), pts);
  std::swap(pts[6].stats, pts[7].stats);
  CHECK(BuildInputCurve(pts, n, c, &line, &w, &h) == CM::VERDICT_MONOTONIC);

  n = make_points(lin, 17, stats0(), pts);
  pts[3].stats.samples = 0;
  CHECK(BuildInputCurve(pts, n, c, &line, &w, &h) == CM::VERDICT_NO_SAMPLES);
  CHECK(BuildInputCurve(pts, 3, c, &line, &w, &h) == CM::VERDICT_NO_SAMPLES);
}

static void test_apply_value_table() {
  const float nominal_gain = CM::SharedGain(15360, 1200);
  CM::Fit line = {nominal_gain, -nominal_gain * 4095.f};  // offset at raw 0
  int32_t q8[kCurvePoints];
  LineToValueTable(line, q8);
  for (uint32_t raw = 0; raw <= 4095; ++raw) {
    const int32_t today = 4095 - int32_t(raw);
    const int32_t got = ApplyValueTable(q8, raw);
    NEAR(got, today, 0.5);  // rounded, not floored: no -0.5..-1 count bias
  }
  // Last raw reads segment 15 only: q8[16] is the highest index touched.
  int32_t guarded[kCurvePoints + 1];
  memcpy(guarded, q8, sizeof q8);
  guarded[kCurvePoints] = 0x7fffffff;  // would wreck the result if read
  CHECK(std::abs(ApplyValueTable(guarded, 4095) - 0) <= 1);
  // CurveToValueTable of the matching mV x 2 curve agrees with the line.
  int16_t c[kCurvePoints];
  for (int i = 0; i < kCurvePoints; ++i)
    c[i] = int16_t(std::lround(2.0 * CM::FitMillivolts(line, 256.f * i)));
  int32_t q8c[kCurvePoints];
  CurveToValueTable(c, q8c);
  for (int i = 0; i < kCurvePoints; ++i) CHECK(std::abs(q8c[i] - q8[i]) <= 128);  // 0.5 count
}

// The exact interpolation of a q8 table, in value counts.
static double exact_apply(const int32_t q8[kCurvePoints], uint32_t raw) {
  if (raw > 4095u) raw = 4095u;
  const uint32_t seg = raw >> kCurveShift;
  const double f = double(raw & 255u) / 256.0;
  return (double(q8[seg]) + f * (double(q8[seg + 1]) - double(q8[seg]))) / 256.0;
}

static void test_apply_value_table_rounds() {
  // Bent tables crossing zero (negative values round correctly too), and the
  // worst case the int16 mV x 2 curve can produce: +/-32767 mV x 2 at
  // alternate breakpoints (|q8| ~1.43e6, |diff| ~2.86e6, x 255 ~7.3e8 < 2^31),
  // which UBSan would flag if anything overflowed.
  uint32_t lcg = 12345;
  for (int t = 0; t < 40; ++t) {
    int32_t q8[kCurvePoints];
    if (t == 0) {
      int16_t c[kCurvePoints];
      for (int i = 0; i < kCurvePoints; ++i) c[i] = (i & 1) ? int16_t(-32767) : int16_t(32767);
      CurveToValueTable(c, q8);
    } else {
      int32_t v = int32_t(lcg % 2000000u) - 600000;
      for (int i = 0; i < kCurvePoints; ++i) {
        lcg = lcg * 1103515245u + 12345u;
        q8[i] = v;
        v -= int32_t(40000 + (lcg >> 8) % 60000);  // falling, crosses zero
      }
    }
    double worst = 0;
    for (uint32_t raw = 0; raw <= 4096; ++raw) {
      const double err = std::fabs(double(ApplyValueTable(q8, raw)) - exact_apply(q8, raw));
      if (err > worst) worst = err;
    }
    CHECK(worst <= 0.5);
  }
}

int main() {
  test_node_converges_linear();
  test_node_converges_bowed();
  test_rail_entry_flagged_not_clipped();
  test_table_checks();
  test_sweep_points();
  test_summarize();
  test_input_curve_linear();
  test_input_curve_bent();
  test_input_curve_verdicts();
  test_apply_value_table();
  test_apply_value_table_rounds();
  printf("test_calroutine_math: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
