#include "OC_core.h"
#include "DeferRing.h"
#include "RtStats.h"
#include "HeapWalk.h"
#include <malloc.h>

extern "C" char _heap_start[], _heap_end[], *__brkval;
extern "C" HeapChunk *__malloc_free_list;  // newlib-nano, see HeapWalk.h

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

// Thin wrapper: the walk itself is HeapWalk.h. The list is only stable while
// nothing mallocs or frees, which loop() guarantees for its own calls and an
// ISR does not, hence loop context only (the walk validates every node, so a
// race yields `truncated`, not a fault).
FLASHMEM void OC::CORE::HeapStats(HeapInfo *out) {
  static_assert(sizeof(HeapChunk) == 8 && offsetof(HeapChunk, next) == 4,
                "newlib-nano free-list node is {size, next} on 32-bit ARM");
  HeapWalk(__malloc_free_list, (uintptr_t)_heap_start, (uintptr_t)__brkval,
           (uintptr_t)_heap_end, out);
}
