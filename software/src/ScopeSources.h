#ifndef SCOPESOURCES_H_
#define SCOPESOURCES_H_
// ---------------------------------------------------------------------------
// What the Scope app (apps/ScopeApp.h) can look at, and the quick groupings
// its channel field steps through. Pure: no hardware, host-tested in
// test/test_scope_sources.cpp.
//
// A view is a SET of sources (a bitmask), drawn as up to kMaxTraces traces
// in ascending Source order. The quick list is the everyday route; its last
// entry is "Custom", which means "use the stored custom mask" (the picker).
//
// Rates are what each source is captured at (hardware/xenomorpher/README.md):
// audio 48 kHz through the codec; CV in 37,350.6 Hz (kCvFsHz, ScopeFreqService.h; MCP33131D at 300 kHz
// through the 8-way mux); CV out and TR sampled in the 16.666 kHz core ISR.
// Only the audio INPUTS are AC-coupled (1 uF / 100 k, ~1.6 Hz).
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace ScopeSources {

enum Source : uint8_t {
  SRC_IN_L, SRC_IN_R, SRC_OUT_L, SRC_OUT_R,
  SRC_CV_IN1, SRC_CV_IN2, SRC_CV_IN3, SRC_CV_IN4,
  SRC_CV_IN5, SRC_CV_IN6, SRC_CV_IN7, SRC_CV_IN8,
  SRC_CV_OUT1, SRC_CV_OUT2, SRC_CV_OUT3, SRC_CV_OUT4,
  SRC_CV_OUT5, SRC_CV_OUT6, SRC_CV_OUT7, SRC_CV_OUT8,
  SRC_TR1, SRC_TR2, SRC_TR3, SRC_TR4,
  SRC_COUNT  // 24
};

using Mask = uint32_t;
constexpr int kMaxTraces = 8;

enum Kind : uint8_t { KIND_AUDIO_IN, KIND_AUDIO_OUT, KIND_CV_IN, KIND_CV_OUT, KIND_TR };

Kind KindOf(Source s);
int SubIndex(Source s);  // 0-based within its kind
float RateHz(Source s);
bool IsAc(Source s);
// "INL", "OUTR", "CV3", "OUT8", "TR2": at most 4 characters.
void Label(Source s, char *buf, size_t n);

// Ascending Source order, truncated at kMaxTraces; bits past SRC_COUNT ignored.
int MaskToList(Mask m, Source out[kMaxTraces]);

int QuickCount();
Mask QuickMask(int i);         // 0 for the Custom entry or out of range
const char *QuickLabel(int i); // at most 6 characters
bool QuickIsCustom(int i);     // true for the last entry only

// Everything, or everything but the four codec sources on a build without it.
Mask AvailableMask(bool codec);

}  // namespace ScopeSources

#endif  // SCOPESOURCES_H_
