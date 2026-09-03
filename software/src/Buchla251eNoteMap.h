#ifndef BUCHLA251ENOTEMAP_H_
#define BUCHLA251ENOTEMAP_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// MIDI note number -> 251e stage raw value.
//
// The 251e stage value is volts*10, confirmed exact (see Buchla251eSlotCodec.h).
// The 251e runs 1.2V/octave -- 0.1V/semitone -- NOT the Eurorack-standard
// 1V/octave (confirmed 2026-08-31, both from the live-diffed raw=volts*10
// encoding itself -- 0.1V/semitone * 12 semitones/octave = 1.2V/octave -- and
// directly from the module's owner). Combined with MIDI note 0 = 0.0V, this
// collapses to an identity mapping: raw stage value == MIDI note number.
//
// Deliberately its own tiny header, not folded into Buchla251eSlotCodec.h --
// the codec is about the byte format itself and shouldn't know MIDI exists;
// this is a separate, MIDI-specific concern that happens to produce values
// the codec consumes. Named as a real function (not inlined at call sites)
// so this asserted hardware convention stays visible and revisitable.
// ---------------------------------------------------------------------------

inline uint8_t Buchla251eNoteToRaw(uint8_t midi_note) {
  return midi_note;  // MIDI's 0-127 range already fits the stage value's 0-255 raw range
}

#endif  // BUCHLA251ENOTEMAP_H_
