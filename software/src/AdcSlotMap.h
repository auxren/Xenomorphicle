#ifndef ADCSLOTMAP_H_
#define ADCSLOTMAP_H_

#include <stdint.h>

#include "CvMeterMath.h"
#include "InputCurveStore.h"

// ---------------------------------------------------------------------------
// Which reading in an ADC33131D frame carries which panel input.
//
// The ADC33131D path (OC_ADC.cpp) deposits one 16-bit reading per trigger
// period; eight consecutive readings from the start of adc_buffer make a
// frame. Which panel input lands in which slot depends on how many mux steps
// came before the first reading. Before 2026-10-24 that depended on the
// trigger level at FLEXEN, i.e. on how long the init code took, so it
// differed between builds (bench: panel n in slot (n+5)&7 on image
// 88d35b7d, (n+4)&7 on 5ebcde9f). ADC33131D_Start() now starts the trigger
// only after FlexIO and the DMA are armed, from a forced low, so the count
// always starts at the same edge.
//
// The slot below is what that start gives. test/test_adc_slot_map.cpp
// derives it from the bench pair under every model of FlexIO's edge
// detection the pair allows, and every one gives this answer. It is also the
// table this code had before (Phazerville's), which was right whenever the
// old init happened to find the trigger low.
//
// Pure and host-tested.
// ---------------------------------------------------------------------------

constexpr int kAdcSlots = 8;

// The frame slot that carries panel input `panel` (0-7): the top row (panels
// 1-4) in slots 4-7, the second row in 0-3.
constexpr int AdcSlotForPanelInput(int panel) { return (panel + 4) & 7; }

// ---- stored data ---------------------------------------------------------

// Which stored per-input data was measured with the deterministic start.
// Older firmware left these reserved bytes 0 and ignores them when it reads a
// page, so no layout changes. Nothing is moved because of them: which slot an
// older image read each input from depended on that image's init timing, so
// older data cannot be put right blindly. They only let the firmware say what
// predates the fix (OC::input_map_check()).
//
// kInputMapV2, not 1: pages from the interim images 22fb63a2 / 5ebcde9f
// carry 1 and were measured one input off, so they count as "before the
// fix" too. A false "re-run" prompt is the safe direction.
//
// - CAL1 reserved0[0] = kInputMapV2: Setup's 0 V offset step ran (it sets all
//   eight offsets at once; apps/SETTINGS.h stamps it there, nowhere else).
// - ICA1 reserved0[0] = kInputMapV2, reserved0[1] = mask of the inputs whose
//   Q fit was measured with the fix (CvMeter's commit stamps its input).
// - Curve record reserved2[0] = kInputMapV2, reserved2[1] = the same mask for
//   curves. Whatever writes new curves (the Cal Routine's SaveCurves) must
//   call MarkMeasuredWithFix for the inputs it measured; copying or dropping
//   rows keeps the bytes as they are.
// All indices are physical (ADC_CHANNEL values), as everywhere else.
constexpr uint8_t kInputMapV2 = 2;

inline bool CalOffsetsPredateFix(uint8_t cal_reserved0_0) { return cal_reserved0_0 != kInputMapV2; }

inline uint8_t IcaFixedMask(const CvMeterMath::InputCalibration &c) {
  return c.reserved0[0] == kInputMapV2 ? c.reserved0[1] : 0;
}
inline void MarkMeasuredWithFix(CvMeterMath::InputCalibration &c, uint8_t inputs) {
  const uint8_t had = IcaFixedMask(c);
  c.reserved0[0] = kInputMapV2;
  c.reserved0[1] = uint8_t(had | inputs);
}
// The fitted inputs whose fit predates the fix.
inline uint8_t IcaPredatesFix(const CvMeterMath::InputCalibration &c) {
  return uint8_t(c.valid_mask & ~IcaFixedMask(c));
}

inline uint8_t CurveFixedMask(const InputCurveRecord &r) {
  return r.reserved2[0] == kInputMapV2 ? r.reserved2[1] : 0;
}
inline void MarkMeasuredWithFix(InputCurveRecord &r, uint8_t inputs) {
  const uint8_t had = CurveFixedMask(r);
  r.reserved2[0] = kInputMapV2;
  r.reserved2[1] = uint8_t(had | inputs);
}
inline uint8_t CurvesPredateFix(const InputCurveRecord &r) {
  return uint8_t(r.valid_mask & ~CurveFixedMask(r));
}

#endif
