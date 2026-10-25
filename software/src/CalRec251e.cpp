// Firmware: all of it runs from flash (FLASHMEM), none of it is ISR code; every function has
// external linkage so the section attribute is honoured under LTO.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define C251R_CODE FLASHMEM
#else
#define C251R_CODE
#endif

#include "CalRec251e.h"

// The fit must be reproducible bit for bit (no fused multiply-add).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#else
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <string.h>

namespace CalRec251e {

namespace {

constexpr uint8_t kFormat = 1;
constexpr int kGainWindow = 4, kOffsetWindow = 6;   // integer refinement around the rounded pair

void Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
uint16_t Get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

}  // namespace

// Worst predicted |error| of (g, o) over the reachable stage codes. A stage code is
// reachable when the ideal raw code (c * k - b) / a lies in 0..4095.
C251R_CODE double Worst(const Analog &an, uint16_t g, int16_t o, double k, double cutoff) {
  double w = 0.0;
  for (int c = 0; c <= kStockCodeAtTop; c++) {
    const double ideal = (double(c) * k - an.b) / an.a;
    if (ideal < 0.0 || ideal > double(kMaxCode)) continue;
    const double e = fabs(an.a * double(Apply((uint16_t)c, g, o)) + an.b - double(c) * k);
    if (e > w) {
      w = e;
      if (w > cutoff) return w;
    }
  }
  return w;
}

C251R_CODE void Encode(const Record &r, uint8_t out[kRecLen]) {
  out[0] = 'C'; out[1] = 'L'; out[2] = kFormat; out[3] = kModule251e; out[4] = kNch; out[5] = 0;
  memcpy(out + 6, r.unit_id, 8);
  Put16(out + 14, r.seq);
  out[16] = (uint8_t)(r.date >> 24); out[17] = (uint8_t)(r.date >> 16);
  out[18] = (uint8_t)(r.date >> 8);  out[19] = (uint8_t)r.date;
  for (int i = 0; i < kNch; i++) {
    Put16(out + 20 + 4 * i, r.ch[i].gain);
    Put16(out + 22 + 4 * i, (uint16_t)r.ch[i].offset);
  }
  Put16(out + 36, CalRec::Crc16(out, 36));
}

C251R_CODE bool Decode(const uint8_t *in, size_t n, Record *r) {
  if (!in || !r || n != (size_t)kRecLen) return false;
  if (in[0] != 'C' || in[1] != 'L' || in[2] != kFormat || in[3] != kModule251e || in[4] != kNch)
    return false;
  if (CalRec::Crc16(in, 36) != Get16(in + 36)) return false;
  memcpy(r->unit_id, in + 6, 8);
  r->seq = Get16(in + 14);
  r->date = ((uint32_t)in[16] << 24) | ((uint32_t)in[17] << 16) | ((uint32_t)in[18] << 8) | in[19];
  for (int i = 0; i < kNch; i++) {
    r->ch[i].gain = Get16(in + 20 + 4 * i);
    r->ch[i].offset = (int16_t)Get16(in + 22 + 4 * i);
  }
  return true;
}

C251R_CODE uint16_t Apply(uint16_t code, uint16_t gain, int16_t offset) {
  int32_t v = (int32_t)(((uint32_t)code * gain) >> 14) + offset;
  if (v < 0) v = 0;
  if (v > kMaxCode) v = kMaxCode;
  return (uint16_t)v;
}

C251R_CODE int CodeOfMv(double mv) {
  const double c = nearbyint(mv * double(kStockCodeAtTop) / kStockTopMv);
  return c < 0.0 ? 0 : c > double(kMaxCode) ? kMaxCode : (int)c;
}

C251R_CODE double PredictMv(const Analog &an, uint16_t raw_code) { return an.a * double(raw_code) + an.b; }

C251R_CODE bool Line(const FitPoint *p, int n, Analog *an) {
  if (!p || !an || n < 2) return false;
  bool distinct = false;
  for (int i = 1; i < n; i++)
    if (p[i].code != p[0].code) { distinct = true; break; }
  if (!distinct) return false;
  const double dn = (double)n;
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  for (int i = 0; i < n; i++) {
    const double x = (double)p[i].code, y = p[i].mv;
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  an->a = (dn * sxy - sx * sy) / (dn * sxx - sx * sx);
  an->b = (sy - an->a * sx) / dn;
  return true;
}

C251R_CODE FitError Fit(const FitPoint *p, int n, double k, CalRec::Channel *out, Analog *an,
             double *worst_mv) {
  if (!out || !an) return FIT_TOO_FEW;
  Analog line;
  if (!Line(p, n, &line)) return FIT_TOO_FEW;
  if (!(line.a > 0.0)) return FIT_SLOPE;
  // gain = 16384 k / a; offset = -b / a, plus 0.5 because the module's shift floors.
  const double g_real = double(kGainOne) * k / line.a;
  const double o_real = -line.b / line.a + 0.5;
  const double g0 = nearbyint(g_real), o0 = nearbyint(o_real);
  if (!(g0 >= 1.0) || g0 > 65535.0) return FIT_GAIN_RANGE;
  if (!(o0 >= -32768.0) || o0 > 32767.0) return FIT_OFFSET_RANGE;
  // Integer refinement over every stage code; the pair with the smallest worst predicted
  // error wins, ties to the one nearest the rounded pair. A pair is dropped as soon as its
  // running worst passes the best so far.
  double best = -1.0;
  int bg = 0, bo = 0;
  for (int dg = -kGainWindow; dg <= kGainWindow; dg++) {
    for (int dof = -kOffsetWindow; dof <= kOffsetWindow; dof++) {
      const double g = g0 + dg, o = o0 + dof;
      if (g < 1.0 || g > 65535.0 || o < -32768.0 || o > 32767.0) continue;
      const double w = Worst(line, (uint16_t)g, (int16_t)o, k, best < 0.0 ? 1.0e300 : best + 1e-9);
      const int dist = (dg < 0 ? -dg : dg) * 8 + (dof < 0 ? -dof : dof);
      const int bdist = (bg < 0 ? -bg : bg) * 8 + (bo < 0 ? -bo : bo);
      if (best < 0.0 || w < best - 1e-9 || (w < best + 1e-9 && dist < bdist)) {
        best = w;
        bg = dg;
        bo = dof;
      }
    }
  }
  out->gain = (uint16_t)(g0 + bg);
  out->offset = (int16_t)(o0 + bo);
  *an = line;
  if (worst_mv) *worst_mv = best;
  return FIT_OK;
}

C251R_CODE double WorstErrorMv(const Analog &an, const CalRec::Channel &c, double k) {
  return Worst(an, c.gain, c.offset, k, 1.0e300);
}

C251R_CODE double Nonlinearity(const FitPoint *p, int n, const Analog &an) {
  double w = 0.0;
  for (int i = 0; i < n; i++) {
    const double e = fabs(p[i].mv - PredictMv(an, p[i].code));
    if (e > w) w = e;
  }
  return w;
}

C251R_CODE bool BowInl(const FitPoint *p, int n, double *bow, double *inl) {
  if (!p || n < 4) return false;
  double x0 = 0.0, x1 = 0.0;
  for (int i = 0; i < n; i++) {
    if (i == 0 || p[i].code < x0) x0 = p[i].code;
    if (i == 0 || p[i].code > x1) x1 = p[i].code;
  }
  if (!(x1 > x0)) return false;
  const double mid = 0.5 * (x0 + x1), half = 0.5 * (x1 - x0);
  // normal equations of y = c0 + c1 t + c2 t^2, t in [-1, 1]
  double s[5] = {0, 0, 0, 0, 0}, r[3] = {0, 0, 0};
  for (int i = 0; i < n; i++) {
    const double t = (double(p[i].code) - mid) / half, y = p[i].mv;
    double tp = 1.0;
    for (int k = 0; k < 5; k++) { s[k] += tp; if (k < 3) r[k] += tp * y; tp *= t; }
  }
  const double m[3][3] = {{s[0], s[1], s[2]}, {s[1], s[2], s[3]}, {s[2], s[3], s[4]}};
  auto det = [](const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
           a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  };
  const double d = det(m);
  if (fabs(d) < 1e-9) return false;
  double c[3];
  for (int col = 0; col < 3; col++) {
    double q[3][3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) q[i][j] = (j == col) ? r[i] : m[i][j];
    c[col] = det(q) / d;
  }
  double w = 0.0;
  for (int i = 0; i < n; i++) {
    const double t = (double(p[i].code) - mid) / half;
    const double e = fabs(p[i].mv - (c[0] + c[1] * t + c[2] * t * t));
    if (e > w) w = e;
  }
  if (bow) *bow = fabs(c[2]) * 2.0 / 3.0;
  if (inl) *inl = w;
  return true;
}

C251R_CODE bool ScaleOk(const Analog &an) {
  if (!(an.a > 0.0)) return false;
  const double v = PredictMv(an, (uint16_t)kStockCodeAtTop);
  return v >= kScaleLoMv && v <= kScaleHiMv && fabs(an.b) <= kZeroMaxMv;
}

C251R_CODE bool AnchorsOk(const Analog &an, const CalRec::Channel &c, double *worst_mv) {
  double w = 0.0;
  for (const Anchor &a : kAnchors) {
    const double e = fabs(PredictMv(an, Apply((uint16_t)a.code, c.gain, c.offset)) - a.mv);
    if (e > w) w = e;
  }
  if (worst_mv) *worst_mv = w;
  return w <= kAnchorTolMv;
}

}  // namespace CalRec251e
