// Real-time budget bookkeeping, firmware side. See RtStats.h and
// docs/Timing-Budget.md. Everything here runs in loop context; the ISR
// hooks are inline in the header.
#include <Arduino.h>
#include <CrashReport.h>

#include "RtStats.h"

namespace OC {
namespace RT {

Counters stats;
volatile bool window_open = false;
volatile bool window_seen = false;

void LoopPass() {
  static uint32_t last_cycles = 0;
  static uint32_t last_persist_ms = 0;
  const uint32_t now = ARM_DWT_CYCCNT;
  if (last_cycles) {
    const uint32_t us = cycles_to_us(now - last_cycles);
    if (window_seen) {
      // this pass held a declared persistence window: its length is the
      // window's to report, not the loop's
      window_seen = false;
    } else {
      if (us > stats.loop_pass_max_us) stats.loop_pass_max_us = us;
      stats.loop_hist.add(us);
    }
  }
  last_cycles = now;

  const uint32_t ms = millis();
  if (ms - last_persist_ms >= 1000) {
    last_persist_ms = ms;
    Persist();
  }
}

void MidiGap(uint32_t us) {
  stats.midi_hist.add(us);
  if (us > stats.midi_gap_max_us) stats.midi_gap_max_us = us;
  if (us > Budget::kMidiGapBudgetUs) stats.midi_gap_violations++;
}

FLASHMEM static void print_hist(const char *label, const Hist8 &h) {
  Serial.printf("  %s:", label);
  for (int i = 0; i < Hist8::kBuckets; ++i) {
    if (i < Hist8::kBuckets - 1)
      Serial.printf(" <%lu:%lu", (unsigned long)Hist8::bound(i), (unsigned long)h.b[i]);
    else
      Serial.printf(" >=10000:%lu", (unsigned long)h.b[i]);
  }
  const uint32_t p95 = h.p95_bound_us();
  if (p95 == Hist8::kOpenEnded) Serial.println("  p95 >= 10000 us");
  else Serial.printf("  p95 < %lu us\n", (unsigned long)p95);
}

FLASHMEM void Report(bool reset_after) {
  const Counters &c = stats;
  Serial.println("=== rt budget ===");
  Serial.printf("audio out: xrun=%lu run_max=%lu half=%lu alloc_fail=%lu in_window=%lu\n",
                (unsigned long)c.audio_out.count, (unsigned long)c.audio_out.run_max,
                (unsigned long)c.audio_out_half, (unsigned long)c.audio_out_alloc_fail,
                (unsigned long)c.audio_out_in_window);
  Serial.printf("audio in:  xrun=%lu alloc_fail=%lu in_window=%lu   f32 alloc_fail=%lu\n",
                (unsigned long)c.audio_in_xrun, (unsigned long)c.audio_in_alloc_fail,
                (unsigned long)c.audio_in_in_window, (unsigned long)c.f32_alloc_fail);
  Serial.printf("core isr:  max=%luus gap_max=%luus missed=%lu in_window=%lu\n",
                (unsigned long)c.core_isr_hiwater_us, (unsigned long)c.core_gap_max_us,
                (unsigned long)c.core_missed_ticks, (unsigned long)c.core_missed_in_window);
  Serial.printf("loop pass: max=%luus\n", (unsigned long)c.loop_pass_max_us);
  print_hist("loop", c.loop_hist);
  Serial.printf("midi poll: gap_max=%luus violations=%lu (budget %luus)\n",
                (unsigned long)c.midi_gap_max_us, (unsigned long)c.midi_gap_violations,
                (unsigned long)Budget::kMidiGapBudgetUs);
  print_hist("midi", c.midi_hist);
  Serial.printf("windows:   count=%lu max=%lums violations=%lu\n",
                (unsigned long)c.window_count, (unsigned long)c.window_max_ms,
                (unsigned long)c.window_violations);
  Serial.printf("defer:     dropped=%lu hiwater=%lu (ring of %u)\n",
                (unsigned long)c.defer_dropped, (unsigned long)c.defer_hiwater, 16u);
  const Verdict v = Evaluate(c);
  for (int i = 0; i < Verdict::kRows; ++i)
    Serial.printf("  %s  %s\n", v.pass[i] ? "PASS" : "FAIL", Verdict::name(i));
  Serial.printf("=== rt budget: %d of %d rows FAIL ===\n", v.failures, (int)Verdict::kRows);
  if (reset_after) {
    stats.reset();
    Serial.println("rt counters reset");
  } else {
    Serial.println("('T' again within 3s resets the counters)");
  }
}

FLASHMEM void Summary() {
  const Verdict v = Evaluate(stats);
  Serial.printf("rt budget: %d of %d rows FAIL (xrun=%lu missed=%lu loop_max=%luus; 'T' for detail)\n",
                v.failures, (int)Verdict::kRows, (unsigned long)stats.audio_out.count,
                (unsigned long)stats.core_missed_ticks, (unsigned long)stats.loop_pass_max_us);
}

// Breadcrumbs 1-6 survive a warm reset and are printed with the crash
// report (Main.cpp appends it to CRASH.LOG). Written only on change: each
// write is a cache flush.
FLASHMEM void Persist() {
  static uint32_t last[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                             0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  const uint32_t cur[6] = {
    stats.audio_out.count,
    stats.core_missed_ticks,
    stats.loop_pass_max_us,
    stats.f32_alloc_fail + stats.audio_out_alloc_fail + stats.audio_in_alloc_fail,
    stats.window_max_ms,
    stats.midi_gap_violations,
  };
  for (int i = 0; i < 6; ++i) {
    if (cur[i] != last[i]) {
      last[i] = cur[i];
      CrashReport.breadcrumb(i + 1, cur[i]);
    }
  }
}

}  // namespace RT
}  // namespace OC
