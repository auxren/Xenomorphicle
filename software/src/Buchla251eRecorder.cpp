// Records a live-played MIDI line into a Buchla251eSequence. Pure logic --
// no hardware includes; see Buchla251eRecorder.h for the contract.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define B251E_REC_CODE FLASHMEM
#else
#define B251E_REC_CODE
#endif

#include "Buchla251eRecorder.h"
#include "Buchla251eNoteMap.h"

B251E_REC_CODE
void Buchla251eRecorder::Reset(Buchla251eSequence &target) {
  target_ = &target;
  count_ = 0;
}

B251E_REC_CODE
bool Buchla251eRecorder::NoteOn(uint8_t midi_note, uint8_t velocity) {
  if (velocity == 0) return false;  // note-off in disguise, per MIDI convention
  if (target_ == nullptr) return false;
  if (full()) return false;

  Buchla251eStage &stage = target_->stages[count_];
  stage.value = Buchla251eNoteToRaw(midi_note);
  ++count_;
  return true;
}

B251E_REC_CODE
void Buchla251eRecorder::Stop() {
  if (target_ == nullptr) return;
  if (count_ == 0) return;  // nothing recorded -- leave the target untouched, a safe cancel

  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    Buchla251eSetEndMarker(target_->stages[i], false);
  }
  Buchla251eSetEndMarker(target_->stages[count_ - 1], true);
}
