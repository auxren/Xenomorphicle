// Standalone Node test (no framework, no deps): run with
//   node tools/251e-sequencer/test/roundtrip.test.mjs
// Exercises:
//   1. sequence-codec: the real 251e bank format -- geometry against the
//      doc's own arithmetic, the observed all-defaults record reproduced
//      byte-for-byte from Buchla_FW/docs/251e-SEQUENCE-FORMAT.md's evidence,
//      encode(decode(bytes)) === bytes over arbitrary bank bytes (the "a
//      user's real preset data survives an edit-and-restore cycle" invariant,
//      undecoded fields included), and an edit touching ONLY its own bytes.
//   2. sysex-transport: buildFrame/parseFrame round-trip, packDump/
//      DumpAssembler reassembly (incl. out-of-order delivery + checksum
//      tamper detection), and buildFrame's 7-bit validation actually
//      rejects an 8-bit byte.
//   3. module addressing: parse/format, and that GET_DUMP/PUT_DUMP actually
//      carry [mod_addr] on the wire (they used to send an empty payload).
//   4. the WHOLE pipeline against MockDevice -- an in-process model of
//      software/src/Bus200eBridge.cpp: read -> edit -> write -> read back,
//      plus the refusal paths (no address, bad checksum, DUMP_DATA with no
//      session, busy).
//
// NOTE on 1 vs 4: protocol v2 widened every packet counter to 14 bits, so a
// whole 63120-byte bank (1503 packets of 42 raw bytes) now fits through one
// transport session -- that is exercised end-to-end below. Most transport
// tests still use small raw fixtures on purpose: they are testing framing, not
// the 251e layout, and 1503-frame fixtures make failures unreadable.
import assert from "node:assert/strict";
import {
  BANK_BYTES,
  SLOT_BYTES,
  SLOT_HEADER_BYTES,
  SLOTS_PER_BANK,
  SEQUENCES_PER_SLOT,
  SEQUENCE_BLOCK_BYTES,
  SEQUENCE_TRAILER_BYTES,
  STAGES_PER_SEQUENCE,
  STAGE_BYTES,
  DEFAULT_STAGE_TIME,
  DEFAULT_SEQUENCE_PARAM,
  SEQUENCE_PARAM_OFFSET,
  SEQUENCE_TRAILER_FLAG_OFFSET,
  decodeBank,
  encodeBank,
  decodeSlot,
  encodeSlot,
  createDefaultSlot,
  createDefaultBank,
  createSyntheticBank,
  cloneBank,
  getSequenceParam,
  setSequenceParam,
} from "../sequence-codec.js";
import {
  buildFrame,
  parseFrame,
  packDump,
  DumpAssembler,
  packChunk,
  unpackChunk,
  SYSEX,
  CMD,
  NAK_ERRORS,
  DUMP_CHUNK_BYTES,
  MAX_PACKED_CHUNK_BYTES,
  MAX_FRAME_BYTES,
  MAX_DUMP_PACKETS,
  MAX_DUMP_BYTES,
  MAX_COUNT_14BIT,
  septetPair,
  fromSeptetPair,
  MockDevice,
  MockTransport,
  parseModuleAddress,
  formatModuleAddress,
  MODULE_ADDRESS_MAX,
} from "../sysex-transport.js";

let passed = 0;

function report(name, err) {
  if (err) {
    console.error(`FAIL - ${name}`);
    console.error(err);
    process.exitCode = 1;
  } else {
    passed++;
    console.log(`ok - ${name}`);
  }
}

function test(name, fn) {
  try {
    fn();
    report(name);
  } catch (err) {
    report(name, err);
  }
}

// Async tests are QUEUED, not started on registration, and then run strictly
// one at a time. Several of them install module-level globals (the fake DOM
// below) or drive a shared MockDevice, so overlapping them would have each
// one stomping the other's world -- which is exactly what happened the first
// time this suite grew async cases.
const asyncQueue = [];
function atest(name, fn) {
  asyncQueue.push({ name, fn });
}
async function runAsyncTests() {
  for (const { name, fn } of asyncQueue) {
    try {
      await fn();
      report(name);
    } catch (err) {
      report(name, err);
    }
  }
}

// ---- fixtures ---------------------------------------------------------------

/**
 * The all-defaults slot record the owner's real 251e actually reported, built
 * BY HAND from the format doc's byte-level description rather than from the
 * codec, so it is an independent check and not a tautology:
 *   header  00 00 00 00 | 3f 80 00 00 | 8 zero bytes      (floats 0.0, 1.0)
 *   stage   00 00 04 00 00 00 00 00 00 00                 (x50, x4 blocks)
 *   trailer 00 78 then 20 zero bytes                      (x4 blocks)
 * (251e-SEQUENCE-FORMAT.md, "2026-08-30, FIRST REAL 251e CAPTURE", sections
 * 1-4. All 15 intact records of that capture are byte-identical, and the
 * reference dump's three unprogrammed blocks match this exactly too.)
 */
function realDefaultSlotBytes() {
  const slot = new Uint8Array(SLOT_BYTES);
  slot.set([0x00, 0x00, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00], 0); // 0.0f, 1.0f BE
  for (let b = 0; b < SEQUENCES_PER_SLOT; b++) {
    const base = SLOT_HEADER_BYTES + b * SEQUENCE_BLOCK_BYTES;
    for (let i = 0; i < STAGES_PER_SEQUENCE; i++) {
      slot[base + i * STAGE_BYTES + 2] = 0x04; // time = 4, little-endian
    }
    slot[base + STAGES_PER_SEQUENCE * STAGE_BYTES + 1] = 0x78; // block param = 120
  }
  return slot;
}

function realDefaultBankBytes() {
  const slot = realDefaultSlotBytes();
  const bank = new Uint8Array(BANK_BYTES);
  for (let s = 0; s < SLOTS_PER_BANK; s++) bank.set(slot, s * SLOT_BYTES);
  return bank;
}

// The one PROGRAMMED block in the Studio H reference dump (251e.json block 0),
// as far as the doc records it: 15 real stage values, 26 real stage times, the
// stray 0x0a at entry 14 byte 7, and trailer `00 8c 00 00 02`. Stages the doc
// doesn't enumerate stay at their defaults -- this fixture is "real where the
// evidence is real", never invented-and-presented-as-captured.
const REF_VALUES = [0x11, 0xf5, 0x91, 0xa1, 0xe4, 0x89, 0xf3, 0x2e, 0x29, 0x99, 0x6a, 0xda, 0x7f, 0xce, 0xb3];
const REF_TIMES = [
  1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293, 1293,
  3, 65, 39, 28, 34, 36, 16, 30, 24, 28, 68,
];

function referenceProgrammedSlotBytes() {
  const slot = realDefaultSlotBytes();
  slot.set([0xbf, 0x80, 0x00, 0x00, 0x3f, 0xa6, 0xf3, 0x7a], 0); // -1.0f, 1.3043053f BE
  const base = SLOT_HEADER_BYTES; // block 0
  for (let i = 0; i < REF_VALUES.length; i++) slot[base + i * STAGE_BYTES] = REF_VALUES[i];
  for (let i = 0; i < REF_TIMES.length; i++) {
    slot[base + i * STAGE_BYTES + 2] = REF_TIMES[i] & 0xff;
    slot[base + i * STAGE_BYTES + 3] = (REF_TIMES[i] >> 8) & 0xff;
  }
  slot[base + 14 * STAGE_BYTES + 7] = 0x0a; // the one non-zero byte in [4:10] anywhere
  const trailer = base + STAGES_PER_SEQUENCE * STAGE_BYTES;
  slot[trailer + SEQUENCE_PARAM_OFFSET] = 0x8c; // 140
  slot[trailer + SEQUENCE_TRAILER_FLAG_OFFSET] = 0x02;
  return slot;
}

/** Deterministic pseudo-random bytes -- a stand-in for an arbitrary capture. */
const rampBytes = (n, mul = 37, add = 11) => Uint8Array.from({ length: n }, (_, i) => (i * mul + add) % 256);

/** A small raw dump for the TRANSPORT tests -- framing coverage, not layout. */
const sampleDump = (n = 517) => rampBytes(n);

// ---- codec: geometry --------------------------------------------------------

test("geometry constants reproduce the doc's arithmetic exactly", () => {
  assert.equal(STAGES_PER_SEQUENCE * STAGE_BYTES + SEQUENCE_TRAILER_BYTES, SEQUENCE_BLOCK_BYTES);
  assert.equal(SEQUENCE_BLOCK_BYTES, 522);
  assert.equal(SLOT_HEADER_BYTES + SEQUENCES_PER_SLOT * SEQUENCE_BLOCK_BYTES, SLOT_BYTES);
  assert.equal(SLOT_BYTES, 2104);
  assert.equal(SLOTS_PER_BANK * SLOT_BYTES, BANK_BYTES);
  assert.equal(BANK_BYTES, 63120); // == the firmware's own `cp.w r7,63120`
});

// ---- codec: ground truth against the real capture ---------------------------

test("createDefaultSlot reproduces the real module's all-defaults record byte-for-byte", () => {
  assert.deepEqual(Array.from(encodeSlot(createDefaultSlot())), Array.from(realDefaultSlotBytes()));
});

test("the default record matches the capture's documented byte census", () => {
  // Every one of these numbers is quoted from 251e-SEQUENCE-FORMAT.md section
  // 1 -- if the codec's layout drifted, one of them would break.
  const slot = encodeSlot(createDefaultSlot());

  // 200 bytes equal 0x04, in four stride-10 runs of 50 at these exact offsets
  const fours = [];
  for (let i = 0; i < slot.length; i++) if (slot[i] === 0x04) fours.push(i);
  assert.equal(fours.length, 200);
  const runStarts = [18, 540, 1062, 1584];
  const runEnds = [508, 1030, 1552, 2074];
  runStarts.forEach((start, r) => {
    for (let i = 0; i < STAGES_PER_SEQUENCE; i++) {
      assert.equal(slot[start + i * 10], 0x04, `expected 0x04 at ${start + i * 10}`);
    }
    assert.equal(start + 49 * 10, runEnds[r]);
  });
  // run starts 18, 540, 1062, 1584 -- deltas 522, 522, 522, zero variance
  assert.deepEqual(runStarts.slice(1).map((v, i) => v - runStarts[i]), [522, 522, 522]);

  // 4 bytes equal 0x78, at block-trailer +1 in every block including the last
  const sevenEights = [];
  for (let i = 0; i < slot.length; i++) if (slot[i] === 0x78) sevenEights.push(i);
  assert.deepEqual(sevenEights, [517, 1039, 1561, 2083]);
  assert.equal(slot.length - 1 - 2083, 20); // the record ends 21 bytes after the last 0x78

  // only 5 distinct byte values exist in the whole intact record
  const distinct = [...new Set(slot)].sort((a, b) => a - b);
  assert.deepEqual(distinct, [0x00, 0x04, 0x3f, 0x78, 0x80]);
});

test("decoding the default record reads back the documented field values", () => {
  const slot = decodeSlot(realDefaultSlotBytes());
  assert.deepEqual(slot.header.floats, [0.0, 1.0]); // the identity offset/scale-looking pair
  assert.equal(slot.sequences.length, SEQUENCES_PER_SLOT);
  for (const seq of slot.sequences) {
    assert.equal(seq.stages.length, STAGES_PER_SEQUENCE);
    assert.equal(getSequenceParam(seq), DEFAULT_SEQUENCE_PARAM);
    assert.equal(getSequenceParam(seq), 120);
    for (const stage of seq.stages) {
      assert.equal(stage.value, 0);
      assert.equal(stage.pad, 0);
      assert.equal(stage.time, DEFAULT_STAGE_TIME);
      assert.equal(stage.time, 4);
      assert.deepEqual(Array.from(stage.reserved), [0, 0, 0, 0, 0, 0]);
    }
  }
});

test("a whole default bank round-trips byte-for-byte (the ground-truth case)", () => {
  const bytes = realDefaultBankBytes();
  assert.equal(bytes.length, 63120);
  const bank = decodeBank(bytes);
  assert.equal(bank.slots.length, SLOTS_PER_BANK);
  assert.deepEqual(Array.from(encodeBank(bank)), Array.from(bytes));
  // ...and the codec's own default bank IS that bank
  assert.deepEqual(Array.from(encodeBank(createDefaultBank())), Array.from(bytes));
});

test("a bank is NOT 7-bit-safe -- it has to go through packChunk", () => {
  // 1.0f contributes 0x80, and a programmed stage value can be anything, so
  // the transport must 7-bit-pack a bank (Bus200eSysExPack). This is a
  // deliberate change from the old placeholder codec, which only ever emitted
  // 7-bit bytes and let that fact quietly leak into the framing layer.
  const bytes = encodeBank(createDefaultBank());
  assert.ok(Array.from(bytes.subarray(0, SLOT_BYTES)).some((b) => b >= 0x80));
});

// ---- codec: the load-bearing round trip -------------------------------------

test("encode(decode(bytes)) is lossless for arbitrary bank bytes, unknown fields included", () => {
  // Every byte position sees a wide spread of values, so undecoded fields
  // (stage byte 1, stage bytes 4-9, all 22 trailer bytes, header bytes 8-15)
  // are all non-zero somewhere -- if any of them were dropped or zeroed this
  // fails. This is the "a real user's preset data survives an edit-and-restore
  // cycle unchanged" invariant.
  const bytes = rampBytes(BANK_BYTES, 37, 11);
  const back = encodeBank(decodeBank(bytes));
  assert.equal(back.length, BANK_BYTES);
  assert.deepEqual(Array.from(back), Array.from(bytes), "byte-for-byte round trip failed");
});

test("encode(decode(bytes)) is lossless for a second, differently-shaped fill", () => {
  const bytes = rampBytes(BANK_BYTES, 101, 200);
  assert.deepEqual(Array.from(encodeBank(decodeBank(bytes))), Array.from(bytes));
});

test("an exotic header float (NaN payload / infinity / denormal) survives byte-for-byte", () => {
  // JS canonicalizes NaN payloads, so re-encoding a NaN from a Number would
  // silently rewrite header bytes. The codec keeps the raw header bytes when
  // the decoded float is NaN; these are the patterns that would expose a
  // regression.
  const bytes = realDefaultBankBytes();
  const patterns = [
    [0x7f, 0xc0, 0x00, 0x01], // NaN with a payload
    [0xff, 0x80, 0x00, 0x00], // -Infinity
    [0x00, 0x00, 0x00, 0x01], // smallest denormal
    [0x7f, 0xff, 0xff, 0xff], // NaN, all mantissa bits
  ];
  patterns.forEach((p, i) => bytes.set(p, i * 4 <= 4 ? i * 4 : (i % 2) * 4 + SLOT_BYTES * i));
  const back = encodeBank(decodeBank(bytes));
  assert.deepEqual(Array.from(back), Array.from(bytes));
});

test("a programmed sequence (the reference dump's block 0) decodes and re-encodes exactly", () => {
  const bytes = referenceProgrammedSlotBytes();
  const slot = decodeSlot(bytes);

  // header: the reference's non-neutral float pair
  assert.equal(slot.header.floats[0], -1.0);
  assert.ok(Math.abs(slot.header.floats[1] - 1.3043053) < 1e-6);

  const block0 = slot.sequences[0];
  REF_VALUES.forEach((v, i) => assert.equal(block0.stages[i].value, v, `stage ${i} value`));
  for (let i = 0; i < 15; i++) assert.equal(block0.stages[i].time, 1293, `stage ${i} time`);
  assert.equal(block0.stages[15].time, 3);
  assert.equal(block0.stages[16].time, 65);
  assert.equal(block0.stages[25].time, 68);
  // the single non-zero byte anywhere in an entry's undecoded [4:10] range
  assert.equal(block0.stages[14].reserved[3], 0x0a);
  assert.equal(getSequenceParam(block0), 0x8c);
  assert.equal(block0.trailer[SEQUENCE_TRAILER_FLAG_OFFSET], 0x02);

  // blocks 1-3 are the same defaults the real capture shows
  for (let b = 1; b < SEQUENCES_PER_SLOT; b++) {
    assert.equal(getSequenceParam(slot.sequences[b]), 120);
    for (const stage of slot.sequences[b].stages) {
      assert.equal(stage.value, 0);
      assert.equal(stage.time, 4);
    }
  }

  assert.deepEqual(Array.from(encodeSlot(slot)), Array.from(bytes));
});

test("editing one stage changes ONLY that stage's bytes in the whole 63KB bank", () => {
  const original = realDefaultBankBytes();
  const bank = decodeBank(original);
  const SLOT = 7;
  const SEQ = 2; // sequence C
  const STAGE = 12;
  const stage = bank.slots[SLOT].sequences[SEQ].stages[STAGE];
  stage.value = 0x5a;
  stage.time = 300; // 0x012c -> two bytes move, the high one from 0

  const edited = encodeBank(bank);
  const moved = [];
  for (let i = 0; i < BANK_BYTES; i++) if (edited[i] !== original[i]) moved.push(i);

  const stageOff = SLOT * SLOT_BYTES + SLOT_HEADER_BYTES + SEQ * SEQUENCE_BLOCK_BYTES + STAGE * STAGE_BYTES;
  assert.deepEqual(moved, [stageOff, stageOff + 2, stageOff + 3]);
  assert.equal(edited[stageOff], 0x5a);
  assert.equal(edited[stageOff + 2], 0x2c);
  assert.equal(edited[stageOff + 3], 0x01);
});

test("setSequenceParam moves exactly one trailer byte", () => {
  const original = realDefaultBankBytes();
  const bank = decodeBank(original);
  setSequenceParam(bank.slots[3].sequences[1], 140);
  const edited = encodeBank(bank);
  const moved = [];
  for (let i = 0; i < BANK_BYTES; i++) if (edited[i] !== original[i]) moved.push(i);
  const off =
    3 * SLOT_BYTES + SLOT_HEADER_BYTES + 1 * SEQUENCE_BLOCK_BYTES + STAGES_PER_SEQUENCE * STAGE_BYTES + SEQUENCE_PARAM_OFFSET;
  assert.deepEqual(moved, [off]);
  assert.equal(edited[off], 0x8c);
});

test("editing a header float moves only its 4 bytes, leaving header[8:16] alone", () => {
  const original = rampBytes(BANK_BYTES, 37, 11); // header bytes 8-15 non-zero here
  const bank = decodeBank(original);
  bank.slots[2].header.floats[1] = 1.0;
  const edited = encodeBank(bank);
  const moved = [];
  for (let i = 0; i < BANK_BYTES; i++) if (edited[i] !== original[i]) moved.push(i);
  const base = 2 * SLOT_BYTES + 4;
  assert.ok(moved.every((i) => i >= base && i < base + 4), `unexpected bytes moved: ${moved}`);
  assert.deepEqual(Array.from(edited.subarray(base, base + 4)), [0x3f, 0x80, 0x00, 0x00]);
});

test("decodeBank rejects anything that isn't exactly one bank", () => {
  assert.throws(() => decodeBank(new Uint8Array(BANK_BYTES - 1)), /63120-byte 251e bank/);
  assert.throws(() => decodeBank(new Uint8Array(BANK_BYTES + 1)), /63120-byte 251e bank/);
  assert.throws(() => decodeBank(new Uint8Array(SLOT_BYTES)), /63120-byte 251e bank/);
  assert.throws(() => decodeSlot(new Uint8Array(SLOT_BYTES - 1)), /2104-byte 251e slot/);
});

test("encodeBank refuses a bank with the wrong number of slots", () => {
  const bank = createDefaultBank();
  bank.slots.pop();
  assert.throws(() => encodeBank(bank), /exactly 30 slots/);
});

test("cloneBank produces an independent deep copy (raw fields included)", () => {
  const bank = createSyntheticBank();
  const clone = cloneBank(bank);
  clone.slots[0].sequences[0].stages[0].value = 0xaa;
  clone.slots[0].sequences[0].stages[0].reserved[0] = 0xbb;
  clone.slots[0].sequences[0].trailer[SEQUENCE_PARAM_OFFSET] = 0xcc;
  clone.slots[0].header.floats[0] = 9;
  assert.notEqual(bank.slots[0].sequences[0].stages[0].value, 0xaa);
  assert.equal(bank.slots[0].sequences[0].stages[0].reserved[0], 0);
  assert.notEqual(getSequenceParam(bank.slots[0].sequences[0]), 0xcc);
  assert.notEqual(bank.slots[0].header.floats[0], 9);
});

test("createSyntheticBank is a valid bank that differs from the default one", () => {
  const bytes = encodeBank(createSyntheticBank());
  assert.equal(bytes.length, BANK_BYTES);
  assert.notDeepEqual(Array.from(bytes), Array.from(realDefaultBankBytes()));
  assert.deepEqual(Array.from(encodeBank(decodeBank(bytes))), Array.from(bytes));
  // only slot 0's sequence A was programmed; slots 1..29 stay at defaults
  const defaultSlot = Array.from(realDefaultSlotBytes());
  for (let s = 1; s < SLOTS_PER_BANK; s++) {
    assert.deepEqual(Array.from(bytes.subarray(s * SLOT_BYTES, (s + 1) * SLOT_BYTES)), defaultSlot, `slot ${s} moved`);
  }
});

// ---- transport: frame build/parse ------------------------------------------

test("buildFrame/parseFrame round-trip a command + payload", () => {
  const frame = buildFrame(CMD.INFO, [1, 2, 3]);
  assert.equal(frame[0], SYSEX.START);
  assert.equal(frame[frame.length - 1], SYSEX.END);
  const parsed = parseFrame(frame);
  assert.equal(parsed.cmd, CMD.INFO);
  assert.deepEqual(parsed.payload, [1, 2, 3]);
});

test("buildFrame rejects non-7-bit payload bytes", () => {
  assert.throws(() => buildFrame(CMD.DUMP_DATA, [0, 2, 255]), RangeError);
});

test("parseFrame returns null for a frame from a different app/family", () => {
  const foreign = Uint8Array.from([0xf0, 0x7d, 0x62, 0x4d, 0x01, CMD.INFO, 0xf7]); // Captain MIDI's app id
  assert.equal(parseFrame(foreign), null);
});

// ---- transport: dump packing / reassembly ----------------------------------

test("packDump + DumpAssembler reassemble a dump exactly", () => {
  const bytes = sampleDump();
  const assembler = new DumpAssembler();
  for (const frame of packDump(bytes)) assembler.feed(frame);
  assert.ok(assembler.done);
  assert.equal(assembler.error, null);
  assert.deepEqual(Array.from(assembler.bytes), Array.from(bytes));
});

test("DumpAssembler tolerates out-of-order DUMP_DATA delivery", () => {
  const bytes = sampleDump();
  const frames = packDump(bytes);
  const shuffled = [...frames.slice(0, -1)].reverse().concat(frames[frames.length - 1]);
  const assembler = new DumpAssembler();
  for (const frame of shuffled) assembler.feed(frame);
  assert.deepEqual(Array.from(assembler.bytes), Array.from(bytes));
});

test("DumpAssembler flags a tampered checksum", () => {
  const bytes = sampleDump();
  const frames = packDump(bytes);
  // v2 payload = [seq_lo, seq_hi, total_lo, total_hi, hibits, data0, ...].
  // Index 4 is the packed group's hibits byte (whose low 7 bits, once
  // unpacked, only ever touch the high/8th bit of each raw byte, which xor7
  // deliberately discards via its own "& 0x7F" fold -- see Bus200eSysExXor7 --
  // so corrupting it wouldn't actually move the checksum). Index 5 is the
  // first packed *data* byte -- flipping its low 7 bits corrupts the
  // reconstructed raw byte's checksum-relevant bits.
  frames[0].payload[5] ^= 0x7f;
  const assembler = new DumpAssembler();
  for (const frame of frames) assembler.feed(frame);
  assert.ok(assembler.done);
  assert.match(assembler.error, /checksum/);
});

test("DumpAssembler flags a missing packet", () => {
  const bytes = sampleDump();
  const frames = packDump(bytes);
  assert.ok(frames.length > 2, "test assumes a multi-packet dump");
  const withGap = frames.filter((_, i) => i !== 1);
  const assembler = new DumpAssembler();
  for (const frame of withGap) assembler.feed(frame);
  assert.ok(assembler.done);
  assert.ok(assembler.error);
});

// ---- transport: header/command constants match firmware exactly -----------
// (software/src/Bus200eSysEx.h -- the "THIS IS THE CONTRACT" header)

test("SYSEX header constants match Bus200eSysEx.h", () => {
  assert.equal(SYSEX.MFR_ID, 0x7d);
  assert.equal(SYSEX.FAMILY_ID, 0x62);
  assert.equal(SYSEX.APP_ID, 0x35);
  assert.equal(SYSEX.PROTO_VERSION, 0x02); // v2 = 14-bit packet counters
});

test("CMD bytes match Bus200eSysExCmd, including STATUS/STATUS_R", () => {
  assert.equal(CMD.INFO, 0x01);
  assert.equal(CMD.GET_DUMP, 0x04);
  assert.equal(CMD.PUT_DUMP, 0x05);
  assert.equal(CMD.STATUS, 0x10);
  assert.equal(CMD.ACK, 0x40);
  assert.equal(CMD.INFO_R, 0x41);
  assert.equal(CMD.DUMP_DATA, 0x44);
  assert.equal(CMD.DUMP_END, 0x45);
  assert.equal(CMD.STATUS_R, 0x50);
  assert.equal(CMD.NAK, 0x7e);
});

test("STATUS/STATUS_R are encodable/decodable even though the UI doesn't use them yet", () => {
  const req = buildFrame(CMD.STATUS, []);
  const parsedReq = parseFrame(req);
  assert.equal(parsedReq.cmd, CMD.STATUS);
  assert.deepEqual(parsedReq.payload, []);

  // STATUS_R payload = [state, error, mod_addr, is_restore] -- plain 7-bit,
  // NOT packed (only DUMP_DATA chunks are packed).
  const reply = buildFrame(CMD.STATUS_R, [2, 0, 0x3c, 1]);
  const parsedReply = parseFrame(reply);
  assert.equal(parsedReply.cmd, CMD.STATUS_R);
  assert.deepEqual(parsedReply.payload, [2, 0, 0x3c, 1]);
});

test("NAK_ERRORS matches Bus200eSysExNakReason (1,2,5,6,7,8,9 -- not 3/4)", () => {
  assert.deepEqual(Object.keys(NAK_ERRORS).map(Number).sort((a, b) => a - b), [1, 2, 5, 6, 7, 8, 9]);
});

// ---- transport: 7-bit packing (Bus200eSysExPack/Unpack port) ---------------
// Mirrors software/test/test_bus200e_sysex.cpp's own coverage so the JS and
// C++ implementations are provably compatible, not just each internally
// self-consistent.

test("packChunk/unpackChunk round-trip every raw byte value, various lengths", () => {
  // Same shape as test_pack_unpack_round_trip in test_bus200e_sysex.cpp: a
  // full 8-bit spread (i*37+5 mod 256) at a range of group-boundary lengths.
  const raw = Array.from({ length: 300 }, (_, i) => (i * 37 + 5) % 256);
  for (const n of [0, 1, 6, 7, 8, 13, 14, 15, 44, 100, 255, 300]) {
    const slice = raw.slice(0, n);
    const packed = packChunk(slice);
    for (const b of packed) assert.ok(b >= 0 && b <= 0x7f, `packed byte ${b} is not 7-bit-clean`);
    const back = unpackChunk(packed);
    assert.deepEqual(back, slice, `round trip failed for n=${n}`);
  }
});

test("packChunk matches test_bus200e_sysex.cpp's worked example exactly", () => {
  // test_pack_exact_bytes: raw = {0x80, 0x7F, 0x81} (MSBs 1,0,1 -> hibits 0x05)
  const packed = packChunk([0x80, 0x7f, 0x81]);
  assert.deepEqual(packed, [0x05, 0x00, 0x7f, 0x01]);
  assert.deepEqual(unpackChunk(packed), [0x80, 0x7f, 0x81]);
});

test("packChunk(42 raw bytes) hits BUS200E_SYSEX_MAX_PACKED (48 bytes) exactly", () => {
  const rawChunk = new Array(DUMP_CHUNK_BYTES).fill(0);
  assert.equal(DUMP_CHUNK_BYTES, 42); // v2: 44 -> 42, the counters grew 2 -> 4 bytes
  const packed = packChunk(rawChunk);
  assert.equal(packed.length, MAX_PACKED_CHUNK_BYTES);
  assert.equal(packed.length, 48);
  // + seq/total septet pairs(4) + header/footer(7) == 59, just under the
  // 60-byte ceiling (pack() grows in 8-byte groups, so nothing lands on it)
  assert.equal(4 + 7 + packed.length, MAX_FRAME_BYTES);
  assert.equal(MAX_FRAME_BYTES, 59);
});

test("unpackChunk returns null (malformed) for a lone hibits byte with no data following", () => {
  // test_unpack_malformed: a single 0x00 byte unpacks to -1 in C++; here
  // that's represented as null rather than an exception/sentinel, per this
  // module's existing "return null, don't throw, for a recognizable-but-bad
  // input" convention (see parseFrame).
  assert.equal(unpackChunk([0x00]), null);
});

test("packDump/DumpAssembler round-trip a dump with full 8-bit bytes (>= 0x80)", () => {
  // A real 251e bank is full 8-bit data, which is exactly what
  // Bus200eSysExPack/Unpack exist for -- spread across 0-255 and several
  // DUMP_DATA chunks, mirroring test_full_dump_stream_shape.
  const bytes = Uint8Array.from({ length: 130 }, (_, i) => (i * 5 + 3) % 256);
  const frames = packDump(bytes);
  // every DUMP_DATA payload byte, including the packed chunk, must be 7-bit
  for (const { payload } of frames) {
    for (const b of payload) assert.ok(b >= 0 && b <= 0x7f, `frame payload byte ${b} is not 7-bit-clean`);
  }
  const assembler = new DumpAssembler();
  for (const frame of frames) assembler.feed(frame);
  assert.ok(assembler.done);
  assert.equal(assembler.error, null);
  assert.deepEqual(Array.from(assembler.bytes), Array.from(bytes));
});

test("DumpAssembler.feed flags a malformed packed DUMP_DATA chunk", () => {
  const assembler = new DumpAssembler();
  // seq=0, total=1 (each a 14-bit septet pair), packed chunk = a lone hibits
  // byte with nothing after it
  const result = assembler.feed({ cmd: CMD.DUMP_DATA, payload: [0, 0, 1, 0, 0x00] });
  assert.equal(result, true);
  assert.ok(assembler.done);
  assert.match(assembler.error, /malformed packed chunk/);
});

test("a whole 63120-byte bank fits through one transport session (protocol v2)", () => {
  // The reason v2's 14-bit counters exist: under v1 (127 packets x 44 bytes =
  // 5588) the one transfer this applet is for did not fit at all.
  const packetsForABank = Math.ceil(BANK_BYTES / DUMP_CHUNK_BYTES);
  assert.equal(packetsForABank, 1503);
  assert.ok(packetsForABank <= MAX_DUMP_PACKETS, "a bank must fit the device's card image");
  assert.ok(BANK_BYTES <= MAX_DUMP_BYTES);
  assert.equal(packDump(encodeBank(createDefaultBank())).length, packetsForABank + 1); // + DUMP_END
});

test("14-bit septet pairs carry the counters, low septet first", () => {
  assert.deepEqual(septetPair(0), [0, 0]);
  assert.deepEqual(septetPair(1), [1, 0]);
  assert.deepEqual(septetPair(128), [0, 1]);
  assert.deepEqual(septetPair(1503), [1503 & 0x7f, 1503 >> 7]);
  assert.equal(fromSeptetPair(...septetPair(1502)), 1502);
  assert.equal(MAX_COUNT_14BIT, 16383);
  assert.throws(() => septetPair(MAX_COUNT_14BIT + 1), RangeError);
});

test("DUMP_DATA counters past 127 survive the wire (the v1 ceiling)", () => {
  // seq 200 is unrepresentable in v1's single-byte counter; here it must
  // round-trip through buildFrame/parseFrame/DumpAssembler intact.
  const bytes = rampBytes(DUMP_CHUNK_BYTES * 300);
  const frames = packDump(bytes);
  assert.equal(frames.length, 301);
  const parsed = parseFrame(buildFrame(frames[200].cmd, frames[200].payload));
  assert.equal(fromSeptetPair(parsed.payload[0], parsed.payload[1]), 200);
  assert.equal(fromSeptetPair(parsed.payload[2], parsed.payload[3]), 300);
  const assembler = new DumpAssembler();
  for (const f of frames) assembler.feed(parseFrame(buildFrame(f.cmd, f.payload)));
  assert.equal(assembler.error, null);
  assert.deepEqual(Array.from(assembler.bytes), Array.from(bytes));
});

test("packDump refuses a dump the device's card image can't stage", () => {
  const tooBig = new Uint8Array(MAX_DUMP_BYTES + DUMP_CHUNK_BYTES);
  assert.throws(() => packDump(tooBig), RangeError);
});

// ---- module addressing -----------------------------------------------------
// GET_DUMP/PUT_DUMP both carry [mod_addr] (Bus200eSysEx.h). Until the applet
// grew an address field these went out with an EMPTY payload and the firmware
// had nothing to target; Bus200eBridge.cpp NAKs that (code 2) rather than
// guessing which module on the bus to touch.

test("parseModuleAddress accepts the forms a bench user actually types", () => {
  assert.equal(parseModuleAddress("2e"), 0x2e);
  assert.equal(parseModuleAddress("2E"), 0x2e);
  assert.equal(parseModuleAddress("0x2E"), 0x2e);
  assert.equal(parseModuleAddress("5c"), 0x5c); // the owner's own 251e
  assert.equal(parseModuleAddress(" 7 ".trim()), 0x07);
  assert.equal(parseModuleAddress("00"), 0x00);
  assert.equal(parseModuleAddress("7f"), MODULE_ADDRESS_MAX);
});

test("parseModuleAddress rejects junk and non-7-bit values", () => {
  for (const bad of ["", "  ", "g1", "1234", "-1", "80", "FF", null, undefined]) {
    assert.throws(() => parseModuleAddress(bad), `expected "${bad}" to be rejected`);
  }
});

test("formatModuleAddress matches the firmware console's %02X", () => {
  assert.equal(formatModuleAddress(0x2e), "2E");
  assert.equal(formatModuleAddress(0x07), "07");
  assert.equal(formatModuleAddress(0), "00");
});

test("MAX_DUMP_PACKETS/MAX_DUMP_BYTES match Bus200eBridge.h", () => {
  // v2's ceiling is the device's 64 KB card image, not the wire's counters
  assert.equal(MAX_DUMP_BYTES, 65536); // BUSCARD_SIZE
  assert.equal(MAX_DUMP_PACKETS, Math.ceil(65536 / DUMP_CHUNK_BYTES));
  assert.equal(MAX_DUMP_PACKETS, 1561);
});

// ---- end-to-end against the MockDevice -------------------------------------
// MockDevice models software/src/Bus200eBridge.cpp at the frame level, so
// these exercise the REAL path: packDump -> packChunk -> buildFrame ->
// (wire) -> parseFrame -> DumpAssembler -> stored bytes.

async function connectedMock(opts = {}) {
  const t = new MockTransport({ latencyMs: 0, ...opts });
  await t.connect();
  return t;
}

atest("GET_DUMP carries the module address on the wire", async () => {
  const sent = [];
  const device = new MockDevice({ initialBytes: sampleDump() });
  const realHandle = device.handle.bind(device);
  device.handle = (frame) => {
    sent.push(frame);
    return realHandle(frame);
  };
  const t = await connectedMock({ device });
  await t.readDump(0x5c);
  const get = sent.find((f) => f.cmd === CMD.GET_DUMP);
  assert.ok(get, "no GET_DUMP reached the device");
  assert.deepEqual(get.payload, [0x5c], "GET_DUMP payload must be [mod_addr]");
  assert.equal(device.lastModAddr, 0x5c);
});

atest("PUT_DUMP carries the module address on the wire", async () => {
  const sent = [];
  const device = new MockDevice();
  const realHandle = device.handle.bind(device);
  device.handle = (frame) => {
    sent.push(frame);
    return realHandle(frame);
  };
  const t = await connectedMock({ device });
  await t.writeDump(sampleDump(), 0x3c);
  const put = sent.find((f) => f.cmd === CMD.PUT_DUMP);
  assert.ok(put, "no PUT_DUMP reached the device");
  assert.deepEqual(put.payload, [0x3c], "PUT_DUMP payload must be [mod_addr]");
  assert.equal(device.lastModAddr, 0x3c);
  assert.equal(device.restores, 1, "a clean DUMP_END must trigger exactly one restore");
});

atest("a hex-string module address works the same as a number (the UI passes text)", async () => {
  const t = await connectedMock({ initialBytes: sampleDump() });
  const bytes = await t.readDump("5c");
  assert.equal(t.device.lastModAddr, 0x5c);
  assert.equal(bytes.length, 517);
});

atest("an out-of-range module address is refused before anything hits the wire", async () => {
  const t = await connectedMock();
  const before = t.device.captures;
  await assert.rejects(() => t.readDump(0x80), /0x00-0x7F/);
  await assert.rejects(() => t.writeDump(sampleDump(), -1), /0x00-0x7F/);
  assert.equal(t.device.captures, before, "nothing should have been sent");
});

atest("full pipeline: read -> edit -> write -> read back, byte-for-byte", async () => {
  // A slot-sized (2104-byte) record: the largest piece of real 251e data that
  // fits through one transport session today, so the framing gets exercised
  // against genuine bank bytes rather than a synthetic ramp.
  const seeded = encodeSlot(createDefaultSlot());
  const t = await connectedMock({ initialBytes: seeded });

  const readBytes = await t.readDump(0x5c);
  assert.deepEqual(Array.from(readBytes), Array.from(seeded));

  // edit it exactly the way the UI does, through the codec
  const slot = decodeSlot(readBytes);
  slot.sequences[1].stages[3].value = 0xe4;
  slot.sequences[1].stages[3].time = 1293;
  setSequenceParam(slot.sequences[1], 140);
  const written = encodeSlot(slot);
  assert.notDeepEqual(Array.from(written), Array.from(seeded));
  await t.writeDump(written, 0x5c);

  const back = await t.readDump(0x5c);
  assert.deepEqual(Array.from(back), Array.from(written));
  const backSlot = decodeSlot(back);
  assert.equal(backSlot.sequences[1].stages[3].value, 0xe4);
  assert.equal(backSlot.sequences[1].stages[3].time, 1293);
  assert.equal(getSequenceParam(backSlot.sequences[1]), 140);
  // and nothing else moved
  assert.equal(backSlot.sequences[0].stages[3].value, 0);
  assert.deepEqual(backSlot.header.floats, [0.0, 1.0]);
});

atest("end to end: a WHOLE bank reads, edits, writes and reads back unchanged", async () => {
  // The transfer this entire applet exists for: 63120 bytes, 1503 DUMP_DATA
  // frames each way, through the real framing code, with the codec on both
  // ends. Slot 12's sequence D gets one stage changed; every other byte of
  // all 30 slots -- undecoded fields included -- must come back identical.
  const seeded = encodeBank(createDefaultBank());
  const t = await connectedMock({ initialBytes: seeded });

  const readBytes = await t.readDump(0x5c);
  assert.equal(readBytes.length, BANK_BYTES);
  assert.deepEqual(Array.from(readBytes), Array.from(seeded));

  const bank = decodeBank(readBytes);
  bank.slots[12].sequences[3].stages[7].value = 0xb3;
  bank.slots[12].sequences[3].stages[7].time = 1293;
  const written = encodeBank(bank);
  await t.writeDump(written, 0x5c);

  const back = await t.readDump(0x5c);
  assert.deepEqual(Array.from(back), Array.from(written));

  const moved = [];
  for (let i = 0; i < BANK_BYTES; i++) if (back[i] !== seeded[i]) moved.push(i);
  const stageOff = 12 * SLOT_BYTES + SLOT_HEADER_BYTES + 3 * SEQUENCE_BLOCK_BYTES + 7 * STAGE_BYTES;
  assert.deepEqual(moved, [stageOff, stageOff + 2, stageOff + 3]);
});

atest("the round trip survives full 8-bit dump bytes (a REAL 200e capture)", async () => {
  const raw = rampBytes(517);
  const t = await connectedMock();
  await t.writeDump(raw, 0x2e);
  const back = await t.readDump(0x2e);
  assert.deepEqual(Array.from(back), Array.from(raw));
});

atest("device NAKs a DUMP_END whose checksum doesn't match, and never restores", async () => {
  const device = new MockDevice();
  const t = await connectedMock({ device });
  // drive the frames by hand so we can corrupt one mid-stream
  const frames = packDump(sampleDump());
  frames[0].payload[5] ^= 0x7f; // corrupt a packed data byte (see the tamper test above)
  device.handle({ cmd: CMD.PUT_DUMP, payload: [0x2e] });
  for (const f of frames.slice(0, -1)) device.handle(f);
  const reply = device.handle(frames[frames.length - 1]);
  assert.equal(reply[0].cmd, CMD.NAK);
  assert.equal(reply[0].payload[1], 6, "NAK 6 = packet-count/checksum mismatch");
  assert.equal(device.restores, 0, "a bad transfer must never reach MasterRestore");
  assert.equal(NAK_ERRORS[6], "dump checksum/packet-count mismatch");
  await t.disconnect();
});

test("device NAKs DUMP_DATA that arrives outside a PUT_DUMP session", () => {
  const device = new MockDevice();
  const reply = device.handle({ cmd: CMD.DUMP_DATA, payload: [0, 0, 1, 0, 0x00, 0x01] });
  assert.equal(reply[0].cmd, CMD.NAK);
  assert.equal(reply[0].payload[1], 2);
  assert.equal(device.restores, 0);
});

test("device NAKs a second job while one is in flight (NAK 5 = busy)", () => {
  const device = new MockDevice();
  device.handle({ cmd: CMD.PUT_DUMP, payload: [0x2e] });
  const reply = device.handle({ cmd: CMD.GET_DUMP, payload: [0x2f] });
  assert.equal(reply[0].cmd, CMD.NAK);
  assert.equal(reply[0].payload[1], 5);
  assert.equal(NAK_ERRORS[5], "busy (a master job is already running)");
});

test("device NAKs an addressless GET_DUMP/PUT_DUMP rather than guessing a module", () => {
  const device = new MockDevice();
  for (const cmd of [CMD.GET_DUMP, CMD.PUT_DUMP]) {
    const reply = device.handle({ cmd, payload: [] });
    assert.equal(reply[0].cmd, CMD.NAK);
    assert.equal(reply[0].payload[1], 2);
  }
  assert.equal(device.lastModAddr, null);
});

atest("INFO_R reports 0/0 sequences+steps: firmware never decodes the 251e layout", async () => {
  const t = await connectedMock({ initialBytes: sampleDump() });
  const info = await t.info();
  assert.equal(info.schema, 2); // schema 2 = protocol v2 layout
  assert.equal(info.nSequences, 0);
  assert.equal(info.maxSteps, 0);
  assert.equal(info.chunkBytes, DUMP_CHUNK_BYTES);
  assert.equal(info.maxPackets, MAX_DUMP_PACKETS);
  assert.equal(info.lastDumpBytes, 517); // 14-bit lo/hi split reassembles
});

atest("a transport that isn't connected refuses to send anything", async () => {
  const t = new MockTransport();
  await assert.rejects(() => t.readDump(0x2e), /not connected/);
  await assert.rejects(() => t.writeDump(sampleDump(), 0x2e), /not connected/);
});

// ---- app.js UI wiring (against a minimal fake DOM) --------------------------
// The address picker only matters if it actually reaches the transport, and
// the Read/Write buttons only being live once an address is entered is the
// safety property (no blind bus traffic). Neither is provable from
// sysex-transport.js alone, so app.js is loaded here against a hand-rolled
// DOM stub -- just enough surface for what app.js touches, no jsdom dep.

function fakeElement(tag = "div") {
  const el = {
    tagName: tag,
    className: "",
    classes: new Set(),
    style: {},
    children: [],
    value: "",
    textContent: "",
    title: "",
    hidden: false,
    disabled: false,
    selectionStart: 0,
    _listeners: {},
    _html: "",
    classList: {
      toggle(cls, force) {
        const on = force === undefined ? !el.classes.has(cls) : !!force;
        if (on) el.classes.add(cls);
        else el.classes.delete(cls);
        return on;
      },
      add: (c) => el.classes.add(c),
      remove: (c) => el.classes.delete(c),
      contains: (c) => el.classes.has(c),
    },
    set innerHTML(v) {
      el._html = v;
      if (v === "") el.children = [];
    },
    get innerHTML() {
      return el._html;
    },
    appendChild: (child) => el.children.push(child),
    prepend: (child) => el.children.unshift(child),
    querySelectorAll: () => [],
    setSelectionRange() {},
    addEventListener(type, fn) {
      (el._listeners[type] ??= []).push(fn);
    },
    removeEventListener() {},
    dispatch(type, ev = {}) {
      for (const fn of el._listeners[type] ?? []) fn(ev);
    },
  };
  return el;
}

const UI_IDS = [
  "grid",
  "slot-select",
  "seq-tabs",
  "slot-meta",
  "transport-mode",
  "connect-btn",
  "module-addr",
  "read-btn",
  "write-btn",
  "status",
  "log",
  "dirty-badge",
];

async function loadAppWithFakeDom({ storedAddr = null, search = "" } = {}) {
  const byId = Object.fromEntries(UI_IDS.map((id) => [id, fakeElement()]));
  byId["transport-mode"].value = "mock";
  const store = new Map();
  if (storedAddr) store.set("251e-sequencer.module-addr", storedAddr);

  globalThis.document = {
    getElementById: (id) => byId[id] ?? null,
    createElement: (tag) => fakeElement(tag),
  };
  globalThis.localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, v),
  };
  globalThis.location = { search };

  // cache-bust so each scenario gets a fresh module instance
  await import(`../app.js?t=${Math.random()}`);
  return { els: byId, store };
}

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

const logText = (els) => els.log.children.map((c) => c.textContent).join("\n");

const byClass = (el, cls) => el.children.find((c) => c.className === cls);
const stageCells = (els) => byClass(els.grid.children[0], "steps").children;

atest("app.js: Read/Write stay disabled until a valid module address is entered", async () => {
  const { els } = await loadAppWithFakeDom();
  assert.equal(els["read-btn"].disabled, true);
  assert.equal(els["write-btn"].disabled, true);

  els["connect-btn"].dispatch("click");
  await settle();
  // connected, but still no address: the buttons must NOT come alive
  assert.match(els.status.textContent, /connected/);
  assert.equal(els["read-btn"].disabled, true, "connecting alone must not enable Read");
  assert.equal(els["write-btn"].disabled, true);

  els["module-addr"].value = "zz";
  els["module-addr"].dispatch("input");
  assert.equal(els["read-btn"].disabled, true, "an invalid address must not enable Read");
  assert.ok(els["module-addr"].classes.has("invalid"));

  els["module-addr"].value = "5c";
  els["module-addr"].dispatch("input");
  assert.equal(els["module-addr"].value, "5C", "the field uppercases as you type");
  assert.equal(els["read-btn"].disabled, false);
  assert.equal(els["write-btn"].disabled, false);
  assert.equal(els["module-addr"].classes.has("invalid"), false);
});

atest("app.js: Read pulls a whole 63120-byte bank and decodes it", async () => {
  const { els, store } = await loadAppWithFakeDom();
  els["connect-btn"].dispatch("click");
  await settle();
  els["module-addr"].value = "5C"; // the owner's own 251e
  els["module-addr"].dispatch("input");

  els["read-btn"].dispatch("click");
  await settle();
  await settle();

  // A whole bank through the mock: 1503 DUMP_DATA frames, reassembled,
  // decoded into 30 slots, and drawn.
  assert.equal(els.status.textContent, "read complete", `status was: ${els.status.textContent}`);
  assert.match(logText(els), new RegExp(`read ${BANK_BYTES} bytes from module 5C`));
  assert.equal(stageCells(els).length, STAGES_PER_SEQUENCE);
  assert.equal(els["dirty-badge"].hidden, true, "a fresh read is not a pending edit");
  // the module the user aimed at is remembered for next session
  assert.equal(store.get("251e-sequencer.module-addr"), "5C");
});

atest("app.js: Write sends the whole bank back, not just the edited slot", async () => {
  const { els } = await loadAppWithFakeDom();
  els["connect-btn"].dispatch("click");
  await settle();
  els["module-addr"].value = "3C";
  els["module-addr"].dispatch("input");

  // edit one stage first, so the write is a real edit-and-restore cycle
  const valueInput = stageCells(els)[0].children.find((c) => c.className === "step-value");
  valueInput.value = "77";
  valueInput.dispatch("input");
  assert.equal(els["dirty-badge"].hidden, false);

  els["write-btn"].dispatch("click");
  await settle();
  await settle();

  assert.equal(els.status.textContent, "write complete", `status was: ${els.status.textContent}`);
  // a RESTORE is all 30 slots, always -- that is what the module accepts
  assert.match(logText(els), new RegExp(`wrote ${BANK_BYTES} bytes to module 3C`));
  assert.equal(els["dirty-badge"].hidden, true, "a successful write clears the dirty badge");
});

atest("app.js: the grid renders 50 stages, 4 sequence tabs and 30 slots", async () => {
  const { els } = await loadAppWithFakeDom();
  assert.equal(stageCells(els).length, STAGES_PER_SEQUENCE);
  assert.equal(els["seq-tabs"].children.length, SEQUENCES_PER_SLOT);
  assert.deepEqual(
    els["seq-tabs"].children.map((c) => c.textContent),
    ["A", "B", "C", "D"]
  );
  assert.equal(els["slot-select"].children.length, SLOTS_PER_BANK);
  assert.equal(els["slot-select"].children[0].textContent, "slot 1");
  assert.equal(els["slot-select"].children[29].textContent, "slot 30");
});

atest("app.js: the slot selector and sequence tabs re-render the grid", async () => {
  const { els } = await loadAppWithFakeDom();
  const caption = () => byClass(els.grid.children[0], "sequence-header").children.find((c) => c.className === "sequence-caption").textContent;
  assert.match(caption(), /slot 1 \/ sequence A/);

  els["seq-tabs"].children[2].dispatch("click");
  assert.match(caption(), /slot 1 \/ sequence C/);
  assert.equal(stageCells(els).length, STAGES_PER_SEQUENCE);

  els["slot-select"].value = "6";
  els["slot-select"].dispatch("change");
  assert.match(caption(), /slot 7 \/ sequence C/);
});

atest("app.js: ?slot= opens on that preset, and editing a stage marks the bank dirty", async () => {
  const { els } = await loadAppWithFakeDom({ search: "?slot=12" });
  const caption = byClass(els.grid.children[0], "sequence-header").children.find((c) => c.className === "sequence-caption");
  assert.match(caption.textContent, /slot 12 \/ sequence A/);

  assert.equal(els["dirty-badge"].hidden, true);
  const valueInput = stageCells(els)[3].children.find((c) => c.className === "step-value");
  valueInput.value = "200";
  valueInput.dispatch("input");
  assert.equal(els["dirty-badge"].hidden, false, "editing a stage must raise the unsaved-edits badge");
});

atest("app.js: a remembered address is restored, and ?module= overrides it", async () => {
  const remembered = await loadAppWithFakeDom({ storedAddr: "5C" });
  assert.equal(remembered.els["module-addr"].value, "5C");
  assert.equal(remembered.els["read-btn"].disabled, true, "still needs a connection");

  const overridden = await loadAppWithFakeDom({ storedAddr: "5C", search: "?module=3c" });
  assert.equal(overridden.els["module-addr"].value, "3C");

  const junk = await loadAppWithFakeDom({ storedAddr: "5C", search: "?module=nope" });
  assert.equal(junk.els["module-addr"].value, "5C", "a junk ?module= must not clobber the field");
});

await runAsyncTests();

console.log(`\n${passed} test(s) passed`);
if (process.exitCode) {
  console.error("SOME TESTS FAILED");
} else {
  console.log("ALL TESTS PASSED");
}
