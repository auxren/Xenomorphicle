// Host tests for the Buchla 251e MIDI-note-map (src/Buchla251eNoteMap.h) and
// recorder (src/Buchla251eRecorder.cpp). No hardware, no MIDI device
// polling -- pure logic against Buchla251eSequence, same style as
// test_buchla251e_slot_codec.cpp.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buchla251e_recorder
//   test_buchla251e_recorder.cpp ../src/Buchla251eSlotCodec.cpp
//   ../src/Buchla251eRecorder.cpp &&
//   ./build/test_buchla251e_recorder
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Buchla251eNoteMap.h"
#include "../src/Buchla251eRecorder.h"
#include "../src/Buchla251eSlotCodec.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// ---- Buchla251eNoteToRaw: identity mapping, pinned -------------------------

static void test_note_map_identity(void) {
  CHECK(Buchla251eNoteToRaw(0) == 0);
  CHECK(Buchla251eNoteToRaw(60) == 60);
  CHECK(Buchla251eNoteToRaw(127) == 127);
}

// ---- basic recording --------------------------------------------------------

static void test_basic_recording(void) {
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);

  CHECK(rec.count() == 0);
  CHECK(rec.NoteOn(60, 100) == true);
  CHECK(rec.NoteOn(64, 100) == true);
  CHECK(rec.NoteOn(67, 100) == true);
  CHECK(rec.count() == 3);

  CHECK(seq.stages[0].value == 60);
  CHECK(seq.stages[1].value == 64);
  CHECK(seq.stages[2].value == 67);

  // Untouched stages/fields stay at Buchla251eSequence's own defaults.
  CHECK(seq.stages[0].pad == 0);
  CHECK(seq.stages[0].time == 4);
  CHECK(seq.stages[1].pad == 0);
  CHECK(seq.stages[1].time == 4);
  CHECK(seq.stages[3].value == 0);  // never recorded into
}

// ---- last-note-wins: no chord grouping, every note-on advances -------------

static void test_last_note_wins(void) {
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);

  // Simulate a "chord" as three back-to-back note-ons (no note-off tracking
  // in this class) -- each one still becomes its own stage.
  rec.NoteOn(48, 100);
  rec.NoteOn(52, 100);
  rec.NoteOn(55, 100);

  CHECK(rec.count() == 3);
  CHECK(seq.stages[0].value == 48);
  CHECK(seq.stages[1].value == 52);
  CHECK(seq.stages[2].value == 55);
}

// ---- velocity 0 is a note-off in disguise: ignored -------------------------

static void test_velocity_zero_ignored(void) {
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);

  CHECK(rec.NoteOn(60, 100) == true);
  CHECK(rec.NoteOn(64, 0) == false);  // note-off in disguise
  CHECK(rec.count() == 1);
  CHECK(seq.stages[1].value == 0);  // slot 1 never consumed
}

// ---- overflow: exactly 50 succeed, the 51st is refused ---------------------

static void test_overflow(void) {
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);

  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    bool ok = rec.NoteOn(static_cast<uint8_t>(i % 128), 100);
    CHECK(ok == true);
  }
  CHECK(rec.count() == kBuchla251eStagesPerSequence);
  CHECK(rec.full() == true);

  // The 51st note is refused and does not touch stage 50 (out of range) or
  // wrap around to overwrite stage 0.
  uint8_t stage0_before = seq.stages[0].value;
  CHECK(rec.NoteOn(99, 100) == false);
  CHECK(rec.count() == kBuchla251eStagesPerSequence);
  CHECK(seq.stages[0].value == stage0_before);
}

// ---- Stop() places the end marker at exactly count-1 -----------------------

static void test_stop_places_marker(void) {
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);

  rec.NoteOn(60, 100);
  rec.NoteOn(62, 100);
  rec.NoteOn(64, 100);
  rec.Stop();

  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    bool expect_marker = (i == 2);
    CHECK(Buchla251eHasEndMarker(seq.stages[i]) == expect_marker);
  }
}

// ---- Stop() clears a stale marker left over from a prior generate/edit ----

static void test_stop_clears_stale_marker(void) {
  Buchla251eSequence seq;
  // Simulate a stale end marker from a previous generate/edit pass, at an
  // index outside where the new (shorter) recording will land.
  Buchla251eSetEndMarker(seq.stages[20], true);

  Buchla251eRecorder rec;
  rec.Reset(seq);
  rec.NoteOn(60, 100);
  rec.NoteOn(62, 100);
  rec.Stop();

  CHECK(Buchla251eHasEndMarker(seq.stages[20]) == false);
  CHECK(Buchla251eHasEndMarker(seq.stages[1]) == true);
  int marker_count = 0;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    if (Buchla251eHasEndMarker(seq.stages[i])) ++marker_count;
  }
  CHECK(marker_count == 1);
}

// ---- Stop() with count == 0 is a no-op: leaves existing markers alone -----

static void test_stop_empty_is_noop(void) {
  Buchla251eSequence seq;
  // Pre-existing marker from a prior pass -- an empty-recording Stop() must
  // NOT wipe it, since count()==0 is documented as a safe cancel, not a
  // commit that clears the target.
  Buchla251eSetEndMarker(seq.stages[5], true);

  Buchla251eRecorder rec;
  rec.Reset(seq);
  rec.Stop();  // nothing recorded

  CHECK(rec.count() == 0);
  CHECK(Buchla251eHasEndMarker(seq.stages[5]) == true);
  int marker_count = 0;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    if (Buchla251eHasEndMarker(seq.stages[i])) ++marker_count;
  }
  CHECK(marker_count == 1);
}

// ---- Reset() re-arms the cursor without touching stage data ---------------

static void test_reset_rearms_without_clearing_data(void) {
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);
  rec.NoteOn(60, 100);
  rec.NoteOn(62, 100);
  CHECK(rec.count() == 2);

  // Re-arm on the SAME target: cursor resets to 0, but Reset() itself does
  // not clear the stage data already written -- only new NoteOn() calls
  // overwrite it, starting again from stage 0.
  rec.Reset(seq);
  CHECK(rec.count() == 0);
  CHECK(seq.stages[0].value == 60);  // untouched by Reset() itself
  CHECK(seq.stages[1].value == 62);

  rec.NoteOn(70, 100);
  CHECK(seq.stages[0].value == 70);  // overwritten by the new recording pass
}

// ---- NoteOn() before Reset() (no attached target) is inert, not a crash ---

static void test_noteon_before_reset_is_inert(void) {
  Buchla251eRecorder rec;
  CHECK(rec.NoteOn(60, 100) == false);
  CHECK(rec.count() == 0);
}

int main(void) {
  test_note_map_identity();
  test_basic_recording();
  test_last_note_wins();
  test_velocity_zero_ignored();
  test_overflow();
  test_stop_places_marker();
  test_stop_clears_stale_marker();
  test_stop_empty_is_noop();
  test_reset_rearms_without_clearing_data();
  test_noteon_before_reset_is_inert();

  printf("test_buchla251e_recorder: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
