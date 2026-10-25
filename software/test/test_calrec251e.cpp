// Host tests for the 251e record codec and fit (src/CalRec251e.h). Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_calrec251e test_calrec251e.cpp
//     ../src/CalRec251e.cpp ../src/CalRec.cpp
// The golden record bytes and Apply values come from the shared reference codec
// (Buchla_FW tools/calrec.py encode / apply_251e) for the inputs named below; the fit
// vectors come from a second, independent implementation of the fit as the spec states it,
// ref_cal251e_fit.py in this directory (python3 -I ref_cal251e_fit.py prints them again).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/CalRec251e.h"

namespace R = CalRec251e;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static std::string Hex(const uint8_t *d, size_t n) {
  std::string s;
  char t[4];
  for (size_t i = 0; i < n; ++i) { snprintf(t, sizeof t, "%02x", d[i]); s += t; }
  return s;
}

static R::Record GoldenRecord() {
  R::Record r;
  for (int i = 0; i < 8; ++i) r.unit_id[i] = uint8_t(i + 1);
  r.seq = 7;
  r.date = 1761264000u;
  r.ch[0] = {0x4012, -7};
  r.ch[1] = {0x3FF0, 3};
  r.ch[2] = {0x41A0, -12};
  r.ch[3] = {0x4000, 0};
  return r;
}
static const char *kGoldenHex =
    "434c015104000102030405060708000768fac1804012fff93ff0000341a0fff4400000008e13";

// A synthetic stage as the second implementation builds it.
static void MakePoints(double gain_err, double zero, double bow, double inl, double scale,
                       R::FitPoint p[8]) {
  static const int mv[8] = {1200, 2400, 3600, 4800, 6000, 7200, 8400, 9600};
  for (int i = 0; i < 8; ++i) {
    const int c = R::CodeOfMv(mv[i]);
    const double x = double(c) / 4095.0;
    p[i].code = (uint16_t)c;
    p[i].mv = scale * R::kMvPerCode * (1.0 + gain_err) * double(c) + zero +
              bow * std::sin(3.14159265358979323846 * x) +
              inl * std::sin(8.0 * 3.14159265358979323846 * x);
  }
}

static void test_codec() {
  const R::Record r = GoldenRecord();
  uint8_t b[R::kRecLen];
  R::Encode(r, b);
  CHECK(Hex(b, sizeof b) == kGoldenHex);
  R::Record d;
  CHECK(R::Decode(b, sizeof b, &d));
  CHECK(d.seq == 7 && d.date == 1761264000u && memcmp(d.unit_id, r.unit_id, 8) == 0);
  for (int i = 0; i < 4; ++i) CHECK(d.ch[i].gain == r.ch[i].gain && d.ch[i].offset == r.ch[i].offset);
  // Any single flipped bit is refused (the CRC covers every byte but its own two).
  bool all_refused = true;
  for (size_t i = 0; i < sizeof b; ++i)
    for (int bit = 0; bit < 8; ++bit) {
      uint8_t c[R::kRecLen];
      memcpy(c, b, sizeof c);
      c[i] ^= uint8_t(1u << bit);
      if (R::Decode(c, sizeof c, &d)) all_refused = false;
    }
  CHECK(all_refused);
  CHECK(!R::Decode(b, sizeof b - 1, &d));
  CHECK(!R::Decode(b, sizeof b + 1, &d));
  // A 259e record (module 0x59, 2 channels, 30 bytes) is not a 251e record, and a 251e header with
  // a good CRC but the wrong module code is refused.
  uint8_t w[R::kRecLen];
  memcpy(w, b, sizeof w);
  w[3] = 0x59;
  const uint16_t crc = CalRec::Crc16(w, 36);
  w[36] = uint8_t(crc >> 8); w[37] = uint8_t(crc);
  CHECK(!R::Decode(w, sizeof w, &d));
  memcpy(w, b, sizeof w);
  w[4] = 2;
  const uint16_t crc2 = CalRec::Crc16(w, 36);
  w[36] = uint8_t(crc2 >> 8); w[37] = uint8_t(crc2);
  CHECK(!R::Decode(w, sizeof w, &d));
  // The 259e decoder refuses a 251e record, so neither module can take the other's.
  CalRec::Record r259;
  CHECK(!CalRec::Decode259e(b, 30, &r259));
}

static void test_apply() {
  CHECK(R::Apply(1000, 0x4012, -7) == 994);
  CHECK(R::Apply(3831, 0x41A0, -12) == 3916);
  CHECK(R::Apply(5, 0x3FF0, -9) == 0);         // clamps at 0
  CHECK(R::Apply(4095, 0x8000, 100) == 4095);  // clamps at 4095
  CHECK(R::Apply(0, 0x4000, -5) == 0);
  CHECK(R::Apply(2047, 0x4000, 0) == 2047);    // unity
  CHECK(R::Apply(1, 0x7FFF, 0) == 1);          // the shift floors: 1 * 1.99994 -> 1
  CHECK(R::Apply(4095, 0xFFFF, 0) == 4095);    // no 32-bit overflow
}

static void test_codes() {
  CHECK(R::CodeOfMv(0) == 0);
  CHECK(R::CodeOfMv(300) == 115);
  CHECK(R::CodeOfMv(1200) == 460);
  CHECK(R::CodeOfMv(9600) == 3678);
  CHECK(R::CodeOfMv(10000) == 3831);
  CHECK(R::CodeOfMv(99999) == 4095);
  CHECK(std::fabs(R::kMvPerCode - 2.610284521) < 1e-8);
}

struct Vec { double ge, zero, bow, inl; double a, b; int g, o; double worst; };
static void test_fit_vectors() {
  static const Vec v[] = {
    {0.012, 6.0, 1.2, 0.5, 2.641563997, 6.926753511, 16189, -2, 1.629538},
    {-0.015, 2.0, 1.0, 0.3, 2.571101010, 2.758937697, 16637, -1, 2.319908},
    {0.0, 0.0, 0.0, 0.0, 2.610284521, 0.0, 16384, 0, 0.0},
    {0.03, -40.0, 2.0, 1.0, 2.688509295, -38.436329535, 15907, 15, 1.891310},
  };
  for (const Vec &x : v) {
    R::FitPoint p[8];
    MakePoints(x.ge, x.zero, x.bow, x.inl, 1.0, p);
    CalRec::Channel c;
    R::Analog an;
    double w = -1.0;
    CHECK(R::Fit(p, 8, R::kMvPerCode, &c, &an, &w) == R::FIT_OK);
    CHECK(std::fabs(an.a - x.a) < 1e-6 && std::fabs(an.b - x.b) < 1e-5);
    CHECK(int(c.gain) == x.g && int(c.offset) == x.o);
    CHECK(std::fabs(w - x.worst) < 1e-4);
    // The pair is the best one in a wider window than the search used (gain +-8, offset +-12),
    // to within 0.3 mV, and within about one DAC code of the line (the module's own floor).
    double wide = 1e300;
    for (int dg = -8; dg <= 8; dg++)
      for (int dof = -12; dof <= 12; dof++) {
        const double ww = R::Worst(an, uint16_t(c.gain + dg), int16_t(c.offset + dof), R::kMvPerCode, wide);
        if (ww < wide) wide = ww;
      }
    CHECK(w <= wide + 0.3);
    CHECK(w <= 1.1 * an.a + 1e-9);
  }
}

static void test_fit_errors() {
  R::FitPoint p[8];
  CalRec::Channel c;
  R::Analog an;
  MakePoints(0.0, 0.0, 0.0, 0.0, 1.0, p);
  CHECK(R::Fit(p, 1, R::kMvPerCode, &c, &an, nullptr) == R::FIT_TOO_FEW);
  R::FitPoint same[3] = {{100, 1.0}, {100, 2.0}, {100, 3.0}};
  CHECK(R::Fit(same, 3, R::kMvPerCode, &c, &an, nullptr) == R::FIT_TOO_FEW);
  R::FitPoint down[2] = {{100, 500.0}, {200, 100.0}};   // falling: a < 0
  CHECK(R::Fit(down, 2, R::kMvPerCode, &c, &an, nullptr) == R::FIT_SLOPE);
  R::FitPoint tiny[2] = {{100, 1.0}, {200, 2.0}};       // a = 0.01 mV per code: gain far above 4.0
  CHECK(R::Fit(tiny, 2, R::kMvPerCode, &c, &an, nullptr) == R::FIT_GAIN_RANGE);
  R::FitPoint huge[2] = {{100, 1.0e6}, {200, 1.0e6 + 261.03}};  // b ~ 1e6 mV: offset beyond s16
  CHECK(R::Fit(huge, 2, R::kMvPerCode, &c, &an, nullptr) == R::FIT_OFFSET_RANGE);
  // *out and *an are written only on success.
  c = CalRec::Channel{0xAAAA, 0x555};
  an = R::Analog{-1.0, -1.0};
  CHECK(R::Fit(tiny, 2, R::kMvPerCode, &c, &an, nullptr) != R::FIT_OK);
  CHECK(c.gain == 0xAAAA && c.offset == 0x555 && an.a == -1.0);
}

static void test_scale_and_anchors() {
  R::Analog an = {R::kMvPerCode, 0.0};
  CHECK(R::ScaleOk(an));
  // The line at code 3831 must be 9600..10400 mV.
  an.a = 9600.0 / 3831.0 - 1e-6;  CHECK(!R::ScaleOk(an));
  an.a = 9600.0 / 3831.0 + 1e-6;  CHECK(R::ScaleOk(an));
  an.a = 10400.0 / 3831.0 + 1e-6; CHECK(!R::ScaleOk(an));
  an.a = 10400.0 / 3831.0 - 1e-6; CHECK(R::ScaleOk(an));
  an.a = R::kMvPerCode;
  an.b = 100.5; CHECK(!R::ScaleOk(an));
  an.b = -100.5; CHECK(!R::ScaleOk(an));
  an.b = 99.0; an.a = (10000.0 - 99.0) / 3831.0; CHECK(R::ScaleOk(an));
  an.b = 0.0; an.a = -1.0; CHECK(!R::ScaleOk(an));

  // Anchors: the ideal record on the ideal stage; one gain step off; a bad offset.
  an = R::Analog{R::kMvPerCode, 0.0};
  double w = -1.0;
  CHECK(R::AnchorsOk(an, CalRec::Channel{0x4000, 0}, &w) && w < 0.7);  // code rounding only
  CHECK(R::AnchorsOk(an, CalRec::Channel{0x4000 + 2, 0}, &w));       // 2 steps of gain: ~2 mV at 10 V
  CHECK(!R::AnchorsOk(an, CalRec::Channel{0x4000, 4}, &w) && w > 8.0);  // 4 codes of offset = 10 mV
}

// The 259e's lesson. A record fitted with the WRONG scale constant agrees with itself: the
// error check that uses the same constant sees nothing. Only the absolute anchors (literal
// volts, not derived from the fit's constant) reject it.
static void test_wrong_scale_replay() {
  R::FitPoint p[8];
  MakePoints(0.012, 6.0, 1.2, 0.5, 1.0, p);   // a stage that really is 383.1 codes per volt
  CalRec::Channel good, bad;
  R::Analog an, an2;
  double wg = 0, wb = 0;
  CHECK(R::Fit(p, 8, R::kMvPerCode, &good, &an, &wg) == R::FIT_OK);
  const double k_wrong = 10000.0 / 4096.0;    // "4096 codes for 10 V": 6.5 % low
  CHECK(R::Fit(p, 8, k_wrong, &bad, &an2, &wb) == R::FIT_OK);
  CHECK(bad.gain != good.gain);
  CHECK(wb < 3.0);                            // self-consistent: its own error looks fine
  CHECK(R::WorstErrorMv(an, bad, k_wrong) < 3.0);      // ... checked against its own constant
  double aw = 0.0;
  CHECK(R::AnchorsOk(an, good, &aw));
  CHECK(!R::AnchorsOk(an, bad, &aw));         // the anchors reject it
  CHECK(aw > 500.0);                          // by hundreds of mV (6.5 % of 10 V = 650 mV)
  // And the true-scale error of the bad record is just as large.
  CHECK(R::WorstErrorMv(an, bad, R::kMvPerCode) > 500.0);
  // A stage that really IS 4096 codes per 10 V (scale 3831/4096): its fit is self-consistent too,
  // but the line at code 3831 is 6.5 % low, which ScaleOk refuses.
  MakePoints(0.0, 0.0, 0.0, 0.0, 3831.0 / 4096.0, p);
  R::Analog low;
  CHECK(R::Line(p, 8, &low));
  CHECK(!R::ScaleOk(low));
  MakePoints(0.0, 0.0, 0.0, 0.0, 1.04, p);    // 4 % high: just outside
  CHECK(R::Line(p, 8, &low) && !R::ScaleOk(low));
  MakePoints(0.0, 0.0, 0.0, 0.0, 1.035, p);   // 3.5 % high: inside
  CHECK(R::Line(p, 8, &low) && R::ScaleOk(low));
}

static void test_nonlinearity() {
  R::FitPoint p[8];
  MakePoints(0.0, 0.0, 0.0, 0.0, 1.0, p);
  R::Analog an;
  CHECK(R::Line(p, 8, &an));
  CHECK(R::Nonlinearity(p, 8, an) < 1e-9);
  p[3].mv += 3.0;
  CHECK(R::Line(p, 8, &an));
  const double nl = R::Nonlinearity(p, 8, an);
  CHECK(nl > 1.5 && nl < 3.0);
}

static void test_bow_inl() {
  R::FitPoint p[8];
  double bow = -1, inl = -1;
  for (int i = 0; i < 8; ++i) {
    p[i].code = uint16_t(460 + 460 * i);
    const double t = (double(p[i].code) - 2070.0) / 1610.0;   // 460..3680 -> about -1..1
    p[i].mv = 2.6 * p[i].code + 5.0 + 3.0 * t * t;
  }
  CHECK(R::BowInl(p, 8, &bow, &inl));
  CHECK(std::fabs(bow - 2.0) < 0.02 && inl < 0.01);          // a pure parabola: all bow, no INL
  p[3].mv += 1.0;
  CHECK(R::BowInl(p, 8, &bow, &inl));
  CHECK(inl > 0.4 && inl < 1.0);                              // a 1 mV glitch shows as INL
  CHECK(!R::BowInl(p, 3, &bow, &inl));
}

// Worst() looks at every reachable stage code including both ends (0 and 3831): compare with a
// plain loop here, on records whose largest error sits at the top code, at code 0 and mid-range.
static void test_worst_covers_every_code() {
  const R::Analog an = {R::kMvPerCode * 1.01, 3.0};
  const CalRec::Channel cases[] = {{0x4000 + 60, 0}, {0x4000 - 60, 40}, {0x4000, -6}, {0x4000 + 3, 5}};
  for (const CalRec::Channel &c : cases) {
    double want = 0.0, at = -1;
    for (int code = 0; code <= 3831; ++code) {
      const double ideal = (code * R::kMvPerCode - an.b) / an.a;
      if (ideal < 0.0 || ideal > 4095.0) continue;
      const double e = std::fabs(an.a * R::Apply((uint16_t)code, c.gain, c.offset) + an.b - code * R::kMvPerCode);
      if (e > want) { want = e; at = code; }
    }
    CHECK(std::fabs(R::WorstErrorMv(an, c, R::kMvPerCode) - want) < 1e-12);
    CHECK(std::fabs(R::Worst(an, c.gain, c.offset, R::kMvPerCode) - want) < 1e-12);
    if (c.gain == 0x4000 + 60) CHECK(at == 3831);    // this one peaks at the very top code
  }
  // the early-out returns something above the cutoff, never below the true worst
  CHECK(R::Worst(an, 0x4000 + 60, 0, R::kMvPerCode, 1.0) > 1.0);
}

int main() {
  test_worst_covers_every_code();
  test_bow_inl();
  test_codec();
  test_apply();
  test_codes();
  test_fit_vectors();
  test_fit_errors();
  test_scale_and_anchors();
  test_wrong_scale_replay();
  test_nonlinearity();
  printf("test_calrec251e: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
