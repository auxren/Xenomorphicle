// Host tests for the Scope's FFT, peak readout and log spectrum columns
// (src/ScopeFft.cpp). Inputs are normalised: +-1.0 is full scale.
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/ScopeFft.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                          \
  } while (0)

using namespace ScopeFft;

static Work g_work;

static const double kPi = 3.14159265358979323846;

static void test_peak_sine() {
  printf("test_peak_sine\n");
  std::vector<float> x(kN), db(kN / 2 + 1);
  for (int i = 0; i < kN; ++i) x[i] = (float)(0.5 * std::sin(2 * kPi * 550 * i / 48000.0));
  MagnitudeDb(x.data(), db.data(), g_work);
  float pk_db = 0;
  const float hz = PeakHz(db.data(), 48000.f, &pk_db);
  printf("  peak %.1f Hz at %.2f dB\n", hz, pk_db);
  CHECK(std::fabs(hz - 550.f) <= 5.f);
  CHECK(std::fabs(pk_db - (-6.02f)) <= 1.f);

  for (int i = 0; i < kN; ++i) x[i] = (float)std::sin(2 * kPi * 46.875 * 64 * i / 48000.0);
  MagnitudeDb(x.data(), db.data(), g_work);
  PeakHz(db.data(), 48000.f, &pk_db);
  CHECK(std::fabs(pk_db) <= 0.5f);  // on-bin full-scale sine reads 0 dB
}

static void test_ringmod_sidebands() {
  printf("test_ringmod_sidebands\n");
  std::vector<float> x(kN), db(kN / 2 + 1);
  for (int i = 0; i < kN; ++i)
    x[i] = (float)(0.8 * std::sin(2 * kPi * 770 * i / 48000.0) * std::sin(2 * kPi * 220 * i / 48000.0));
  MagnitudeDb(x.data(), db.data(), g_work);
  uint8_t rows[128];
  Columns(db.data(), 48000.f, 52, rows);
  // the two largest local maxima
  int a = -1, b = -1;
  for (int c = 1; c < 127; ++c) {
    if (!(rows[c] >= rows[c - 1] && rows[c] > rows[c + 1])) continue;
    if (a < 0 || rows[c] > rows[a]) { b = a; a = c; }
    else if (b < 0 || rows[c] > rows[b]) b = c;
  }
  const int c550 = ColumnForHz(550, 48000), c990 = ColumnForHz(990, 48000);
  printf("  maxima at %d,%d; expect %d,%d\n", a, b, c550, c990);
  const int lo = a < b ? a : b, hi = a < b ? b : a;
  CHECK(std::abs(lo - c550) <= 1);
  CHECK(std::abs(hi - c990) <= 1);
}

static void test_silence_floor() {
  printf("test_silence_floor\n");
  std::vector<float> x(kN, 0.f), db(kN / 2 + 1);
  MagnitudeDb(x.data(), db.data(), g_work);
  bool finite = true;
  for (float v : db) if (!std::isfinite(v) || v < -120.5f) finite = false;
  CHECK(finite);
  uint8_t rows[128];
  Columns(db.data(), 48000.f, 52, rows);
  bool zero = true;
  for (uint8_t r : rows) if (r) zero = false;
  CHECK(zero);
}

static void test_cv_rate_axis() {
  printf("test_cv_rate_axis\n");
  CHECK(ColumnForHz(18750, 37500) == 127);
  CHECK(ColumnForHz(20, 37500) == 0);
  CHECK(ColumnForHz(20000, 48000) == 127);
  CHECK(ColumnForHz(5, 48000) == 0);       // clamped
  CHECK(ColumnForHz(90000, 48000) == 127); // clamped
  const int c1k = ColumnForHz(1000, 48000);
  CHECK(c1k > 60 && c1k < 80);  // 1 kHz sits ~57 % along a 20 Hz..20 kHz log axis
}

int main() {
  test_peak_sine();
  test_ringmod_sidebands();
  test_silence_floor();
  test_cv_rate_axis();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
