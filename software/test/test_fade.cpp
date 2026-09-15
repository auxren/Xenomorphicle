// Host tests for the persistence-window fade (src/Fade.h): the raised-cosine
// ramp that takes audio to silence before an unavoidable interrupts-masked
// flash write, and brings it back afterwards.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_fade test_fade.cpp && ./build/test_fade
#include <cstdio>
#include <cmath>

#include "../src/Fade.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
static bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

static void test_endpoints_are_exact() {
  // A fade that does not reach exactly 0 leaves a DC step when the buffer is
  // zeroed; one that does not reach exactly 1 leaves the instrument quiet.
  CHECK(near(Fade::raised_cosine(0.0f), 1.0f, 0.0f));
  CHECK(near(Fade::raised_cosine(1.0f), 0.0f, 0.0f));
}

static void test_midpoint_is_half() {
  CHECK(near(Fade::raised_cosine(0.5f), 0.5f));
}

static void test_monotonic_and_bounded() {
  float prev = 2.0f;
  for (int i = 0; i <= 256; ++i) {
    const float g = Fade::raised_cosine((float)i / 256.0f);
    CHECK(g <= 1.0f && g >= 0.0f);
    CHECK(g <= prev + 1e-6f);   // never rises on the way down
    prev = g;
  }
}

static void test_slope_is_zero_at_both_ends() {
  // The point of a raised cosine over a straight line: the corners are what
  // a listener hears as a click, and both ends here leave at zero slope.
  const float d0 = Fade::raised_cosine(0.0f) - Fade::raised_cosine(0.004f);
  const float d1 = Fade::raised_cosine(0.996f) - Fade::raised_cosine(1.0f);
  const float dmid = Fade::raised_cosine(0.498f) - Fade::raised_cosine(0.502f);
  CHECK(d0 < dmid * 0.1f);
  CHECK(d1 < dmid * 0.1f);
}

static void test_out_of_range_positions_clamp() {
  CHECK(near(Fade::raised_cosine(-1.0f), 1.0f, 0.0f));
  CHECK(near(Fade::raised_cosine(2.0f), 0.0f, 0.0f));
}

static void test_ramp_walks_from_one_to_zero_over_its_length() {
  Fade::Ramp r;
  r.start(20, /*to_silence=*/true);
  CHECK(near(r.gain_at(0), 1.0f, 0.0f));
  CHECK(near(r.gain_at(20), 0.0f, 0.0f));
  CHECK(r.gain_at(10) < 0.9f && r.gain_at(10) > 0.1f);
  CHECK(r.done(20));
  CHECK(!r.done(19));
  CHECK(near(r.gain_at(999), 0.0f, 0.0f));   // past the end stays at the end
}

static void test_ramp_the_other_way_comes_back_up() {
  Fade::Ramp r;
  r.start(25, /*to_silence=*/false);
  CHECK(near(r.gain_at(0), 0.0f, 0.0f));
  CHECK(near(r.gain_at(25), 1.0f, 0.0f));
  CHECK(r.gain_at(12) > 0.1f && r.gain_at(12) < 0.9f);
}

static void test_zero_length_ramp_is_already_done() {
  // A window with no fade budget must not divide by zero or hang: it lands
  // on its destination immediately.
  Fade::Ramp r;
  r.start(0, true);
  CHECK(r.done(0));
  CHECK(near(r.gain_at(0), 0.0f, 0.0f));
}

int main() {
  test_endpoints_are_exact();
  test_midpoint_is_half();
  test_monotonic_and_bounded();
  test_slope_is_zero_at_both_ends();
  test_out_of_range_positions_clamp();
  test_ramp_walks_from_one_to_zero_over_its_length();
  test_ramp_the_other_way_comes_back_up();
  test_zero_length_ramp_is_already_done();
  printf("test_fade: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
