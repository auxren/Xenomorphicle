#include "CalRec.h"

// The fit must match Python's IEEE doubles bit for bit, so the compiler may
// not fuse a*b+c into an FMA (Teensy 4.1's FPU has one) or reassociate.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")  // GCC ignores the STDC pragma
#else
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <string.h>

namespace CalRec {

namespace {

constexpr uint8_t kFormat = 1;

void Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
uint16_t Get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

}  // namespace

uint16_t Crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)(d[i] << 8);
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

void Encode259e(const Record &r, uint8_t out[kRecLen259e]) {
  out[0] = 'C'; out[1] = 'L'; out[2] = kFormat; out[3] = kModule259e; out[4] = 2; out[5] = 0;
  memcpy(out + 6, r.unit_id, 8);
  Put16(out + 14, r.seq);
  out[16] = (uint8_t)(r.date >> 24); out[17] = (uint8_t)(r.date >> 16);
  out[18] = (uint8_t)(r.date >> 8);  out[19] = (uint8_t)r.date;
  for (int i = 0; i < 2; i++) {
    Put16(out + 20 + 4 * i, r.ch[i].gain);
    Put16(out + 22 + 4 * i, (uint16_t)r.ch[i].offset);
  }
  Put16(out + 28, Crc16(out, 28));
}

bool Decode259e(const uint8_t *in, size_t n, Record *r) {
  if (!in || !r || n != (size_t)kRecLen259e) return false;
  if (in[0] != 'C' || in[1] != 'L' || in[2] != kFormat || in[3] != kModule259e || in[4] != 2)
    return false;
  if (Crc16(in, 28) != Get16(in + 28)) return false;
  memcpy(r->unit_id, in + 6, 8);
  r->seq = Get16(in + 14);
  r->date = ((uint32_t)in[16] << 24) | ((uint32_t)in[17] << 16) | ((uint32_t)in[18] << 8) | in[19];
  for (int i = 0; i < 2; i++) {
    r->ch[i].gain = Get16(in + 20 + 4 * i);
    r->ch[i].offset = (int16_t)Get16(in + 22 + 4 * i);
  }
  return true;
}

bool FrameAllowed(const uint8_t *f, uint8_t n) {
  if (!f || n < 6 || f[0] != (uint8_t)(n - 1)) return false;
  if (f[1] != 0x00 || f[2] != kMaster || f[5] != kMagic) return false;
  const uint8_t len = f[0];
  switch (f[3]) {
    case kCmdRead:     return len == 7;
    case kCmdCommit:   return len == 8 || len == 9;
    case kCmdPut:
      if (len < 7 || len > 14) return false;
      return (int)f[6] + (len - 6) <= 40;  // off + datalen
    case kCmdOverride: return len == 8;
    case kCmdRelease:  return len == 5;
    default:           return false;
  }
}

int BuildFrame(uint8_t cmd, uint8_t target, const uint8_t *payload, uint8_t plen,
               uint8_t out[kMaxFrame]) {
  if (!out || (plen && !payload) || plen > kMaxFrame - 6) return -1;
  uint8_t f[kMaxFrame];
  f[0] = (uint8_t)(5 + plen);
  f[1] = 0x00; f[2] = kMaster; f[3] = cmd; f[4] = target; f[5] = kMagic;
  if (plen) memcpy(f + 6, payload, plen);
  const uint8_t total = (uint8_t)(6 + plen);
  if (!FrameAllowed(f, total)) return -1;
  memcpy(out, f, total);
  return total;
}

bool ParseReply(const uint8_t *f, uint8_t n, Reply *r) {
  if (!f || !r || n < 6 || f[0] != (uint8_t)(n - 1)) return false;
  if (f[1] != kMaster || f[3] != kCmdReply) return false;
  if (n - 4 > 15) return false;
  r->src = f[2]; r->what = f[4]; r->arg = f[5];
  r->len = (uint8_t)(n - 6);
  memcpy(r->data, f + 6, r->len);
  return true;
}

uint16_t Apply259e(uint16_t x, uint16_t gain, int16_t offset) {
  int32_t v = (int32_t)(((uint32_t)x * gain) >> 16) + offset;
  if (v < 0) v = 0;
  if (v > 0x7FFF) v = 0x7FFF;
  return (uint16_t)v;
}

bool ConsoleCommandBlockedDuringRun(char c) {
  return c == 'w' || c == 'k' || c == 'm' || c == 'x';
}

bool ConsoleFrameAllowedDuringRun(const uint8_t *f, uint8_t n) {
  if (!f || n < 4) return false;
  if (f[3] == kCmdPut || f[3] == kCmdCommit) return false;  // whatever the address or magic
  return FrameAllowed(f, n);
}

double TargetCounts(double mv, double counts_per_oct) { return mv * counts_per_oct / 1200.0; }
double TargetCounts(double mv) { return TargetCounts(mv, kCountsPerOctGolden); }

FitError Fit259e(const FitPoint *p, int n, double counts_per_oct, Channel *out) {
  if (!p || !out || n < 2) return FIT_TOO_FEW;
  bool distinct = false;
  for (int i = 1; i < n; i++)
    if (p[i].raw != p[0].raw) { distinct = true; break; }
  if (!distinct) return FIT_TOO_FEW;
  const double dn = (double)n;
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  for (int i = 0; i < n; i++) {
    const double x = (double)p[i].raw;
    const double y = p[i].mv * counts_per_oct / 1200.0;
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  const double slope = (dn * sxy - sx * sy) / (dn * sxx - sx * sx);
  const double intercept = (sy - slope * sx) / dn;
  const double g = nearbyint(slope * 65536.0);
  const double o = nearbyint(intercept);
  if (!(g > 0.0) || g >= 65536.0) return FIT_GAIN_RANGE;
  if (!(o >= -32768.0) || o > 32767.0) return FIT_OFFSET_RANGE;
  out->gain = (uint16_t)g;
  out->offset = (int16_t)o;
  return FIT_OK;
}

FitError Fit259e(const FitPoint *p, int n, Channel *out) {
  return Fit259e(p, n, kCountsPerOctGolden, out);
}

double ResidualCents(uint16_t raw, double mv, const Channel &c, double counts_per_oct) {
  return ((double)Apply259e(raw, c.gain, c.offset) - TargetCounts(mv, counts_per_oct)) * 1200.0 /
         counts_per_oct;
}
double ResidualCents(uint16_t raw, double mv, const Channel &c) {
  return ResidualCents(raw, mv, c, kCountsPerOctGolden);
}

double ResidualCentsUnclamped(uint16_t raw, double mv, const Channel &c, double counts_per_oct) {
  const double v = (double)(int32_t)(((uint32_t)raw * c.gain) >> 16) + (double)c.offset;
  return (v - TargetCounts(mv, counts_per_oct)) * 1200.0 / counts_per_oct;
}
double ResidualCentsUnclamped(uint16_t raw, double mv, const Channel &c) {
  return ResidualCentsUnclamped(raw, mv, c, kCountsPerOctGolden);
}

double ExpectedGain(double counts_per_oct) { return counts_per_oct * 65536.0 / kRawPerOct; }

Sanity Sanity259e(const Channel &c, double counts_per_oct) {
  const double g_exp = ExpectedGain(counts_per_oct);
  const double g = (double)c.gain;
  if (g < g_exp * (1.0 - kGainTol)) return SANITY_GAIN_LOW;
  if (g > g_exp * (1.0 + kGainTol)) return SANITY_GAIN_HIGH;
  const int o = (int)c.offset;
  if (o < 0 || o > kOffsetMax || o > kOffsetHardCap || o < -kOffsetHardCap) return SANITY_OFFSET_RANGE;
  return SANITY_OK;
}

Sanity Sanity259e(const Channel &c0, const Channel &c1, double counts_per_oct) {
  Sanity s = Sanity259e(c0, counts_per_oct);
  if (s != SANITY_OK) return s;
  s = Sanity259e(c1, counts_per_oct);
  if (s != SANITY_OK) return s;
  if (fabs((double)c0.gain - (double)c1.gain) > kChannelGainTol * ExpectedGain(counts_per_oct))
    return SANITY_CH_MISMATCH;
  return SANITY_OK;
}

const char *SanityName(Sanity s) {
  switch (s) {
    case SANITY_OK: return "OK";
    case SANITY_GAIN_LOW: return "GAIN_LOW";
    case SANITY_GAIN_HIGH: return "GAIN_HIGH";
    case SANITY_OFFSET_RANGE: return "OFFSET_RANGE";
    case SANITY_CH_MISMATCH: return "CH_MISMATCH";
  }
  return "?";
}

bool OctaveRatioOk(double f_lo, double f_hi, double n_oct, double tol) {
  if (!(f_lo > 0.0) || !(f_hi > 0.0) || !(n_oct > 0.0) || !(tol >= 0.0)) return false;
  return fabs(log2(f_hi / f_lo) / n_oct - 1.0) <= tol;
}

}  // namespace CalRec
