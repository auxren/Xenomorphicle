// Host tests for the float <-> integer boundary conversions
// (src/Audio/f32_int_convert.h), extracted from extern/f32/AudioConvert_F32.h
// so the arithmetic can be checked exhaustively instead of listened to.
//
// The bug being fixed: the F32->int16 direction scaled by 32678 where the
// int16->F32 direction divides by 32768 (a transposed digit inherited from
// upstream OpenAudio). The naive correction wraps full scale to -32768,
// because scale and clamp shared the one constant. These tests pin both the
// gain and the absence of wrapping.
// Standalone: g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 (one line:)
//   -o build/test_f32_int_convert test_f32_int_convert.cpp && ./build/...
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <limits>

#include "../src/Audio/f32_int_convert.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

using f32conv::to_i16;
using f32conv::to_i32;
using f32conv::from_i16;

static void test_zero_and_midscale() {
  CHECK(to_i16(0.0f) == 0);
  CHECK(to_i16(0.5f) == 16384);
  CHECK(to_i16(-0.5f) == -16384);
}

// The whole point. Full-scale positive must saturate to INT16_MAX, never
// wrap to INT16_MIN: 1.0f * 32768.0f is 32768, which does not fit an int16.
static void test_full_scale_does_not_wrap() {
  CHECK(to_i16(1.0f) == 32767);
  CHECK(to_i16(-1.0f) == -32768);
  CHECK(to_i16(1.5f) == 32767);
  CHECK(to_i16(-1.5f) == -32768);
  CHECK(to_i16(1000.0f) == 32767);
  CHECK(to_i16(-1000.0f) == -32768);
}

// No input of any magnitude may produce a value of the opposite sign.
// A wrap shows up here as a positive input returning a large negative.
static void test_no_sign_flip_anywhere() {
  for (int i = 0; i <= 4000; ++i) {
    const float x = (float)i / 1000.0f;       // 0 .. 4.0, well past full scale
    CHECK(to_i16(x) >= 0);
    CHECK(to_i16(-x) <= 0);
  }
}

static void test_output_always_in_range() {
  for (int i = -4000; i <= 4000; ++i) {
    const float x = (float)i / 1000.0f;
    const int32_t v = to_i16(x);
    CHECK(v >= -32768 && v <= 32767);
  }
}

// Monotonic: a larger input never converts to a smaller output.
static void test_monotonic() {
  int16_t prev = to_i16(-2.0f);
  for (int i = -2000; i <= 2000; ++i) {
    const int16_t v = to_i16((float)i / 1000.0f);
    CHECK(v >= prev);
    prev = v;
  }
}

// The codec round trip: conv_in divides by 32768, conv_out multiplies by it.
// Exhaustive over every int16. With the 32678 typo this fails for the vast
// majority of values; with matched constants it is exact.
static void test_int16_round_trip_is_exact() {
  int bad = 0;
  for (int32_t s = -32768; s <= 32767; ++s) {
    const int16_t in = (int16_t)s;
    if (to_i16(from_i16(in)) != in) ++bad;
  }
  CHECK(bad == 0);
  if (bad) printf("  (%d of 65536 int16 values did not survive the round trip)\n", bad);
}

// The 32-bit direction has its own trap: 2^31-1 is not representable as a
// float, so the positive clamp has to be the largest float below 2^31.
static void test_i32_boundaries() {
  CHECK(to_i32(0.0f) == 0);
  CHECK(to_i32(0.5f) == 1073741824);
  CHECK(to_i32(1.0f) == 2147483520);     // largest float below 2^31
  CHECK(to_i32(-1.0f) == std::numeric_limits<int32_t>::min());
  CHECK(to_i32(2.0f) == 2147483520);
  CHECK(to_i32(-2.0f) == std::numeric_limits<int32_t>::min());
  for (int i = 0; i <= 3000; ++i) {
    CHECK(to_i32((float)i / 1000.0f) >= 0);
    CHECK(to_i32(-(float)i / 1000.0f) <= 0);
  }
}

// Unity gain, not 0.99725x: a half-scale input must land exactly on
// half scale, which the old constant missed by 45 counts.
static void test_gain_is_unity() {
  CHECK(to_i16(0.25f) == 8192);
  CHECK(to_i16(0.125f) == 4096);
  CHECK(from_i16(16384) == 0.5f);
  CHECK(from_i16(-32768) == -1.0f);
}

int main() {
  test_zero_and_midscale();
  test_full_scale_does_not_wrap();
  test_no_sign_flip_anywhere();
  test_output_always_in_range();
  test_monotonic();
  test_int16_round_trip_is_exact();
  test_i32_boundaries();
  test_gain_is_unity();
  printf("test_f32_int_convert: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
