#ifndef BUCHLA251ERECORDER_H_
#define BUCHLA251ERECORDER_H_

#include <stdint.h>

#include "Buchla251eSlotCodec.h"

// ---------------------------------------------------------------------------
// Records a live-played MIDI line into a Buchla251eSequence, one note = one
// stage. Operates on a Buchla251eSequence& the caller owns -- no heap, no
// std::vector (unlike applets/MidiLoop.h's std::array<std::vector<...>>
// buffer, which is NOT the pattern to copy here: Buchla251eSequence already
// has a fixed 50-stage array, this class uses it directly).
//
// Chord policy: LAST NOTE WINS (confirmed 2026-08-31). Every accepted
// note-on immediately becomes the next stage, full stop, regardless of what
// else might still be held -- no grouping/buffering of simultaneous notes.
//
// Note-off/duration: explicitly out of scope for v1. A recorded stage's
// `time` field is left at whatever default Buchla251eSequence/Buchla251eStage
// already carries; this class never derives duration from note-on/note-off
// timing.
//
// Pitch: raw stage value = Buchla251eNoteToRaw(note) -- see
// Buchla251eNoteMap.h for the confirmed 1.2V/octave, note-0-is-0V mapping.
//
// BSP-free and host-testable, same split as Buchla251eSlotCodec/
// Buchla251eGenerator -- no hardware access, no Arduino includes.
// ---------------------------------------------------------------------------

class Buchla251eRecorder {
 public:
  // Attaches the recorder to `target` and resets the recording cursor to 0.
  // Does NOT touch target's stage data -- the caller decides whether the
  // sequence being recorded into should first be cleared to defaults (e.g.
  // by handing in a fresh Buchla251eSequence) or left as-is (e.g. re-arming
  // to re-record over a sequence that came from a prior generate/edit pass,
  // where only Stop() -- not Reset() -- clears the 50 stages' end markers).
  // Safe to call again on the same or a different target to re-arm.
  void Reset(Buchla251eSequence &target);

  // Records one note as the next stage, using LAST-NOTE-WINS: this is called
  // per note-on, unconditionally advancing the cursor, no chord grouping.
  // velocity == 0 is treated as a note-off per MIDI convention and ignored
  // (does not advance, does not consume a stage). Returns true if the note
  // was recorded, false if it was ignored (velocity 0, or the recorder is
  // full / not attached via Reset()).
  bool NoteOn(uint8_t midi_note, uint8_t velocity);

  // Places the end marker at the last recorded stage (index count()-1) and
  // clears it everywhere else in all 50 stages first -- same "clear all,
  // then set one" discipline as Buchla251eGenerateEuclid, so the result is
  // unambiguous even if the target sequence had stale markers in it before
  // recording started. If count() == 0 (Stop() called with nothing
  // recorded), this is a complete no-op: it does not touch the target at
  // all, so an arm-then-immediately-stop is a safe cancel, not a silent
  // wipe of whatever markers the target already had.
  void Stop();

  uint8_t count() const { return count_; }
  bool full() const { return count_ >= kBuchla251eStagesPerSequence; }

 private:
  Buchla251eSequence *target_ = nullptr;
  uint8_t count_ = 0;
};

#endif  // BUCHLA251ERECORDER_H_
