// Pure logic for the Sampler app. See the header for why this is split out.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define SAMPLER_MATH_CODE FLASHMEM
#else
#define SAMPLER_MATH_CODE
#endif

#include "SamplerMath.h"

namespace SamplerMath {

SAMPLER_MATH_CODE
int WrapSlotIndex(int idx) {
  int r = idx % kSlotCount;
  if (r < 0) r += kSlotCount;
  return r;
}

SAMPLER_MATH_CODE
int ClampFileNum(int n) {
  if (n < kMinFileNum) return kMinFileNum;
  if (n > kMaxFileNum) return kMaxFileNum;
  return n;
}

SAMPLER_MATH_CODE
int ClampRatePercent(int pct) {
  if (pct < kMinRatePercent) return kMinRatePercent;
  if (pct > kMaxRatePercent) return kMaxRatePercent;
  return pct;
}

SAMPLER_MATH_CODE
uint8_t ClampTriggerMode(int m) {
  if (m < 0) return TRIG_ONE_SHOT;
  if (m >= (int)kTriggerModeCount) return (uint8_t)(kTriggerModeCount - 1);
  return (uint8_t)m;
}

SAMPLER_MATH_CODE
void BuildFilename(int file_num, char *buf, size_t buflen) {
  if (!buf) return;
  if (buflen < kFilenameBufLen) {
    if (buflen) buf[0] = '\0';
    return;
  }
  const int n = ClampFileNum(file_num);
  buf[0] = (char)('0' + n / 100);
  buf[1] = (char)('0' + (n / 10) % 10);
  buf[2] = (char)('0' + n % 10);
  buf[3] = '.';
  buf[4] = 'W';
  buf[5] = 'A';
  buf[6] = 'V';
  buf[7] = '\0';
}

// FLASHMEM, like every function in this file: unlike TweightyTapPhase.cpp's
// TweightyTapTargetSecs() (deliberately off FLASHMEM because it runs from
// TweightyApp's audio-ISR Controller() at 16.666kHz), Sampler has no
// Controller()/audio-ISR-hot surface at all (see apps/SamplerApp.h's class
// comment) -- UpdateGate() and ComputeRateMultiplier() below are called
// from Loop(), the main non-ISR loop, at most 8x per pass, the same cold
// cadence as every other function here. This matches ScopeMath.cpp's own
// choice to FLASHMEM everything uniformly, for the same reason (ScopeApp is
// also Controller()-less).
SAMPLER_MATH_CODE
GateEdge UpdateGate(int32_t raw, bool &prev_high) {
  const bool high = raw > kGateThresholdRaw;
  GateEdge edge = EDGE_NONE;
  if (high && !prev_high) edge = EDGE_RISING;
  else if (!high && prev_high) edge = EDGE_FALLING;
  prev_high = high;
  return edge;
}

SAMPLER_MATH_CODE
float ComputeRateMultiplier(int rate_percent, int32_t cv_raw) {
  float norm = (float)cv_raw / (float)kCvFullScale;
  if (norm > 1.0f) norm = 1.0f;
  else if (norm < -1.0f) norm = -1.0f;

  const float base = (float)ClampRatePercent(rate_percent) * 0.01f;
  float rate = base * (1.0f + norm * kPitchCvDepth);

  if (rate < kMinRateMult) rate = kMinRateMult;
  else if (rate > kMaxRateMult) rate = kMaxRateMult;
  return rate;
}

}  // namespace SamplerMath
