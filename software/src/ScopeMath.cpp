// Pure logic for the Scope app. See the header for why this is split out.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define SCOPE_MATH_CODE FLASHMEM
#else
#define SCOPE_MATH_CODE
#endif

#include "ScopeMath.h"

#include <cmath>
#include <cstdio>

namespace ScopeMath {

SCOPE_MATH_CODE
int WrapIndex(int idx, int count) {
  if (count <= 0) return 0;
  int r = idx % count;
  if (r < 0) r += count;
  return r;
}

SCOPE_MATH_CODE
int ClampGainIndex(int idx) {
  if (idx < 0) return 0;
  if (idx >= kGainStepCount) return kGainStepCount - 1;
  return idx;
}

SCOPE_MATH_CODE
ChannelKind ChannelKindOf(int ch) {
  if (ch < kCvInCount) return KIND_CV_IN;
  if (ch < kCvChannelCount) return KIND_CV_OUT;
  return KIND_AUDIO;
}

SCOPE_MATH_CODE
int ChannelSubIndex(int ch) {
  if (ch < kCvInCount) return ch;
  if (ch < kCvChannelCount) return ch - kCvInCount;
  return ch - kCvChannelCount;
}

SCOPE_MATH_CODE
int32_t ChannelFullScale(int ch) {
  switch (ChannelKindOf(ch)) {
    case KIND_CV_IN: return kCvInFullScale;
    case KIND_CV_OUT: return kCvOutFullScale;
    default: return kAudioFullScale;
  }
}

SCOPE_MATH_CODE
void ChannelLabel(int ch, char *buf, size_t buflen) {
  if (!buf || buflen == 0) return;
  const int sub = ChannelSubIndex(ch);
  switch (ChannelKindOf(ch)) {
    case KIND_CV_IN:
      snprintf(buf, buflen, "CV IN %d", sub + 1);
      break;
    case KIND_CV_OUT:
      snprintf(buf, buflen, "CV OUT %d", sub + 1);
      break;
    default: {
      // sub: 0=in L, 1=in R, 2=out L, 3=out R
      const char *dir = (sub < 2) ? "IN" : "OUT";
      const char *lr = (sub % 2 == 0) ? "L" : "R";
      snprintf(buf, buflen, "AUDIO %s %s", dir, lr);
      break;
    }
  }
}

SCOPE_MATH_CODE
int RingReadIndex(int head, int column, int ring_size) {
  return WrapIndex(head + column, ring_size);
}

SCOPE_MATH_CODE
int ValueToRow(int32_t raw, float gain, int32_t full_scale, int plot_h) {
  if (full_scale <= 0) full_scale = 1;
  if (plot_h <= 0) return 0;
  float norm = (static_cast<float>(raw) * gain) / static_cast<float>(full_scale);
  if (norm > 1.0f) norm = 1.0f;
  if (norm < -1.0f) norm = -1.0f;
  const float half = static_cast<float>(plot_h - 1) * 0.5f;
  int row = static_cast<int>(std::lroundf(half - norm * half));
  if (row < 0) row = 0;
  if (row >= plot_h) row = plot_h - 1;
  return row;
}

}  // namespace ScopeMath
