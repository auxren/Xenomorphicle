// Host tests for the Buchla 259e 33-byte preset-record codec
// (src/Buchla259eSlotCodec.cpp). No hardware, no bus.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buchla259e_slot_codec
//   test_buchla259e_slot_codec.cpp ../src/Buchla259eSlotCodec.cpp &&
//   ./build/test_buchla259e_slot_codec
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Buchla259eSlotCodec.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// ---- roundtrip identity ---------------------------------------------------
// The whole point: a RESTORE re-sends every byte, so anything this codec does
// not understand has to survive untouched.

static void RoundTrip(const uint8_t *in, const char *what) {
  Buchla259eSlot slot;
  Buchla259eDecodeSlot(in, slot);
  uint8_t out[kBuchla259eRecordBytes];
  memset(out, 0xAA, sizeof(out));
  Buchla259eEncodeSlot(slot, out);
  if (memcmp(in, out, kBuchla259eRecordBytes) != 0) {
    printf("  (roundtrip mismatch: %s)\n", what);
  }
  CHECK(memcmp(in, out, kBuchla259eRecordBytes) == 0);
}

static void test_roundtrip_zeroed(void) {
  uint8_t b[kBuchla259eRecordBytes];
  memset(b, 0x00, sizeof(b));
  RoundTrip(b, "zeroed");
}

static void test_roundtrip_all_0xff(void) {
  uint8_t b[kBuchla259eRecordBytes];
  memset(b, 0xff, sizeof(b));
  RoundTrip(b, "all 0xff");
}

static void test_roundtrip_arbitrary_patterns(void) {
  // Arbitrary bytes, including non-zero low nibbles everywhere and junk in
  // the residue -- none of it may be normalised away.
  for (int seed = 0; seed < 64; ++seed) {
    uint8_t b[kBuchla259eRecordBytes];
    for (int i = 0; i < kBuchla259eRecordBytes; ++i)
      b[i] = (uint8_t)((seed * 31 + i * 17 + (i & 3)) & 0xff);
    RoundTrip(b, "pattern");
  }
}

static void test_roundtrip_preserves_residue(void) {
  // Offsets 24-25 are initialiser residue the module never reads. It still
  // has to come back byte-identical.
  uint8_t b[kBuchla259eRecordBytes];
  memset(b, 0, sizeof(b));
  b[24] = 0x7f; b[25] = 0xff;
  Buchla259eSlot slot;
  Buchla259eDecodeSlot(b, slot);
  CHECK(slot.residue[0] == 0x7f);
  CHECK(slot.residue[1] == 0xff);
  RoundTrip(b, "residue");

  // ...and something that is NOT the usual 7f ff, in case a module ever
  // stores otherwise.
  b[24] = 0x12; b[25] = 0x34;
  RoundTrip(b, "unusual residue");
}

// ---- 12-bit decode --------------------------------------------------------

static void test_param_decode_offsets(void) {
  uint8_t b[kBuchla259eRecordBytes];
  memset(b, 0, sizeof(b));
  // param 9 (warp) lives at offsets 18-19. Live-confirmed field.
  b[18] = 0xff; b[19] = 0xf0;
  // param 8 (morph) at 16-17.
  b[16] = 0x5f; b[17] = 0x30;

  Buchla259eSlot s;
  Buchla259eDecodeSlot(b, s);
  CHECK(s.param[9] == 0xfff0);
  CHECK(Buchla259eParam12(s.param[9]) == 4095);
  CHECK(s.param[8] == 0x5f30);
  CHECK(Buchla259eParam12(s.param[8]) == 1523);   // matches the live capture
}

static void test_param12_nonzero_low_nibble(void) {
  // The documented trap: offsets 2-3 (pitch) legitimately carry a non-zero
  // low nibble when written by a bus note. The decode must not assume it is
  // zero, and the raw word must keep it so an encode round-trips.
  uint8_t b[kBuchla259eRecordBytes];
  memset(b, 0, sizeof(b));
  // 683 in the doc's example is a note-table value; store it as the low 12
  // bits' worth with a dirty nibble.
  b[2] = 0x2A; b[3] = 0xBD;      // word 0x2ABD -> 12-bit 0x2AB = 683
  Buchla259eSlot s;
  Buchla259eDecodeSlot(b, s);
  CHECK(s.param[1] == 0x2ABD);
  CHECK(Buchla259eParam12(s.param[1]) == 683);
  CHECK((s.param[1] & 0x000f) == 0x0D);   // the nibble really is non-zero
  RoundTrip(b, "dirty pitch nibble");
}

// ---- bipolar --------------------------------------------------------------

static void test_bipolar_membership(void) {
  // Exactly five: params 0, 2, 6, 7, 11 (offsets 0-1, 4-5, 12-13, 14-15,
  // 22-23). Params 4 and 5 are NOT bipolar, despite an earlier claim.
  for (int p = 0; p < kBuchla259eParamCount; ++p) {
    const bool want = (p == 0 || p == 2 || p == 6 || p == 7 || p == 11);
    CHECK(Buchla259eParamIsBipolar(p) == want);
  }
  CHECK(!Buchla259eParamIsBipolar(4));
  CHECK(!Buchla259eParamIsBipolar(5));
}

static void test_bipolar_centre_and_rails(void) {
  // Centre reads as bytes 80 00 and must come out 0%.
  CHECK(Buchla259eBipolarPercent(0x8000) == 0);
  // Bottom rail: fully inverted.
  CHECK(Buchla259eBipolarPercent(0x0000) == -100);
  // Top rail: just under +100 (offset binary is asymmetric by one step).
  CHECK(Buchla259eBipolarPercent(0xFFF0) == 99);
  // Halfway up from centre.
  CHECK(Buchla259eBipolarPercent(0xC000) == 50);
  // Halfway down.
  CHECK(Buchla259eBipolarPercent(0x4000) == -50);
}

static void test_unipolar_percent(void) {
  CHECK(Buchla259eUnipolarPercent(0x0000) == 0);
  CHECK(Buchla259eUnipolarPercent(0xFFF0) == 100);
  CHECK(Buchla259eUnipolarPercent(0x8000) == 50);
}

// ---- pitch ----------------------------------------------------------------

static void test_pitch_semitones(void) {
  // 512 counts/octave over 8 octaves: full scale is 96 semitones.
  CHECK(Buchla259eSemitoneTenths(0x0000) == 0);
  CHECK(Buchla259eSemitoneTenths(0xFFF0) == 959);        // 95.9 st
  // One octave up from the bottom = 512 counts = 12.0 semitones.
  CHECK(Buchla259eSemitoneTenths((uint16_t)(512 << 4)) == 120);
  // One semitone = 512/12 counts; 42 counts is just under 1.0 st.
  CHECK(Buchla259eSemitoneTenths((uint16_t)(43 << 4)) == 10);
}

static void test_tracking_interval(void) {
  // Centred at 2048 = unison, +/-4 octaves.
  CHECK(Buchla259eIntervalTenths((uint16_t)(2048 << 4)) == 0);
  CHECK(Buchla259eIntervalTenths((uint16_t)((2048 + 512) << 4)) == 120);   // +1 oct
  CHECK(Buchla259eIntervalTenths((uint16_t)((2048 - 512) << 4)) == -120);  // -1 oct
}

// ---- warp scaling ---------------------------------------------------------

static void test_warp_scan_percent(void) {
  // The knob spans 20%..60% of scan width and reaches neither 0 nor 100 --
  // full scan is CV-only. A UI that showed the raw value would imply the
  // knob at 0 means "off", which is wrong.
  CHECK(Buchla259eWarpScanPercent(0x0000) == 20);
  CHECK(Buchla259eWarpScanPercent(0xFFF0) == 60);
  const int mid = Buchla259eWarpScanPercent((uint16_t)(2048 << 4));
  CHECK(mid >= 39 && mid <= 41);
}

// ---- dual-use params 4 and 5 ---------------------------------------------

static void test_dual_use_switches_on_timbre(void) {
  Buchla259eSlot s;
  // Param 4 follows the RED timbre, param 5 the GREEN one.
  s.red_timbre = 0; s.green_timbre = 0;
  CHECK(!Buchla259eParam4IsSkew(s));
  CHECK(!Buchla259eParam5IsSkew(s));

  s.red_timbre = 4; s.green_timbre = 4;
  CHECK(!Buchla259eParam4IsSkew(s));   // 4 is still below the threshold
  CHECK(!Buchla259eParam5IsSkew(s));

  s.red_timbre = 5; s.green_timbre = 4;
  CHECK(Buchla259eParam4IsSkew(s));    // red crossed, green did not
  CHECK(!Buchla259eParam5IsSkew(s));

  s.red_timbre = 4; s.green_timbre = 7;
  CHECK(!Buchla259eParam4IsSkew(s));
  CHECK(Buchla259eParam5IsSkew(s));    // and independently the other way
}

static void test_skew_base_scaling(void) {
  CHECK(Buchla259eSkewBase(0x0000) == 0u);
  CHECK(Buchla259eSkewBase(0xFFF0) == 4095u * 16u);
  CHECK(Buchla259eSkewBase((uint16_t)(100 << 4)) == 1600u);
}

// ---- discrete fields ------------------------------------------------------

static void test_discrete_fields_decode(void) {
  uint8_t b[kBuchla259eRecordBytes];
  memset(b, 0, sizeof(b));
  b[26] = 3;      // engine mode
  b[27] = 0x06;   // warp + morph, no freq
  b[28] = 2;      // sawtooth
  b[29] = 1;      // normal
  b[30] = 1;      // WAVE button edits red
  b[31] = 5;
  b[32] = 2;

  Buchla259eSlot s;
  Buchla259eDecodeSlot(b, s);
  CHECK(s.engine_mode == 3);
  CHECK(s.mod_dest_mask == 0x06);
  CHECK(!(s.mod_dest_mask & kBuchla259eModDestFreq));
  CHECK(s.mod_dest_mask & kBuchla259eModDestWarp);
  CHECK(s.mod_dest_mask & kBuchla259eModDestMorph);
  CHECK(s.mod_waveform == 2);
  CHECK(s.mod_freq_mode == 1);
  CHECK(s.wave_button_target == 1);
  CHECK(s.red_timbre == 5);
  CHECK(s.green_timbre == 2);
  RoundTrip(b, "discrete fields");
}

static void test_engine_mode_inert_case(void) {
  // Mode 1 does nothing when the modulator is in slow mode; the UI has to
  // say so instead of showing a setting with no effect.
  Buchla259eSlot s;
  s.engine_mode = 1; s.mod_freq_mode = 0;
  CHECK(Buchla259eEngineModeIsInert(s));

  s.mod_freq_mode = 1;
  CHECK(!Buchla259eEngineModeIsInert(s));

  s.engine_mode = 2; s.mod_freq_mode = 0;
  CHECK(!Buchla259eEngineModeIsInert(s));   // only mode 1 is affected
}

// ---- geometry -------------------------------------------------------------

static void test_geometry_constants(void) {
  CHECK(kBuchla259eRecordBytes == 33);
  CHECK(kBuchla259eSlotsPerBank == 30);
  CHECK(kBuchla259eBankBytes == 990);       // confirmed three ways on hardware
  CHECK(kBuchla259eParamCount == 12);
}

// ---- a real captured record ----------------------------------------------

static void test_real_capture_slot0(void) {
  // Slot 0 of a real 259e bank read over the bus on 2026-08-31.
  const uint8_t rec[kBuchla259eRecordBytes] = {
    0xff, 0x40, 0x68, 0x40, 0xfb, 0x10, 0xe9, 0xe0,
    0xa2, 0xa0, 0xff, 0x40, 0x04, 0x20, 0xeb, 0x50,
    0x5f, 0x30, 0xcc, 0x80, 0x5a, 0x70, 0xed, 0x60,
    0x7f, 0xff, 0x03, 0x00, 0x01, 0x02, 0x00, 0x02, 0x06
  };
  Buchla259eSlot s;
  Buchla259eDecodeSlot(rec, s);

  CHECK(Buchla259eParam12(s.param[0]) == 4084);
  CHECK(Buchla259eParam12(s.param[1]) == 1668);
  CHECK(Buchla259eParam12(s.param[8]) == 1523);   // morph, matches live diff
  CHECK(Buchla259eParam12(s.param[9]) == 3272);   // warp
  CHECK(s.residue[0] == 0x7f && s.residue[1] == 0xff);
  CHECK(s.engine_mode == 3);                       // whole bank is mode 3
  CHECK(s.red_timbre == 2);
  CHECK(s.green_timbre == 6);
  RoundTrip(rec, "real capture slot 0");
}

int main() {
  test_roundtrip_zeroed();
  test_roundtrip_all_0xff();
  test_roundtrip_arbitrary_patterns();
  test_roundtrip_preserves_residue();
  test_param_decode_offsets();
  test_param12_nonzero_low_nibble();
  test_bipolar_membership();
  test_bipolar_centre_and_rails();
  test_unipolar_percent();
  test_pitch_semitones();
  test_tracking_interval();
  test_warp_scan_percent();
  test_dual_use_switches_on_timbre();
  test_skew_base_scaling();
  test_discrete_fields_decode();
  test_engine_mode_inert_case();
  test_geometry_constants();
  test_real_capture_slot0();

  printf("\ntest_buchla259e_slot_codec: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
