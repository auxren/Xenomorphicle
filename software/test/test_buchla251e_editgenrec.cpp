// Host tests for the pieces the 251e Edit/Gen/Rec screens are built on:
// the note->stage pitch path, Euclidean parameter clamping, and the
// generator/recorder interaction with a real decoded sequence.
//
// The app class itself needs the Teensy graphics/UI stack, so what is tested
// here is deliberately the logic the screens delegate to -- which is also
// the part where being wrong is silent rather than obvious.
//
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -I host_stubs -o build/test_buchla251e_editgenrec
//   test_buchla251e_editgenrec.cpp ../src/Buchla251eSlotCodec.cpp
//   ../src/Buchla251eGenerator.cpp ../src/Buchla251eRecorder.cpp
//   ../src/src/extern/bjorklund.cpp &&
//   ./build/test_buchla251e_editgenrec
#include <cassert>
#include <cstdio>

#include "../src/Buchla251eGenerator.h"
#include "../src/Buchla251eNoteMap.h"
#include "../src/Buchla251eRecorder.h"
#include "../src/Buchla251eSlotCodec.h"

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

// The screens hand the generator `note / 10.0f` because the generator takes
// volts while the UI speaks note numbers. That conversion goes through a
// float and comes back through Buchla251eVoltsToRaw's rounding, so it is
// exactly the kind of path that loses a count somewhere in the middle of the
// range and is never noticed. Pin every note.
static void test_note_volts_raw_roundtrip_every_note() {
  printf("test_note_volts_raw_roundtrip_every_note\n");
  for (int note = 0; note <= 127; ++note) {
    const uint8_t raw = Buchla251eNoteToRaw((uint8_t)note);
    CHECK(raw == (uint8_t)note);   // the confirmed identity mapping

    const float volts = (float)raw / 10.0f;
    const uint8_t back = Buchla251eVoltsToRaw(volts);
    if (back != (uint8_t)note) {
      printf("    note %d -> %.4fV -> raw %u\n", note, (double)volts, back);
    }
    CHECK(back == (uint8_t)note);
  }
}

// 1.2V/octave, not 1V/octave: twelve semitones must be 1.2V apart, which on
// this format is twelve raw counts. A 1V/oct assumption would put an octave
// ten counts apart and every generated interval would be wrong.
static void test_octave_is_twelve_counts() {
  printf("test_octave_is_twelve_counts\n");
  CHECK(Buchla251eNoteToRaw(60) - Buchla251eNoteToRaw(48) == 12);
  CHECK(Buchla251eRawToVolts(72) - Buchla251eRawToVolts(60) > 1.19f);
  CHECK(Buchla251eRawToVolts(72) - Buchla251eRawToVolts(60) < 1.21f);
}

static void test_clamp_basic_ranges() {
  printf("test_clamp_basic_ranges\n");
  uint8_t len, fill, rot;

  len = 0; fill = 0; rot = 0;
  Buchla251eClampEuclidParams(len, fill, rot);
  CHECK(len == 2);                 // minimum length

  len = 200; fill = 0; rot = 0;
  Buchla251eClampEuclidParams(len, fill, rot);
  CHECK(len == 32);                // bjorklund's uint32_t mask ceiling

  len = 16; fill = 200; rot = 0;
  Buchla251eClampEuclidParams(len, fill, rot);
  CHECK(fill == 16);               // fill may equal length (every step on)

  len = 16; fill = 4; rot = 200;
  Buchla251eClampEuclidParams(len, fill, rot);
  CHECK(rot == 15);                // rotation is length-1 at most
}

// The dependent fixup: shrinking length has to drag fill and rotation down.
// This is the part a UI gets wrong, because the user edits one number and
// two others silently become illegal.
static void test_clamp_shrinking_length_drags_dependents() {
  printf("test_clamp_shrinking_length_drags_dependents\n");
  uint8_t len = 8, fill = 24, rot = 30;
  Buchla251eClampEuclidParams(len, fill, rot);
  CHECK(len == 8);
  CHECK(fill == 8);
  CHECK(rot == 7);

  // Growing length must NOT move them.
  len = 32; fill = 3; rot = 2;
  Buchla251eClampEuclidParams(len, fill, rot);
  CHECK(len == 32);
  CHECK(fill == 3);
  CHECK(rot == 2);

  // Already-legal params are untouched (idempotence).
  len = 16; fill = 5; rot = 0;
  uint8_t l2 = len, f2 = fill, r2 = rot;
  Buchla251eClampEuclidParams(l2, f2, r2);
  CHECK(l2 == len && f2 == fill && r2 == rot);
}

// What the Gen screen actually does: note numbers in, stages carrying those
// exact note numbers out.
static void test_generate_from_note_numbers() {
  printf("test_generate_from_note_numbers\n");
  Buchla251eSequence seq;

  Buchla251eEuclidParams p;
  p.length = 8;
  p.fill = 3;
  p.rotation = 0;
  p.base_volts = (float)Buchla251eNoteToRaw(60) / 10.0f;
  p.rest_volts = (float)Buchla251eNoteToRaw(0) / 10.0f;
  Buchla251eGenerateEuclid(p, seq);

  int actives = 0;
  for (int i = 0; i < 8; ++i) {
    const uint8_t v = seq.stages[i].value;
    CHECK(v == 60 || v == 0);      // only the two pitches asked for
    if (v == 60) ++actives;
  }
  CHECK(actives == 3);             // exactly `fill` active stages

  // Exactly one end marker, at length-1.
  int markers = 0, marker_at = -1;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
    if (Buchla251eHasEndMarker(seq.stages[i])) { ++markers; marker_at = i; }
  CHECK(markers == 1);
  CHECK(marker_at == 7);

  // Stages past the working length are untouched.
  for (int i = 8; i < kBuchla251eStagesPerSequence; ++i)
    CHECK(seq.stages[i].value == 0);
}

// What the Rec screen does: a played line becomes stages whose raw values
// ARE the note numbers.
static void test_record_notes_become_stage_values() {
  printf("test_record_notes_become_stage_values\n");
  Buchla251eSequence seq;

  Buchla251eRecorder rec;
  rec.Reset(seq);

  const uint8_t line[] = {60, 62, 64, 65, 67};
  for (uint8_t n : line) CHECK(rec.NoteOn(n, 100));
  CHECK(rec.count() == 5);

  rec.Stop();
  for (int i = 0; i < 5; ++i) CHECK(seq.stages[i].value == line[i]);

  int markers = 0, marker_at = -1;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
    if (Buchla251eHasEndMarker(seq.stages[i])) { ++markers; marker_at = i; }
  CHECK(markers == 1);
  CHECK(marker_at == 4);           // last recorded stage
}

// Velocity-0 note-on is a note-off. The ISR path forwards it rather than
// filtering, so the recorder must be the one place that decision lives.
static void test_velocity_zero_is_not_a_stage() {
  printf("test_velocity_zero_is_not_a_stage\n");
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);

  CHECK(rec.NoteOn(60, 100));
  CHECK(!rec.NoteOn(62, 0));       // ignored
  CHECK(rec.NoteOn(64, 100));
  CHECK(rec.count() == 2);
  CHECK(seq.stages[0].value == 60);
  CHECK(seq.stages[1].value == 64);   // 62 never consumed a stage
}

// Generating and then recording over the same sequence must leave exactly
// one loop point -- the recorder clears all 50 before setting its own.
static void test_record_after_generate_leaves_one_marker() {
  printf("test_record_after_generate_leaves_one_marker\n");
  Buchla251eSequence seq;

  Buchla251eEuclidParams p;
  p.length = 16; p.fill = 5; p.rotation = 0;
  p.base_volts = 6.0f; p.rest_volts = 0.0f;
  Buchla251eGenerateEuclid(p, seq);
  CHECK(Buchla251eHasEndMarker(seq.stages[15]));

  Buchla251eRecorder rec;
  rec.Reset(seq);
  CHECK(rec.NoteOn(48, 90));
  CHECK(rec.NoteOn(50, 90));
  rec.Stop();

  int markers = 0, marker_at = -1;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
    if (Buchla251eHasEndMarker(seq.stages[i])) { ++markers; marker_at = i; }
  CHECK(markers == 1);
  CHECK(marker_at == 1);           // the generator's marker at 15 is gone
}

// Recording is capped at the format's 50 stages; the 51st must be refused
// rather than wrapping onto stage 0.
static void test_record_stops_at_fifty() {
  printf("test_record_stops_at_fifty\n");
  Buchla251eSequence seq;
  Buchla251eRecorder rec;
  rec.Reset(seq);

  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
    CHECK(rec.NoteOn((uint8_t)(40 + (i % 40)), 100));
  CHECK(rec.full());
  CHECK(!rec.NoteOn(99, 100));     // refused, not wrapped
  CHECK(rec.count() == kBuchla251eStagesPerSequence);
  CHECK(seq.stages[0].value == 40);   // stage 0 untouched by the overflow
}

// An edit only means anything if it survives the encode the write path runs.
static void test_edited_stage_survives_encode_decode() {
  printf("test_edited_stage_survives_encode_decode\n");
  uint8_t bytes[kBuchla251eSlotBytes];
  for (int i = 0; i < kBuchla251eSlotBytes; ++i) bytes[i] = (uint8_t)(i * 7);

  Buchla251eSlot slot;
  Buchla251eDecodeSlot(bytes, slot);

  slot.sequences[2].stages[9].value = 67;
  Buchla251eSetEndMarker(slot.sequences[2].stages[9], true);

  uint8_t out[kBuchla251eSlotBytes];
  Buchla251eEncodeSlot(slot, out);

  Buchla251eSlot back;
  Buchla251eDecodeSlot(out, back);
  CHECK(back.sequences[2].stages[9].value == 67);
  CHECK(Buchla251eHasEndMarker(back.sequences[2].stages[9]));
}

int main() {
  test_note_volts_raw_roundtrip_every_note();
  test_octave_is_twelve_counts();
  test_clamp_basic_ranges();
  test_clamp_shrinking_length_drags_dependents();
  test_generate_from_note_numbers();
  test_record_notes_become_stage_values();
  test_velocity_zero_is_not_a_stage();
  test_record_after_generate_leaves_one_marker();
  test_record_stops_at_fifty();
  test_edited_stage_survives_encode_decode();

  printf("\ntest_buchla251e_editgenrec: %d checks, %d failures\n",
         checks, failures);
  return failures == 0 ? 0 : 1;
}
