// Buchla 227e 14-byte preset record codec. Pure logic -- no hardware
// includes; see Buchla227eSlotCodec.h for the contract and the
// 200e_bus_protocol repo (modules/227e-preset-record-format.md) for where
// the geometry came from and what still needs a live BACKUP to confirm.

// On target, keep this cold code out of ITCM; host builds compile it bare.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define B227E_CODEC_CODE FLASHMEM
#else
#define B227E_CODEC_CODE
#endif

#include "Buchla227eSlotCodec.h"

B227E_CODEC_CODE
void Buchla227eDecodeSlot(const uint8_t *bytes, Buchla227eSlot &out) {
  // No field is understood yet, so every byte is kept raw. This is the whole
  // codec's job for now: a lossless round trip (see the header).
  for (int i = 0; i < kBuchla227eRecordBytes; ++i) out.raw[i] = bytes[i];
}

B227E_CODEC_CODE
void Buchla227eEncodeSlot(const Buchla227eSlot &slot, uint8_t *out) {
  for (int i = 0; i < kBuchla227eRecordBytes; ++i) out[i] = slot.raw[i];
}
