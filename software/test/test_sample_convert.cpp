// Host-side tests for the USB/F32 sample conversion math.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_sample_convert test_sample_convert.cpp && ./build/test_sample_convert
#include "../src/Audio/sample_convert.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <initializer_list>

using namespace samplefmt;

static int checks = 0;
#define CHECK(cond) do { assert(cond); checks++; } while (0)

int main() {
  // --- 24-bit LE decode: byte order and sign extension ---
  {
    const uint8_t zero[3] = {0, 0, 0};
    CHECK(f32_from_i24le(zero) == 0.0f);
    const uint8_t one_lsb[3] = {1, 0, 0}; // +1 LSB
    CHECK(fabsf(f32_from_i24le(one_lsb) - 1.0f / 8388608.0f) < 1e-12f);
    const uint8_t max_pos[3] = {0xFF, 0xFF, 0x7F}; // +8388607
    CHECK(fabsf(f32_from_i24le(max_pos) - 8388607.0f / 8388608.0f) < 1e-12f);
    const uint8_t min_neg[3] = {0x00, 0x00, 0x80}; // -8388608
    CHECK(f32_from_i24le(min_neg) == -1.0f);
    const uint8_t neg_one[3] = {0xFF, 0xFF, 0xFF}; // -1 LSB
    CHECK(fabsf(f32_from_i24le(neg_one) + 1.0f / 8388608.0f) < 1e-12f);
  }

  // --- 24-bit encode: saturation and byte order ---
  {
    uint8_t b[3];
    f32_to_i24le(0.0f, b);
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 0);
    f32_to_i24le(2.0f, b); // over-full-scale saturates to +max
    CHECK(b[0] == 0xFF && b[1] == 0xFF && b[2] == 0x7F);
    f32_to_i24le(-2.0f, b); // saturates to -full-scale
    CHECK(b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x80);
    f32_to_i24le(1.0f / 8388608.0f, b); // +1 LSB
    CHECK(b[0] == 1 && b[1] == 0 && b[2] == 0);
  }

  // --- 24-bit round trip is exact for every representable value (sampled) ---
  {
    for (int32_t v = -8388608; v <= 8388607; v += 997) { // prime stride
      uint8_t b[3] = {
        (uint8_t)(v & 255), (uint8_t)((v >> 8) & 255), (uint8_t)((v >> 16) & 255)
      };
      uint8_t out[3];
      f32_to_i24le(f32_from_i24le(b), out);
      CHECK(out[0] == b[0] && out[1] == b[1] && out[2] == b[2]);
    }
    // and the exact extremes
    for (int32_t v : {-8388608, -1, 0, 1, 8388607}) {
      uint8_t b[3] = {
        (uint8_t)(v & 255), (uint8_t)((v >> 8) & 255), (uint8_t)((v >> 16) & 255)
      };
      uint8_t out[3];
      f32_to_i24le(f32_from_i24le(b), out);
      CHECK(out[0] == b[0] && out[1] == b[1] && out[2] == b[2]);
    }
  }

  // --- 16-bit round trip exact for all 65536 values ---
  {
    for (int32_t s = -32768; s <= 32767; s++) {
      CHECK(f32_to_i16(f32_from_i16((int16_t)s)) == (int16_t)s);
    }
  }

  // --- 16-bit saturation ---
  {
    CHECK(f32_to_i16(2.0f) == 32767);
    CHECK(f32_to_i16(-2.0f) == -32768);
    CHECK(f32_to_i16(1.0f) == 32767);   // +1.0 has no exact int16; saturates
    CHECK(f32_to_i16(-1.0f) == -32768);
  }

  // --- scale consistency: a 16-bit value and its 24-bit left-shifted twin
  //     decode to the same float (int16<<8 == same amplitude in 24-bit) ---
  {
    for (int32_t s = -32768; s <= 32767; s += 127) {
      int32_t v = s << 8;
      uint8_t b[3] = {
        (uint8_t)(v & 255), (uint8_t)((v >> 8) & 255), (uint8_t)((v >> 16) & 255)
      };
      CHECK(fabsf(f32_from_i24le(b) - f32_from_i16((int16_t)s)) < 1e-12f);
    }
  }

  printf("test_sample_convert: all %d checks passed\n", checks);
  return 0;
}
