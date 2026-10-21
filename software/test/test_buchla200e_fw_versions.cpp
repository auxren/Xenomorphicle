// Host tests for the per-address firmware-version cache
// (src/Buchla200eFwVersions.cpp). Pure state; no hardware, no bus.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buchla200e_fw_versions
//   test_buchla200e_fw_versions.cpp ../src/Buchla200eFwVersions.cpp &&
//   ./build/test_buchla200e_fw_versions
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Buchla200eFwVersions.h"

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

// The frame a 251e v307 at 0x5C builds (Buchla_FW docs, 251e dispatch §6):
// [0A 22 5C 13] "30.7   " -- four characters and three spaces.
static const uint8_t k251e[7] = {'3', '0', '.', '7', ' ', ' ', ' '};
// A 259e v312 at 0x28: "31.2   ".
static const uint8_t k259e[7] = {'3', '1', '.', '2', ' ', ' ', ' '};

static void test_empty() {
  printf("test_empty\n");
  Buchla200eFwVersionReset();
  char out[16] = "xx";
  CHECK(Buchla200eFwVersionCount() == 0);
  CHECK(!Buchla200eFwVersionKnown(0x5C));
  CHECK(Buchla200eFwVersionGet(0x5C, out, sizeof(out)) == 0);
  CHECK(out[0] == 0);                       // unknown reads as an empty string
  CHECK(Buchla200eFwVersionGeneration() == 0);
  CHECK(Buchla200eFwVersionPack(0x5C) == 0);
}

static void test_note_and_get() {
  printf("test_note_and_get\n");
  Buchla200eFwVersionReset();
  Buchla200eFwVersionNote(0x5C, k251e, 7);
  char out[16];
  CHECK(Buchla200eFwVersionKnown(0x5C));
  CHECK(Buchla200eFwVersionCount() == 1);
  CHECK(Buchla200eFwVersionGet(0x5C, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "30.7") == 0);          // trailing spaces trimmed
  CHECK(Buchla200eFwVersionGeneration() == 1);

  // a second module is a second entry, not a replacement (unlike the WPM)
  Buchla200eFwVersionNote(0x28, k259e, 7);
  CHECK(Buchla200eFwVersionCount() == 2);
  CHECK(Buchla200eFwVersionGet(0x28, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "31.2") == 0);
  CHECK(Buchla200eFwVersionGet(0x5C, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "30.7") == 0);
  CHECK(Buchla200eFwVersionGeneration() == 2);
}

static void test_last_wins_and_generation() {
  printf("test_last_wins_and_generation\n");
  Buchla200eFwVersionReset();
  Buchla200eFwVersionNote(0x5C, k251e, 7);
  CHECK(Buchla200eFwVersionGeneration() == 1);

  // the same text again: nothing changed, nothing to persist
  Buchla200eFwVersionNote(0x5C, k251e, 7);
  CHECK(Buchla200eFwVersionGeneration() == 1);
  CHECK(Buchla200eFwVersionCount() == 1);

  // reflashed: the new text replaces the old at that address
  const uint8_t after[7] = {'3', '0', '.', '8', ' ', ' ', ' '};
  Buchla200eFwVersionNote(0x5C, after, 7);
  char out[16];
  CHECK(Buchla200eFwVersionGet(0x5C, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "30.8") == 0);
  CHECK(Buchla200eFwVersionGeneration() == 2);
  CHECK(Buchla200eFwVersionCount() == 1);
}

static void test_lengths_and_bytes() {
  printf("test_lengths_and_bytes\n");
  Buchla200eFwVersionReset();

  // nothing to say: ignored entirely
  Buchla200eFwVersionNote(0x10, k251e, 0);
  Buchla200eFwVersionNote(0x10, nullptr, 7);
  CHECK(!Buchla200eFwVersionKnown(0x10));
  CHECK(Buchla200eFwVersionGeneration() == 0);

  // shorter than the wire form is padded, longer is truncated
  const uint8_t four[4] = {'3', '1', '.', '0'};
  Buchla200eFwVersionNote(0x11, four, 4);
  char out[16];
  CHECK(Buchla200eFwVersionGet(0x11, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "31.0") == 0);
  const uint8_t nine[9] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I'};
  Buchla200eFwVersionNote(0x12, nine, 9);
  CHECK(Buchla200eFwVersionGet(0x12, out, sizeof(out)) == BUCHLA200E_FWVER_LEN);
  CHECK(strcmp(out, "ABCDEFG") == 0);

  // the 251e's Studio H release strings carry a suffix: "31.0C1"
  const uint8_t c1[7] = {'3', '1', '.', '0', 'C', '1', ' '};
  Buchla200eFwVersionNote(0x13, c1, 7);
  CHECK(Buchla200eFwVersionGet(0x13, out, sizeof(out)) == 6);
  CHECK(strcmp(out, "31.0C1") == 0);

  // a byte that is not text is shown, not hidden, and never ends the string
  const uint8_t junk[7] = {'3', 0x00, '.', 0xFF, ' ', ' ', ' '};
  Buchla200eFwVersionNote(0x14, junk, 7);
  CHECK(Buchla200eFwVersionGet(0x14, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "3...") == 0);

  // a 272e at 0x5E, bench 2026-10-20: [33 31 2E 30 43 31 00] -- six
  // characters and its C-string terminator, where the 259e sends a space.
  // The NUL is padding, not a seventh character, so no trailing '.'.
  const uint8_t k272e[7] = {'3', '1', '.', '0', 'C', '1', 0x00};
  Buchla200eFwVersionNote(0x5E, k272e, 7);
  CHECK(Buchla200eFwVersionGet(0x5E, out, sizeof(out)) == 6);
  CHECK(strcmp(out, "31.0C1") == 0);
  // mixed padding, and an all-padding string, both trim to nothing extra
  const uint8_t mixed[7] = {'3', '0', '.', '7', 0x00, ' ', 0x00};
  Buchla200eFwVersionNote(0x15, mixed, 7);
  CHECK(Buchla200eFwVersionGet(0x15, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "30.7") == 0);
  const uint8_t blank[7] = {0x00, ' ', 0x00, ' ', 0x00, ' ', 0x00};
  Buchla200eFwVersionNote(0x16, blank, 7);
  CHECK(Buchla200eFwVersionKnown(0x16));           // heard, but nothing to show
  CHECK(Buchla200eFwVersionGet(0x16, out, sizeof(out)) == 0);
  CHECK(out[0] == 0);

  // a tight output buffer is honoured and still terminated
  char tiny[3] = "zz";
  CHECK(Buchla200eFwVersionGet(0x12, tiny, sizeof(tiny)) == 2);
  CHECK(strcmp(tiny, "AB") == 0);
  CHECK(Buchla200eFwVersionGet(0x12, tiny, 0) == 0);
  CHECK(Buchla200eFwVersionGet(0x12, nullptr, 8) == 0);

  // the address is 7-bit on the wire; a set high bit is the same module
  CHECK(Buchla200eFwVersionKnown(0x11 | 0x80));
}

static void test_eviction() {
  printf("test_eviction\n");
  Buchla200eFwVersionReset();
  for (int i = 0; i < BUCHLA200E_FWVER_SLOTS; ++i)
    Buchla200eFwVersionNote((uint8_t)(0x20 + i), k251e, 7);
  CHECK(Buchla200eFwVersionCount() == BUCHLA200E_FWVER_SLOTS);

  // hearing 0x20 again makes it the freshest, so 0x21 is now the stalest
  Buchla200eFwVersionNote(0x20, k251e, 7);
  Buchla200eFwVersionNote(0x7F, k259e, 7);
  CHECK(Buchla200eFwVersionCount() == BUCHLA200E_FWVER_SLOTS);
  CHECK(Buchla200eFwVersionKnown(0x20));
  CHECK(!Buchla200eFwVersionKnown(0x21));
  CHECK(Buchla200eFwVersionKnown(0x7F));
}

static void test_pack_restore() {
  printf("test_pack_restore\n");
  Buchla200eFwVersionReset();
  Buchla200eFwVersionNote(0x5C, k251e, 7);
  const uint64_t packed = Buchla200eFwVersionPack(0x5C);
  CHECK(packed != 0);
  CHECK((uint8_t)(packed >> 0) == '3');       // the bytes as sent, low first
  CHECK((uint8_t)(packed >> 24) == '7');
  CHECK((uint8_t)(packed >> 48) == ' ');
  CHECK((uint8_t)(packed >> 56) != 0);        // marker: a known entry is never 0

  // a fresh session restores it, and that is not news from the bus
  Buchla200eFwVersionReset();
  CHECK(Buchla200eFwVersionRestore(0x5C, packed));
  char out[16];
  CHECK(Buchla200eFwVersionGet(0x5C, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "30.7") == 0);
  CHECK(Buchla200eFwVersionGeneration() == 0);
  CHECK(Buchla200eFwVersionPack(0x5C) == packed);

  // a word that was never a version is refused
  Buchla200eFwVersionReset();
  CHECK(!Buchla200eFwVersionRestore(0x5C, 0));
  CHECK(!Buchla200eFwVersionRestore(0x5C, 0x0000000020372E3033ULL));  // no marker
  CHECK(!Buchla200eFwVersionKnown(0x5C));

  // what the bus said this session beats what storage remembers
  Buchla200eFwVersionReset();
  Buchla200eFwVersionNote(0x5C, k259e, 7);
  CHECK(!Buchla200eFwVersionRestore(0x5C, packed));
  CHECK(Buchla200eFwVersionGet(0x5C, out, sizeof(out)) == 4);
  CHECK(strcmp(out, "31.2") == 0);

  // ...but a later frame still replaces a restored entry, and IS news
  Buchla200eFwVersionReset();
  CHECK(Buchla200eFwVersionRestore(0x5C, packed));
  Buchla200eFwVersionNote(0x5C, k259e, 7);
  CHECK(Buchla200eFwVersionGeneration() == 1);
  CHECK(strcmp((Buchla200eFwVersionGet(0x5C, out, sizeof(out)), out), "31.2") == 0);
}

int main() {
  test_empty();
  test_note_and_get();
  test_last_wins_and_generation();
  test_lengths_and_bytes();
  test_eviction();
  test_pack_restore();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
