// Host tests for the Buchla 227e 14-byte preset-record codec
// (src/Buchla227eSlotCodec.cpp). No hardware, no bus.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buchla227e_slot_codec
//   test_buchla227e_slot_codec.cpp ../src/Buchla227eSlotCodec.cpp &&
//   ./build/test_buchla227e_slot_codec
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Buchla227eSlotCodec.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// The core invariant: Encode(Decode(x)) == x for ANY 14 bytes, so a captured
// slot can be sliced out and written back byte-for-byte.
static void RoundTrip(const uint8_t *in) {
  Buchla227eSlot slot;
  Buchla227eDecodeSlot(in, slot);
  uint8_t out[kBuchla227eRecordBytes];
  Buchla227eEncodeSlot(slot, out);
  CHECK(memcmp(in, out, kBuchla227eRecordBytes) == 0);
}

static void test_geometry() {
  printf("test_geometry\n");
  CHECK(kBuchla227eRecordBytes == 14);
  CHECK(kBuchla227eSlotsPerBank == 30);
  CHECK(kBuchla227eBankBytes == 420);
  CHECK(Buchla227eSlotOffset(0) == 0);
  CHECK(Buchla227eSlotOffset(1) == 14);
  CHECK(Buchla227eSlotOffset(29) == 406);
  CHECK(Buchla227eSlotOffset(29) + kBuchla227eRecordBytes == kBuchla227eBankBytes);
}

static void test_roundtrip() {
  printf("test_roundtrip\n");
  uint8_t zeros[14] = {0};
  RoundTrip(zeros);
  uint8_t ffs[14]; memset(ffs, 0xFF, sizeof(ffs));
  RoundTrip(ffs);
  uint8_t ramp[14];
  for (int i = 0; i < 14; ++i) ramp[i] = (uint8_t)(i * 17 + 3);
  RoundTrip(ramp);
}

static void test_slice_a_bank() {
  printf("test_slice_a_bank\n");
  // A synthetic 420-byte bank where byte 0 of each record is the slot number,
  // then decode/encode each slot and confirm the bank rebuilds exactly.
  uint8_t bank[kBuchla227eBankBytes];
  for (int s = 0; s < kBuchla227eSlotsPerBank; ++s)
    for (int i = 0; i < kBuchla227eRecordBytes; ++i)
      bank[Buchla227eSlotOffset(s) + i] = (uint8_t)((s * 7 + i) & 0xFF);

  uint8_t rebuilt[kBuchla227eBankBytes];
  for (int s = 0; s < kBuchla227eSlotsPerBank; ++s) {
    Buchla227eSlot slot;
    Buchla227eDecodeSlot(bank + Buchla227eSlotOffset(s), slot);
    Buchla227eEncodeSlot(slot, rebuilt + Buchla227eSlotOffset(s));
  }
  CHECK(memcmp(bank, rebuilt, kBuchla227eBankBytes) == 0);
}

int main() {
  test_geometry();
  test_roundtrip();
  test_slice_a_bank();
  printf("\ntest_buchla227e_slot_codec: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
