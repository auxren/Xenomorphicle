// Host tests for the newlib-nano free-list walk (src/HeapWalk.h), run over a
// fabricated list laid out in a local buffer with the same {size, next} nodes.
// Every node sits at a HeapChunk-aligned (8 on the host) offset: the walk reads
// a valid node through a HeapChunk*, which UBSan checks.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2
//   -o build/test_heap_walk test_heap_walk.cpp && ./build/test_heap_walk
#include <cstdio>
#include <cstring>

#include "../src/HeapWalk.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// A 128 KB pool; "addresses" are real addresses inside it, so a chunk's end
// (addr + size) is comparable to brk exactly as on the target.
alignas(16) static unsigned char pool[131072];
static const size_t kPool = sizeof pool;

static HeapChunk *chunk(size_t off, size_t size) {
  HeapChunk *c = (HeapChunk *)(pool + off);
  c->size = size;
  c->next = nullptr;
  return c;
}
// A node at an address that is not HeapChunk-aligned, written bytewise so the
// test itself never forms a misaligned HeapChunk access (UBSan -alignment). The
// walk must reject it on its (a & 3) check before it dereferences it.
static HeapChunk *chunk_raw(size_t off, size_t size) {
  HeapChunk tmp;
  tmp.size = size;
  tmp.next = nullptr;
  memcpy(pool + off, &tmp, sizeof tmp);
  return (HeapChunk *)(pool + off);
}
static uintptr_t at(size_t off) { return (uintptr_t)(pool + off); }

static HeapInfo walk(const HeapChunk *head, size_t brk_off, size_t end_off,
                     size_t start_off = 0) {
  HeapInfo h;
  memset(&h, 0xAA, sizeof h);  // the walk must set every field
  HeapWalk(head, at(start_off), at(brk_off), at(end_off), &h);
  return h;
}

static void test_empty_list() {
  HeapInfo h = walk(nullptr, 1000, 5000);
  CHECK(h.holes == 0 && h.hole_free == 0 && h.largest_hole == 0);
  CHECK(h.top_gap == 4000 && h.largest_run == 4000 && !h.truncated);
}

static void test_brk_at_end_nothing_left() {
  HeapInfo h = walk(nullptr, 5000, 5000);
  CHECK(h.top_gap == 0 && h.largest_run == 0);
}

static void test_one_coalesced_hole_below_brk() {
  // one freed block, not touching brk: the top gap is separate
  HeapChunk *a = chunk(104, 3000);
  HeapInfo h = walk(a, 10000, 12000);
  CHECK(h.holes == 1 && h.hole_free == 3000 && h.largest_hole == 3000);
  CHECK(h.top_gap == 2000);
  CHECK(h.largest_run == 3000);  // hole beats the top
}

static void test_fragmented_total_large_run_small() {
  // 4 holes of 1000: total 4000, but the biggest run is only 1000, and the
  // top (500) is smaller -- the situation the Scope arena failed in
  HeapChunk *a = chunk(104, 1000), *b = chunk(2104, 1000);
  HeapChunk *c = chunk(4104, 1000), *d = chunk(6104, 1000);
  a->next = b; b->next = c; c->next = d;
  HeapInfo h = walk(a, 9000, 9500);
  CHECK(h.holes == 4 && h.hole_free == 4000);
  CHECK(h.largest_hole == 1000 && h.top_gap == 500);
  CHECK(h.largest_run == 1000);
  CHECK(h.hole_free > h.largest_run);  // fragmentation, not exhaustion
}

static void test_top_larger_than_holes() {
  HeapChunk *a = chunk(104, 800), *b = chunk(2000, 600);
  a->next = b;
  HeapInfo h = walk(a, 8000, 20000);
  CHECK(h.largest_hole == 800 && h.top_gap == 12000);
  CHECK(h.largest_run == 12000);
}

static void test_hole_touching_brk_extends_the_top() {
  // last hole ends exactly at brk: malloc grows it from the top, so the run
  // is hole + top gap, bigger than either alone
  HeapChunk *a = chunk(104, 500), *b = chunk(7000, 2000);
  a->next = b;
  HeapInfo h = walk(a, 9000, 10000);  // b ends at offset 9000 == brk
  CHECK(h.holes == 2 && h.hole_free == 2500 && h.largest_hole == 2000);
  CHECK(h.top_gap == 1000);
  CHECK(h.largest_run == 3000);
}

static void test_hole_one_byte_short_of_brk_does_not_touch() {
  HeapChunk *a = chunk(7000, 1999);
  HeapInfo h = walk(a, 9000, 10000);  // ends at 8999
  CHECK(h.largest_run == 1999);       // not 2999
}

static void test_only_touching_hole_no_top() {
  HeapChunk *a = chunk(7000, 2000);
  HeapInfo h = walk(a, 9000, 9000);
  CHECK(h.top_gap == 0 && h.largest_run == 2000);
}

static void test_cycle_is_bounded() {
  HeapChunk *a = chunk(104, 16), *b = chunk(200, 16);
  a->next = b; b->next = a;  // corrupt list
  HeapInfo h = walk(a, 9000, 9000);
  CHECK(h.truncated && h.holes == 2);  // stopped by the order check, not the cap
}

static void test_cap_stops_a_valid_but_huge_list() {
  HeapChunk *prev = nullptr, *head = nullptr;
  const size_t n = kHeapWalkMax + 100;
  for (size_t i = 0; i < n; ++i) {
    HeapChunk *c = chunk(104 + i * 16, 16);
    if (prev) prev->next = c; else head = c;
    prev = c;
  }
  HeapInfo h = walk(head, 104 + n * 16, 104 + n * 16);
  CHECK(h.truncated && h.holes == kHeapWalkMax);
}

static void test_long_list_under_cap_not_truncated() {
  // 100 adjacent-but-listed 16-byte nodes, well below the cap
  HeapChunk *prev = nullptr, *head = nullptr;
  for (int i = 0; i < 100; ++i) {
    HeapChunk *c = chunk(104 + i * 32, 16);
    if (prev) prev->next = c; else head = c;
    prev = c;
  }
  HeapInfo h = walk(head, 9000, 9000);
  CHECK(!h.truncated && h.holes == 100 && h.hole_free == 1600);
}

static void test_next_outside_pool_stops_walk() {
  // a node handed out mid-walk: next now holds user data (peripheral space)
  HeapChunk *a = chunk(104, 1000);
  a->next = (HeapChunk *)(uintptr_t)0x40001000u;
  HeapInfo h = walk(a, 9000, 9500);
  CHECK(h.truncated && h.holes == 1 && h.hole_free == 1000);
}
static void test_next_below_current_stops_walk() {
  HeapChunk *a = chunk(2000, 1000), *b = chunk(104, 1000);
  a->next = b;
  HeapInfo h = walk(a, 9000, 9500);
  CHECK(h.truncated && h.holes == 1);
}
static void test_next_unaligned_stops_walk() {
  HeapChunk *a = chunk(104, 1000);
  HeapChunk *u = chunk_raw(3001, 1000);  // a plausible-looking node, off alignment
  a->next = u;
  HeapInfo h = walk(a, 9000, 9500);
  CHECK(h.truncated && h.holes == 1);
}
static void test_size_overrunning_brk_is_rejected() {
  HeapChunk *a = chunk(104, 1000), *b = chunk(8504, 600);  // ends at 9104
  a->next = b;
  HeapInfo h = walk(a, 9000, 9500);
  CHECK(h.truncated && h.holes == 1 && h.hole_free == 1000);
  CHECK(h.largest_run == 1000);  // not 1000 + the bogus 500 gap
}
static void test_tiny_or_wild_size_is_rejected() {
  HeapChunk *a = chunk(104, 8);
  CHECK(walk(a, 9000, 9500).truncated);
  HeapChunk *b = chunk(104, (size_t)-1);
  CHECK(walk(b, 9000, 9500).truncated);
}
static void test_overlapping_chunks_are_rejected() {
  HeapChunk *a = chunk(104, 1000), *b = chunk(600, 400);  // inside a
  a->next = b;
  HeapInfo h = walk(a, 9000, 9500);
  CHECK(h.truncated && h.holes == 1);
}
static void test_head_outside_pool_or_at_brk() {
  HeapChunk *a = chunk(104, 1000);
  CHECK(walk(a, 9000, 9500, 200).truncated);  // below heap_start
  HeapChunk *b = chunk(9000, 16);             // starts at brk
  CHECK(walk(b, 9000, 9500).truncated);
}
static void test_heap_end_below_brk_has_no_top() {
  HeapInfo h = walk(nullptr, 9000, 8000);
  CHECK(h.top_gap == 0 && h.largest_run == 0);
}

int main() {
  test_empty_list();
  test_brk_at_end_nothing_left();
  test_one_coalesced_hole_below_brk();
  test_fragmented_total_large_run_small();
  test_top_larger_than_holes();
  test_hole_touching_brk_extends_the_top();
  test_hole_one_byte_short_of_brk_does_not_touch();
  test_only_touching_hole_no_top();
  test_cycle_is_bounded();
  test_cap_stops_a_valid_but_huge_list();
  test_long_list_under_cap_not_truncated();
  test_next_outside_pool_stops_walk();
  test_next_below_current_stops_walk();
  test_next_unaligned_stops_walk();
  test_size_overrunning_brk_is_rejected();
  test_tiny_or_wild_size_is_rejected();
  test_overlapping_chunks_are_rejected();
  test_head_outside_pool_or_at_brk();
  test_heap_end_below_brk_has_no_top();
  (void)kPool;
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
