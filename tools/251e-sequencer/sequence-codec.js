// sequence-codec.js
// ---------------------------------------------------------------------------
// Buchla 251e preset-BANK codec.
//
// This replaces the old self-invented placeholder layout. Everything below is
// the container geometry established in
// `Buchla_FW/docs/251e-SEQUENCE-FORMAT.md`, section
// "2026-08-30, FIRST REAL 251e CAPTURE" -- derived from a live
// `MasterBackup(0x5C)` of the owner's own 251e and independently
// cross-validated against the Studio H `251e.json` reference bank. Read that
// document before changing anything here; its per-claim confidence levels are
// mirrored in the comments below and are deliberately NOT upgraded here.
//
// UNIT OF TRANSFER: one whole 63120-byte bank
// -------------------------------------------
// BACKUP/RESTORE moves the entire bank as one continuous transfer -- it is not
// addressable per slot (confirmed live). So decode/encode work on a full bank
// and a RESTORE re-sends all 30 slots even when the user touched one stage.
// That makes byte-exact preservation of everything we do NOT understand a hard
// requirement, not a nicety: every unknown field is carried through the state
// object as raw bytes and written back unchanged.
//
// GEOMETRY (confidence: HIGH -- `16 + 4*522 = 2104` exactly, four stride-10
// runs of exactly 50 separated by three exactly-22-byte gaps, corroborated by
// a second, independent dump):
//
//   bank  = 30 slots x 2104 bytes                                  = 63120
//   slot  = 16-byte header + 4 blocks x 522 bytes                  = 2104
//   block = 50 stage entries x 10 bytes + 22-byte trailer          = 522
//
// One block is one sequence (A/B/C/D) of the Quad Sequential Voltage Source.
//
// FIELD MEANINGS -- what is known and what is not:
//
//   header[0:4], header[4:8]  IEEE-754 BIG-endian floats.  Shape: HIGH
//       confidence (two independent sources, four canonical values: real
//       module `0.0`/`1.0`, reference `-1.0`/`1.3043053`).  Meaning: MODERATE
//       -- they read like an offset/scale or range pair sitting at identity on
//       the real module, but that is a reading, not a fact, so they are named
//       `floats[0]`/`floats[1]` here and nothing else.
//   header[8:16]              zero in every sample.  Unknown.  Preserved raw.
//
//   stage byte 0    per-stage value, default 0. Full 0..255 range in the
//       programmed reference block. CONFIRMED (2026-08-30/31, live single-
//       variable diff on a real 251e): raw value = volts x 10 exactly, zero
//       offset -- one LSB = 0.1V = one semitone, matching the owner's manual
//       ("each .1 volts adds 1 semitone"). This codec still exposes the raw
//       byte rather than a volts field, so callers doing the volts<->raw
//       conversion should use `value / 10` / `Math.round(volts * 10)`
//       themselves -- kept explicit rather than silently baked into the
//       codec's data model.
//   stage byte 1    `0x00` in 250/250 observed entries. Might be a pad, might
//       be the high byte of a 16-bit value at [0:2]. Unresolvable from
//       defaults. Exposed as `pad`, preserved.
//   stage [2:4]     uint16 LITTLE-endian, default 4. Structure HIGH
//       confidence; units unknown. Named `time` after the doc's "per-stage
//       time/duration" reading -- a descriptive name for the best-supported
//       reading, NOT a confirmed semantic. NOTE: setting `time = 0` to try to
//       "skip" a stage does NOT cleanly skip it on real hardware -- the
//       module still briefly steps through it (confirmed live, audible
//       glitch). Use the end marker below for clean looping instead.
//   stage [4:10]    `reserved[3]` (stage byte 7) CONFIRMED (2026-08-31, live
//       diff: setting the panel's "end" field to "Always" on a stage and
//       saving flips exactly this byte 0x00 -> 0x0a, nothing else in the
//       stage) as the loop-termination / "end: Always" marker -- 0x0a = 10
//       decimal, consistent with the manual's end-count field taking values
//       1-9 or the letter "A" (Always) as the next value after 9. The
//       remaining 5 bytes of `reserved` (indices 0,1,2,4,5) are still
//       unknown and preserved raw. See
//       `~/Documents/GitHub/claude_trix/tricks/reverse-engineering/re-buchla-251e-sequence-format.md`
//       for the full live-diff methodology and writeup.
//
//   block trailer [500:522]   22 bytes, mostly unknown. Byte +1 is a
//       per-sequence parameter: `0x78` = 120 default, `0x8c` = 140 in the one
//       programmed reference block. Byte +4 is 0 by default and 2 there.
//       Everything else is zero in both sources. The whole trailer is kept as
//       raw bytes; `getSequenceParam`/`setSequenceParam` are a named accessor
//       for byte +1 only, with no claim about what it controls.
//       Live note (2026-08-31): setting the "end: Always" marker above and
//       saving also flipped trailer byte +4 from 0x00 to 0x02 on ALL FOUR
//       sequences in the slot, even though only one sequence's end marker was
//       touched -- possibly a save-revision/session stamp similar to the
//       header floats above, possibly load-bearing for the end marker to
//       actually take effect. Genuinely ambiguous either way: do not
//       deliberately clear or set it, just preserve whatever a fresh backup
//       already has (the codec's raw-passthrough behavior already does this
//       correctly by default -- this note is a warning against "cleaning it
//       up", not an instruction to change any code).
//
// Anything a user did not touch survives an edit-and-restore cycle
// byte-for-byte. `encodeBank(decodeBank(bytes))` is the identity on any
// 63120-byte input -- that is the invariant the test suite pins down.
//
// NOTE ON THE TRANSPORT: a bank is full 8-bit data (`0x78`, `0x80`, `0x3f`
// all occur in the real capture). It is NOT 7-bit-safe, and must go through
// sysex-transport.js's packChunk/unpackChunk (Bus200eSysExPack) like any real
// 200e capture. sequence-codec.js emits plain bytes and takes no view on
// framing or chunking.
// ---------------------------------------------------------------------------

export const SLOTS_PER_BANK = 30;
export const SEQUENCES_PER_SLOT = 4;
export const STAGES_PER_SEQUENCE = 50;

export const STAGE_BYTES = 10;
export const SEQUENCE_TRAILER_BYTES = 22;
export const SEQUENCE_BLOCK_BYTES = STAGES_PER_SEQUENCE * STAGE_BYTES + SEQUENCE_TRAILER_BYTES; // 522
export const SLOT_HEADER_BYTES = 16;
export const SLOT_BYTES = SLOT_HEADER_BYTES + SEQUENCES_PER_SLOT * SEQUENCE_BLOCK_BYTES; // 2104
export const BANK_BYTES = SLOTS_PER_BANK * SLOT_BYTES; // 63120

export const SEQUENCE_LABELS = ["A", "B", "C", "D"];

// Observed defaults, straight out of the real capture (all 15 intact records).
export const DEFAULT_STAGE_VALUE = 0;
export const DEFAULT_STAGE_TIME = 4;
export const DEFAULT_SEQUENCE_PARAM = 0x78; // 120
export const DEFAULT_HEADER_FLOATS = Object.freeze([0.0, 1.0]);

export const STAGE_VALUE_MAX = 0xff;
export const STAGE_TIME_MAX = 0xffff;
/** Offset, within the 22-byte block trailer, of the per-sequence parameter. */
export const SEQUENCE_PARAM_OFFSET = 1;
/** Offset of the other trailer byte ever seen non-zero (2 in the programmed reference block). */
export const SEQUENCE_TRAILER_FLAG_OFFSET = 4;

const STAGE_RESERVED_BYTES = STAGE_BYTES - 4; // bytes 4..9

/**
 * @typedef {{value:number, pad:number, time:number, reserved:Uint8Array}} Stage
 * @typedef {{stages:Stage[], trailer:Uint8Array}} Sequence
 * @typedef {{header:{floats:number[], bytes:Uint8Array}, sequences:Sequence[]}} Slot
 * @typedef {{slots:Slot[]}} Bank
 */

// ---- small helpers ---------------------------------------------------------

function asBytes(input, what) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  if (Array.isArray(input)) return Uint8Array.from(input);
  throw new TypeError(`${what}: expected a Uint8Array of bytes`);
}

/** Copy `src` into exactly `len` bytes -- short input zero-pads, long input truncates. */
function fitBytes(src, len) {
  const out = new Uint8Array(len);
  if (src) {
    const bytes = asBytes(src, "raw field");
    out.set(bytes.subarray(0, len));
  }
  return out;
}

function clampInt(v, lo, hi) {
  const n = Math.round(Number(v));
  if (!Number.isFinite(n)) return lo;
  return Math.min(hi, Math.max(lo, n));
}

// ---- stage entry (10 bytes) ------------------------------------------------

/** A stage at the module's observed defaults: value 0, time 4, rest zero. */
export function createDefaultStage() {
  return {
    value: DEFAULT_STAGE_VALUE,
    pad: 0,
    time: DEFAULT_STAGE_TIME,
    reserved: new Uint8Array(STAGE_RESERVED_BYTES),
  };
}

function decodeStage(bytes, off) {
  return {
    value: bytes[off],
    pad: bytes[off + 1],
    time: bytes[off + 2] | (bytes[off + 3] << 8), // little-endian uint16
    reserved: bytes.slice(off + 4, off + STAGE_BYTES),
  };
}

function encodeStage(stage, out, off) {
  const s = stage ?? createDefaultStage();
  out[off] = clampInt(s.value ?? 0, 0, STAGE_VALUE_MAX);
  out[off + 1] = clampInt(s.pad ?? 0, 0, 0xff);
  const time = clampInt(s.time ?? 0, 0, STAGE_TIME_MAX);
  out[off + 2] = time & 0xff;
  out[off + 3] = (time >> 8) & 0xff;
  out.set(fitBytes(s.reserved, STAGE_RESERVED_BYTES), off + 4);
}

// ---- sequence block (522 bytes) --------------------------------------------

/** One sequence at the module's observed defaults (50 default stages, param 120). */
export function createDefaultSequence() {
  const trailer = new Uint8Array(SEQUENCE_TRAILER_BYTES);
  trailer[SEQUENCE_PARAM_OFFSET] = DEFAULT_SEQUENCE_PARAM;
  return {
    stages: Array.from({ length: STAGES_PER_SEQUENCE }, createDefaultStage),
    trailer,
  };
}

function decodeSequence(bytes, off) {
  const stages = [];
  for (let i = 0; i < STAGES_PER_SEQUENCE; i++) {
    stages.push(decodeStage(bytes, off + i * STAGE_BYTES));
  }
  const trailerOff = off + STAGES_PER_SEQUENCE * STAGE_BYTES;
  return { stages, trailer: bytes.slice(trailerOff, trailerOff + SEQUENCE_TRAILER_BYTES) };
}

function encodeSequence(seq, out, off) {
  const s = seq ?? createDefaultSequence();
  for (let i = 0; i < STAGES_PER_SEQUENCE; i++) {
    encodeStage(s.stages?.[i], out, off + i * STAGE_BYTES);
  }
  out.set(fitBytes(s.trailer, SEQUENCE_TRAILER_BYTES), off + STAGES_PER_SEQUENCE * STAGE_BYTES);
}

/** The per-sequence trailer byte +1 (120 by default). Meaning unknown -- see file header. */
export function getSequenceParam(seq) {
  return seq?.trailer?.[SEQUENCE_PARAM_OFFSET] ?? DEFAULT_SEQUENCE_PARAM;
}

/** Sets the per-sequence trailer byte +1, leaving the other 21 trailer bytes alone. */
export function setSequenceParam(seq, value) {
  seq.trailer = fitBytes(seq.trailer, SEQUENCE_TRAILER_BYTES);
  seq.trailer[SEQUENCE_PARAM_OFFSET] = clampInt(value, 0, 0xff);
  return seq;
}

// ---- slot record (2104 bytes) ----------------------------------------------

/** One slot at the module's observed defaults: header (0.0, 1.0), 4 default sequences. */
export function createDefaultSlot() {
  const slot = {
    header: { floats: [...DEFAULT_HEADER_FLOATS], bytes: new Uint8Array(SLOT_HEADER_BYTES) },
    sequences: Array.from({ length: SEQUENCES_PER_SLOT }, createDefaultSequence),
  };
  writeHeaderFloats(slot.header.bytes, slot.header.floats);
  return slot;
}

function writeHeaderFloats(bytes, floats) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let i = 0; i < 2; i++) {
    const v = Number(floats?.[i]);
    // A NaN (or absent) float keeps whatever raw bytes were already there:
    // JS canonicalizes NaN payloads, so re-encoding one from a Number would
    // silently rewrite bytes we were handed. Losslessness wins over tidiness.
    if (Number.isNaN(v)) continue;
    view.setFloat32(i * 4, v, false); // big-endian
  }
}

/**
 * Decode one 2104-byte slot record.
 * @param {Uint8Array|number[]} input
 * @returns {Slot}
 */
export function decodeSlot(input) {
  const bytes = asBytes(input, "slot");
  if (bytes.length !== SLOT_BYTES) {
    throw new Error(`expected a ${SLOT_BYTES}-byte 251e slot record, got ${bytes.length} bytes`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const header = {
    floats: [view.getFloat32(0, false), view.getFloat32(4, false)],
    bytes: bytes.slice(0, SLOT_HEADER_BYTES),
  };
  const sequences = [];
  for (let b = 0; b < SEQUENCES_PER_SLOT; b++) {
    sequences.push(decodeSequence(bytes, SLOT_HEADER_BYTES + b * SEQUENCE_BLOCK_BYTES));
  }
  return { header, sequences };
}

/**
 * Encode one slot record back to its 2104 bytes.
 * @param {Slot} slot
 * @returns {Uint8Array}
 */
export function encodeSlot(slot) {
  const out = new Uint8Array(SLOT_BYTES);
  // Header: the raw 16 bytes are authoritative (that is what preserves
  // [8:16], whose meaning nobody knows), then the two decoded floats are
  // written over [0:8] so an edited float actually takes effect.
  out.set(fitBytes(slot?.header?.bytes, SLOT_HEADER_BYTES), 0);
  writeHeaderFloats(out.subarray(0, SLOT_HEADER_BYTES), slot?.header?.floats ?? DEFAULT_HEADER_FLOATS);
  for (let b = 0; b < SEQUENCES_PER_SLOT; b++) {
    encodeSequence(slot?.sequences?.[b], out, SLOT_HEADER_BYTES + b * SEQUENCE_BLOCK_BYTES);
  }
  return out;
}

// ---- bank (63120 bytes) ----------------------------------------------------

/** A whole bank of 30 slots at the module's observed defaults. */
export function createDefaultBank() {
  return { slots: Array.from({ length: SLOTS_PER_BANK }, createDefaultSlot) };
}

/**
 * Decode a full 63120-byte 251e preset bank.
 * @param {Uint8Array|number[]} input
 * @returns {Bank}
 */
export function decodeBank(input) {
  const bytes = asBytes(input, "bank");
  if (bytes.length !== BANK_BYTES) {
    throw new Error(
      `expected a ${BANK_BYTES}-byte 251e bank (${SLOTS_PER_BANK} slots x ${SLOT_BYTES} bytes), got ${bytes.length} bytes`
    );
  }
  const slots = [];
  for (let s = 0; s < SLOTS_PER_BANK; s++) {
    slots.push(decodeSlot(bytes.subarray(s * SLOT_BYTES, (s + 1) * SLOT_BYTES)));
  }
  return { slots };
}

/**
 * Encode a bank back into the 63120 bytes a RESTORE sends.
 * @param {Bank} bank
 * @returns {Uint8Array}
 */
export function encodeBank(bank) {
  if (!bank?.slots || bank.slots.length !== SLOTS_PER_BANK) {
    throw new Error(`a bank must have exactly ${SLOTS_PER_BANK} slots`);
  }
  const out = new Uint8Array(BANK_BYTES);
  for (let s = 0; s < SLOTS_PER_BANK; s++) {
    out.set(encodeSlot(bank.slots[s]), s * SLOT_BYTES);
  }
  return out;
}

// ---- cloning ---------------------------------------------------------------

export function cloneStage(stage) {
  return { value: stage.value, pad: stage.pad, time: stage.time, reserved: Uint8Array.from(stage.reserved) };
}

export function cloneSequence(seq) {
  return { stages: seq.stages.map(cloneStage), trailer: Uint8Array.from(seq.trailer) };
}

export function cloneSlot(slot) {
  return {
    header: { floats: [...slot.header.floats], bytes: Uint8Array.from(slot.header.bytes) },
    sequences: slot.sequences.map(cloneSequence),
  };
}

/** Deep clone -- editors should mutate a clone, never the last-read bank directly. */
export function cloneBank(bank) {
  return { slots: bank.slots.map(cloneSlot) };
}

// ---- demo / development data -----------------------------------------------

// The 251e.json reference bank's one PROGRAMMED sequence (block 0), as far as
// the format doc actually records it: the first 15 stage values and the first
// 26 stage times are real observed numbers, the rest is filler so the applet
// has something non-flat to draw. This is DEMO data for the mock transport and
// UI development -- it is not a verbatim capture and must never be presented
// as one.
const REFERENCE_STAGE_VALUES = [0x11, 0xf5, 0x91, 0xa1, 0xe4, 0x89, 0xf3, 0x2e, 0x29, 0x99, 0x6a, 0xda, 0x7f, 0xce, 0xb3];
const REFERENCE_STAGE_TIMES = [
  1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293,
  3, 65, 39, 28, 34, 36, 16, 30, 24, 28, 68,
];

/**
 * A bank with one programmed sequence (slot 0, sequence A) shaped like the
 * reference dump's programmed block, everything else at module defaults --
 * i.e. exactly the "one sequence written, the rest untouched" state a real
 * bank is usually in. For the mock transport and UI work only.
 */
export function createSyntheticBank() {
  const bank = createDefaultBank();
  const seq = bank.slots[0].sequences[0];
  for (let i = 0; i < STAGES_PER_SEQUENCE; i++) {
    const stage = seq.stages[i];
    stage.value = REFERENCE_STAGE_VALUES[i] ?? (i * 37 + 11) % 256;
    stage.time = REFERENCE_STAGE_TIMES[i] ?? 16 + ((i * 7) % 48);
  }
  seq.trailer[SEQUENCE_PARAM_OFFSET] = 0x8c; // 140, the reference block's value
  seq.trailer[SEQUENCE_TRAILER_FLAG_OFFSET] = 2;
  return bank;
}
