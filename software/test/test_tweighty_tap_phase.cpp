// Host tests for the Tweighty app's 8-tap phase-ring math
// (src/TweightyTapPhase.cpp). No hardware, no audio engine.
//
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_tweighty_tap_phase
//   test_tweighty_tap_phase.cpp ../src/TweightyTapPhase.cpp &&
//   ./build/test_tweighty_tap_phase
#include <cassert>
#include <cmath>
#include <cstdio>

#include "../src/TweightyTapPhase.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                           \
  } while (0)

static bool NearlyEqual(float a, float b, float eps = 1e-4f) {
  return std::fabs(a - b) <= eps;
}

static void test_phase_fullscale_is_pinned() {
  printf("test_phase_fullscale_is_pinned\n");
  // The default preset and every caller's phase math are built on this
  // constant; pin it so a change to it is a deliberate edit, not a slip.
  CHECK(NearlyEqual(PHASE_FULLSCALE, 160.0f));
  CHECK(kTweightyTapCount == 8);
}

static void test_default_preset_is_evenly_spaced_eighths() {
  printf("test_default_preset_is_evenly_spaced_eighths\n");
  TweightyTapPhaseTable table;
  TweightyTapPhaseDefault(table);
  for (int i = 0; i < kTweightyTapCount; ++i) {
    CHECK(NearlyEqual(table.phase[i], 20.0f * static_cast<float>(i + 1)));
  }
  // The last tap sits exactly at full scale -- the full loop length.
  CHECK(NearlyEqual(table.phase[kTweightyTapCount - 1], PHASE_FULLSCALE));
}

static void test_default_preset_needs_no_clamping() {
  printf("test_default_preset_needs_no_clamping\n");
  TweightyTapPhaseTable table;
  TweightyTapPhaseDefault(table);
  CHECK(!TweightyTapPhaseValidate(table));
  // Validate must not have moved anything on a clean table.
  for (int i = 0; i < kTweightyTapCount; ++i) {
    CHECK(NearlyEqual(table.phase[i], 20.0f * static_cast<float>(i + 1)));
  }
}

static void test_validate_clamps_out_of_range_entries() {
  printf("test_validate_clamps_out_of_range_entries\n");
  TweightyTapPhaseTable table;
  TweightyTapPhaseDefault(table);
  table.phase[0] = -50.0f;
  table.phase[3] = 400.0f;
  table.phase[7] = PHASE_FULLSCALE;   // already in range: not a clamp case
  CHECK(TweightyTapPhaseValidate(table));
  CHECK(NearlyEqual(table.phase[0], 0.0f));
  CHECK(NearlyEqual(table.phase[3], PHASE_FULLSCALE));
  CHECK(NearlyEqual(table.phase[7], PHASE_FULLSCALE));
}

static void test_target_secs_at_the_ring_extremes() {
  printf("test_target_secs_at_the_ring_extremes\n");
  CHECK(NearlyEqual(TweightyTapTargetSecs(2.0f, 0.0f, 1.0f), 0.0f));
  CHECK(NearlyEqual(TweightyTapTargetSecs(2.0f, PHASE_FULLSCALE, 1.0f), 2.0f));
  CHECK(NearlyEqual(TweightyTapTargetSecs(2.0f, 80.0f, 1.0f), 1.0f));
}

static void test_target_secs_scales_with_time_mult() {
  printf("test_target_secs_scales_with_time_mult\n");
  CHECK(NearlyEqual(TweightyTapTargetSecs(1.0f, PHASE_FULLSCALE, 0.5f), 0.5f));
  CHECK(NearlyEqual(TweightyTapTargetSecs(1.0f, PHASE_FULLSCALE, 2.0f), 2.0f));
  CHECK(NearlyEqual(TweightyTapTargetSecs(1.0f, PHASE_FULLSCALE, 0.0f), 0.0f));
}

static void test_target_secs_clamps_out_of_range_inputs() {
  printf("test_target_secs_clamps_out_of_range_inputs\n");
  // A negative base (should never happen, but a caller passing raw CV math
  // must not produce a negative delay time) reads as zero.
  CHECK(NearlyEqual(TweightyTapTargetSecs(-1.0f, 80.0f, 1.0f), 0.0f));
  // An out-of-range phase clamps to the same result as the nearest bound,
  // rather than extrapolating past the loop.
  CHECK(NearlyEqual(TweightyTapTargetSecs(2.0f, 400.0f, 1.0f),
                     TweightyTapTargetSecs(2.0f, PHASE_FULLSCALE, 1.0f)));
  CHECK(NearlyEqual(TweightyTapTargetSecs(2.0f, -40.0f, 1.0f),
                     TweightyTapTargetSecs(2.0f, 0.0f, 1.0f)));
}

static void test_default_taps_are_monotonic_for_a_fixed_base() {
  printf("test_default_taps_are_monotonic_for_a_fixed_base\n");
  TweightyTapPhaseTable table;
  TweightyTapPhaseDefault(table);
  float prev = -1.0f;
  for (int i = 0; i < kTweightyTapCount; ++i) {
    const float secs = TweightyTapTargetSecs(3.0f, table.phase[i], 1.0f);
    CHECK(secs > prev);
    prev = secs;
  }
  // And the last tap reaches exactly the base delay.
  CHECK(NearlyEqual(prev, 3.0f));
}

int main() {
  test_phase_fullscale_is_pinned();
  test_default_preset_is_evenly_spaced_eighths();
  test_default_preset_needs_no_clamping();
  test_validate_clamps_out_of_range_entries();
  test_target_secs_at_the_ring_extremes();
  test_target_secs_scales_with_time_mult();
  test_target_secs_clamps_out_of_range_inputs();
  test_default_taps_are_monotonic_for_a_fixed_base();

  printf("\ntest_tweighty_tap_phase: %d checks, %d failures\n", checks,
         failures);
  return failures == 0 ? 0 : 1;
}
