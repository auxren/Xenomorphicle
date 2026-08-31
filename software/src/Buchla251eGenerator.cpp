// Euclidean-rhythm generator for one Buchla 251e sequence. Pure logic --
// no hardware includes beyond bjorklund.h's declarations (bjorklund.cpp
// itself needs Arduino.h for PROGMEM; host test builds supply a minimal
// stub, see software/test/host_stubs/Arduino.h). See Buchla251eGenerator.h
// for the contract.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define B251E_GEN_CODE FLASHMEM
#else
#define B251E_GEN_CODE
#endif

#include "Buchla251eGenerator.h"
#include "src/extern/bjorklund.h"

namespace {

uint8_t ClampU8(int v, int lo, int hi) {
  if (v < lo) return static_cast<uint8_t>(lo);
  if (v > hi) return static_cast<uint8_t>(hi);
  return static_cast<uint8_t>(v);
}

}  // namespace

B251E_GEN_CODE
void Buchla251eGenerateEuclid(const Buchla251eEuclidParams &params, Buchla251eSequence &sequence) {
  const uint8_t length = ClampU8(params.length, 2, 32);
  const uint8_t fill = ClampU8(params.fill, 0, length);
  const uint8_t rotation = length > 1 ? ClampU8(params.rotation, 0, length - 1) : 0;

  // Unambiguous result: clear every stage's end marker first, then set only
  // the one at stages[length-1] -- see the header comment for why this is
  // stricter than "just clear the ones outside the new working length".
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    Buchla251eSetEndMarker(sequence.stages[i], false);
  }

  const uint32_t pattern = EuclideanPattern(length, fill, rotation);
  const uint8_t active_raw = Buchla251eVoltsToRaw(params.base_volts);
  const uint8_t rest_raw = Buchla251eVoltsToRaw(params.rest_volts);

  for (uint8_t i = 0; i < length; ++i) {
    const bool active = (pattern >> i) & 0x1;
    sequence.stages[i].value = active ? active_raw : rest_raw;
  }

  Buchla251eSetEndMarker(sequence.stages[length - 1], true);
}
