// Host tests for the Buchla 251e single-slot codec (src/Buchla251eSlotCodec.cpp)
// and the Euclidean generator (src/Buchla251eGenerator.cpp), against the
// real bjorklund.h/.cpp Euclidean pattern implementation (no re-
// implementation of the pattern math) via a minimal host-only Arduino.h stub
// for the PROGMEM it needs. No hardware, no bus.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -I host_stubs -o build/test_buchla251e_slot_codec
//   test_buchla251e_slot_codec.cpp ../src/Buchla251eSlotCodec.cpp
//   ../src/Buchla251eGenerator.cpp ../src/src/extern/bjorklund.cpp &&
//   ./build/test_buchla251e_slot_codec
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "../src/Buchla251eSlotCodec.h"
#include "../src/Buchla251eGenerator.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// ---- roundtrip identity -----------------------------------------------------

static void FillPattern(uint8_t *buf, int n, uint8_t seed) {
  for (int i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>((seed + i * 7) & 0xff);
}

static void test_roundtrip_zeroed(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0, sizeof(bytes));

  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);
  uint8_t out[kBuchla251eSlotBytes];
  Buchla251eEncodeSlot(slot, out);

  CHECK(memcmp(bytes, out, kBuchla251eSlotBytes) == 0);
}

static void test_roundtrip_all_0xff(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0xff, sizeof(bytes));

  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);
  uint8_t out[kBuchla251eSlotBytes];
  Buchla251eEncodeSlot(slot, out);

  CHECK(memcmp(bytes, out, kBuchla251eSlotBytes) == 0);
}

static void test_roundtrip_synthetic_realistic(void) {
  // A synthetic but plausible slot: non-default header, one sequence with
  // real-looking stage values/times/end-marker, unknown bytes carrying
  // non-zero "garbage" to prove they survive raw.
  uint8_t bytes[kBuchla251eSlotBytes];
  FillPattern(bytes, kBuchla251eSlotBytes, 0x11);

  // Real-looking float header (not that decode/encode interprets it -- it's
  // raw passthrough -- but keep it plausible: won't affect the check).
  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);
  uint8_t out[kBuchla251eSlotBytes];
  Buchla251eEncodeSlot(slot, out);

  CHECK(memcmp(bytes, out, kBuchla251eSlotBytes) == 0);
}

static void test_roundtrip_many_random_seeds(void) {
  // A handful of different fill patterns, not just 0x00/0xff/one fixed
  // pattern -- catches any offset-by-one or field-width bug the three fixed
  // cases above might miss.
  for (uint8_t seed = 1; seed < 40; seed += 5) {
    uint8_t bytes[kBuchla251eSlotBytes];
    FillPattern(bytes, kBuchla251eSlotBytes, seed);
    Buchla251eSlot slot;
    Buchla251eDecodeSlot(bytes, slot);
    uint8_t out[kBuchla251eSlotBytes];
    Buchla251eEncodeSlot(slot, out);
    CHECK(memcmp(bytes, out, kBuchla251eSlotBytes) == 0);
  }
}

// ---- decoded field sanity ---------------------------------------------------

static void test_decode_field_offsets(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0, sizeof(bytes));
  // Sequence A (block 0) starts right after the 16-byte header. Stage 2
  // (index 2) starts at block_off + 2*10.
  const int seqA_off = kBuchla251eSlotHeaderBytes;
  const int stage2_off = seqA_off + 2 * kBuchla251eStageBytes;
  bytes[stage2_off + 0] = 42;              // value
  bytes[stage2_off + 1] = 0x55;            // pad
  bytes[stage2_off + 2] = 0x34;            // time low byte
  bytes[stage2_off + 3] = 0x12;            // time high byte -> time = 0x1234
  bytes[stage2_off + 4 + 3] = 0x0A;        // reserved[3] = end marker

  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);

  const Buchla251eStage &s = slot.sequences[0].stages[2];
  CHECK(s.value == 42);
  CHECK(s.pad == 0x55);
  CHECK(s.time == 0x1234);
  CHECK(Buchla251eHasEndMarker(s));

  // Sequence D (block 3, last) trailer byte +1.
  const int seqD_off = kBuchla251eSlotHeaderBytes + 3 * kBuchla251eSequenceBlockBytes;
  const int trailer_off = seqD_off + kBuchla251eStagesPerSequence * kBuchla251eStageBytes;
  bytes[trailer_off + kBuchla251eSequenceParamOffset] = 0x8c;
  Buchla251eDecodeSlot(bytes, slot);
  CHECK(Buchla251eGetSequenceParam(slot.sequences[3]) == 0x8c);
}

// ---- end marker helpers ----------------------------------------------------

static void test_end_marker_roundtrip(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0, sizeof(bytes));

  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);
  CHECK(!Buchla251eHasEndMarker(slot.sequences[0].stages[15]));

  Buchla251eSetEndMarker(slot.sequences[0].stages[15], true);
  CHECK(Buchla251eHasEndMarker(slot.sequences[0].stages[15]));

  uint8_t out[kBuchla251eSlotBytes];
  Buchla251eEncodeSlot(slot, out);
  const int off = kBuchla251eSlotHeaderBytes + 15 * kBuchla251eStageBytes + 4 + 3;
  CHECK(out[off] == 0x0A);

  Buchla251eSlot slot2;
  Buchla251eDecodeSlot(out, slot2);
  CHECK(Buchla251eHasEndMarker(slot2.sequences[0].stages[15]));

  Buchla251eSetEndMarker(slot2.sequences[0].stages[15], false);
  CHECK(!Buchla251eHasEndMarker(slot2.sequences[0].stages[15]));
}

// ---- volts <-> raw ----------------------------------------------------------

static void test_volts_conversion(void) {
  CHECK(Buchla251eVoltsToRaw(0.0f) == 0);
  CHECK(Buchla251eVoltsToRaw(1.3f) == 13);
  CHECK(Buchla251eRawToVolts(0) == 0.0f);
  CHECK(Buchla251eRawToVolts(13) > 1.29f && Buchla251eRawToVolts(13) < 1.31f);

  // Clamping: above 25.5V (raw would exceed 255) clamps to 255; below 0V
  // clamps to 0.
  CHECK(Buchla251eVoltsToRaw(30.0f) == 255);
  CHECK(Buchla251eVoltsToRaw(-5.0f) == 0);
}

// ---- diff / patch list ------------------------------------------------------

static void test_diff_no_changes(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  FillPattern(bytes, kBuchla251eSlotBytes, 3);
  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);

  Buchla251eBytePatch patches[16];
  int n = Buchla251eDiffSlot(bytes, slot, patches, 16);
  CHECK(n == 0);
}

static void test_diff_single_stage_change(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0, sizeof(bytes));
  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);

  // Edit stage 5's value in sequence B (block 1).
  slot.sequences[1].stages[5].value = 77;

  Buchla251eBytePatch patches[16];
  int n = Buchla251eDiffSlot(bytes, slot, patches, 16);
  CHECK(n == 1);
  const int expected_off = kBuchla251eSlotHeaderBytes + 1 * kBuchla251eSequenceBlockBytes
      + 5 * kBuchla251eStageBytes + 0;
  CHECK(patches[0].offset == expected_off);
  CHECK(patches[0].value == 77);
}

static void test_diff_overflow_returns_negative_one(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0, sizeof(bytes));
  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);

  // Touch every stage's value across all 4 sequences -- 200 changed bytes,
  // far more than a deliberately-too-small buffer of 3.
  for (int seq = 0; seq < kBuchla251eSequencesPerSlot; ++seq) {
    for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
      slot.sequences[seq].stages[i].value = static_cast<uint8_t>(i + 1);
    }
  }

  Buchla251eBytePatch patches[3];
  int n = Buchla251eDiffSlot(bytes, slot, patches, 3);
  CHECK(n == -1);
}

// ---- Euclidean generator ----------------------------------------------------

static void test_generate_euclid_length8_fill3(void) {
  // Real bjorklund output for (8 steps, 3 beats, rotation 0), taken directly
  // from src/src/extern/bjorklund.cpp's own lookup table (comment "3 beats
  // 10010010", value 73 = 0b01001001, i.e. bit0/bit3/bit6 active) -- not
  // guessed, read from the actual table this generator links against.
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0, sizeof(bytes));
  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);
  Buchla251eSequence &seq = slot.sequences[0];

  // Seed stages beyond the working length, and non-value fields of in-range
  // stages, with non-default data to prove the generator leaves them alone.
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    seq.stages[i].pad = 0x42;
    seq.stages[i].time = 999;
    seq.stages[i].reserved[0] = 0x77;
  }
  for (int i = 8; i < kBuchla251eStagesPerSequence; ++i) {
    seq.stages[i].value = 0xAB;
  }
  // A stray end marker inside the OLD range that should be cleared by the
  // new generation (it lands within [0, length-1] = [0,7] this time, so it
  // must not survive).
  Buchla251eSetEndMarker(seq.stages[2], true);
  // ...and one outside the new working length that must also be cleared.
  Buchla251eSetEndMarker(seq.stages[20], true);

  Buchla251eEuclidParams params;
  params.length = 8;
  params.fill = 3;
  params.rotation = 0;
  params.base_volts = 2.0f;   // raw 20
  params.rest_volts = 0.0f;   // raw 0

  Buchla251eGenerateEuclid(params, seq);

  const bool expected_active[8] = {true, false, false, true, false, false, true, false};
  for (int i = 0; i < 8; ++i) {
    CHECK(seq.stages[i].value == (expected_active[i] ? 20 : 0));
    // Non-value fields of in-range stages are untouched.
    CHECK(seq.stages[i].pad == 0x42);
    CHECK(seq.stages[i].time == 999);
    CHECK(seq.stages[i].reserved[0] == 0x77);
  }

  // End marker lands exactly at index length-1 = 7 and nowhere else.
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    CHECK(Buchla251eHasEndMarker(seq.stages[i]) == (i == 7));
  }

  // Stages beyond the working length are completely untouched (value
  // included -- the generator must not have zeroed or overwritten them).
  for (int i = 8; i < kBuchla251eStagesPerSequence; ++i) {
    CHECK(seq.stages[i].value == 0xAB);
  }
}

static void test_generate_euclid_length_clamped_above_32(void) {
  uint8_t bytes[kBuchla251eSlotBytes];
  memset(bytes, 0, sizeof(bytes));
  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);
  Buchla251eSequence &seq = slot.sequences[0];

  Buchla251eEuclidParams params;
  params.length = 50;  // beyond bjorklund's 32-step ceiling
  params.fill = 10;
  params.rotation = 0;
  params.base_volts = 1.0f;
  params.rest_volts = 0.0f;

  Buchla251eGenerateEuclid(params, seq);

  // Clamped to 32: the end marker must land at index 31, not 49, and
  // stages[32..49] must be untouched (still default 0 here, but the point
  // is the generator didn't reach them).
  CHECK(Buchla251eHasEndMarker(seq.stages[31]));
  CHECK(!Buchla251eHasEndMarker(seq.stages[49]));
}

int main() {
  test_roundtrip_zeroed();
  test_roundtrip_all_0xff();
  test_roundtrip_synthetic_realistic();
  test_roundtrip_many_random_seeds();
  test_decode_field_offsets();
  test_end_marker_roundtrip();
  test_volts_conversion();
  test_diff_no_changes();
  test_diff_single_stage_change();
  test_diff_overflow_returns_negative_one();
  test_generate_euclid_length8_fill3();
  test_generate_euclid_length_clamped_above_32();

  printf("\ntest_buchla251e_slot_codec: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
