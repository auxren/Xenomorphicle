// Host tests for the Scope's timebase/volts tables and damped auto-ranging
// (src/ScopeRange.cpp).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/ScopeRange.h"

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

using namespace ScopeRange;

static void test_tables_and_decim() {
  printf("test_tables_and_decim\n");
  CHECK(kTimeDivUs[3] == 1000);
  CHECK(kTimeDivUs[kTimeCount - 1] == 2000000);
  CHECK(kVoltDivMv[6] == 1000);
  CHECK(DecimFor(3, 48000) == 3);
  CHECK(DecimFor(0, 48000) == 1);
  CHECK(ColsPerBucket(0, 48000) > 1.f);
  CHECK(ColsPerBucket(3, 48000) == 1.f);
  CHECK(DecimFor(13, 37500) == 4688);
  CHECK(DecimFor(-1, 48000) == 1);  // clamped
  char b[8];
  FormatTime(3, b, sizeof(b));
  CHECK(strcmp(b, "1ms") == 0);
  FormatTime(0, b, sizeof(b));
  CHECK(strcmp(b, "100us") == 0);
  FormatTime(13, b, sizeof(b));
  CHECK(strcmp(b, "2s") == 0);
  FormatVolt(6, b, sizeof(b));
  CHECK(strcmp(b, "1V") == 0);
  FormatVolt(0, b, sizeof(b));
  CHECK(strcmp(b, "10mV") == 0);
  CHECK(kDefaultTimeIdx == 3 && kDefaultVoltIdx == 6);
  CHECK(ClampTime(99) == kTimeCount - 1 && ClampVolt(-3) == 0);
}

static void test_autotime_walks_and_settles() {
  printf("test_autotime_walks_and_settles\n");
  AutoTime a;
  a.Reset(0);
  int changes = 0, last = a.Idx(), reversals = 0, dir = 0;
  uint32_t last_change = 0;
  bool spaced = true;
  for (uint32_t t = 0; t <= 30000; t += 50) {
    a.Update(0.5f, t);
    if (a.Idx() != last) {
      const int d = a.Idx() > last ? 1 : -1;
      if (dir && d != dir) ++reversals;
      dir = d;
      if (changes && t - last_change < 1000) spaced = false;
      last_change = t;
      ++changes;
      last = a.Idx();
    }
  }
  CHECK(a.Idx() == 10);  // 200 ms/div: 1.6 s on screen >= 2.5 x 0.5 s
  CHECK(changes == 10);
  CHECK(reversals == 0);
  CHECK(spaced);

  // a 0.1 Hz LFO: slowest division
  a.Reset(3);
  for (uint32_t t = 0; t <= 30000; t += 50) a.Update(10.f, t);
  CHECK(a.Idx() == kTimeCount - 1);
}

static void test_autotime_no_flap() {
  printf("test_autotime_no_flap\n");
  AutoTime a;
  a.Reset(5);  // 5 ms/div, 40 ms on screen
  for (uint32_t t = 0; t <= 10000; t += 50) a.Update((t / 50) % 2 ? 0.015f : 0.017f, t);
  CHECK(a.Idx() == 5);
}

static void test_autotime_no_period_slows() {
  printf("test_autotime_no_period_slows\n");
  AutoTime a;
  a.Reset(0);
  for (uint32_t t = 0; t <= 20000; t += 50) a.Update(0.f, t);
  CHECK(a.Idx() == 10);  // walks no further than 200 ms/div without a period
}

static void test_autovolt_fast_attack_slow_release() {
  printf("test_autovolt_fast_attack_slow_release\n");
  AutoVolt v;
  v.Reset(6);
  uint32_t t = 0;
  for (; t < 3000; t += 50) v.Update(-1000, 1000, t);
  const int steady = v.Idx();
  CHECK(kVoltDivMv[steady] * 8 >= 2500);  // 1.25 x 2000 mV fits
  CHECK(steady == 5);                     // 500 mV/div is the smallest that fits
  t += 50;
  v.Update(-4000, 4000, t);  // would clip: coarsen on the very next update
  CHECK(v.Idx() > steady);
  CHECK(kVoltDivMv[v.Idx()] * 8 >= 8000);
  const int big = v.Idx();
  const uint32_t back = t + 50;
  for (t = back; t < back + 950; t += 50) {
    v.Update(-1000, 1000, t);
    CHECK(v.Idx() == big);  // no release inside a second
  }
  for (; t < back + 3000; t += 50) v.Update(-1000, 1000, t);
  CHECK(v.Idx() == steady);
}

static void test_flat_input_holds() {
  printf("test_flat_input_holds\n");
  AutoVolt v;
  v.Reset(6);
  for (uint32_t t = 0; t < 2000; t += 50) v.Update(4000, 6000, t);
  const int idx = v.Idx();
  const int32_t c = v.CenterMv();
  CHECK(c == 5000);
  for (uint32_t t = 2000; t < 12000; t += 50) v.Update(2500, 2500, t);  // flat
  CHECK(v.Idx() == idx);
  CHECK(v.CenterMv() == c);
}

static void test_at_most_once_per_second() {
  printf("test_at_most_once_per_second\n");
  AutoVolt v;
  v.Reset(6);
  AutoTime a;
  a.Reset(3);
  srand(1234);
  double amp = 1000, per = 0.01;
  int vl = v.Idx(), al = a.Idx();
  uint32_t vlast = 0, alast = 0;
  bool vok = true, aok = true;
  bool vfirst = true, afirst = true;
  for (uint32_t t = 0; t <= 60000; t += 20) {
    amp *= 1.0 + ((rand() % 2001) - 1000) / 40000.0;  // slow random walk
    if (amp < 20) amp = 20;
    if (amp > 4000) amp = 4000;
    per *= 1.0 + ((rand() % 2001) - 1000) / 40000.0;
    if (per < 0.0005) per = 0.0005;
    if (per > 0.5) per = 0.5;
    v.Update((int32_t)-amp, (int32_t)amp, t);
    a.Update((float)per, t);
    if (v.Idx() != vl) {
      // a fast attack (coarser) may follow at once; a release may not
      if (!vfirst && v.Idx() < vl && t - vlast < 1000) vok = false;
      vfirst = false;
      vlast = t;
      vl = v.Idx();
    }
    if (a.Idx() != al) {
      if (!afirst && t - alast < 1000) aok = false;
      afirst = false;
      alast = t;
      al = a.Idx();
    }
  }
  CHECK(vok);
  CHECK(aok);
}

static void test_center_follows_offset() {
  printf("test_center_follows_offset\n");
  AutoVolt v;
  v.Reset(6);
  for (uint32_t t = 0; t < 3000; t += 50) v.Update(4900, 5100, t);  // 5 V +- 100 mV
  CHECK(std::abs(v.CenterMv() - 5000) <= kVoltDivMv[v.Idx()]);
  CHECK(kVoltDivMv[v.Idx()] * 8 >= 250);
}

int main() {
  test_tables_and_decim();
  test_autotime_walks_and_settles();
  test_autotime_no_flap();
  test_autotime_no_period_slows();
  test_autovolt_fast_attack_slow_release();
  test_flat_input_holds();
  test_at_most_once_per_second();
  test_center_follows_offset();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
