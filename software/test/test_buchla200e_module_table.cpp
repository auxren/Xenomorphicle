// Host tests for the Buchla 200e address->model table
// (src/Buchla200eModuleTable.cpp). Pure lookup logic; no hardware, no bus.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buchla200e_module_table
//   test_buchla200e_module_table.cpp ../src/Buchla200eModuleTable.cpp &&
//   ./build/test_buchla200e_module_table
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Buchla200eModuleTable.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++checks;                                                          \
    if (!(cond)) {                                                     \
      ++failures;                                                      \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                  \
  } while (0)

// The two addresses this project has actually probed on real hardware --
// these are the ones a regression would hurt most.
static void test_bench_confirmed_addresses() {
  printf("test_bench_confirmed_addresses\n");
  const char *m5c = Buchla200eModelForAddress(0x5C);
  CHECK(m5c != nullptr);
  CHECK(m5c && strcmp(m5c, "251 A") == 0);

  const char *m28 = Buchla200eModelForAddress(0x28);
  CHECK(m28 != nullptr);
  CHECK(m28 && strcmp(m28, "259 A") == 0);
}

static void test_spot_checks_across_table() {
  printf("test_spot_checks_across_table\n");
  const char *first = Buchla200eModelForAddress(0x10);
  CHECK(first && strcmp(first, "257 A") == 0);

  const char *last = Buchla200eModelForAddress(0x72);
  CHECK(last && strcmp(last, "266h") == 0);

  // Longest name in the table -- guards the name[10] buffer size.
  const char *longest = Buchla200eModelForAddress(0x4E);
  CHECK(longest && strcmp(longest, "285 BM A") == 0);

  // Our own module's default address collides with a 281e C1. Documented
  // in the header; pinned here so the collision can't vanish silently.
  const char *ours = Buchla200eModelForAddress(0x3C);
  CHECK(ours && strcmp(ours, "281 C1") == 0);
}

static void test_unknown_addresses_return_null() {
  printf("test_unknown_addresses_return_null\n");
  // 0x22 is deliberately absent: a 225s there would collide with the preset
  // manager's own source address, and Studio H commented it out too.
  CHECK(Buchla200eModelForAddress(0x22) == nullptr);
  CHECK(Buchla200eModelForAddress(0x00) == nullptr);
  CHECK(Buchla200eModelForAddress(0x01) == nullptr);
  CHECK(Buchla200eModelForAddress(0x7F) == nullptr);
  CHECK(Buchla200eModelForAddress(0xFF) == nullptr);
  // Gaps inside the populated range, not just outside it.
  CHECK(Buchla200eModelForAddress(0x43) == nullptr);
  CHECK(Buchla200eModelForAddress(0x61) == nullptr);
}

static void test_table_invariants() {
  printf("test_table_invariants\n");
  const int n = Buchla200eModuleCount();
  CHECK(n == 61);

  CHECK(Buchla200eModuleAt(-1) == nullptr);
  CHECK(Buchla200eModuleAt(n) == nullptr);
  CHECK(Buchla200eModuleAt(0) != nullptr);
  CHECK(Buchla200eModuleAt(n - 1) != nullptr);

  uint8_t prev = 0;
  for (int i = 0; i < n; ++i) {
    const Buchla200eModuleEntry *e = Buchla200eModuleAt(i);
    CHECK(e != nullptr);
    if (!e) continue;

    // Strictly ascending: catches both a duplicate address (which would make
    // the lookup order-dependent) and an out-of-order transcription slip.
    if (i > 0) CHECK(e->addr > prev);
    prev = e->addr;

    // Every name NUL-terminated within the buffer, and non-empty.
    bool terminated = false;
    for (size_t c = 0; c < sizeof(e->name); ++c) {
      if (e->name[c] == '\0') { terminated = true; break; }
    }
    CHECK(terminated);
    CHECK(e->name[0] != '\0');

    // Every table entry must be reachable through the by-address lookup.
    const char *viaLookup = Buchla200eModelForAddress(e->addr);
    CHECK(viaLookup != nullptr);
    CHECK(viaLookup && strcmp(viaLookup, e->name) == 0);
  }
}

int main() {
  test_bench_confirmed_addresses();
  test_spot_checks_across_table();
  test_unknown_addresses_return_null();
  test_table_invariants();

  printf("\ntest_buchla200e_module_table: %d checks, %d failures\n",
         checks, failures);
  return failures == 0 ? 0 : 1;
}
