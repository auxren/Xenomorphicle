// Host tests for the Scope's period estimate and triggers
// (src/ScopeTrigger.cpp). Signals are synthesised here: the ring-mod case is
// Oren's patch, a folded 259e (f1) multiplied by a 258 sine (f2).
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/ScopeTrigger.h"

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

using namespace ScopeTrigger;

static constexpr int kR = 4096 / 2 + 4;  // scratch for the autocorrelation
static float g_r[kR];

static const double kRate = 48000.0;
static const double kPi = 3.14159265358979323846;

static double Fold(double s) {
  while (std::fabs(s) > 1.0) s = (s > 0 ? 2.0 : -2.0) - s;
  return s;
}

static double RingMod(double t, double f1, double f2, double depth) {
  return Fold(depth * std::sin(2 * kPi * f1 * t)) * std::sin(2 * kPi * f2 * t) * 4000.0;  // mV
}

// Circular distance between two phases modulo p.
static double PhaseDist(double a, double b, double p) {
  double d = std::fmod(std::fabs(a - b), p);
  return d > p / 2 ? p - d : d;
}

static void test_period_sine() {
  printf("test_period_sine\n");
  std::vector<float> x(1536);
  for (size_t i = 0; i < x.size(); ++i) x[i] = (float)std::sin(2 * kPi * 1000 * i / kRate);
  const float p = EstimatePeriod(x.data(), (int)x.size(), 2, 768, g_r, kR);
  CHECK(std::fabs(p - 48.f) <= 0.5f);
  // scratch too small or missing reads as no period, never an overrun
  CHECK(EstimatePeriod(x.data(), (int)x.size(), 2, 768, g_r, 100) == 0.f);
  CHECK(EstimatePeriod(x.data(), (int)x.size(), 2, 768, nullptr, 0) == 0.f);
}

static void test_period_ringmod_full_repeat() {
  printf("test_period_ringmod_full_repeat\n");
  std::vector<float> x(1536);
  for (size_t i = 0; i < x.size(); ++i) x[i] = (float)RingMod(i / kRate, 220, 770, 1.8);
  const float p = EstimatePeriod(x.data(), (int)x.size(), 2, 768, g_r, kR);
  printf("  ringmod period %.2f (expect 436.36)\n", p);
  CHECK(std::fabs(p - 436.36f) <= 2.f);
}

static void test_clipped_square_locks() {
  printf("test_clipped_square_locks\n");
  std::vector<float> x(1536);
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = std::sin(2 * kPi * 500 * (i + 0.5) / kRate) >= 0 ? 32767.f : -32767.f;
  const float p = EstimatePeriod(x.data(), (int)x.size(), 2, 768, g_r, kR);
  CHECK(std::fabs(p - 96.f) <= 1.f);
  AutoTrigger a;
  a.Reset();
  const int s = a.Find(x.data(), (int)x.size(), 384, g_r, kR);
  CHECK(a.Locked());
  CHECK(s >= 1 && s + 384 <= (int)x.size());
  CHECK(x[s] > 0 && x[s - 1] < 0);  // starts on the rising edge
}

static void test_no_period_on_flat() {
  printf("test_no_period_on_flat\n");
  std::vector<float> x(1536, 2500.f);
  CHECK(EstimatePeriod(x.data(), (int)x.size(), 2, 768, g_r, kR) == 0.f);
  AutoTrigger a;
  a.Reset();
  CHECK(a.Find(x.data(), (int)x.size(), 384, g_r, kR) == (int)x.size() - 384);
  CHECK(!a.Locked());
  CHECK(a.Period() == 0.f);
  // a tiny wiggle below min_pp is flat too
  for (size_t i = 0; i < x.size(); ++i) x[i] = 2500.f + (float)std::sin(i * 0.3) * 0.1f;
  CHECK(a.Find(x.data(), (int)x.size(), 384, g_r, kR, 5.f) == (int)x.size() - 384);
  CHECK(!a.Locked());
}

// Frame k sees n samples of signal starting at time k * advance samples.
template <typename F>
static void Frame(std::vector<float> &x, double start, F sig) {
  for (size_t i = 0; i < x.size(); ++i) x[i] = (float)sig((start + i) / kRate);
}

static void test_jitter_steady_ringmod() {
  printf("test_jitter_steady_ringmod\n");
  const double P = kRate / 110.0;
  const double adv = 1234.56;
  std::vector<float> x(1536);
  AutoTrigger a;
  a.Reset();
  double first = -1, worst = 0;
  int locked = 0;
  for (int k = 0; k < 50; ++k) {
    Frame(x, k * adv, [](double t) { return RingMod(t, 220, 770, 1.8); });
    const int s = a.Find(x.data(), (int)x.size(), 384, g_r, kR);
    if (!a.Locked()) continue;
    ++locked;
    const double ph = std::fmod(k * adv + s, P);
    if (first < 0) first = ph;
    worst = std::fmax(worst, PhaseDist(ph, first, P));
  }
  printf("  locked %d/50, worst phase drift %.2f samples\n", locked, worst);
  CHECK(locked == 50);
  CHECK(worst <= 1.0);
}

static void test_morph_stays_on_feature() {
  printf("test_morph_stays_on_feature\n");
  const double P = kRate / 110.0;
  const double adv = 1234.56;
  std::vector<float> x(1536);
  AutoTrigger a;
  a.Reset();
  double prev = -1, worst = 0;
  int locked = 0;
  for (int k = 0; k < 50; ++k) {
    const double depth = 1.2 + 1.2 * k / 49.0;
    Frame(x, k * adv, [depth](double t) { return RingMod(t, 220, 770, depth); });
    const int s = a.Find(x.data(), (int)x.size(), 384, g_r, kR);
    if (!a.Locked()) continue;
    ++locked;
    const double ph = std::fmod(k * adv + s, P);
    if (prev >= 0) worst = std::fmax(worst, PhaseDist(ph, prev, P));
    prev = ph;
  }
  printf("  locked %d/50, worst frame-to-frame jump %.2f samples (limit %.2f)\n", locked, worst,
         0.05 * P);
  CHECK(locked == 50);
  CHECK(worst <= 0.05 * P);
}

static void test_period_change_relocks() {
  printf("test_period_change_relocks\n");
  std::vector<float> x(1536);
  AutoTrigger a;
  a.Reset();
  for (int k = 0; k < 10; ++k) {
    Frame(x, k * 1234.56, [](double t) { return RingMod(t, 220, 770, 1.8); });
    a.Find(x.data(), (int)x.size(), 384, g_r, kR);
  }
  CHECK(std::fabs(a.Period() - 436.36f) <= 4.4f);
  bool ok = false;
  for (int k = 10; k < 13; ++k) {  // period 48000/165 = 290.9
    Frame(x, k * 1234.56, [](double t) { return RingMod(t, 330, 1155, 1.8); });
    a.Find(x.data(), (int)x.size(), 384, g_r, kR);
    if (a.Locked() && std::fabs(a.Period() - 290.91f) <= 2.91f) ok = true;
  }
  CHECK(ok);
}

static void test_level_trigger() {
  printf("test_level_trigger\n");
  std::vector<float> x(1536);
  for (size_t i = 0; i < x.size(); ++i) x[i] = (float)std::sin(2 * kPi * 1000 * i / kRate + 0.3);
  const int s = LevelTrigger(x.data(), (int)x.size(), 384, 0.f, 0.05f, true);
  CHECK(s > 0 && s + 384 <= (int)x.size());
  CHECK(x[s] >= 0.f && x[s] < 0.14f);
  CHECK(x[s + 1] > x[s]);
  CHECK(x[s - 1] < 0.f);
  // the latest qualifying crossing: no rising crossing later that still fits
  for (int i = s + 1; i + 384 <= (int)x.size(); ++i) CHECK(!(x[i - 1] < 0.f && x[i] >= 0.f));
  const int f = LevelTrigger(x.data(), (int)x.size(), 384, 0.f, 0.05f, false);
  CHECK(f > 0 && x[f] <= 0.f && x[f - 1] > 0.f);
  CHECK(LevelTrigger(x.data(), (int)x.size(), 384, 2.f, 0.05f, true) == -1);
  CHECK(LevelTrigger(x.data(), 100, 384, 0.f, 0.05f, true) == -1);  // window longer than data
}

static void test_edge_period() {
  printf("test_edge_period\n");
  // a 258 sync at 110 Hz in microseconds, with jitter and one missed edge
  uint32_t e[12];
  uint32_t t = 4000000000u;  // wraps during the run
  int n = 0;
  for (int k = 0; k < 13; ++k) {
    t += 9091 + (k % 3) - 1;
    if (k == 6) continue;  // missed
    e[n++] = t;
  }
  const float p = EdgePeriod(e, n);
  CHECK(std::fabs(p - 9091.f) <= 2.f);
  CHECK(EdgePeriod(e, 1) == 0.f);
  CHECK(EdgePeriod(e, 0) == 0.f);
}

int main() {
  test_edge_period();
  test_period_sine();
  test_period_ringmod_full_repeat();
  test_clipped_square_locks();
  test_no_period_on_flat();
  test_jitter_steady_ringmod();
  test_morph_stays_on_feature();
  test_period_change_relocks();
  test_level_trigger();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
