// C++ port of tools/251e-sequencer/sequence-codec.js's decodeSlot/encodeSlot.
// Pure logic -- no hardware includes; see Buchla251eSlotCodec.h for the
// contract and field-meaning documentation.
#include <cstring>

// On target, keep this cold code out of ITCM; host builds compile it bare.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define B251E_CODEC_CODE FLASHMEM
#else
#define B251E_CODEC_CODE
#endif

#include "Buchla251eSlotCodec.h"

namespace {

void DecodeStage(const uint8_t *bytes, Buchla251eStage &stage) {
  stage.value = bytes[0];
  stage.pad = bytes[1];
  stage.time = static_cast<uint16_t>(bytes[2]) | (static_cast<uint16_t>(bytes[3]) << 8);
  memcpy(stage.reserved, bytes + 4, kBuchla251eStageReservedBytes);
}

void EncodeStage(const Buchla251eStage &stage, uint8_t *out) {
  out[0] = stage.value;
  out[1] = stage.pad;
  out[2] = static_cast<uint8_t>(stage.time & 0xff);
  out[3] = static_cast<uint8_t>((stage.time >> 8) & 0xff);
  memcpy(out + 4, stage.reserved, kBuchla251eStageReservedBytes);
}

void DecodeSequence(const uint8_t *bytes, Buchla251eSequence &seq) {
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    DecodeStage(bytes + i * kBuchla251eStageBytes, seq.stages[i]);
  }
  const uint8_t *trailer = bytes + kBuchla251eStagesPerSequence * kBuchla251eStageBytes;
  memcpy(seq.trailer, trailer, kBuchla251eSequenceTrailerBytes);
}

void EncodeSequence(const Buchla251eSequence &seq, uint8_t *out) {
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    EncodeStage(seq.stages[i], out + i * kBuchla251eStageBytes);
  }
  uint8_t *trailer = out + kBuchla251eStagesPerSequence * kBuchla251eStageBytes;
  memcpy(trailer, seq.trailer, kBuchla251eSequenceTrailerBytes);
}

}  // namespace

B251E_CODEC_CODE
void Buchla251eDecodeSlot(const uint8_t *bytes, Buchla251eSlot &out) {
  memcpy(out.header, bytes, kBuchla251eSlotHeaderBytes);
  for (int b = 0; b < kBuchla251eSequencesPerSlot; ++b) {
    DecodeSequence(bytes + kBuchla251eSlotHeaderBytes + b * kBuchla251eSequenceBlockBytes,
                   out.sequences[b]);
  }
}

B251E_CODEC_CODE
void Buchla251eEncodeSlot(const Buchla251eSlot &slot, uint8_t *out) {
  memcpy(out, slot.header, kBuchla251eSlotHeaderBytes);
  for (int b = 0; b < kBuchla251eSequencesPerSlot; ++b) {
    EncodeSequence(slot.sequences[b], out + kBuchla251eSlotHeaderBytes + b * kBuchla251eSequenceBlockBytes);
  }
}

B251E_CODEC_CODE
int Buchla251eDiffSlot(const uint8_t *fresh_backup_2104, const Buchla251eSlot &intended,
                        Buchla251eBytePatch *out_patches, int max_patches) {
  uint8_t target[kBuchla251eSlotBytes];
  Buchla251eEncodeSlot(intended, target);

  int count = 0;
  for (int i = 0; i < kBuchla251eSlotBytes; ++i) {
    if (target[i] != fresh_backup_2104[i]) {
      if (count >= max_patches) return -1;
      out_patches[count].offset = static_cast<uint16_t>(i);
      out_patches[count].value = target[i];
      ++count;
    }
  }
  return count;
}
