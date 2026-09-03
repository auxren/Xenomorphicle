#ifndef BUCHLA251ESLOTCODEC_H_
#define BUCHLA251ESLOTCODEC_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// C++ port of tools/251e-sequencer/sequence-codec.js's decodeSlot/encodeSlot,
// scoped to ONE 2104-byte slot record (not the whole 63120-byte bank -- the
// on-device app this serves edits one slot/sequence at a time). The JS file
// is the authoritative reference for field meanings, confidence levels, and
// what is confirmed vs. still unknown; read its header comment before
// changing anything here. BSP-free and host-testable, same split as
// PresetBus200e.{h,cpp}/Bus200eMaster.{h,cpp} -- no hardware access, no
// Arduino includes.
//
// BYTE-EXACT PRESERVATION IS THE CORE INVARIANT: everything this codec does
// not understand (header[8:16], stage pad/reserved bytes 0/1/2/4/5, most of
// the 22-byte sequence trailer) round-trips unchanged. A RESTORE re-sends
// the WHOLE bank even when only one stage was touched, so
// Buchla251eEncodeSlot(Buchla251eDecodeSlot(bytes)) must equal bytes for any
// 2104-byte input, including ones with garbage in the unknown fields -- this
// is what makes writing back an edited slot safe.
// ---------------------------------------------------------------------------

static constexpr int kBuchla251eStagesPerSequence = 50;
static constexpr int kBuchla251eSequencesPerSlot = 4;
static constexpr int kBuchla251eStageBytes = 10;
static constexpr int kBuchla251eStageReservedBytes = kBuchla251eStageBytes - 4; // bytes 4..9
static constexpr int kBuchla251eSequenceTrailerBytes = 22;
static constexpr int kBuchla251eSequenceBlockBytes =
    kBuchla251eStagesPerSequence * kBuchla251eStageBytes + kBuchla251eSequenceTrailerBytes; // 522
static constexpr int kBuchla251eSlotHeaderBytes = 16;
static constexpr int kBuchla251eSlotBytes =
    kBuchla251eSlotHeaderBytes + kBuchla251eSequencesPerSlot * kBuchla251eSequenceBlockBytes; // 2104

// Offset, within a stage's 6-byte `reserved` field, of the confirmed
// "end: Always" loop marker (stage byte 7 overall). 0x0A = marker set,
// 0x00 = not set. See sequence-codec.js's header comment for the live-diff
// evidence; indices 0,1,2,4,5 of `reserved` remain unknown and are only
// ever passed through raw.
static constexpr int kBuchla251eEndMarkerReservedIndex = 3;
static constexpr uint8_t kBuchla251eEndMarkerValue = 0x0A;

// Confirmed exact, zero offset (2026-08-30/31 live single-variable diff):
// raw stage value = volts * 10.
static constexpr float kBuchla251eVoltsPerRaw = 0.1f;

struct Buchla251eStage {
  uint8_t value = 0;  // volts*10, confirmed exact -- see Buchla251eVoltsToRaw/RawToVolts
  uint8_t pad = 0;     // stage byte 1, unknown, preserved raw
  uint16_t time = 4;   // stage bytes [2:4], LE on the wire, unknown units, default 4.
                        // NOTE: time=0 does NOT cleanly "skip" a stage on real hardware
                        // (confirmed live -- the module still briefly steps through it).
                        // Use the end marker for clean looping, not a zero interval.
  uint8_t reserved[kBuchla251eStageReservedBytes] = {0, 0, 0, 0, 0, 0}; // stage bytes [4:10]
};

struct Buchla251eSequence {
  Buchla251eStage stages[kBuchla251eStagesPerSequence];
  uint8_t trailer[kBuchla251eSequenceTrailerBytes] = {0}; // mostly unknown; trailer[1] is a
                                                            // named per-sequence param (see
                                                            // Buchla251eGetSequenceParam);
                                                            // trailer[4]'s ambiguous co-change
                                                            // with the end marker is documented
                                                            // in sequence-codec.js -- preserved
                                                            // raw here, never touched
                                                            // deliberately.
};

struct Buchla251eSlot {
  uint8_t header[kBuchla251eSlotHeaderBytes] = {0}; // two big-endian float32s at [0:4]/[4:8]
                                                      // (moderate-confidence meaning, HIGH-
                                                      // confidence shape) + 8 unknown zero
                                                      // bytes at [8:16]. Preserved raw -- v1
                                                      // has no need to decode the floats, byte-
                                                      // exact passthrough is what matters.
  Buchla251eSequence sequences[kBuchla251eSequencesPerSlot]; // A, B, C, D
};

// Decode/encode exactly kBuchla251eSlotBytes (2104) bytes. Callers pass a
// pointer into a larger resident card image (offset to the right slot is
// the caller's responsibility -- this module is slot-relative and
// addressing-agnostic, matching Buchla251eDiffSlot below).
void Buchla251eDecodeSlot(const uint8_t *bytes, Buchla251eSlot &out);
void Buchla251eEncodeSlot(const Buchla251eSlot &slot, uint8_t *out);

// ---- named accessors for the two confirmed semantics -----------------------
// The raw byte stays the source of truth in Buchla251eStage (mirrors the JS
// codec's own choice not to bake unit conversion into the data model) --
// these are convenience helpers for callers (the generator, a future UI)
// that want to work in volts and booleans instead of raw bytes.

inline uint8_t Buchla251eVoltsToRaw(float volts) {
  float raw = volts / kBuchla251eVoltsPerRaw; // volts * 10
  if (raw < 0.0f) return 0;
  if (raw > 255.0f) return 255;
  return static_cast<uint8_t>(raw + 0.5f); // round to nearest
}

inline float Buchla251eRawToVolts(uint8_t raw) {
  return raw * kBuchla251eVoltsPerRaw;
}

inline void Buchla251eSetEndMarker(Buchla251eStage &stage, bool end_always) {
  stage.reserved[kBuchla251eEndMarkerReservedIndex] =
      end_always ? kBuchla251eEndMarkerValue : 0x00;
}

inline bool Buchla251eHasEndMarker(const Buchla251eStage &stage) {
  return stage.reserved[kBuchla251eEndMarkerReservedIndex] == kBuchla251eEndMarkerValue;
}

// Offset of the named per-sequence trailer param (byte +1), matching
// sequence-codec.js's getSequenceParam/setSequenceParam. No claim about
// what it controls -- see that file's header comment.
static constexpr int kBuchla251eSequenceParamOffset = 1;

inline uint8_t Buchla251eGetSequenceParam(const Buchla251eSequence &seq) {
  return seq.trailer[kBuchla251eSequenceParamOffset];
}

inline void Buchla251eSetSequenceParam(Buchla251eSequence &seq, uint8_t value) {
  seq.trailer[kBuchla251eSequenceParamOffset] = value;
}

// ---- write-safety diff --------------------------------------------------
// Given a fresh backup of this slot's 2104 bytes (read immediately before
// computing any change -- see this project's established write-safety
// discipline in Main.cpp's console commands / PresetBus.h) and an edited
// Buchla251eSlot, compute the list of (offset, new_byte) patches needed to
// turn the backup into the intended slot. offset is relative to the start
// of the 2104-byte slot; the caller adds this slot's absolute position in
// the resident 65536-byte card image separately.
struct Buchla251eBytePatch {
  uint16_t offset;
  uint8_t value;
};

// Returns the number of patches written into out_patches (capacity
// max_patches). Returns -1 if the diff would need more than max_patches --
// callers MUST enlarge the buffer and retry rather than accept a truncated
// patch list; silently truncating is exactly the kind of mistake this
// project's write-safety discipline exists to prevent.
int Buchla251eDiffSlot(const uint8_t *fresh_backup_2104, const Buchla251eSlot &intended,
                        Buchla251eBytePatch *out_patches, int max_patches);

#endif  // BUCHLA251ESLOTCODEC_H_
