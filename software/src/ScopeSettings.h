#ifndef SCOPESETTINGS_H_
#define SCOPESETTINGS_H_
// The Scope app's saved settings and their byte format. Pure; host-tested in
// test/test_scope_sources.cpp.
//
// Layouts, by length:
//   3   the pre-2026-10 Scope: {channel 0..19, gain index, screensaver flag}.
//       The channel carries over where the new source list still has it.
//       (A save padded to an even chunk may arrive as 4 bytes; byte 3 is padding.)
//   16  version 1: bytes 0-15 below, no version byte, no clock.
//   20  version 2 (current): bytes 0-15 exactly as in version 1, then
//       16 = layout version (2), 17 = clock calibrated (0/1),
//       18-19 = clock correction, int16 little endian, tenths of a ppm,
//       true sample rate above nominal, +-10000 (+-1000 ppm).
// Bytes 0-15 keep their meaning, but that does not help a DOWNGRADE: firmware
// from before this layout drops a chunk whose length is not its own (main's
// ApplyAppData), so it resets the Scope to defaults and ignores the save.
// The framework drops a chunk whose length is not the app's current one
// (OC_apps.cpp ApplyAppData) unless OlderAppDataLayoutReadable there says the
// app can read it: for the Scope, ScopeSettingsPayloadLength(n) != 0.

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
  uint8_t clock_cal;               // 1 once the crystal has been calibrated
  int16_t clock_ppm10;             // tenths of a ppm; 0 unless clock_cal
};

constexpr size_t kScopeSettingsBytes = 20;
constexpr size_t kScopeSettingsBytesV1 = 16;
constexpr size_t kScopeSettingsBytesLegacy = 3;
constexpr uint8_t kScopeSettingsVersion = 2;
constexpr int kScopeClockMaxPpm10 = 10000;

// Whether n is a length ScopeSettingsUnpack reads (3, 4, 16 or 20).
bool ScopeSettingsAcceptsLength(size_t n);
// A chunk is rounded up to an even length, so the app-data bytes the framework
// hands back can be one more than the app wrote. The layout length inside
// `stored` (20, 16 or 3), or 0 when `stored` is none of them or one past one.
// Inline: OC_apps.cpp asks it in builds that do not carry the Scope's sources.
inline size_t ScopeSettingsPayloadLength(size_t stored) {
  const size_t kinds[3] = {kScopeSettingsBytes, kScopeSettingsBytesV1, kScopeSettingsBytesLegacy};
  for (size_t k : kinds)
    if (stored == k || stored == k + 1) return k;
  return 0;
}

void ScopeSettingsDefaults(ScopeSettings *s);
size_t ScopeSettingsPack(const ScopeSettings &s, uint8_t out[kScopeSettingsBytes]);
// False (and *out = defaults) for a length that is neither format, or a
// value out of range.
bool ScopeSettingsUnpack(const uint8_t *in, size_t n, ScopeSettings *out);

#endif  // SCOPESETTINGS_H_
