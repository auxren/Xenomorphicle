// Feeds the Scope's rings from the hot paths. See the header.
#if defined(__IMXRT1062__)

#include "ScopeCapture.h"

#include "OC_ADC.h"
#include "OC_DAC.h"
#include "OC_digital_inputs.h"
#include "OC_gpio.h"
#include "ScopeRange.h"

namespace ScopeCapture {

using namespace ScopeSources;

ScopeRing *g_rings = nullptr;
ScopeEdgeRing g_edges[4];
volatile int8_t g_audio_slot[4] = {-1, -1, -1, -1};
volatile int8_t g_cvin_slot[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
volatile int8_t g_cvout_slot[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
volatile int8_t g_tr_slot[4] = {-1, -1, -1, -1};

namespace {

int slot_count = 0;
int edge_tr = -1;  // the TR whose interrupt is attached, or -1

uint8_t TrPin(int tr) {
  switch (tr) {
    case 0: return TR1;
    case 1: return TR2;
    case 2: return TR3;
    default: return TR4;
  }
}

// Edge times in microseconds; 100 us between stored edges bounds an
// audio-rate clock on a TR jack.
constexpr uint32_t kMinGapUs = 100;
void EdgeIsr0() { g_edges[0].Note(micros(), kMinGapUs); }
void EdgeIsr1() { g_edges[1].Note(micros(), kMinGapUs); }
void EdgeIsr2() { g_edges[2].Note(micros(), kMinGapUs); }
void EdgeIsr3() { g_edges[3].Note(micros(), kMinGapUs); }
void (*const kEdgeIsr[4])() = {EdgeIsr0, EdgeIsr1, EdgeIsr2, EdgeIsr3};

void Unmap() {
  for (auto &v : g_audio_slot) v = -1;
  for (auto &v : g_cvin_slot) v = -1;
  for (auto &v : g_cvout_slot) v = -1;
  for (auto &v : g_tr_slot) v = -1;
}

void DetachEdge() {
  if (edge_tr >= 0) {
    detachInterrupt(TrPin(edge_tr));
    edge_tr = -1;
  }
}

}  // namespace

FLASHMEM void Bind(ScopeRing *rings) {
  if (!rings) Disarm();  // never leave a slot pointing at freed storage
  g_rings = rings;
}

FLASHMEM void Arm(const Source *list, int n, int time_idx, int fft_slot, int ext_tr) {
  if (!g_rings) {
    Disarm();
    return;
  }
  if (n > kMaxTraces) n = kMaxTraces;
  if (n < 0) n = 0;
  DetachEdge();
  __disable_irq();  // no writer may push into a ring while it resets
  Unmap();
  for (int s = 0; s < n; ++s) {
    const uint16_t d = (s == fft_slot) ? 1 : ScopeRange::DecimFor(time_idx, RateHz(list[s]));
    g_rings[s].Reset(d);
  }
  for (int s = 0; s < n; ++s) {
    const int sub = SubIndex(list[s]);
    switch (KindOf(list[s])) {
      case KIND_AUDIO_IN: g_audio_slot[sub] = (int8_t)s; break;
      case KIND_AUDIO_OUT: g_audio_slot[2 + sub] = (int8_t)s; break;
      case KIND_CV_IN: g_cvin_slot[sub] = (int8_t)s; break;
      case KIND_CV_OUT: g_cvout_slot[sub] = (int8_t)s; break;
      case KIND_TR: g_tr_slot[sub] = (int8_t)s; break;
    }
  }
  slot_count = n;
  __enable_irq();
  OC::ADC::set_scope_frame_hook(n ? OnCvFrame : nullptr);
  if (ext_tr >= 0 && ext_tr < 4) {
    g_edges[ext_tr].Reset();
    attachInterrupt(TrPin(ext_tr), kEdgeIsr[ext_tr], ADC33131D_Uses_FlexIO ? RISING : FALLING);
    edge_tr = ext_tr;
  }
}

FLASHMEM void Disarm() {
  DetachEdge();
  OC::ADC::set_scope_frame_hook(nullptr);
  __disable_irq();
  Unmap();
  slot_count = 0;
  __enable_irq();
}

int SlotCount() { return slot_count; }

// Core ISR, once per FlexIO frame. Slot order as Scan_DMA's own:
// in[0..3] = CV5-8, in[4..7] = CV1-4. Stores the raw12 code Scan_DMA would
// hand update<>(); calibration is applied when the frame is analysed.
void OnCvFrame(const int16_t in[8]) {
  const uint32_t now = ARM_DWT_CYCCNT;
  for (int j = 0; j < 8; ++j) {
    const int cv = j < 4 ? j + 4 : j - 4;
    const int s = g_cvin_slot[cv];
    if (s < 0) continue;
    const int32_t v = in[j] < 0 ? 0 : in[j];
    g_rings[s].Push((int16_t)((v * 2) >> (OC::ADC::kAdcScanResolution - OC::ADC::kAdcResolution)));
    g_rings[s].commit_cyc = now;
  }
}

// Core ISR (AppScope::Process). CV out codes recentred to int16; TR as 0/max.
void OnCoreTick() {
  const uint32_t now = ARM_DWT_CYCCNT;
  for (int i = 0; i < 8; ++i) {
    const int s = g_cvout_slot[i];
    if (s < 0) continue;
    g_rings[s].Push((int16_t)((int32_t)OC::DAC::value((size_t)i) - 32768));
    g_rings[s].commit_cyc = now;
  }
  for (int i = 0; i < 4; ++i) {
    const int s = g_tr_slot[i];
    if (s < 0) continue;
    const bool on = OC::DigitalInputs::read_immediate((OC::DigitalInput)i);
    g_rings[s].Push(on ? 32767 : 0);
    g_rings[s].commit_cyc = now;
  }
}

}  // namespace ScopeCapture

#endif  // __IMXRT1062__
