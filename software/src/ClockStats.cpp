// See ClockStats.h. Console-only code: in flash, no jump tables.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define CLOCKSTATS_CODE FLASHMEM
#else
#define CLOCKSTATS_CODE
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-jump-tables")
#endif

#include "ClockStats.h"

namespace ClockStats {

CLOCKSTATS_CODE Stamp Clock::Sample(const Raw &r) {
  // cycles: the modular delta plus whole wraps, chosen to match the elapsed millis().
  const uint32_t d = r.cyc - last_cyc_;
  const uint64_t expect = (uint64_t)(uint32_t)(r.ms - last_ms_) * cpm_;
  uint64_t wraps = 0;
  if (expect > d) wraps = (expect - d + (1ull << 31)) >> 32;
  cycles_ += (uint64_t)d + (wraps << 32);
  last_cyc_ = r.cyc;
  last_ms_ = r.ms;
  blocks_ += (uint32_t)(r.blocks - last_blocks_);
  last_blocks_ = r.blocks;
  frames_ += (uint32_t)(r.frames - last_frames_);
  last_frames_ = r.frames;
  Stamp s;
  s.cycles = cycles_;
  s.audio = blocks_ * kBlockSamples;
  s.cv = frames_;
  s.seq = seq_++;
  return s;
}

CLOCKSTATS_CODE int U64ToDec(char *buf, uint64_t v) {
  char t[21];
  int k = 0;
  do {
    t[k++] = (char)('0' + (int)(v % 10));
    v /= 10;
  } while (v);
  for (int i = 0; i < k; ++i) buf[i] = t[k - 1 - i];
  buf[k] = 0;
  return k;
}

CLOCKSTATS_CODE int FormatStime(char *buf, size_t n, const Stamp &s) {
  char t[96];
  int k = 0;
  const char *p = "STIME,";
  while (*p) t[k++] = *p++;
  const uint64_t v[3] = {s.cycles, s.audio, s.cv};
  for (int i = 0; i < 3; ++i) {
    k += U64ToDec(t + k, v[i]);
    t[k++] = ',';
  }
  k += U64ToDec(t + k, s.seq);
  if (n) {
    const size_t m = (size_t)k < n - 1 ? (size_t)k : n - 1;
    for (size_t i = 0; i < m; ++i) buf[i] = t[i];
    buf[m] = 0;
  }
  return k;
}

#if defined(__IMXRT1062__)
CLOCKSTATS_CODE Raw Capture() {
  Raw r;
  __disable_irq();
  r.cyc = ARM_DWT_CYCCNT;
  r.ms = millis();
  r.blocks = g_audio_blocks;
  r.frames = g_cv_frames;
  __enable_irq();
  return r;
}
#endif

}  // namespace ClockStats
