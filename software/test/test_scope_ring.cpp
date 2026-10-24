// Host tests for the Scope's decimating min/max ring and TR edge ring
// (src/ScopeRing.h, header-only).
#include <cstdio>

#include "../src/ScopeRing.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                          \
  } while (0)

static ScopeRing ring;  // 4 KB: static, as on the target

static void test_decim1_passthrough() {
  printf("test_decim1_passthrough\n");
  ring.Reset(1);
  for (int k = 0; k < 10; ++k) ring.Push((int16_t)k);
  CHECK(ring.Committed() == 10);
  CHECK(ring.Decim() == 1);
  for (int k = 0; k < 10; ++k) {
    const ScopeBucket b = ring.At((uint32_t)k);
    CHECK(b.lo == k && b.hi == k);
  }
}

static void test_decim_minmax() {
  printf("test_decim_minmax\n");
  ring.Reset(4);
  const int16_t in[8] = {5, -3, 7, 1, 2, 2, 2, 2};
  for (int16_t s : in) ring.Push(s);
  CHECK(ring.Committed() == 2);
  CHECK(ring.At(0).lo == -3 && ring.At(0).hi == 7);
  CHECK(ring.At(1).lo == 2 && ring.At(1).hi == 2);
  ring.Push(9);  // an open bucket is not committed
  CHECK(ring.Committed() == 2);
}

static void test_wraps() {
  printf("test_wraps\n");
  ring.Reset(1);
  const int total = ScopeRing::kLen + 5;
  for (int k = 0; k < total; ++k) ring.Push((int16_t)(k & 0x3FFF));
  CHECK(ring.Committed() == (uint32_t)total);
  for (uint32_t n = (uint32_t)(total - ScopeRing::kLen); n < (uint32_t)total; ++n)
    CHECK(ring.At(n).lo == (int16_t)(n & 0x3FFF));
}

static void test_reset_clears() {
  printf("test_reset_clears\n");
  ring.Reset(1);
  for (int k = 0; k < 50; ++k) ring.Push(1000);
  ring.Reset(2);
  CHECK(ring.Committed() == 0);
  CHECK(ring.Decim() == 2);
  ring.Push(-5);
  ring.Push(3);
  CHECK(ring.Committed() == 1);
  CHECK(ring.At(0).lo == -5 && ring.At(0).hi == 3);  // no 1000 left in it
  ring.Reset(0);  // decim 0 is clamped to 1
  CHECK(ring.Decim() == 1);
}

static void test_edge_ring_rate_limit() {
  printf("test_edge_ring_rate_limit\n");
  ScopeEdgeRing e;
  e.Reset();
  CHECK(e.Count() == 0);
  e.Note(0, 15000);
  e.Note(10, 15000);
  e.Note(20000, 15000);
  CHECK(e.Count() == 2);
  CHECK(e.At(0) == 0 && e.At(1) == 20000);

  e.Reset();
  for (uint32_t k = 0; k < 200; ++k) e.Note(k * 20000u, 15000);
  CHECK(e.Count() == 200);
  CHECK(e.At(e.Count() - 1) == 199u * 20000u);

  e.Reset();  // the cycle counter wraps
  e.Note(0xFFFFFF00u, 1000);
  e.Note(0x00000500u, 1000);
  CHECK(e.Count() == 2);
}

int main() {
  test_decim1_passthrough();
  test_decim_minmax();
  test_wraps();
  test_reset_clears();
  test_edge_ring_rate_limit();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
