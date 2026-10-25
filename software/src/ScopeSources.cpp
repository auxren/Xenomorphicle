// Pure logic for the Scope app's sources. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define SCOPE_CODE FLASHMEM
#else
#define SCOPE_CODE
#endif

#include "ScopeSources.h"

#include <stdio.h>

namespace ScopeSources {

namespace {

constexpr Mask Bit(Source s) { return (Mask)1u << s; }
constexpr Mask Range(Source first, int count) {
  return ((((Mask)1u << count) - 1u) << first);
}

struct QuickEntry {
  const char *label;
  Mask mask;
};

const QuickEntry kQuick[] = {
  {"IN L", Bit(SRC_IN_L)},
  {"IN R", Bit(SRC_IN_R)},
  {"IN LR", Bit(SRC_IN_L) | Bit(SRC_IN_R)},
  {"CV1", Bit(SRC_CV_IN1)},
  {"CV2", Bit(SRC_CV_IN2)},
  {"CV3", Bit(SRC_CV_IN3)},
  {"CV4", Bit(SRC_CV_IN4)},
  {"CV5", Bit(SRC_CV_IN5)},
  {"CV6", Bit(SRC_CV_IN6)},
  {"CV7", Bit(SRC_CV_IN7)},
  {"CV8", Bit(SRC_CV_IN8)},
  {"CV1-4", Range(SRC_CV_IN1, 4)},
  {"CV5-8", Range(SRC_CV_IN5, 4)},
  {"CV+TR", Range(SRC_CV_IN1, 4) | Range(SRC_TR1, 4)},
  {"OUT LR", Bit(SRC_OUT_L) | Bit(SRC_OUT_R)},
  {"OUT1-4", Range(SRC_CV_OUT1, 4)},
  {"Custom", 0},
};
constexpr int kQuickCount = sizeof(kQuick) / sizeof(kQuick[0]);

}  // namespace

SCOPE_CODE Kind KindOf(Source s) {
  if (s <= SRC_IN_R) return KIND_AUDIO_IN;
  if (s <= SRC_OUT_R) return KIND_AUDIO_OUT;
  if (s <= SRC_CV_IN8) return KIND_CV_IN;
  if (s <= SRC_CV_OUT8) return KIND_CV_OUT;
  return KIND_TR;
}

SCOPE_CODE int SubIndex(Source s) {
  switch (KindOf(s)) {
    case KIND_AUDIO_IN: return s - SRC_IN_L;
    case KIND_AUDIO_OUT: return s - SRC_OUT_L;
    case KIND_CV_IN: return s - SRC_CV_IN1;
    case KIND_CV_OUT: return s - SRC_CV_OUT1;
    default: return s - SRC_TR1;
  }
}

SCOPE_CODE float RateHz(Source s) {
  switch (KindOf(s)) {
    case KIND_AUDIO_IN:
    case KIND_AUDIO_OUT: return 48000.f;
    case KIND_CV_IN: return 37500.f;
    default: return 16666.f;
  }
}

SCOPE_CODE bool IsAc(Source s) { return KindOf(s) == KIND_AUDIO_IN; }

SCOPE_CODE void Label(Source s, char *buf, size_t n) {
  if (!buf || n == 0) return;
  const int sub = SubIndex(s);
  switch (KindOf(s)) {
    case KIND_AUDIO_IN: snprintf(buf, n, "IN%c", sub ? 'R' : 'L'); break;
    case KIND_AUDIO_OUT: snprintf(buf, n, "OUT%c", sub ? 'R' : 'L'); break;
    case KIND_CV_IN: snprintf(buf, n, "CV%d", sub + 1); break;
    case KIND_CV_OUT: snprintf(buf, n, "OUT%d", sub + 1); break;
    default: snprintf(buf, n, "TR%d", sub + 1); break;
  }
}

SCOPE_CODE int MaskToList(Mask m, Source out[kMaxTraces]) {
  int n = 0;
  for (int s = 0; s < SRC_COUNT && n < kMaxTraces; ++s)
    if (m & ((Mask)1u << s)) out[n++] = (Source)s;
  return n;
}

SCOPE_CODE int QuickCount() { return kQuickCount; }

SCOPE_CODE Mask QuickMask(int i) {
  return (i >= 0 && i < kQuickCount) ? kQuick[i].mask : 0;
}

SCOPE_CODE const char *QuickLabel(int i) {
  return (i >= 0 && i < kQuickCount) ? kQuick[i].label : "";
}

SCOPE_CODE bool QuickIsCustom(int i) { return i == kQuickCount - 1; }

SCOPE_CODE Mask AvailableMask(bool codec) {
  const Mask all = ((Mask)1u << SRC_COUNT) - 1u;
  return codec ? all : (all & ~Range(SRC_IN_L, 4));
}

}  // namespace ScopeSources
