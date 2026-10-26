// The Scope's FFT and spectrum columns. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define SCOPE_CODE FLASHMEM
#else
#define SCOPE_CODE
#endif

#include "ScopeFft.h"

#include <math.h>

namespace ScopeFft {

namespace {

constexpr int kLog2N = 10;
constexpr double kPi = 3.14159265358979323846;

void Tables(Work &w) {
  if (w.tables) return;
  for (int i = 0; i < kN / 2; ++i) {
    w.cos_t[i] = (float)cos(2 * kPi * i / kN);
    w.sin_t[i] = (float)sin(2 * kPi * i / kN);
  }
  w.tables = true;
}

// Periodic windows (a sum of cosines); Hann is the formula the view always used.
void MakeWindow(Work &w, Window kind) {
  if (w.win_kind == (uint8_t)kind) return;
  double sum = 0;
  for (int i = 0; i < kN; ++i) {
    const double x = 2 * kPi * i / kN;
    double v;
    switch (kind) {
      case Window::BLACKMAN_HARRIS:
        v = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
        break;
      case Window::FLATTOP:
        v = 0.21557895 - 0.41663158 * cos(x) + 0.277263158 * cos(2 * x) - 0.083578947 * cos(3 * x) +
            0.006947368 * cos(4 * x);
        break;
      case Window::RECT:
        v = 1.0;
        break;
      default:
        v = 0.5 - 0.5 * cos(x);
        break;
    }
    w.win[i] = (float)v;
    sum += v;
  }
  // Hann keeps its exact constant (4/N); the others are normalised to the same meaning.
  w.win_scale = kind == Window::HANN ? 4.f / kN : (float)(2.0 / sum);
  w.win_kind = (uint8_t)kind;
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

namespace {
// Mean removed, windowed, transformed: the result is left in w.re / w.im.
void Transform(const float *in, Work &w, Window win) {
  Tables(w);
  MakeWindow(w, win);
  float *const re_ = w.re, *const im_ = w.im, *const cos_ = w.cos_t, *const sin_ = w.sin_t;
  const float *const win_ = w.win;
  // The mean comes off before the window, for every source: any offset (a codec's,
  // an ADC's, a CV input's) leaks through the Hann window into the lowest bins and
  // swamps them. Exact, and no filter: a bin is 47 Hz wide, so a high-pass at 10 Hz
  // would be below one bin and only add settling.
  float mean = 0.f;
  for (int i = 0; i < kN; ++i) mean += in[i];
  mean *= 1.f / kN;
  for (int i = 0; i < kN; ++i) {
    // bit-reversed load, windowed
    unsigned r = 0;
    for (int b = 0; b < kLog2N; ++b) r |= ((i >> b) & 1u) << (kLog2N - 1 - b);
    re_[r] = (in[i] - mean) * win_[i];
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
}
}  // namespace

SCOPE_CODE void MagnitudeDb(const float *in, float *db_out, Work &w, Window win) {
  Transform(in, w, win);
  const float *const re_ = w.re, *const im_ = w.im;
  // a sine of amplitude A peaks at A*N/4 with Hann (coherent gain 0.5, one-sided)
  const float scale = w.win_scale;
  for (int k = 0; k <= kN / 2; ++k) {
    const float m = sqrtf(re_[k] * re_[k] + im_[k] * im_[k]) * scale;
    const float d = m > 0.f ? 20.f * log10f(m) : kFloorDb;
    db_out[k] = d < kFloorDb ? kFloorDb : d;
  }
}

SCOPE_CODE void PowerSpectrum(const float *in, float *pow_out, Work &w, Window win) {
  Transform(in, w, win);
  const float scale = w.win_scale;
  for (int k = 0; k <= kN / 2; ++k) {
    const float m = sqrtf(w.re[k] * w.re[k] + w.im[k] * w.im[k]) * scale;
    pow_out[k] = m * m;
  }
}

SCOPE_CODE void PowerToDb(const float *pow, float *db_out) {
  for (int k = 0; k <= kN / 2; ++k) {
    const float d = pow[k] > 0.f ? 10.f * log10f(pow[k]) : kFloorDb;
    db_out[k] = d < kFloorDb ? kFloorDb : d;
  }
}

SCOPE_CODE float PeakHz(const float *db, float rate_hz, float *peak_db, float min_hz, float max_hz) {
  const float df = rate_hz / kN;
  int k0 = (int)ceilf(min_hz / df);
  if (k0 < 1) k0 = 1;
  int k1 = (int)floorf(max_hz / df);
  if (k1 > kN / 2 - 1) k1 = kN / 2 - 1;
  if (k1 < k0) k1 = k0;
  int best = k0;
  for (int k = k0; k <= k1; ++k)
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

namespace {
// Level at fractional bin position pos: linear in dB between the bins around it.
float DbAt(const float *db, float pos) {
  if (pos < 0.f) pos = 0.f;
  if (pos > (float)(kN / 2)) pos = (float)(kN / 2);
  int k = (int)pos;
  if (k > kN / 2 - 1) k = kN / 2 - 1;
  const float t = pos - (float)k;
  return db[k] * (1.f - t) + db[k + 1] * t;
}

uint8_t RowFor(float m, int plot_h) {
  float r = (m - kRowBottomDb) / (kRowTopDb - kRowBottomDb) * plot_h;
  if (r < 0) r = 0;
  if (r > plot_h) r = (float)plot_h;
  return (uint8_t)lroundf(r);
}
}  // namespace

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
      m = DbAt(db, HzAt((float)c, rate_hz) / df);   // no bin in this column: interpolate
    }
    rows_out[c] = RowFor(m, plot_h);
  }
}

SCOPE_CODE int ColumnForHzLinear(float hz, float span_hz) {
  const int c = (int)lroundf(hz / span_hz * 127.f);
  return c < 0 ? 0 : c > 127 ? 127 : c;
}

SCOPE_CODE void ColumnsLinear(const float *db, float rate_hz, float span_hz, int plot_h, uint8_t rows_out[128]) {
  const float df = rate_hz / kN;
  for (int c = 0; c < 128; ++c) {
    const float f0 = span_hz * (c - 0.5f) / 127.f, f1 = span_hz * (c + 0.5f) / 127.f;
    int k0 = (int)ceilf(f0 / df), k1 = (int)floorf(f1 / df);
    if (k0 < 1) k0 = 1;
    if (k1 > kN / 2) k1 = kN / 2;
    float m;
    if (k0 <= k1) {
      m = db[k0];
      for (int k = k0 + 1; k <= k1; ++k) if (db[k] > m) m = db[k];
    } else {
      m = DbAt(db, span_hz * c / 127.f / df);
    }
    rows_out[c] = RowFor(m, plot_h);
  }
}

}  // namespace ScopeFft
