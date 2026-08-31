// Host tests for the 200e whole-bank write guard
// (src/Buchla200eWriteGuard.cpp). No hardware, no bus.
//
// These guards are the only thing standing between an edit and overwriting 30
// real presets, so the emphasis is on the REFUSAL cases: every field that can
// block a write is tested in isolation, and the permit case is verified to
// need all of them simultaneously.
//
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buchla200e_write_guard
//   test_buchla200e_write_guard.cpp ../src/Buchla200eWriteGuard.cpp &&
//   ./build/test_buchla200e_write_guard
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Buchla200eWriteGuard.h"

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

// A context that passes every guard. Each test below breaks exactly one
// field, which is what makes a failure point at a single cause.
static Buchla200eWriteContext Good() {
  Buchla200eWriteContext c;
  c.have_read = true;
  c.read_addr = 0x5C;
  c.read_type = 1;            // MODTYPE_251E
  c.target_addr = 0x5C;
  c.target_type = 1;
  c.bytes_transferred = 63120;  // 30 * 2104
  c.expected_bank_bytes = 63120;
  c.card_serving = true;
  c.image_valid = true;
  c.master_idle = true;
  c.changed_bytes = 5;
  return c;
}

static void test_permits_only_when_everything_holds() {
  printf("test_permits_only_when_everything_holds\n");
  CHECK(Buchla200eCheckWrite(Good()) == BUCHLA200E_WRITE_OK);
}

static void test_requires_a_read() {
  printf("test_requires_a_read\n");
  Buchla200eWriteContext c = Good();
  c.have_read = false;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_NO_READ);
}

static void test_read_must_be_of_this_module() {
  printf("test_read_must_be_of_this_module\n");
  // Read 0x5C then retarget 0x28: the resident image is a 251e bank and the
  // target is a 259e. This is the case that would push 63,120 bytes of the
  // wrong module's data into a module expecting 990.
  Buchla200eWriteContext c = Good();
  c.target_addr = 0x28;
  c.target_type = 2;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_WRONG_MODULE);

  // Same address, different type -- a clone can squat an address, so address
  // alone must not be treated as proof of identity.
  c = Good();
  c.target_type = 2;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_WRONG_MODULE);

  // Different address, same type: still another module's bank.
  c = Good();
  c.target_addr = 0x5D;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_WRONG_MODULE);
}

static void test_short_read_blocks_the_write() {
  printf("test_short_read_blocks_the_write\n");
  // The real incident: one record short, reported as success.
  Buchla200eWriteContext c = Good();
  c.expected_bank_bytes = 990;
  c.bytes_transferred = 957;   // 990 - 33
  c.read_type = 2; c.target_type = 2;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_SHORT_READ);

  // One byte short is still short.
  c = Good();
  c.bytes_transferred = 63119;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_SHORT_READ);

  // Zero expected bytes means the caller could not size the bank -- refuse
  // rather than treat "0 >= 0" as a pass.
  c = Good();
  c.expected_bank_bytes = 0;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_SHORT_READ);

  // A longer-than-expected transfer is fine: the bank is a prefix of it.
  c = Good();
  c.bytes_transferred = 65536;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_OK);
}

static void test_image_must_still_exist() {
  printf("test_image_must_still_exist\n");
  Buchla200eWriteContext c = Good();
  c.card_serving = false;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_NO_IMAGE);

  c = Good();
  c.image_valid = false;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_NO_IMAGE);
}

static void test_busy_blocks_and_outranks_everything() {
  printf("test_busy_blocks_and_outranks_everything\n");
  Buchla200eWriteContext c = Good();
  c.master_idle = false;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_BUSY);

  // Busy is checked first on purpose: starting a transfer while another is in
  // flight corrupts both, so it must win even when other things are also wrong.
  c = Good();
  c.master_idle = false;
  c.have_read = false;
  c.card_serving = false;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_BUSY);
}

static void test_no_changes_is_refused_not_permitted() {
  printf("test_no_changes_is_refused_not_permitted\n");
  Buchla200eWriteContext c = Good();
  c.changed_bytes = 0;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_NO_CHANGES);

  // A single changed byte is a real write.
  c = Good();
  c.changed_bytes = 1;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_OK);
}

static void test_diff_overflow_never_reads_as_no_changes() {
  printf("test_diff_overflow_never_reads_as_no_changes\n");
  // Buchla251eDiffSlot returns -1 when the patch list overflows. Treating a
  // negative as "nothing changed" would silently permit an unverified write;
  // treating it as falsy (0) would too. It must block.
  Buchla200eWriteContext c = Good();
  c.changed_bytes = -1;
  const Buchla200eWriteBlock b = Buchla200eCheckWrite(c);
  CHECK(b != BUCHLA200E_WRITE_OK);
  CHECK(b != BUCHLA200E_WRITE_NO_CHANGES);
}

static void test_ordering_reports_the_root_cause_first() {
  printf("test_ordering_reports_the_root_cause_first\n");
  // No read AND nothing changed: "read the module first" is the useful
  // message; "no changes" would be technically true and actively misleading.
  Buchla200eWriteContext c = Good();
  c.have_read = false;
  c.changed_bytes = 0;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_NO_READ);

  // Wrong module AND short read: wrong module is the more fundamental error.
  c = Good();
  c.target_addr = 0x28;
  c.bytes_transferred = 100;
  CHECK(Buchla200eCheckWrite(c) == BUCHLA200E_WRITE_WRONG_MODULE);
}

static void test_every_block_has_distinct_text() {
  printf("test_every_block_has_distinct_text\n");
  const Buchla200eWriteBlock all[] = {
      BUCHLA200E_WRITE_OK,        BUCHLA200E_WRITE_NO_READ,
      BUCHLA200E_WRITE_WRONG_MODULE, BUCHLA200E_WRITE_SHORT_READ,
      BUCHLA200E_WRITE_NO_IMAGE,  BUCHLA200E_WRITE_BUSY,
      BUCHLA200E_WRITE_NO_CHANGES};
  const int n = (int)(sizeof(all) / sizeof(all[0]));
  for (int i = 0; i < n; ++i) {
    const char *a = Buchla200eWriteBlockText(all[i]);
    CHECK(a != nullptr);
    CHECK(a && a[0] != '\0');
    // Must fit a 21-character OLED line.
    CHECK(a && strlen(a) <= 21);
    for (int j = i + 1; j < n; ++j) {
      const char *b = Buchla200eWriteBlockText(all[j]);
      CHECK(a && b && strcmp(a, b) != 0);
    }
  }
}

int main() {
  test_permits_only_when_everything_holds();
  test_requires_a_read();
  test_read_must_be_of_this_module();
  test_short_read_blocks_the_write();
  test_image_must_still_exist();
  test_busy_blocks_and_outranks_everything();
  test_no_changes_is_refused_not_permitted();
  test_diff_overflow_never_reads_as_no_changes();
  test_ordering_reports_the_root_cause_first();
  test_every_block_has_distinct_text();

  printf("\ntest_buchla200e_write_guard: %d checks, %d failures\n", checks,
         failures);
  return failures == 0 ? 0 : 1;
}
