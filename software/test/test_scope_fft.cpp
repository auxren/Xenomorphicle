// Host tests for the Scope's FFT, peak readout and log spectrum columns
// (src/ScopeFft.cpp). Inputs are normalised: +-1.0 is full scale.
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// An offset must not reach the spectrum: it comes off before the window for every
// source (a codec's or ADC's offset, not just CV inputs'). Hann leaks a DC of 0.5
// into bins 0 and 1 at about -6 dB unless it is removed.
static void test_dc_removed() {
  printf("test_dc_removed\n");
  const float rate = 48000.f;
  std::vector<float> clean(kN), dc(kN), db_clean(kN / 2 + 1), db_dc(kN / 2 + 1);
  for (int i = 0; i < kN; ++i) {
    clean[i] = 0.1f * sinf(6.2831853f * 1000.f * i / rate);
    dc[i] = clean[i] + 0.5f;
  }
  MagnitudeDb(clean.data(), db_clean.data(), g_work);
  MagnitudeDb(dc.data(), db_dc.data(), g_work);
  float pk_clean = 0, pk_dc = 0;
  const float hz_clean = PeakHz(db_clean.data(), rate, &pk_clean);
  const float hz_dc = PeakHz(db_dc.data(), rate, &pk_dc);
  CHECK(fabsf(hz_dc - hz_clean) < 0.01f && fabsf(hz_dc - 1000.f) < 5.f);
  CHECK(fabsf(pk_dc - pk_clean) < 0.01f);
  for (int k = 0; k < 6; ++k) CHECK(fabsf(db_dc[k] - db_clean[k]) < 0.5f);   // the low bins are the clean ones
  CHECK(db_dc[0] < 0.f - 40.f && db_dc[1] < -40.f);   // nowhere near the -6 dB an uncorrected 0.5 offset leaks
  // a pure offset is silence
  std::vector<float> flat(kN, 0.7f), db_flat(kN / 2 + 1);
  MagnitudeDb(flat.data(), db_flat.data(), g_work);
  for (int k = 0; k <= kN / 2; ++k) CHECK(db_flat[k] < -90.f);
  // a small offset on a low tone still finds the tone above 20 Hz, not bin 0
  std::vector<float> lo(kN), db_lo(kN / 2 + 1);
  for (int i = 0; i < kN; ++i) lo[i] = 0.3f + 0.05f * sinf(6.2831853f * 94.f * i / rate);
  MagnitudeDb(lo.data(), db_lo.data(), g_work);
  CHECK(fabsf(PeakHz(db_lo.data(), rate) - 94.f) < 6.f);
}

static std::vector<float> Tone(double hz, double amp, double rate = 48000.0, double dc = 0.0) {
  std::vector<float> x(kN);
  for (int i = 0; i < kN; ++i) x[i] = (float)(dc + amp * sin(6.283185307179586 * hz * i / rate));
  return x;
}

// Every window reads a tone's amplitude as its amplitude (bin-centred tone), and they
// differ where they should: leakage far from the tone.
static void test_windows() {
  printf("test_windows\n");
  const double rate = 48000.0, bin = rate / kN;
  std::vector<float> db(kN / 2 + 1);
  const Window all[] = {Window::HANN, Window::BLACKMAN_HARRIS, Window::FLATTOP, Window::RECT};
  for (Window w : all) {
    auto x = Tone(64 * bin, 0.5, rate);                       // bin 64, exactly
    MagnitudeDb(x.data(), db.data(), g_work, w);
    float pk = 0;
    const float hz = PeakHz(db.data(), (float)rate, &pk);
    CHECK(fabsf(hz - 64 * (float)bin) < 0.5f);
    CHECK(fabsf(pk - 20.f * log10f(0.5f)) < 0.1f);            // -6.02 dB: amplitude 0.5
  }
  // between two bins: flat-top reads the amplitude to 0.01 dB, Hann loses up to 1.4 dB
  auto mid = Tone(64.5 * bin, 0.5, rate);
  float pk_ft = 0, pk_han = 0;
  MagnitudeDb(mid.data(), db.data(), g_work, Window::FLATTOP);
  PeakHz(db.data(), (float)rate, &pk_ft);
  MagnitudeDb(mid.data(), db.data(), g_work, Window::HANN);
  PeakHz(db.data(), (float)rate, &pk_han);
  CHECK(fabsf(db[64] - 20.f * log10f(0.5f)) < 0.1f || fabsf(db[65] - 20.f * log10f(0.5f)) < 1.6f);
  // leakage 10 bins from an off-bin tone: rect > hann > blackman-harris
  auto off = Tone(100.37 * bin, 0.5, rate);
  float leak[4];
  for (int k = 0; k < 4; ++k) {
    MagnitudeDb(off.data(), db.data(), g_work, all[k]);
    leak[k] = db[110];
    printf("  window %d: %.1f dB 10 bins from the tone\n", k, leak[k]);
  }
  CHECK(leak[3] > leak[0] + 10.f);                          // rect leaks far more than Hann
  CHECK(leak[1] < leak[0] - 10.f);                          // BH far less
  CHECK(leak[2] < leak[0]);
  (void)pk_ft; (void)pk_han;
  // switching windows back and forth gives the same answer each time (table reuse)
  auto x = Tone(64 * bin, 0.5, rate);
  std::vector<float> a(kN / 2 + 1), b(kN / 2 + 1);
  MagnitudeDb(x.data(), a.data(), g_work, Window::HANN);
  MagnitudeDb(x.data(), b.data(), g_work, Window::FLATTOP);
  MagnitudeDb(x.data(), b.data(), g_work, Window::HANN);
  for (int k = 0; k <= kN / 2; ++k) CHECK(a[k] == b[k]);
}

static void test_power_and_averaging() {
  printf("test_power_and_averaging\n");
  std::vector<float> db(kN / 2 + 1), pw(kN / 2 + 1), db2(kN / 2 + 1);
  auto x = Tone(1000.0, 0.3);
  MagnitudeDb(x.data(), db.data(), g_work);
  PowerSpectrum(x.data(), pw.data(), g_work);
  PowerToDb(pw.data(), db2.data());
  for (int k = 0; k <= kN / 2; ++k) CHECK(fabsf(db[k] - db2[k]) < 0.01f || (db[k] < -100.f && db2[k] < -100.f));
  // averaging noise spectra in power: the dB scatter between neighbouring bins shrinks
  srand(5);
  auto scatter = [&](int frames) {
    std::vector<float> avg(kN / 2 + 1, 0.f), p(kN / 2 + 1), d(kN / 2 + 1);
    for (int f = 0; f < frames; ++f) {
      std::vector<float> n(kN);
      for (auto &v : n) v = ((rand() / (float)RAND_MAX) - 0.5f) * 0.2f;
      PowerSpectrum(n.data(), p.data(), g_work);
      for (int k = 0; k <= kN / 2; ++k) avg[k] += (p[k] - avg[k]) / (float)(f + 1);
    }
    PowerToDb(avg.data(), d.data());
    double sum = 0;
    for (int k = 20; k < 480; ++k) sum += fabs(d[k + 1] - d[k]);
    return sum / 460.0;
  };
  const double s1 = scatter(1), s16 = scatter(16);
  printf("  mean |step| between bins: 1 frame %.2f dB, 16 frames %.2f dB\n", s1, s16);
  CHECK(s16 < 0.5 * s1);
}

static void test_columns_low_end() {
  printf("test_columns_low_end\n");
  // 1 kHz tone, 50 Hz hum, an offset and a noise floor: the 24+ columns below ~47 Hz used
  // to be one block at bin 1's level, the next 9 another, then a cliff
  const float rate = 48000.f;
  std::vector<float> x(kN), db(kN / 2 + 1);
  srand(3);
  for (int i = 0; i < kN; ++i)
    x[i] = 0.5f * sinf(6.2831853f * 1000.f * i / rate) + 0.01f + 0.003f * sinf(6.2831853f * 50.f * i / rate) +
           ((rand() / (float)RAND_MAX) - 0.5f) * 2e-4f;
  MagnitudeDb(x.data(), db.data(), g_work);
  uint8_t rows[128];
  Columns(db.data(), rate, 44, rows);
  int biggest_step = 0, equal_run = 0, longest_run = 0;
  for (int c = 1; c < 40; ++c) {
    const int d = abs((int)rows[c] - (int)rows[c - 1]);
    if (d > biggest_step) biggest_step = d;
    equal_run = rows[c] == rows[c - 1] ? equal_run + 1 : 0;
    if (equal_run > longest_run) longest_run = equal_run;
  }
  printf("  columns 0-39:"); for (int c = 0; c < 40; ++c) printf(" %d", rows[c]); printf("\n");
  CHECK(biggest_step <= 3);          // a slope: the old display stepped 24 -> 17 -> 1
  CHECK(longest_run <= 12);          // and no 24-column plateau (the old run was 23)
  // one bin per column or more: the loudest bin still wins
  Columns(db.data(), rate, 44, rows);
  CHECK(rows[ColumnForHz(1000.f, rate)] >= 40);
}

static void test_columns_linear() {
  printf("test_columns_linear\n");
  const float rate = 12000.f;                     // a /4 zoom of 48 kHz
  std::vector<float> db(kN / 2 + 1);
  for (double hz : {300.0, 1000.0, 2500.0, 4700.0}) {
    auto x = Tone(hz, 0.5, rate);
    MagnitudeDb(x.data(), db.data(), g_work);
    uint8_t rows[128];
    ColumnsLinear(db.data(), rate, 4800.f, 44, rows);
    int best = 0;
    for (int c = 1; c < 128; ++c) if (rows[c] > rows[best]) best = c;
    printf("  %.0f Hz: loudest column %d (expected %d)\n", hz, best, ColumnForHzLinear((float)hz, 4800.f));
    CHECK(abs(best - ColumnForHzLinear((float)hz, 4800.f)) <= 1);
    CHECK(rows[best] >= 38);
  }
  // columns narrower than a bin (rate 48 kHz, span 1.5 kHz): interpolated, the peak sits on
  // the tone's own column
  for (int bin : {20, 30}) {
    auto t = Tone(bin * (48000.0 / kN), 0.5, 48000.0);
    MagnitudeDb(t.data(), db.data(), g_work);
    uint8_t rows[128];
    ColumnsLinear(db.data(), 48000.f, 1500.f, 44, rows);
    int best = 0;
    for (int c = 1; c < 128; ++c) if (rows[c] > rows[best]) best = c;
    printf("  bin %d: loudest column %d (axis says %d)\n", bin, best, ColumnForHzLinear(bin * 48000.f / kN, 1500.f));
    CHECK(best == ColumnForHzLinear(bin * 48000.f / kN, 1500.f));
    CHECK(abs((int)rows[best - 3] - (int)rows[best + 3]) <= 1);   // the skirt is symmetric about the tone's column
  }
  CHECK(ColumnForHzLinear(0.f, 4800.f) == 0);
  CHECK(ColumnForHzLinear(4800.f, 4800.f) == 127);
  CHECK(ColumnForHzLinear(9999.f, 4800.f) == 127);
  CHECK(ColumnForHzLinear(-5.f, 4800.f) == 0);
  // PeakHz bounds: a loud tone outside [min, max] is ignored
  auto x = Tone(1000.0, 0.5, rate);
  auto y = Tone(5500.0, 0.9, rate);
  for (int i = 0; i < kN; ++i) x[i] += y[i];
  MagnitudeDb(x.data(), db.data(), g_work);
  CHECK(fabsf(PeakHz(db.data(), rate, nullptr, 20.f, 1e9f) - 5500.f) < 15.f);
  CHECK(fabsf(PeakHz(db.data(), rate, nullptr, 20.f, 4800.f) - 1000.f) < 15.f);
  CHECK(fabsf(PeakHz(db.data(), rate, nullptr, 1500.f, 4800.f) - 1000.f) > 100.f);   // 1 kHz is below the floor
}

// The zoom's geometry, shared by the draw code and the tests.
static void test_zoom_geometry() {
  printf("test_zoom_geometry\n");
  // shown span = 0.8 of the decimated Nyquist = 0.4 of its rate, whatever the source
  CHECK(fabs(ZoomSpanHz(48000.0 / 4) - 4800.0) < 1e-9);
  CHECK(fabs(ZoomSpanHz(48000.0 / 16) - 1200.0) < 1e-9);
  CHECK(fabs(ZoomSpanHz(48000.0 / 64) - 300.0) < 1e-9);
  const double cv = 37350.598;      // the verified CV frame rate
  CHECK(fabs(ZoomSpanHz(cv / 4) - 3735.06) < 0.01);
  CHECK(fabs(ZoomSpanHz(cv / 16) - 933.76) < 0.01);
  CHECK(fabs(ZoomSpanHz(cv / 64) - 233.44) < 0.01);
  CHECK(kPeakMinDb == -70.f);
  // the peak search floor: 20 Hz, or three bins where the bins are finer
  CHECK(fabs(PeakFloorHz(48000.f / 4 / 1024) - 20.f) < 1e-3f);    // 11.7 Hz bins: 20 Hz
  CHECK(fabs(PeakFloorHz(48000.f / 16 / 1024) - 8.789f) < 1e-2f); // 2.93 Hz bins: 3 bins
  CHECK(fabs(PeakFloorHz(48000.f / 64 / 1024) - 2.197f) < 1e-2f);
  // averaging advances once enough fresh samples have arrived, wrap-safe
  CHECK(!AvgDue(1000, 1000) && !AvgDue(1511, 1000) && AvgDue(1512, 1000) && AvgDue(5000, 1000));
  CHECK(AvgDue(300, 0xFFFFFF00u) && !AvgDue(0xFFFFFFF0u, 0xFFFFFF00u));
}

int main() {
  test_peak_sine();
  test_ringmod_sidebands();
  test_silence_floor();
  test_cv_rate_axis();
  test_dc_removed();
  test_windows();
  test_power_and_averaging();
  test_columns_low_end();
  test_columns_linear();
  test_zoom_geometry();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
