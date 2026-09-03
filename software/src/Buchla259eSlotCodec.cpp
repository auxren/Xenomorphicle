// Buchla 259e 33-byte preset record codec. Pure logic -- no hardware
// includes; see Buchla259eSlotCodec.h for the contract and
// Buchla_FW/docs/259e-PRESET-FORMAT.md for where the field map came from.

// On target, keep this cold code out of ITCM; host builds compile it bare.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define B259E_CODEC_CODE FLASHMEM
#else
#define B259E_CODEC_CODE
#endif

#include "Buchla259eSlotCodec.h"

B259E_CODEC_CODE
void Buchla259eDecodeSlot(const uint8_t *bytes, Buchla259eSlot &out) {
  // Offsets 0..23: twelve big-endian 16-bit words, kept raw so the low
  // nibble (real data for pitch) survives an encode round trip.
  for (int p = 0; p < kBuchla259eParamCount; ++p) {
    out.param[p] = (uint16_t)((uint16_t)bytes[p * 2] << 8) |
                   (uint16_t)bytes[p * 2 + 1];
  }
  out.residue[0] = bytes[24];
  out.residue[1] = bytes[25];
  out.engine_mode = bytes[26];
  out.mod_dest_mask = bytes[27];
  out.mod_waveform = bytes[28];
  out.mod_freq_mode = bytes[29];
  out.wave_button_target = bytes[30];
  out.red_timbre = bytes[31];
  out.green_timbre = bytes[32];
}

B259E_CODEC_CODE
void Buchla259eEncodeSlot(const Buchla259eSlot &slot, uint8_t *out) {
  for (int p = 0; p < kBuchla259eParamCount; ++p) {
    out[p * 2] = (uint8_t)(slot.param[p] >> 8);
    out[p * 2 + 1] = (uint8_t)(slot.param[p] & 0xff);
  }
  out[24] = slot.residue[0];
  out[25] = slot.residue[1];
  out[26] = slot.engine_mode;
  out[27] = slot.mod_dest_mask;
  out[28] = slot.mod_waveform;
  out[29] = slot.mod_freq_mode;
  out[30] = slot.wave_button_target;
  out[31] = slot.red_timbre;
  out[32] = slot.green_timbre;
}
