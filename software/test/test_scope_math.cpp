// Host tests for the Scope app's pure logic (src/ScopeMath.cpp). No
// hardware, no Audio.h.
//
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_scope_math
//   test_scope_math.cpp ../src/ScopeMath.cpp &&
//   ./build/test_scope_math
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/ScopeMath.h"

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

using namespace ScopeMath;

static void test_channel_count_is_20() {
  printf("test_channel_count_is_20\n");
  CHECK(kChannelCount == 20);
  CHECK(kCvChannelCount == 16);
  CHECK(kCvInCount == 8);
  CHECK(kCvOutCount == 8);
  CHECK(kAudioChannelCount == 4);
}

static void test_channel_kind_boundaries() {
  printf("test_channel_kind_boundaries\n");
  CHECK(ChannelKindOf(0) == KIND_CV_IN);
  CHECK(ChannelKindOf(7) == KIND_CV_IN);
  CHECK(ChannelKindOf(8) == KIND_CV_OUT);
  CHECK(ChannelKindOf(15) == KIND_CV_OUT);
  CHECK(ChannelKindOf(16) == KIND_AUDIO);
  CHECK(ChannelKindOf(19) == KIND_AUDIO);
}

static void test_channel_sub_index() {
  printf("test_channel_sub_index\n");
  CHECK(ChannelSubIndex(0) == 0);
  CHECK(ChannelSubIndex(7) == 7);
  CHECK(ChannelSubIndex(8) == 0);
  CHECK(ChannelSubIndex(15) == 7);
  CHECK(ChannelSubIndex(16) == 0);
  CHECK(ChannelSubIndex(19) == 3);
}

static void test_channel_labels() {
  printf("test_channel_labels\n");
  char buf[16];
  ChannelLabel(0, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "CV IN 1") == 0);
  ChannelLabel(7, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "CV IN 8") == 0);
  ChannelLabel(8, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "CV OUT 1") == 0);
  ChannelLabel(15, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "CV OUT 8") == 0);
  ChannelLabel(16, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "AUDIO IN L") == 0);
  ChannelLabel(17, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "AUDIO IN R") == 0);
  ChannelLabel(18, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "AUDIO OUT L") == 0);
  ChannelLabel(19, buf, sizeof(buf));
  CHECK(std::strcmp(buf, "AUDIO OUT R") == 0);
}

static void test_channel_full_scale() {
  printf("test_channel_full_scale\n");
  CHECK(ChannelFullScale(0) == kCvInFullScale);
  CHECK(ChannelFullScale(8) == kCvOutFullScale);
  CHECK(ChannelFullScale(16) == kAudioFullScale);
  CHECK(ChannelFullScale(19) == kAudioFullScale);
}

static void test_wrap_index() {
  printf("test_wrap_index\n");
  CHECK(WrapIndex(0, 20) == 0);
  CHECK(WrapIndex(19, 20) == 19);
  CHECK(WrapIndex(20, 20) == 0);
  CHECK(WrapIndex(-1, 20) == 19);
  CHECK(WrapIndex(-20, 20) == 0);
  CHECK(WrapIndex(45, 20) == 5);
  CHECK(WrapIndex(5, 128) == 5);
}

static void test_clamp_gain_index() {
  printf("test_clamp_gain_index\n");
  CHECK(ClampGainIndex(-5) == 0);
  CHECK(ClampGainIndex(0) == 0);
  CHECK(ClampGainIndex(kGainStepCount - 1) == kGainStepCount - 1);
  CHECK(ClampGainIndex(kGainStepCount) == kGainStepCount - 1);
  CHECK(ClampGainIndex(100) == kGainStepCount - 1);
  // no wraparound -- gain does not cycle like the channel selector does
  CHECK(ClampGainIndex(kGainStepCount + 1) == kGainStepCount - 1);
}

static void test_gain_steps_span_quarter_to_4x() {
  printf("test_gain_steps_span_quarter_to_4x\n");
  CHECK(NearlyEqual(kGainSteps[0], 0.25f));
  CHECK(NearlyEqual(kGainSteps[kGainStepCount - 1], 4.0f));
  CHECK(NearlyEqual(kGainSteps[kDefaultGainIndex], 1.0f));
  for (int i = 1; i < kGainStepCount; ++i)
    CHECK(kGainSteps[i] > kGainSteps[i - 1]);  // strictly increasing
}

static void test_ring_read_index_maps_oldest_to_newest() {
  printf("test_ring_read_index_maps_oldest_to_newest\n");
  // head=0: buffer has just wrapped, so column 0 (oldest) is slot 0 and
  // column kRingSize-1 (newest) is the slot just before it, i.e. the last.
  CHECK(RingReadIndex(0, 0) == 0);
  CHECK(RingReadIndex(0, kRingSize - 1) == kRingSize - 1);
  // head=5: the 5 most-recently-written slots are 0..4; the newest column
  // (rightmost) must land on slot 4.
  CHECK(RingReadIndex(5, kRingSize - 1) == 4);
  CHECK(RingReadIndex(5, 0) == 5);
}

static void test_value_to_row_extremes_and_center() {
  printf("test_value_to_row_extremes_and_center\n");
  const int plot_h = 40;
  // full-scale positive -> top row
  CHECK(ValueToRow(2048, 1.0f, 2048, plot_h) == 0);
  // full-scale negative -> bottom row
  CHECK(ValueToRow(-2048, 1.0f, 2048, plot_h) == plot_h - 1);
  // zero -> vertical center
  const int mid = ValueToRow(0, 1.0f, 2048, plot_h);
  CHECK(mid >= plot_h / 2 - 1 && mid <= plot_h / 2);
}

static void test_value_to_row_clamps_beyond_full_scale() {
  printf("test_value_to_row_clamps_beyond_full_scale\n");
  const int plot_h = 40;
  CHECK(ValueToRow(100000, 1.0f, 2048, plot_h) == 0);
  CHECK(ValueToRow(-100000, 1.0f, 2048, plot_h) == plot_h - 1);
}

static void test_value_to_row_gain_scales_before_clamp() {
  printf("test_value_to_row_gain_scales_before_clamp\n");
  const int plot_h = 40;
  // half full-scale at 1x sits above center; at 2x gain it should clamp to
  // the same row as full-scale at 1x (both saturate the plot top).
  const int half_1x = ValueToRow(1024, 1.0f, 2048, plot_h);
  const int half_2x = ValueToRow(1024, 2.0f, 2048, plot_h);
  const int full_1x = ValueToRow(2048, 1.0f, 2048, plot_h);
  CHECK(half_2x < half_1x);  // 2x gain pushes it further from center (up)
  CHECK(half_2x == full_1x);
}

int main() {
  test_channel_count_is_20();
  test_channel_kind_boundaries();
  test_channel_sub_index();
  test_channel_labels();
  test_channel_full_scale();
  test_wrap_index();
  test_clamp_gain_index();
  test_gain_steps_span_quarter_to_4x();
  test_ring_read_index_maps_oldest_to_newest();
  test_value_to_row_extremes_and_center();
  test_value_to_row_clamps_beyond_full_scale();
  test_value_to_row_gain_scales_before_clamp();

  printf("\ntest_scope_math: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
