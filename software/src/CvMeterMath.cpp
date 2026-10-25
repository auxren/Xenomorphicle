// Pure logic for the bench CV meter. See the header for why this is split out.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define CVMETER_MATH_CODE FLASHMEM
#else
#define CVMETER_MATH_CODE
#endif

#include "CvMeterMath.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace CvMeterMath {

CVMETER_MATH_CODE
void SharedReadableRange(int32_t offset, uint16_t scale, int32_t octave_mv,
                         int32_t *lo_mv, int32_t *hi_mv) {
  const float at_max = SharedMillivolts(offset, float(kRawMax), scale, octave_mv);
  const float at_zero = SharedMillivolts(offset, 0.f, scale, octave_mv);
  const float lo = at_max < at_zero ? at_max : at_zero;
  const float hi = at_max < at_zero ? at_zero : at_max;
  *lo_mv = int32_t(std::floor(lo + 0.5f));
  *hi_mv = int32_t(std::floor(hi + 0.5f));
}

CVMETER_MATH_CODE
bool InputCalValid(const InputCalibration &cal, int phys) {
  return phys >= 0 && phys < kChannels && (cal.valid_mask & (1u << phys));
}

CVMETER_MATH_CODE
void InputCalReset(InputCalibration &cal) {
  memset(&cal, 0, sizeof(cal));
  cal.version = InputCalibration::kVersion;
}

CVMETER_MATH_CODE
bool GainPlausible(float gain) {
  if (!std::isfinite(gain)) return false;
  // This front end inverts (raw falls as volts rise), and so does the shared
  // path's arithmetic; a positive gain is a fit to garbage.
  if (gain >= 0.f) return false;
  const float mag = -gain;
  return mag >= kGainAbsMin && mag <= kGainAbsMax;
}

CVMETER_MATH_CODE
int InputCalSanitize(InputCalibration &cal) {
  int dropped = 0;
  if (cal.version != InputCalibration::kVersion) {
    for (int ch = 0; ch < kChannels; ++ch)
      if (cal.valid_mask & (1u << ch)) ++dropped;
    InputCalReset(cal);
    return dropped;
  }
  // Bits for channels this layout does not have are meaningless.
  cal.valid_mask &= uint8_t((1u << kChannels) - 1);
  for (int ch = 0; ch < kChannels; ++ch) {
    if (!(cal.valid_mask & (1u << ch))) continue;
    if (!GainPlausible(cal.gain[ch]) || !std::isfinite(cal.offset_mv[ch])) {
      cal.valid_mask &= uint8_t(~(1u << ch));
      ++dropped;
    }
  }
  return dropped;
}

CVMETER_MATH_CODE
float InputMillivolts(const InputCalibration &cal, int phys, float raw,
                      int32_t shared_offset, uint16_t shared_scale, int32_t octave_mv,
                      bool *per_input) {
  if (InputCalValid(cal, phys)) {
    if (per_input) *per_input = true;
    return cal.gain[phys] * raw + cal.offset_mv[phys];
  }
  if (per_input) *per_input = false;
  return SharedMillivolts(shared_offset, raw, shared_scale, octave_mv);
}

CVMETER_MATH_CODE
bool TwoPointFit(float raw1, float mv1, float raw2, float mv2, Fit *fit) {
  const float span = raw2 - raw1;
  if (!(std::fabs(span) >= kMinFitSpanCounts)) return false;  // also rejects NaN
  fit->gain = (mv2 - mv1) / span;
  fit->offset_mv = mv1 - fit->gain * raw1;
  return true;
}

CVMETER_MATH_CODE
const char *VerdictName(Verdict v) {
  switch (v) {
    case VERDICT_OK: return "OK";
    case VERDICT_NO_SAMPLES: return "NO_SAMPLES";
    case VERDICT_RAIL: return "RAIL";
    case VERDICT_NOISY: return "NOISY";
    case VERDICT_FLAT: return "FLAT";
    case VERDICT_SIGN: return "SIGN";
    case VERDICT_GAIN: return "GAIN";
    case VERDICT_MONOTONIC: return "MONOTONIC";
    case VERDICT_RESIDUAL: return "RESIDUAL";
  }
  return "?";
}

CVMETER_MATH_CODE
Verdict CheckCalibration(const PointStats points[3], const int32_t mv[3],
                         uint16_t shared_scale, int32_t octave_mv, Fit *fit,
                         float *residual_mv) {
  fit->gain = 0.f;
  fit->offset_mv = 0.f;
  *residual_mv = 0.f;

  for (int i = 0; i < 3; ++i)
    if (!points[i].samples) return VERDICT_NO_SAMPLES;
  for (int i = 0; i < 3; ++i) {
    const float m = points[i].mean_raw;
    if (m <= kRailMarginCounts || m >= float(kRawMax) - kRailMarginCounts)
      return VERDICT_RAIL;
  }
  for (int i = 0; i < 3; ++i)
    if (points[i].max_raw - points[i].min_raw > kMaxNoisePpCounts)
      return VERDICT_NOISY;

  Fit f;
  if (!TwoPointFit(points[0].mean_raw, float(mv[0]), points[1].mean_raw, float(mv[1]), &f))
    return VERDICT_FLAT;
  *fit = f;
  *residual_mv = FitMillivolts(f, points[2].mean_raw) - float(mv[2]);

  const float nominal = SharedGain(shared_scale, octave_mv);
  if ((f.gain < 0.f) != (nominal < 0.f) || f.gain == 0.f) return VERDICT_SIGN;
  const float ratio = f.gain / nominal;
  if (!GainPlausible(f.gain) || ratio < 1.f - kGainTolerance || ratio > 1.f + kGainTolerance)
    return VERDICT_GAIN;

  // The verification setting lies between the two fit settings, so its
  // reading must lie between theirs too. A residual test alone can pass a
  // reading that went the wrong way if the line happens to be flat enough.
  const bool mv_between = (mv[2] - mv[0]) * int64_t(mv[1] - mv[2]) > 0;
  if (mv_between) {
    const float a = points[0].mean_raw, b = points[1].mean_raw, c = points[2].mean_raw;
    if (!((c - a) * (b - c) > 0.f)) return VERDICT_MONOTONIC;
  }
  if (std::fabs(*residual_mv) > kMaxVerifyResidualMv) return VERDICT_RESIDUAL;
  return VERDICT_OK;
}

CVMETER_MATH_CODE
int32_t SnapToNode(int32_t v, int32_t node) {
  if (node <= 0) return v;
  // round half away from zero to a multiple of node
  const int32_t q = (v >= 0) ? (v + node / 2) / node : -((-v + node / 2) / node);
  return q * node;
}

CVMETER_MATH_CODE
bool ChooseCalPoints(int32_t lo_mv, int32_t hi_mv, int32_t node_mv, int32_t out[3]) {
  const int32_t span = hi_mv - lo_mv;
  if (span < 2000) return false;
  const int32_t margin = span / 20;  // 5 %
  const int32_t raw_pts[3] = { lo_mv + span / 10, hi_mv - span / 10, lo_mv + span / 2 };
  for (int i = 0; i < 3; ++i) {
    const int32_t snapped = SnapToNode(raw_pts[i], node_mv);
    out[i] = (snapped >= lo_mv + margin && snapped <= hi_mv - margin) ? snapped : raw_pts[i];
  }
  // Snapping must not collapse the three onto each other.
  if (out[0] == out[1] || out[0] == out[2] || out[1] == out[2]) {
    for (int i = 0; i < 3; ++i) out[i] = raw_pts[i];
  }
  return true;
}

CVMETER_MATH_CODE
void DacRange(int points, int octave_zero, int32_t mv_per_step, int32_t *lo_mv, int32_t *hi_mv) {
  *lo_mv = -int32_t(octave_zero) * mv_per_step;
  *hi_mv = int32_t(points - 1 - octave_zero) * mv_per_step;
}

CVMETER_MATH_CODE
bool MillivoltsToDacCode(const uint16_t *table, int points, int octave_zero,
                         int32_t mv_per_step, int32_t mv, uint16_t *code) {
  if (points < 2 || mv_per_step <= 0) return false;
  int32_t lo, hi;
  DacRange(points, octave_zero, mv_per_step, &lo, &hi);
  if (mv < lo || mv > hi) return false;
  const int32_t pos = mv - lo;
  int32_t idx = pos / mv_per_step;
  int32_t frac = pos - idx * mv_per_step;
  if (idx >= points - 1) { idx = points - 2; frac = mv_per_step; }
  const int32_t a = table[idx], b = table[idx + 1];
  const int32_t d = (b - a) * frac;
  // round to nearest, either sign
  const int32_t step = (d >= 0) ? (d + mv_per_step / 2) / mv_per_step
                                : -((-d + mv_per_step / 2) / mv_per_step);
  int32_t c = a + step;
  if (c < 0) c = 0;
  if (c > 65535) c = 65535;
  *code = uint16_t(c);
  return true;
}

CVMETER_MATH_CODE
double PeriodsToHz(uint64_t sum_counts, uint32_t periods, double clock_hz) {
  if (!sum_counts || !periods) return 0.0;
  return clock_hz * double(periods) / double(sum_counts);
}

// 2^(k/12), k = 0..12, and the geometric midpoints 2^((k+0.5)/12) between
// them. Literal so that no libm log/pow gets linked: on Teensy those land in
// ITCM, which the console build has almost none of left (log2 alone was
// 516 bytes).
static constexpr double kSemitone[13] = {
  1.0, 1.0594630943592953, 1.122462048309373, 1.189207115002721,
  1.2599210498948732, 1.3348398541700344, 1.4142135623730951, 1.4983070768766815,
  1.5874010519681994, 1.681792830507429, 1.7817974362806785, 1.8877486253633868, 2.0
};
static constexpr double kSemitoneMid[12] = {
  1.029302236643492, 1.0905077326652577, 1.1553526968722729, 1.2240535433046553,
  1.2968395546510096, 1.3739536474580891, 1.4556531828421873, 1.5422108254079407,
  1.6339154532410005, 1.7310731220122859, 1.8340080864093424, 1.9430638823072117
};

CVMETER_MATH_CODE
bool NearestNote(double hz, double a4_hz, int *midi, double *cents) {
  if (!(hz > 1e-3 && hz < 1e7) || !(a4_hz > 0.0)) return false;
  double r = hz / a4_hz;
  int octave = 0;
  while (r >= 2.0) { r *= 0.5; ++octave; }
  while (r < 1.0) { r *= 2.0; --octave; }
  int k = 0;                       // nearest semitone above A, 0..12
  while (k < 12 && r >= kSemitoneMid[k]) ++k;
  // cents = 1200 * log2(q), q within a quarter tone of 1:
  // ln q = 2 atanh(z), z = (q-1)/(q+1), |z| < 0.0147, so three terms are
  // good to ~1e-13 cents.
  const double q = r / kSemitone[k];
  const double z = (q - 1.0) / (q + 1.0);
  const double z2 = z * z;
  const double ln_q = 2.0 * z * (1.0 + z2 * (1.0 / 3.0 + z2 * (1.0 / 5.0 + z2 * (1.0 / 7.0))));
  *cents = ln_q * (1200.0 / 0.6931471805599453);
  *midi = 69 + 12 * octave + k;
  return true;
}

CVMETER_MATH_CODE
void NoteName(int midi, char *out) {
  static const char *const kNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
  };
  int pc = midi % 12;
  if (pc < 0) pc += 12;
  const int octave = (midi - pc) / 12 - 1;
  // At most 4 characters + NUL reach `out`; format into a roomy local first so the
  // int octave is provably bounded (GCC -Wformat-truncation), then cut.
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%s%d", kNames[pc], octave);
  size_t i = 0;
  for (; i < 4 && tmp[i]; ++i) out[i] = tmp[i];
  out[i] = '\0';
}

CVMETER_MATH_CODE
int64_t ScaleRound(double v, int decimals) {
  double s = v;
  for (int i = 0; i < decimals; ++i) s *= 10.0;
  return int64_t(s >= 0 ? std::floor(s + 0.5) : -std::floor(-s + 0.5));
}

CVMETER_MATH_CODE
int FormatFixed(char *buf, size_t n, int64_t v, int decimals) {
  // No %f, no %lld: neither is safe to assume in the Teensy printf. Split
  // the value into 32-bit halves by hand.
  const bool neg = v < 0;
  uint64_t mag = neg ? uint64_t(-(v + 1)) + 1 : uint64_t(v);
  uint64_t div = 1;
  for (int i = 0; i < decimals; ++i) div *= 10;
  const uint64_t ip = mag / div, fp = mag % div;
  char digits[24];
  int len = 0;
  uint64_t t = ip;
  do { digits[len++] = char('0' + int(t % 10)); t /= 10; } while (t && len < 22);
  char tmp[48];
  int k = 0;
  if (neg) tmp[k++] = '-';
  while (len) tmp[k++] = digits[--len];
  if (decimals > 0) {
    tmp[k++] = '.';
    uint64_t scale = div / 10;
    for (int i = 0; i < decimals; ++i) {
      tmp[k++] = char('0' + int((fp / scale) % 10));
      if (scale > 1) scale /= 10;
    }
  }
  tmp[k] = '\0';
  return snprintf(buf, n, "%s", tmp);
}

CVMETER_MATH_CODE
char ClipFlag(uint32_t min_raw_q8, uint32_t max_raw_q8) {
  if (min_raw_q8 < (2u << 8)) return 'L';
  if (max_raw_q8 >= (uint32_t(kRawMax - 1) << 8)) return 'H';
  return '.';
}

// Append helper: keeps the running length honest when the buffer runs out.
CVMETER_MATH_CODE
void Append(char *buf, size_t n, size_t *pos, const char *s) {
  if (*pos >= n) return;
  const int w = snprintf(buf + *pos, n - *pos, "%s", s);
  if (w > 0) *pos += size_t(w) < n - *pos ? size_t(w) : n - *pos - 1;
}

CVMETER_MATH_CODE
int FormatCvLine(char *buf, size_t n, uint32_t ms, uint32_t seq, uint32_t dropped,
                 const uint32_t raw_q8[kChannels], const int32_t mv10[kChannels],
                 const char cal[kChannels], const char clip[kChannels]) {
  if (!n) return 0;
  buf[0] = '\0';
  size_t pos = 0;
  char f[40];  // "CV," + three uint32 + two commas = 35 worst case
  snprintf(f, sizeof(f), "CV,%lu,%lu,%lu", (unsigned long)ms, (unsigned long)seq,
           (unsigned long)dropped);
  Append(buf, n, &pos, f);
  for (int i = 0; i < kChannels; ++i) {
    // q8 -> hundredths of a count, rounded
    const int64_t c100 = (int64_t(raw_q8[i]) * 100 + 128) >> 8;
    f[0] = ',';
    FormatFixed(f + 1, sizeof(f) - 1, c100, 2);
    Append(buf, n, &pos, f);
  }
  for (int i = 0; i < kChannels; ++i) {
    f[0] = ',';
    FormatFixed(f + 1, sizeof(f) - 1, mv10[i], 1);
    Append(buf, n, &pos, f);
  }
  char flags[kChannels + 2];
  flags[0] = ',';
  for (int i = 0; i < kChannels; ++i) flags[i + 1] = cal[i];
  flags[kChannels + 1] = '\0';
  Append(buf, n, &pos, flags);
  for (int i = 0; i < kChannels; ++i) flags[i + 1] = clip[i];
  Append(buf, n, &pos, flags);
  return int(pos);
}

CVMETER_MATH_CODE
int FormatFreqLine(char *buf, size_t n, uint32_t ms, double hz, uint32_t periods,
                   double a4_hz) {
  char hzs[24], cs[16], note[6];
  FormatFixed(hzs, sizeof(hzs), ScaleRound(hz, 4), 4);
  int midi = 0;
  double cents = 0.0;
  if (NearestNote(hz, a4_hz, &midi, &cents)) {
    NoteName(midi, note);
    const int64_t c = ScaleRound(cents, 2);
    cs[0] = c < 0 ? '-' : '+';
    FormatFixed(cs + 1, sizeof(cs) - 1, c < 0 ? -c : c, 2);
  } else {
    snprintf(note, sizeof(note), "-");
    snprintf(cs, sizeof(cs), "-");
  }
  return snprintf(buf, n, "FREQ,%lu,%s,%lu,%s,%s", (unsigned long)ms, hzs,
                  (unsigned long)periods, note, cs);
}

CVMETER_MATH_CODE
MvArg::Result MvArg::Feed(char c) {
  if (c == '\r' || c == '\n' || c == ' ')
    return digits ? DONE : BAD;
  if ((c == '-' || c == '+') && !digits && !signed_) {
    signed_ = true;
    neg = (c == '-');
    return MORE;
  }
  if (c >= '0' && c <= '9' && digits < 6) {
    value = value * 10 + (c - '0');
    ++digits;
    return MORE;
  }
  return BAD;
}

}  // namespace CvMeterMath
