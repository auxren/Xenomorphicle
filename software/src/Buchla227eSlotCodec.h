#ifndef BUCHLA227ESLOTCODEC_H_
#define BUCHLA227ESLOTCODEC_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// Buchla 227e (System Interface) preset record.
//
// GEOMETRY ONLY, for now. The record SIZE and bank layout are decoded from
// firmware (227Ev301.hex) with high confidence -- a single shared address
// helper computes slot*14 + 0x3A01 in XDATA -- but the MEANING of the 14
// bytes has NOT been reverse-engineered yet, and the size has NOT been
// confirmed on hardware (the bench was unreachable when this was written).
// See the 200e_bus_protocol repo, modules/227e-preset-record-format.md, for
// the firmware evidence and what still needs a live BACKUP to confirm.
//
// Because no field is understood, this codec stores all 14 bytes raw and its
// only job is BYTE-EXACT PRESERVATION -- the same core invariant as the 251e
// and 259e codecs: a RESTORE re-sends the whole bank, so
// Encode(Decode(x)) must equal x for any 14 bytes. That makes it safe to
// slice a captured bank into per-slot records and write an (unedited) one
// back. Field accessors get added here once the panel-diff decode is done.
//
// NOT the 259e (33-byte, dual waveshaper) and NOT the 272e (25-byte, poly
// tuner). Shares only the shape of the other codecs on purpose.
// ---------------------------------------------------------------------------

static constexpr int kBuchla227eRecordBytes = 14;      // firmware: MOV B,#0x0E / MUL AB
static constexpr int kBuchla227eSlotsPerBank = 30;     // firmware: SUBB A,#0x1E bound
static constexpr int kBuchla227eBankBytes =
    kBuchla227eRecordBytes * kBuchla227eSlotsPerBank;  // 420

// Byte offset of a slot's record within a whole-bank capture. (On the module
// itself the record lives at XDATA 0x3A01 + 14*slot; a capture is 0-based.)
static constexpr int Buchla227eSlotOffset(int slot) {
  return slot * kBuchla227eRecordBytes;
}

struct Buchla227eSlot {
  // All 14 bytes, raw. No field is decoded yet, so nothing here is named.
  // Preserved verbatim; see the header comment.
  uint8_t raw[kBuchla227eRecordBytes] = {0};
};

// Decode/encode exactly kBuchla227eRecordBytes (14) bytes. Callers pass a
// pointer into a larger resident bank image; offsetting to the right slot
// (Buchla227eSlotOffset) is the caller's job -- same convention as the 251e
// and 259e codecs.
void Buchla227eDecodeSlot(const uint8_t *bytes, Buchla227eSlot &out);
void Buchla227eEncodeSlot(const Buchla227eSlot &slot, uint8_t *out);

#endif  // BUCHLA227ESLOTCODEC_H_
