#ifndef SCOPESETTINGS_H_
#define SCOPESETTINGS_H_
// The Scope app's saved settings and their byte format. Pure; host-tested in
// test/test_scope_sources.cpp. Also restores the pre-2026-10 Scope's 3-byte
// save {channel 0..19, gain index, screensaver flag}: the channel carries
// over where the new source list still has it, the rest goes to defaults.

#include <stddef.h>
#include <stdint.h>

#include "ScopeSources.h"

struct ScopeSettings {
  uint8_t quick_idx;               // index into ScopeSources' quick list
  ScopeSources::Mask custom_mask;  // used when the quick entry is Custom
  uint8_t view;                    // 0 scope, 1 FFT
  uint8_t stacked;                 // 0 overlaid, 1 stacked
  uint8_t trig_src;                // index into the shown trace list
  uint8_t trig_mode;               // 0 auto, 1 level, 2.. ext TR (2 + n)
  int16_t trig_level_mv;
  uint8_t time_idx;
  uint8_t time_auto;
  uint8_t volt_idx;
  uint8_t volt_auto;
  uint8_t screensaver_trace;
};

constexpr size_t kScopeSettingsBytes = 16;

void ScopeSettingsDefaults(ScopeSettings *s);
size_t ScopeSettingsPack(const ScopeSettings &s, uint8_t out[kScopeSettingsBytes]);
// False (and *out = defaults) for a length that is neither format, or a
// value out of range.
bool ScopeSettingsUnpack(const uint8_t *in, size_t n, ScopeSettings *out);

#endif  // SCOPESETTINGS_H_
