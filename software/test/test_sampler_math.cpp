// Host tests for the Sampler app's pure logic (src/SamplerMath.cpp). No
// hardware, no Audio.h.
//
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_sampler_math
//   test_sampler_math.cpp ../src/SamplerMath.cpp &&
//   ./build/test_sampler_math
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/SamplerMath.h"

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

using namespace SamplerMath;

static void test_slot_count_is_8() {
  printf("test_slot_count_is_8\n");
  CHECK(kSlotCount == 8);
}

static void test_wrap_slot_index() {
  printf("test_wrap_slot_index\n");
  CHECK(WrapSlotIndex(0) == 0);
  CHECK(WrapSlotIndex(7) == 7);
  CHECK(WrapSlotIndex(8) == 0);
  CHECK(WrapSlotIndex(-1) == 7);
  CHECK(WrapSlotIndex(-8) == 0);
  CHECK(WrapSlotIndex(9) == 1);
}

static void test_clamp_file_num() {
  printf("test_clamp_file_num\n");
  CHECK(ClampFileNum(-1) == 0);
  CHECK(ClampFileNum(0) == 0);
  CHECK(ClampFileNum(999) == 999);
  CHECK(ClampFileNum(1000) == 999);
  CHECK(ClampFileNum(50) == 50);
}

static void test_clamp_rate_percent() {
  printf("test_clamp_rate_percent\n");
  CHECK(ClampRatePercent(0) == kMinRatePercent);
  CHECK(ClampRatePercent(kMinRatePercent) == kMinRatePercent);
  CHECK(ClampRatePercent(kMaxRatePercent) == kMaxRatePercent);
  CHECK(ClampRatePercent(9999) == kMaxRatePercent);
  CHECK(ClampRatePercent(100) == 100);
}

static void test_clamp_trigger_mode() {
  printf("test_clamp_trigger_mode\n");
  CHECK(ClampTriggerMode(-1) == TRIG_ONE_SHOT);
  CHECK(ClampTriggerMode(0) == TRIG_ONE_SHOT);
  CHECK(ClampTriggerMode(1) == TRIG_GATE_SUSTAIN);
  CHECK(ClampTriggerMode(2) == TRIG_GATE_SUSTAIN);  // clamps, does not wrap
  CHECK(ClampTriggerMode(100) == TRIG_GATE_SUSTAIN);
}

static void test_build_filename() {
  printf("test_build_filename\n");
  char buf[kFilenameBufLen];
  BuildFilename(0, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "000.WAV") == 0);
  BuildFilename(7, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "007.WAV") == 0);
  BuildFilename(42, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "042.WAV") == 0);
  BuildFilename(999, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "999.WAV") == 0);
  BuildFilename(1000, buf, sizeof(buf));  // clamps to 999 first
  CHECK(std::strcmp(buf, "999.WAV") == 0);
  BuildFilename(-5, buf, sizeof(buf));  // clamps to 0 first
  CHECK(std::strcmp(buf, "000.WAV") == 0);
}

static void test_build_filename_short_buffer_does_not_overrun() {
  printf("test_build_filename_short_buffer_does_not_overrun\n");
  char buf[4] = {'X', 'X', 'X', 'X'};
  BuildFilename(5, buf, sizeof(buf));
  CHECK(buf[0] == '\0');
  // zero-length buffer: must not touch buf at all
  BuildFilename(5, nullptr, 0);  // should not crash
}

static void test_update_gate_rising_and_falling() {
  printf("test_update_gate_rising_and_falling\n");
  bool prev = false;
  // below threshold -> no edge, stays low
  CHECK(UpdateGate(0, prev) == EDGE_NONE);
  CHECK(prev == false);
  // crosses above threshold -> rising edge
  CHECK(UpdateGate(kGateThresholdRaw + 1, prev) == EDGE_RISING);
  CHECK(prev == true);
  // stays high -> no further edge
  CHECK(UpdateGate(kCvFullScale, prev) == EDGE_NONE);
  CHECK(prev == true);
  // drops below threshold -> falling edge
  CHECK(UpdateGate(0, prev) == EDGE_FALLING);
  CHECK(prev == false);
  // stays low -> no further edge
  CHECK(UpdateGate(-kCvFullScale, prev) == EDGE_NONE);
  CHECK(prev == false);
}

static void test_update_gate_threshold_is_exclusive() {
  printf("test_update_gate_threshold_is_exclusive\n");
  bool prev = false;
  // exactly at threshold does NOT count as high (raw > threshold, not >=)
  CHECK(UpdateGate(kGateThresholdRaw, prev) == EDGE_NONE);
  CHECK(prev == false);
}

static void test_update_gate_negative_raw_is_low() {
  printf("test_update_gate_negative_raw_is_low\n");
  bool prev = true;
  CHECK(UpdateGate(-100, prev) == EDGE_FALLING);
  CHECK(prev == false);
}

static void test_compute_rate_multiplier_zero_cv_is_base_only() {
  printf("test_compute_rate_multiplier_zero_cv_is_base_only\n");
  CHECK(NearlyEqual(ComputeRateMultiplier(100, 0), 1.0f));
  CHECK(NearlyEqual(ComputeRateMultiplier(50, 0), 0.5f));
  CHECK(NearlyEqual(ComputeRateMultiplier(200, 0), 2.0f));
}

static void test_compute_rate_multiplier_positive_cv_speeds_up() {
  printf("test_compute_rate_multiplier_positive_cv_speeds_up\n");
  // full-scale positive CV at kPitchCvDepth==1.0 doubles the base rate
  const float r = ComputeRateMultiplier(100, kCvFullScale);
  CHECK(NearlyEqual(r, 2.0f));
}

static void test_compute_rate_multiplier_negative_cv_slows_down() {
  printf("test_compute_rate_multiplier_negative_cv_slows_down\n");
  // full-scale negative CV would drive the multiplier to 0.0, clamped up to
  // kMinRateMult instead of stopping/reversing playback.
  const float r = ComputeRateMultiplier(100, -kCvFullScale);
  CHECK(NearlyEqual(r, kMinRateMult));
}

static void test_compute_rate_multiplier_clamps_to_absolute_range() {
  printf("test_compute_rate_multiplier_clamps_to_absolute_range\n");
  // base rate already at its own ceiling (400%) plus full positive CV would
  // be 8.0x -- must clamp to kMaxRateMult.
  const float r_hi = ComputeRateMultiplier(kMaxRatePercent, kCvFullScale);
  CHECK(NearlyEqual(r_hi, kMaxRateMult));
  // base rate at its own floor (25%) plus full negative CV would be 0.0x --
  // must clamp to kMinRateMult, never zero or negative (no reverse playback).
  const float r_lo = ComputeRateMultiplier(kMinRatePercent, -kCvFullScale);
  CHECK(r_lo >= kMinRateMult);
  CHECK(r_lo > 0.0f);
}

static void test_compute_rate_multiplier_clamps_out_of_range_cv() {
  printf("test_compute_rate_multiplier_clamps_out_of_range_cv\n");
  // cv beyond full-scale (should not happen from real hardware, but must not
  // blow past the +/-1.0 normalization or the absolute clamp).
  const float r_over = ComputeRateMultiplier(100, kCvFullScale * 10);
  CHECK(NearlyEqual(r_over, 2.0f));
  const float r_under = ComputeRateMultiplier(100, -kCvFullScale * 10);
  CHECK(NearlyEqual(r_under, kMinRateMult));
}

int main() {
  test_slot_count_is_8();
  test_wrap_slot_index();
  test_clamp_file_num();
  test_clamp_rate_percent();
  test_clamp_trigger_mode();
  test_build_filename();
  test_build_filename_short_buffer_does_not_overrun();
  test_update_gate_rising_and_falling();
  test_update_gate_threshold_is_exclusive();
  test_update_gate_negative_raw_is_low();
  test_compute_rate_multiplier_zero_cv_is_base_only();
  test_compute_rate_multiplier_positive_cv_speeds_up();
  test_compute_rate_multiplier_negative_cv_slows_down();
  test_compute_rate_multiplier_clamps_to_absolute_range();
  test_compute_rate_multiplier_clamps_out_of_range_cv();

  printf("\ntest_sampler_math: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
