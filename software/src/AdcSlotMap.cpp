// The ADC33131D slot map and the one-time move of stored per-input data.
// See the header. Pure: host-tested by test/test_adc_slot_map.cpp.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define ADC_SLOT_MAP_CODE FLASHMEM
#else
#define ADC_SLOT_MAP_CODE
#endif

#include "AdcSlotMap.h"

#include <string.h>

// A named namespace, not an anonymous one: FLASHMEM's externally_visible
// attribute needs external linkage.
namespace adc_slot_map_detail {

constexpr int kIdentity[kAdcSlots] = {0, 1, 2, 3, 4, 5, 6, 7};

inline const int *table(const int *chan) { return chan ? chan : kIdentity; }

// new[chan[m]] = old[chan[m+1]]: the entry recorded for panel m sat where the
// old scan put it, at ADC_CHANNEL_(m+1); it moves to ADC_CHANNEL_m.
template <typename T>
ADC_SLOT_MAP_CODE void rotate(T *a, const int *chan) {
  T old[kAdcSlots];
  memcpy(old, a, sizeof old);
  for (int m = 0; m < kAdcSlots; ++m) a[chan[m]] = old[chan[(m + 1) % kAdcSlots]];
}

ADC_SLOT_MAP_CODE uint8_t rotate_mask(uint8_t mask, const int *chan) {
  uint8_t out = 0;
  for (int m = 0; m < kAdcSlots; ++m)
    if (mask & (1u << chan[(m + 1) % kAdcSlots])) out = uint8_t(out | (1u << chan[m]));
  return out;
}

}  // namespace adc_slot_map_detail

using namespace adc_slot_map_detail;

ADC_SLOT_MAP_CODE bool InputMapMarkerPresent(const uint8_t m[4]) {
  return memcmp(m, kInputMapMarker, sizeof kInputMapMarker) == 0;
}

ADC_SLOT_MAP_CODE void InputMapMarkerClear(uint8_t m[4]) { memset(m, 0, 4); }

ADC_SLOT_MAP_CODE void RotateLegacyOffsets(uint16_t offset[8], const int *chan) {
  rotate(offset, table(chan));
}

ADC_SLOT_MAP_CODE void RotateLegacyIca(CvMeterMath::InputCalibration &ica, const int *chan) {
  static_assert(CvMeterMath::kChannels == kAdcSlots, "ICA1 has one entry per slot");
  const int *t = table(chan);
  rotate(ica.gain, t);
  rotate(ica.offset_mv, t);
  rotate(ica.verify_mv10, t);
  rotate(ica.source_output, t);
  ica.valid_mask = rotate_mask(ica.valid_mask, t);
}

ADC_SLOT_MAP_CODE void RotateLegacyCurves(InputCurveRecord &rec, const int *chan) {
  static_assert(kInputCurveInputs == kAdcSlots, "the curve record has one row per slot");
  const int *t = table(chan);
  int16_t old[kAdcSlots][kInputCurvePoints];
  memcpy(old, rec.mv_x2, sizeof old);
  for (int m = 0; m < kAdcSlots; ++m)
    memcpy(rec.mv_x2[t[m]], old[t[(m + 1) % kAdcSlots]], sizeof old[0]);
  rotate(rec.source_output, t);
  rec.valid_mask = rotate_mask(rec.valid_mask, t);
}

ADC_SLOT_MAP_CODE void RotateLegacyInputData(uint16_t offset[8], CvMeterMath::InputCalibration &ica,
                                             InputCurveRecord *curves, const int *chan) {
  RotateLegacyOffsets(offset, chan);
  RotateLegacyIca(ica, chan);
  if (curves) RotateLegacyCurves(*curves, chan);
}

ADC_SLOT_MAP_CODE bool InputCurveNormalize(InputCurveRecord &rec, const int *chan) {
  if (CurveMapFlag(rec) == kInputMapV1) return false;
  RotateLegacyCurves(rec, chan);
  CurveMapFlag(rec) = kInputMapV1;
  InputCurveEncode(rec);
  return true;
}

ADC_SLOT_MAP_CODE InputMapPlan InputMapPrepare(InputMapPages &p, const int *chan) {
  InputMapPlan plan = {false, false, false, false};
  // The marker only says the curve file was done. CAL1 and ICA1 are in RAM
  // at every boot, so their own flags are always checked: a marker that came
  // from somewhere else must never hide a legacy page.
  const bool marker = InputMapMarkerPresent(p.marker);
  plan.run = !marker;

  // CAL1. Defaults (no page) are the same on every input: stamp RAM only.
  if (p.cal_loaded && *p.cal_flag != kInputMapV1) {
    RotateLegacyOffsets(p.cal_offset, chan);
    plan.save_cal = true;
  }
  *p.cal_flag = kInputMapV1;

  // ICA1. Only the fitted channels mean anything; a page with none (or one
  // the loader reset, e.g. a newer version) is not rewritten.
  if (p.ica_loaded && IcaMapFlag(*p.ica) != kInputMapV1 && p.ica->valid_mask) {
    RotateLegacyIca(*p.ica, chan);
    plan.save_ica = true;
  }
  IcaMapFlag(*p.ica) = kInputMapV1;

  // The curve record (A/B store).
  if (!marker && p.curves_loaded && InputCurveNormalize(*p.curves, chan)) plan.save_curves = true;
  return plan;
}
