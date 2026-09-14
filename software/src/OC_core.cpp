#include "OC_core.h"
#include "DeferRing.h"
#include "RtStats.h"
#include <malloc.h>

extern "C" char _heap_end[], *__brkval;

// ISR producer (CORE timer), loop consumer. This used to be a
// std::queue<std::function<void()>> with the lock commented out: every
// DeferTask() from the 16.667 kHz ISR was a heap allocation racing
// loop()'s own malloc, and a FlushTasks() mid-pop could be interleaved
// with an emplace. The ring is fixed-size and lock-free by construction.
static DeferRing defer_ring;

void OC::CORE::DeferTask(void (*fn)()) {
  defer_ring.push(fn);
}

void OC::CORE::DeferTask(void (*fn)(void *), void *ctx) {
  defer_ring.push(fn, ctx);
}

void OC::CORE::FlushTasks() {
  defer_ring.run_all();
  // budget rows: a dropped entry is a realtime MIDI byte that never went out
  RT::stats.defer_dropped = defer_ring.dropped;
  RT::stats.defer_hiwater = defer_ring.high_water;
}

int OC::CORE::FreeRam() {
  return _heap_end - __brkval;
}
