// Host tests for the compile-time crossfade tables (src/Audio/XfadeTable.h),
// which replaced two per-instance `new float[2048]` in AudioDelayExtF32 and
// AudioTweightyF32 (16 KB of RAM2 heap each).
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 test_xfade_table.cpp
#include <cmath>
#include <cstdio>

#include "../src/Audio/XfadeTable.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { if (++fails <= 10) printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// The point of the change: these are evaluated by the compiler. If Build() or
// EqualPowerFade() stops being constexpr, this stops compiling, and the table
// would silently have to be filled (and allocated) at run time again.
static_assert(XfadeLut::kTable<2048>.out[0] == 1.0f, "out starts at 1");
static_assert(XfadeLut::kTable<2048>.in[0] == 0.0f, "in starts at 0");
static_assert(XfadeLut::kTable<2048>.in[2047] == 1.0f, "in ends at 1");
static_assert(XfadeLut::kTable<2048>.out[2047] == 0.0f, "out ends at 0");
// One table is exactly two arrays of N floats: 16 KB at the shipped N.
static_assert(sizeof(XfadeLut::Table<2048>) == 2 * 2048 * sizeof(float), "layout");

template <size_t N>
static void check_matches_runtime_fill() {
  // What the old Acquire() loop computed, at run time, entry by entry.
  const auto &t = XfadeLut::kTable<N>;
  const float n = static_cast<float>(N - 1);
  for (size_t i = 0; i < N; i++) {
    float out, in;
    EqualPowerFade(out, in, i / n);
    CHECK(std::fabs(t.out[i] - out) <= 1e-6f);
    CHECK(std::fabs(t.in[i] - in) <= 1e-6f);
  }
}

template <size_t N>
static void check_shape() {
  const auto &t = XfadeLut::kTable<N>;
  // The curve overshoots 1.0 slightly near the ends (about 0.03 %), so it is
  // bounded rather than strictly monotonic; it must stay in a tight band and
  // rise/fall overall.
  for (size_t i = 0; i < N; i++) {
    CHECK(t.in[i] >= 0.0f && t.in[i] <= 1.01f);
    CHECK(t.out[i] >= 0.0f && t.out[i] <= 1.01f);
  }
  CHECK(t.in[N / 2] > t.in[N / 4] && t.in[N / 4] > t.in[1]);
  CHECK(t.out[N / 2] < t.out[N / 4] && t.out[N / 4] < t.out[1]);
  // Equal power: in^2 + out^2 stays near 1 across the whole fade (no dip or
  // bump in loudness at the middle of a tap change).
  for (size_t i = 0; i < N; i++) {
    const float p = t.in[i] * t.in[i] + t.out[i] * t.out[i];
    CHECK(p > 0.7f && p < 1.3f);
  }
  // Mirror: out is in read backwards (the curve is symmetric in t).
  for (size_t i = 0; i < N; i++) CHECK(std::fabs(t.out[i] - t.in[N - 1 - i]) <= 2e-6f);
}

// Golden values: pin the curve itself (EqualPowerFade's 1.4186 constant and the
// table index mapping), not just its shape. Changing the constant to 1.5 moves
// these by about 3e-3; the old run-time fill differs from the compile-time
// values by at most 2.4e-7.
static void check_golden() {
  const auto &t = XfadeLut::kTable<2048>;
  CHECK(std::fabs(t.in[512] - 0.237742364f) <= 1e-5f);
  CHECK(std::fabs(t.out[512] - 0.974848211f) <= 1e-5f);
  CHECK(std::fabs(t.in[1024] - 0.703764379f) <= 1e-5f);
  CHECK(std::fabs(t.out[1024] - 0.702944934f) <= 1e-5f);
  CHECK(std::fabs(t.in[1500] - 0.966262817f) <= 1e-5f);
  CHECK(std::fabs(t.out[1500] - 0.267731041f) <= 1e-5f);
}

int main() {
  check_golden();
  check_matches_runtime_fill<2048>();
  check_matches_runtime_fill<16>();
  check_shape<2048>();
  check_shape<16>();
  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
