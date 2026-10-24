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

// Every page this firmware writes carries kInputMapV1 in one reserved byte
// (CAL1 reserved0[0], ICA1 reserved0[0], the curve record's reserved2[0]):
// "measured with the deterministic ADC33131D start". Older firmware left 0
// there and ignores the byte when it reads a page, so no layout changes.
// Nothing is moved because of it: which slot an older image read each input
// from depended on that image's init timing, so older data cannot be put
// right blindly. The flag only lets the firmware say that a page predates
// the fix (OC::input_map_check()).
constexpr uint8_t kInputMapV1 = 1;

inline uint8_t &IcaMapFlag(CvMeterMath::InputCalibration &c) { return c.reserved0[0]; }
inline uint8_t IcaMapFlag(const CvMeterMath::InputCalibration &c) { return c.reserved0[0]; }
inline uint8_t &CurveMapFlag(InputCurveRecord &r) { return r.reserved2[0]; }
inline uint8_t CurveMapFlag(const InputCurveRecord &r) { return r.reserved2[0]; }

// A stored page with per-input data in it that predates the fix.
inline bool IcaPredatesFix(const CvMeterMath::InputCalibration &c) {
  return c.valid_mask && IcaMapFlag(c) != kInputMapV1;
}
inline bool CurvesPredateFix(const InputCurveRecord &r) {
  return r.valid_mask && CurveMapFlag(r) != kInputMapV1;
}

#endif
