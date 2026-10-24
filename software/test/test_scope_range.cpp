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

static void test_autotime_no_period_dir() {
  printf("test_autotime_no_period_dir\n");
  AutoTime a;
  a.Reset(0);  // no period, signal slower than the screen: walk to the slowest
  for (uint32_t t = 0; t <= 20000; t += 50) a.Update(0.f, t, +1);
  CHECK(a.Idx() == kTimeCount - 1);
  a.Reset(kTimeCount - 1);  // buckets dense: the signal is far faster, walk faster
  for (uint32_t t = 0; t <= 20000; t += 50) a.Update(0.f, t, -1);
  CHECK(a.Idx() == 0);
  a.Reset(7);  // flat: nothing changes
  for (uint32_t t = 0; t <= 20000; t += 50) a.Update(0.f, t, 0);
  CHECK(a.Idx() == 7);
}

static void test_no_period_dir() {
  printf("test_no_period_dir\n");
  float lo[128], hi[128];
  for (int i = 0; i < 128; ++i) { lo[i] = -1000; hi[i] = 1000; }  // a fast tone, every bucket full range
  CHECK(NoPeriodDir(lo, hi, 128, 5.f) == -1);
  for (int i = 0; i < 128; ++i) { lo[i] = hi[i] = 10.f * i; }      // a slow ramp
  CHECK(NoPeriodDir(lo, hi, 128, 5.f) == +1);
  for (int i = 0; i < 128; ++i) { lo[i] = 2500.f; hi[i] = 2501.f; } // flat
  CHECK(NoPeriodDir(lo, hi, 128, 5.f) == 0);
  CHECK(NoPeriodDir(lo, hi, 0, 5.f) == 0);
}

static void test_column_to_bucket_by_time() {
  printf("test_column_to_bucket_by_time\n");
  // 500 us/div at 48 kHz: a column is 31.25 us, but a bucket is 2 samples = 41.67 us
  const float col = ColSeconds(2), bucket = DecimFor(2, 48000) / 48000.f;
  CHECK(std::fabs(col - 31.25e-6f) < 1e-9f);
  CHECK(DecimFor(2, 48000) == 2);
  CHECK(BucketForColumn(0, col, bucket) == 0);
  CHECK(BucketForColumn(127, col, bucket) == 95);
  CHECK(WindowBuckets(col, bucket) == 96);  // the screen spans 96 buckets, not 128
  // every column lands within one bucket of its true time
  for (int x = 0; x < 128; ++x) {
    const float t = x * col, tb = BucketForColumn(x, col, bucket) * bucket;
    CHECK(t - tb >= -1e-9f && t - tb < bucket);
  }
  // 100 us/div audio: one sample spans 3.3 columns
  CHECK(BucketForColumn(4, ColSeconds(0), 1 / 48000.f) == 1);
  CHECK(WindowBuckets(ColSeconds(0), 1 / 48000.f) == 39);
  // CV at 1 ms/div: decim 2 at 37.5 kHz is 53.3 us, columns 62.5 us
  CHECK(WindowBuckets(ColSeconds(3), DecimFor(3, 37500) / 37500.f) == 149);
}

static void test_align_bucket() {
  printf("test_align_bucket\n");
  CHECK(AlignBucket(1000, 0.001f, 0.f) == 999);
  CHECK(AlignBucket(1000, 0.001f, 0.0104f) == 989);
  CHECK(AlignBucket(1000, 0.001f, -0.002f) == 1001);  // ahead of this ring's newest
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
  test_autotime_no_period_dir();
  test_no_period_dir();
  test_column_to_bucket_by_time();
  test_align_bucket();
  test_autovolt_fast_attack_slow_release();
  test_flat_input_holds();
  test_at_most_once_per_second();
  test_center_follows_offset();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
