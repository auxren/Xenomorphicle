#ifndef BUCHLA251EGENERATOR_H_
#define BUCHLA251EGENERATOR_H_

#include <stdint.h>

#include "Buchla251eSlotCodec.h"

// ---------------------------------------------------------------------------
// Euclidean-rhythm generator for one Buchla 251e sequence (v1 of the
// on-device sequencer app's generator -- scale-constrained random walk and
// other generators are a later phase). Reuses this project's existing
// Bjorklund/Euclidean pattern implementation (src/src/extern/bjorklund.h,
// already shipped and used by the Hemisphere EuclidX/EuclidO applets)
// instead of reimplementing Euclidean rhythm math.
//
// KNOWN LIMIT: bjorklund.h's EuclideanPattern() returns a uint32_t bitmask,
// so it only supports 2..32 steps -- even though a 251e sequence has 50
// stage slots. `length` below is clamped to that range. Longer working
// lengths (up to the full 50) would need either a wider pattern generator
// or pattern-chaining logic; out of scope for this pass, flagged here so
// nobody is surprised a 40-step Euclidean request gets clamped to 32.
//
// PITCH: this generator takes an already-decided base_volts/rest_volts pair
// rather than doing scale quantization itself -- wiring this into this
// project's existing quantizer (used by H1200/ASR/QQ) is on-device UI
// integration work for a later phase, not a concern of this host-testable
// library.
// ---------------------------------------------------------------------------

struct Buchla251eEuclidParams {
  uint8_t length;    // working sequence length; clamped to [2, 32] -- see the
                      // KNOWN LIMIT note above.
  uint8_t fill;       // active/pulse count; clamped to [0, length].
  uint8_t rotation;   // rotation offset; clamped to [0, length-1].
  float base_volts;   // pitch written to an active (pulse) stage.
  float rest_volts;   // pitch written to an inactive stage; 0.0V is a
                       // reasonable default (matches this codec's own
                       // DEFAULT_STAGE_VALUE = 0 convention for "nothing
                       // programmed here").
};

// Fills sequence.stages[0..length-1]'s `value` from the Euclidean pattern
// (active -> base_volts, inactive -> rest_volts, both via
// Buchla251eVoltsToRaw) and places the end marker at stages[length-1].
//
// Every OTHER field of every stage (pad, time, and the non-end-marker
// reserved bytes) is left exactly as it was in the sequence passed in --
// the caller must start from a real decoded Buchla251eSequence, not a
// zeroed one, so anything the generator doesn't own survives untouched.
// stages[length..49] are likewise completely untouched (value included).
//
// End-marker handling: ALL 50 stages' end markers are cleared before the
// new one is set at stages[length-1] -- not just ones outside the new
// working length -- so the result is unambiguous even if an earlier
// generation or a hand-edit left a marker somewhere inside the new range.
void Buchla251eGenerateEuclid(const Buchla251eEuclidParams &params, Buchla251eSequence &sequence);

// The same limits Buchla251eGenerateEuclid applies internally, exposed so a
// UI can enforce them WHILE the user is turning an encoder rather than
// letting the generator silently clamp at apply time, where the user never
// sees what happened to the value they chose.
//
// Note the dependency direction: shrinking `length` drags `fill` and
// `rotation` down with it, because a fill of 12 in a length of 8 is not a
// value anything can honour. Growing `length` leaves them alone.
void Buchla251eClampEuclidParams(uint8_t &length, uint8_t &fill, uint8_t &rotation);

#endif  // BUCHLA251EGENERATOR_H_
