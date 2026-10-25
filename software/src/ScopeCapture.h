#pragma once
// ---------------------------------------------------------------------------
// Feeds the Scope's rings (ScopeRing.h) from the instrument's hot paths.
// Target-only. Up to ScopeSources::kMaxTraces slots, each bound to one
// source while the Scope is open:
//   - audio in/out L/R: AudioScopeCapture::update() (audio ISR), OnAudio()
//   - CV in 1-8: the per-frame hook in OC::ADC::Scan_DMA() (core ISR, every
//     FlexIO frame, 37.5 kHz per channel), OnCvFrame(), storing raw12 codes
//   - CV out 1-8 and TR lanes: OnCoreTick() from AppScope::Process()
//   - TR rising edges, when a TR is the trigger: a pin interrupt timestamps
//     each one, in micros(), into an edge ring (microseconds, not cycles:
//     the cycle counter wraps every ~7 s, shorter than a 2 s/div screen)
// Each source maps to a slot through a small table; -1 means "not shown",
// and an unmapped source costs one load and a branch. Disarm() unmaps all,
// so a suspended Scope costs nothing beyond that.
//
// Nothing here is FLASHMEM except Arm()/Disarm(), which run in loop context.
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "ScopeRing.h"
#include "ScopeSources.h"

namespace ScopeCapture {

// Fixed offset between an audio ring's commit_cyc and the newest sample's
// real arrival, in samples: one input DMA block. Measured on the bench in
// plan Task 9.
constexpr int kAudioInLatencySamples = 128;
// The frame hook runs on the frames written since Scan_DMA()'s last pass,
// oldest first, so a CV ring's newest frame is about one frame old when
// commit_cyc is stamped.
constexpr int kCvInLatencyFrames = 1;

// Points at kMaxTraces rings the caller owns (the Scope's lazy arena), set by
// Bind() while the Scope is open; null otherwise. Arm() needs it bound.
extern ScopeRing *g_rings;
extern ScopeEdgeRing g_edges[4];
extern volatile int8_t g_audio_slot[4];  // tap: 0 in L, 1 in R, 2 out L, 3 out R
extern volatile int8_t g_cvin_slot[8];
extern volatile int8_t g_cvout_slot[8];
extern volatile int8_t g_tr_slot[4];

// Loop context. Binds list[0..n) to slots 0..n-1 with each ring's decimation
// for time_idx; slot fft_slot (or -1) is captured undecimated for the FFT.
// ext_tr (0..3, or -1) arms that TR jack's edge interrupt.
// Loop context. Bind(rings) before Arm(); Bind(nullptr) after Disarm() and
// before the storage is freed. Disarm() leaves the rings readable (a frozen
// Scope shows them), so freeing is a separate, later step.
void Bind(ScopeRing *rings);
void Arm(const ScopeSources::Source *list, int n, int time_idx, int fft_slot, int ext_tr);
void Disarm();
int SlotCount();

inline void OnAudio(int tap, const int16_t *data, int n) {
  const int s = g_audio_slot[tap];
  if (s < 0) return;
  ScopeRing &r = g_rings[s];
  for (int i = 0; i < n; ++i) r.Push(data[i]);
  r.commit_cyc = ARM_DWT_CYCCNT;
}

void OnCvFrame(const int16_t in[8]);
void OnCoreTick();

}  // namespace ScopeCapture
