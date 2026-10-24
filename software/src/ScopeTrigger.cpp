// Period estimate and triggering for the Scope. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define SCOPE_CODE FLASHMEM
#define SCOPE_BSS DMAMEM  // loop-context work buffer: RAM2, not RAM1
#else
#define SCOPE_CODE
#define SCOPE_BSS
#endif

#include "ScopeTrigger.h"

#include <math.h>

namespace ScopeTrigger {

namespace {

constexpr float kPeakNear = 0.9f;      // within 10 % of the best lag
constexpr float kMinCorr = 0.5f;       // weaker than this is no period
constexpr float kHoldMinCorr = 0.3f;   // a held lock below this re-locks
constexpr float kRefreshCorr = 0.9f;   // below this the template follows the morph
constexpr float kPeriodSlack = 0.1f;   // a period change beyond 10 % re-locks
constexpr float kHystFrac = 0.05f;     // of peak-to-peak
constexpr int kMaxCandidates = 64;

void Stats(const float *x, int n, float *mean, float *lo, float *hi) {
  double s = 0;
  float l = x[0], h = x[0];
  for (int i = 0; i < n; ++i) {
    s += x[i];
    if (x[i] < l) l = x[i];
    if (x[i] > h) h = x[i];
  }
  *mean = (float)(s / n);
  *lo = l;
  *hi = h;
}

// Normalised correlation of a[0..len) with b[0..len), means removed.
float Correlate(const float *a, const float *b, int len) {
  double ma = 0, mb = 0;
  for (int i = 0; i < len; ++i) { ma += a[i]; mb += b[i]; }
  ma /= len;
  mb /= len;
  double ab = 0, aa = 0, bb = 0;
  for (int i = 0; i < len; ++i) {
    const double da = a[i] - ma, db = b[i] - mb;
    ab += da * db;
    aa += da * da;
    bb += db * db;
  }
  if (aa <= 0 || bb <= 0) return 0.f;
  return (float)(ab / sqrt(aa * bb));
}

// Rising mean crossings (with hysteresis) that leave a full window after them.
int Candidates(const float *x, int n, int window, float mean, float hyst, int *out) {
  int count = 0;
  bool armed = false;
  for (int i = 1; i + window <= n; ++i) {
    if (x[i - 1] < mean - hyst) armed = true;
    if (armed && x[i - 1] < mean && x[i] >= mean) {
      if (count == kMaxCandidates) {  // keep the newest
        for (int k = 1; k < kMaxCandidates; ++k) out[k - 1] = out[k];
        --count;
      }
      out[count++] = i;
      armed = false;
    }
  }
  return count;
}

}  // namespace

SCOPE_CODE float EstimatePeriod(const float *x, int n, int min_lag, int max_lag, float min_pp) {
  if (!x || n < 8) return 0.f;
  if (min_lag < 2) min_lag = 2;
  if (max_lag > n / 2) max_lag = n / 2;
  if (max_lag <= min_lag + 1) return 0.f;
  float mean, lo, hi;
  Stats(x, n, &mean, &lo, &hi);
  if (!(hi - lo > 0.f) || hi - lo < min_pp) return 0.f;

  // Running energies of the two overlapping segments, so no work array.
  double e1 = 0, e2 = 0;
  for (int i = 0; i < n; ++i) { const double a = x[i] - mean; e1 += a * a; }
  e2 = e1;
  for (int L = 1; L < min_lag - 1; ++L) {
    const double a = x[n - L] - mean, b = x[L - 1] - mean;
    e1 -= a * a;
    e2 -= b * b;
  }

  // r(L) for L in [min_lag - 1, max_lag + 1]: one extra each side for peaks.
  static SCOPE_BSS float r[4096 / 2 + 4];
  const int first = min_lag - 1, last = max_lag + 1;
  if (last - first + 1 > (int)(sizeof(r) / sizeof(r[0]))) return 0.f;
  for (int L = first; L <= last; ++L) {
    if (L >= 1) {
      const double a = x[n - L] - mean, b = x[L - 1] - mean;
      e1 -= a * a;
      e2 -= b * b;
    }
    double s = 0;
    for (int i = 0; i + L < n; ++i) s += (double)(x[i] - mean) * (x[i + L] - mean);
    r[L - first] = (e1 > 0 && e2 > 0) ? (float)(s / sqrt(e1 * e2)) : 0.f;
  }

  float best = -1.f;
  for (int L = min_lag; L <= max_lag; ++L) {
    const float v = r[L - first];
    if (v > r[L - 1 - first] && v >= r[L + 1 - first] && v > best) best = v;
  }
  if (best < kMinCorr) return 0.f;
  for (int L = min_lag; L <= max_lag; ++L) {
    const float v = r[L - first];
    if (v > r[L - 1 - first] && v >= r[L + 1 - first] && v >= kPeakNear * best) {
      const float ym = r[L - 1 - first], y0 = v, yp = r[L + 1 - first];
      const float den = ym - 2 * y0 + yp;
      const float d = den != 0.f ? 0.5f * (ym - yp) / den : 0.f;
      return (float)L + (d > 0.5f ? 0.5f : d < -0.5f ? -0.5f : d);
    }
  }
  return 0.f;
}

SCOPE_CODE int LevelTrigger(const float *x, int n, int window, float level, float hyst,
                            bool rising) {
  if (!x || window < 1 || window > n) return -1;
  int found = -1;
  bool armed = false;
  for (int i = 1; i + window <= n; ++i) {
    if (rising) {
      if (x[i - 1] < level - hyst) armed = true;
      if (armed && x[i - 1] < level && x[i] >= level) { found = i; armed = false; }
    } else {
      if (x[i - 1] > level + hyst) armed = true;
      if (armed && x[i - 1] > level && x[i] <= level) { found = i; armed = false; }
    }
  }
  return found;
}

SCOPE_CODE void AutoTrigger::Reset() {
  prev_len_ = 0;
  period_ = 0.f;
  locked_ = false;
}

SCOPE_CODE int AutoTrigger::Find(const float *x, int n, int window, float min_pp) {
  if (!x || window < 1 || window > n || window > kMaxWindow) {
    locked_ = false;
    return n > window ? n - window : 0;
  }
  const float p = EstimatePeriod(x, n, 2, n / 2, min_pp);
  if (p <= 0.f) {
    Reset();
    return n - window;
  }
  float mean, lo, hi;
  Stats(x, n, &mean, &lo, &hi);
  int cand[kMaxCandidates];
  const int nc = Candidates(x, n, window, mean, kHystFrac * (hi - lo), cand);
  if (nc == 0) {
    Reset();
    return n - window;
  }

  int pick = -1;
  float held_corr = -2.f;
  const bool hold = prev_len_ == window && period_ > 0.f &&
                    fabsf(p - period_) <= kPeriodSlack * period_;
  if (hold) {
    // Slide the previous frame over one period of start positions and take
    // the best match. Tracking the feature itself, not a crossing, is what
    // survives a morph: a crossing can fade or stop clearing the hysteresis
    // as the fold deepens, while the shape around it still matches.
    const int hi_s = n - window;
    int lo_s = hi_s - (int)ceilf(p);
    if (lo_s < 0) lo_s = 0;
    float best = -2.f;
    for (int st = lo_s; st <= hi_s; ++st) {
      const float c = Correlate(x + st, prev_, window);
      if (c > best) { best = c; pick = st; }
    }
    if (best < kHoldMinCorr) pick = -1;
    held_corr = best;
  }
  if (pick < 0) {  // fresh lock: steepest crossing within the newest period
    const float from = (float)(n - window) - p;
    float steep = -1e30f;
    for (int k = 0; k < nc; ++k) {
      if (cand[k] < from) continue;
      const float s = x[cand[k]] - x[cand[k] - 1];
      if (s > steep) { steep = s; pick = cand[k]; }
    }
    if (pick < 0) pick = cand[nc - 1];
  }

  // The template stays anchored to the frame it locked on while the shape
  // still matches; re-taking it every frame would let each frame's
  // sub-sample rounding random-walk the trace sideways.
  if (held_corr < kRefreshCorr) {
    for (int i = 0; i < window; ++i) prev_[i] = x[pick + i];
    prev_len_ = window;
  }
  period_ = p;
  locked_ = true;
  return pick;
}

}  // namespace ScopeTrigger
