// The Scope app's settings format. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define SCOPE_CODE FLASHMEM
#else
#define SCOPE_CODE
#endif

#include "ScopeSettings.h"
#include "ScopeRange.h"

using namespace ScopeSources;

namespace {

// The old Scope's flat channel numbering: 0..7 CV in, 8..15 CV out,
// 16..19 audio in L, in R, out L, out R.
constexpr int kLegacyChannels = 20;

Source LegacySource(int ch) {
  if (ch < 8) return (Source)(SRC_CV_IN1 + ch);
  if (ch < 16) return (Source)(SRC_CV_OUT1 + ch - 8);
  return (Source)(SRC_IN_L + ch - 16);
}

}  // namespace

SCOPE_CODE void ScopeSettingsDefaults(ScopeSettings *s) {
  if (!s) return;
  s->quick_idx = 0;  // IN L
  s->custom_mask = 0;
  s->view = 0;
  s->stacked = 0;
  s->trig_src = 0;
  s->trig_mode = 0;
  s->trig_level_mv = 0;
  s->time_idx = ScopeRange::kDefaultTimeIdx;
  s->time_auto = 1;
  s->volt_idx = ScopeRange::kDefaultVoltIdx;
  s->volt_auto = 1;
  s->screensaver_trace = 0;
}

SCOPE_CODE size_t ScopeSettingsPack(const ScopeSettings &s, uint8_t out[kScopeSettingsBytes]) {
  out[0] = s.quick_idx;
  out[1] = (uint8_t)(s.custom_mask);
  out[2] = (uint8_t)(s.custom_mask >> 8);
  out[3] = (uint8_t)(s.custom_mask >> 16);
  out[4] = (uint8_t)(s.custom_mask >> 24);
  out[5] = s.view;
  out[6] = s.stacked;
  out[7] = s.trig_src;
  out[8] = s.trig_mode;
  out[9] = (uint8_t)((uint16_t)s.trig_level_mv);
  out[10] = (uint8_t)((uint16_t)s.trig_level_mv >> 8);
  out[11] = s.time_idx;
  out[12] = s.time_auto;
  out[13] = s.volt_idx;
  out[14] = s.volt_auto;
  out[15] = s.screensaver_trace;
  return kScopeSettingsBytes;
}

SCOPE_CODE bool ScopeSettingsUnpack(const uint8_t *in, size_t n, ScopeSettings *out) {
  if (!out) return false;
  ScopeSettingsDefaults(out);
  if (!in) return false;

  if (n == 3) {
    if (in[0] >= kLegacyChannels) return false;
    const Mask want = (Mask)1u << LegacySource(in[0]);
    int idx = QuickCount() - 1;  // Custom unless a quick entry is exactly it
    for (int i = 0; i < QuickCount() - 1; ++i)
      if (QuickMask(i) == want) { idx = i; break; }
    out->quick_idx = (uint8_t)idx;
    if (QuickIsCustom(idx)) out->custom_mask = want;
    out->screensaver_trace = in[2] ? 1 : 0;
    return true;
  }

  if (n != kScopeSettingsBytes) return false;
  if (in[0] >= QuickCount()) return false;
  ScopeSettings s;
  s.quick_idx = in[0];
  s.custom_mask = ((Mask)in[1] | ((Mask)in[2] << 8) | ((Mask)in[3] << 16) | ((Mask)in[4] << 24)) &
                  (((Mask)1u << SRC_COUNT) - 1u);
  s.view = in[5] ? 1 : 0;
  s.stacked = in[6] ? 1 : 0;
  s.trig_src = in[7] < kMaxTraces ? in[7] : 0;
  s.trig_mode = in[8] < 6 ? in[8] : 0;
  s.trig_level_mv = (int16_t)((uint16_t)in[9] | ((uint16_t)in[10] << 8));
  s.time_idx = (uint8_t)ScopeRange::ClampTime(in[11]);
  s.time_auto = in[12] ? 1 : 0;
  s.volt_idx = (uint8_t)ScopeRange::ClampVolt(in[13]);
  s.volt_auto = in[14] ? 1 : 0;
  s.screensaver_trace = in[15] ? 1 : 0;
  *out = s;
  return true;
}
