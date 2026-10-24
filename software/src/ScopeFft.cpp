// The Scope's FFT and spectrum columns. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define SCOPE_CODE FLASHMEM
#define SCOPE_BSS DMAMEM  // loop-context work buffers: RAM2, not RAM1
#else
#define SCOPE_CODE
#define SCOPE_BSS
#endif

#include "ScopeFft.h"

#include <math.h>

namespace ScopeFft {

namespace {

constexpr int kLog2N = 10;
constexpr double kPi = 3.14159265358979323846;

SCOPE_BSS float re_[kN];
SCOPE_BSS float im_[kN];
SCOPE_BSS float cos_[kN / 2];
SCOPE_BSS float sin_[kN / 2];
SCOPE_BSS float win_[kN];
bool tables_ = false;

void Tables() {
  if (tables_) return;
  for (int i = 0; i < kN / 2; ++i) {
    cos_[i] = (float)cos(2 * kPi * i / kN);
    sin_[i] = (float)sin(2 * kPi * i / kN);
  }
  for (int i = 0; i < kN; ++i) win_[i] = (float)(0.5 - 0.5 * cos(2 * kPi * i / kN));  // periodic Hann
  tables_ = true;
}

float TopHz(float rate_hz) {
  const float nyq = rate_hz * 0.5f;
  return nyq < 20000.f ? nyq : 20000.f;
}

// Frequency at fractional column c.
float HzAt(float c, float rate_hz) {
  const float l0 = log10f(20.f), l1 = log10f(TopHz(rate_hz));
  return powf(10.f, l0 + (l1 - l0) * c / 127.f);
}

}  // namespace

SCOPE_CODE void MagnitudeDb(const float *in, float *db_out) {
  Tables();
  for (int i = 0; i < kN; ++i) {
    // bit-reversed load, windowed
    unsigned r = 0;
    for (int b = 0; b < kLog2N; ++b) r |= ((i >> b) & 1u) << (kLog2N - 1 - b);
    re_[r] = in[i] * win_[i];
    im_[r] = 0.f;
  }
  for (int len = 2; len <= kN; len <<= 1) {
    const int half = len >> 1, step = kN / len;
    for (int base = 0; base < kN; base += len) {
      for (int k = 0; k < half; ++k) {
        const float wr = cos_[k * step], wi = -sin_[k * step];
        const int a = base + k, b = a + half;
        const float tr = re_[b] * wr - im_[b] * wi;
        const float ti = re_[b] * wi + im_[b] * wr;
        re_[b] = re_[a] - tr;
        im_[b] = im_[a] - ti;
        re_[a] += tr;
        im_[a] += ti;
      }
    }
  }
  // Hann coherent gain 0.5, one-sided: a sine of amplitude A peaks at A*N/4.
  const float scale = 4.f / kN;
  for (int k = 0; k <= kN / 2; ++k) {
    const float m = sqrtf(re_[k] * re_[k] + im_[k] * im_[k]) * scale;
    const float d = m > 0.f ? 20.f * log10f(m) : kFloorDb;
    db_out[k] = d < kFloorDb ? kFloorDb : d;
  }
}

SCOPE_CODE float PeakHz(const float *db, float rate_hz, float *peak_db) {
  const float df = rate_hz / kN;
  int k0 = (int)ceilf(20.f / df);
  if (k0 < 1) k0 = 1;
  int best = k0;
  for (int k = k0; k < kN / 2; ++k)
    if (db[k] > db[best]) best = k;
  const float a = db[best - 1], b = db[best], c = db[best + 1];
  const float den = a - 2 * b + c;
  float d = den != 0.f ? 0.5f * (a - c) / den : 0.f;
  if (d > 0.5f) d = 0.5f;
  if (d < -0.5f) d = -0.5f;
  if (peak_db) *peak_db = b - 0.25f * (a - c) * d;
  return (best + d) * df;
}

SCOPE_CODE int ColumnForHz(float hz, float rate_hz) {
  if (hz <= 20.f) return 0;
  const float l0 = log10f(20.f), l1 = log10f(TopHz(rate_hz));
  const int c = (int)lroundf((log10f(hz) - l0) / (l1 - l0) * 127.f);
  return c < 0 ? 0 : c > 127 ? 127 : c;
}

SCOPE_CODE void Columns(const float *db, float rate_hz, int plot_h, uint8_t rows_out[128]) {
  const float df = rate_hz / kN;
  for (int c = 0; c < 128; ++c) {
    const float f0 = HzAt(c - 0.5f, rate_hz), f1 = HzAt(c + 0.5f, rate_hz);
    int k0 = (int)ceilf(f0 / df), k1 = (int)floorf(f1 / df);
    if (k0 < 1) k0 = 1;
    if (k1 > kN / 2) k1 = kN / 2;
    float m;
    if (k0 <= k1) {
      m = db[k0];
      for (int k = k0 + 1; k <= k1; ++k) if (db[k] > m) m = db[k];
    } else {
      int k = (int)lroundf(HzAt((float)c, rate_hz) / df);
      if (k < 1) k = 1;
      if (k > kN / 2) k = kN / 2;
      m = db[k];
    }
    float r = (m - kRowBottomDb) / (kRowTopDb - kRowBottomDb) * plot_h;
    if (r < 0) r = 0;
    if (r > plot_h) r = (float)plot_h;
    rows_out[c] = (uint8_t)lroundf(r);
  }
}

}  // namespace ScopeFft
