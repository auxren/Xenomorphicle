#ifndef HEAPWALK_H_
#define HEAPWALK_H_

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Free-list walk for newlib-nano's malloc, BSP-free and host-testable
// (test/test_heap_walk.cpp).
//
// The `heap free` figure the self-test used to print is only the untouched
// top of the pool (_heap_end - __brkval); memory that was malloc'd and freed
// again sits below brk, in holes, and is invisible there. A request can fail
// with plenty of total free if no single run is big enough, and that figure
// cannot tell the two apart. This walks the holes too.
//
// Layout, read from the disassembly of libc_nano.a (_mallinfo_r sums
// [chunk+0] while following [chunk+4]; _malloc_r splits/extends on the same
// two words): the free list `__malloc_free_list` is a singly linked list in
// ADDRESS order, each node {size, next}. `size` counts the whole chunk
// including its 4-byte size word, so a chunk spans [addr, addr + size). malloc
// grows the heap from the LAST node when that node ends exactly at brk, so a
// hole touching brk is usable together with the untouched top.
// ---------------------------------------------------------------------------
struct HeapChunk {
  size_t size;
  HeapChunk *next;
};

struct HeapInfo {
  uint32_t holes;         // free chunks below brk
  uint32_t hole_free;     // bytes in those chunks
  uint32_t largest_hole;  // biggest single chunk
  uint32_t top_gap;       // _heap_end - brk, the untouched top
  uint32_t largest_run;   // biggest CHUNK malloc could carve from now; the
                          // largest malloc(n) that can succeed is this - 8
  bool truncated;         // walk stopped early: list invalid or over the cap
};

static const uint32_t kHeapWalkMax = 4096;

// Pure over the list: `head` is the free-list head, `heap_start`/`brk`/
// `heap_end` the pool bounds as addresses. Nothing else is touched.
//
// The list is read without malloc's lock (the nano lock is a no-op) while an
// ISR may malloc/free, so a node can be handed out mid-walk and its `next`
// word overwritten with user data. Every node is therefore checked before it
// is dereferenced or believed: inside [heap_start, brk), 4-aligned, at least
// 12 bytes, ending at or below brk, and followed only by a node that starts
// at or after its end (the list is address-ordered and chunks never overlap).
// The first node that fails stops the walk with `truncated` set; the order
// check also guarantees termination, the cap is a second fence.
static inline void HeapWalk(const HeapChunk *head, uintptr_t heap_start,
                            uintptr_t brk, uintptr_t heap_end, HeapInfo *out) {
  HeapInfo h = {};
  h.top_gap = heap_end > brk ? (uint32_t)(heap_end - brk) : 0;
  uint32_t touching = 0;  // the hole that ends at brk, if any
  uint32_t n = 0;
  uintptr_t floor = heap_start;  // lowest address the next node may have
  for (const HeapChunk *c = head; c; c = c->next) {
    const uintptr_t a = (uintptr_t)c;
    if (n++ >= kHeapWalkMax || a < floor || a >= brk || (a & 3)) {
      h.truncated = true;
      break;
    }
    const size_t size = c->size;
    if (size < 12 || size > brk - a) { h.truncated = true; break; }
    const uint32_t sz = (uint32_t)size;
    h.holes++;
    h.hole_free += sz;
    if (sz > h.largest_hole) h.largest_hole = sz;
    if (a + sz == brk) touching = sz;
    floor = a + sz;
  }
  h.largest_run = h.largest_hole;
  if (h.top_gap > h.largest_run) h.largest_run = h.top_gap;
  if (h.top_gap + touching > h.largest_run) h.largest_run = h.top_gap + touching;
  *out = h;
}

#endif  // HEAPWALK_H_
