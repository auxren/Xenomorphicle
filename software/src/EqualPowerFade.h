#pragma once

// The cheap equal-power crossfade curve, on its own so it has no dependencies
// and can run at compile time (Audio/XfadeTable.h builds its lookup table from
// it) as well as per sample.
//
// From https://signalsmith-audio.co.uk/writing/2021/cheap-energy-crossfade/
// Could use a LUT, but meh
constexpr void EqualPowerFade(float& fade_out, float& fade_in, float t) {
  float mt = 1.0f - t;
  float a = t * mt;
  float b = a * (1.0f + 1.4186f * a);
  float c = b + t;
  float d = b + mt;
  fade_out = d * d;
  fade_in = c * c;
}
