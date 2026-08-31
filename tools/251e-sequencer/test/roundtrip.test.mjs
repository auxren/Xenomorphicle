// Standalone Node test (no framework, no deps): run with
//   node tools/251e-sequencer/test/roundtrip.test.mjs
// Exercises:
//   1. sequence-codec: encode(decode(bytes)) === bytes for every byte value
//      (the critical "won't corrupt a stored dump" invariant), plus
//      decode(encode(state)) round-trips the synthetic + empty states.
//   2. sysex-transport: buildFrame/parseFrame round-trip, packDump/
//      DumpAssembler reassembly (incl. out-of-order delivery + checksum
//      tamper detection), and buildFrame's 7-bit validation actually
//      rejects an 8-bit byte (guards against the exact bug this test
//      suite caught during development: codec bytes that weren't 7-bit-safe).
import assert from "node:assert/strict";
import {
  DUMP_LENGTH,
  NUM_SEQUENCES,
  MAX_STEPS,
  decodeDump,
  encodeDump,
  createEmptyState,
  createSyntheticState,
  cloneState,
} from "../sequence-codec.js";
import { buildFrame, parseFrame, packDump, DumpAssembler, SYSEX, CMD } from "../sysex-transport.js";

let passed = 0;
function test(name, fn) {
  try {
    fn();
    passed++;
    console.log(`ok - ${name}`);
  } catch (err) {
    console.error(`FAIL - ${name}`);
    console.error(err);
    process.exitCode = 1;
  }
}

// ---- codec: structural sanity ---------------------------------------------

test("createEmptyState has the right shape", () => {
  const s = createEmptyState();
  assert.equal(s.sequences.length, NUM_SEQUENCES);
  for (const seq of s.sequences) {
    assert.equal(seq.steps.length, MAX_STEPS);
    assert.equal(seq.length, MAX_STEPS);
  }
});

test("encodeDump produces exactly DUMP_LENGTH 7-bit-safe bytes", () => {
  const bytes = encodeDump(createSyntheticState());
  assert.equal(bytes.length, DUMP_LENGTH);
  for (const b of bytes) {
    assert.ok(b >= 0 && b <= 127, `byte ${b} is not 7-bit-safe`);
  }
});

test("decodeDump rejects the wrong length", () => {
  assert.throws(() => decodeDump(new Uint8Array(DUMP_LENGTH - 1)));
  assert.throws(() => decodeDump(new Uint8Array(DUMP_LENGTH + 1)));
});

// ---- codec: the load-bearing round trip ------------------------------------

test("encode(decode(bytes)) is lossless for every possible raw byte value", () => {
  // Fill a full dump with a repeating 0..127 ramp so every field position
  // sees every possible byte value across the buffer.
  const bytes = new Uint8Array(DUMP_LENGTH);
  for (let i = 0; i < DUMP_LENGTH; i++) bytes[i] = i % 128;
  // length bytes must stay in 1..MAX_STEPS or decode legitimately clamps
  // them (that's intentional defensive clamping, not the property under
  // test) -- fix those positions up before comparing.
  const BYTES_PER_SEQUENCE = DUMP_LENGTH / NUM_SEQUENCES;
  for (let s = 0; s < NUM_SEQUENCES; s++) {
    bytes[s * BYTES_PER_SEQUENCE] = ((s % MAX_STEPS) + 1);
  }
  // gateOn bytes (offset 1 of each 3-byte step) must be exactly 0 or 1 to
  // round-trip byte-for-byte (encode only ever emits 0/1); non-boolean
  // "on" bytes are a decode-side leniency (`!== 0`), not part of the
  // wire contract, so normalize those positions too. Same story for
  // gateLength (offset 2): decode defensively clamps to 0..100, so any
  // byte above 100 is intentionally not round-trippable -- clamp it here
  // too before comparing.
  for (let s = 0; s < NUM_SEQUENCES; s++) {
    const seqStart = s * BYTES_PER_SEQUENCE + 1;
    for (let i = 0; i < MAX_STEPS; i++) {
      const gateOnIdx = seqStart + i * 3 + 1;
      const gateLengthIdx = seqStart + i * 3 + 2;
      bytes[gateOnIdx] = bytes[gateOnIdx] % 2;
      bytes[gateLengthIdx] = Math.min(bytes[gateLengthIdx], 100);
    }
  }

  const state = decodeDump(bytes);
  const roundTripped = encodeDump(state);
  assert.deepEqual(Array.from(roundTripped), Array.from(bytes), "byte-for-byte round trip failed");
});

test("decode(encode(state)) preserves the synthetic state (mod voltage quantization)", () => {
  const original = createSyntheticState();
  const decoded = decodeDump(encodeDump(original));
  assert.equal(decoded.sequences.length, original.sequences.length);
  for (let s = 0; s < NUM_SEQUENCES; s++) {
    assert.equal(decoded.sequences[s].length, original.sequences[s].length);
    for (let i = 0; i < MAX_STEPS; i++) {
      const o = original.sequences[s].steps[i];
      const d = decoded.sequences[s].steps[i];
      // ~0.0787V/LSB quantization (10V / 127 steps) is expected lossiness,
      // documented in sequence-codec.js -- assert it's within one LSB.
      assert.ok(Math.abs(o.voltage - d.voltage) <= 10 / 127 + 1e-9, `seq ${s} step ${i} voltage drifted too far`);
      assert.equal(d.gateOn, o.gateOn, `seq ${s} step ${i} gateOn mismatch`);
      assert.equal(d.gateLength, o.gateLength, `seq ${s} step ${i} gateLength mismatch`);
    }
  }
});

test("encode(decode(encode(state))) is a fixed point (second pass is exact)", () => {
  const state = createSyntheticState();
  const bytes1 = encodeDump(state);
  const bytes2 = encodeDump(decodeDump(bytes1));
  assert.deepEqual(Array.from(bytes2), Array.from(bytes1));
});

test("cloneState produces an independent deep copy", () => {
  const state = createSyntheticState();
  const clone = cloneState(state);
  clone.sequences[0].steps[0].voltage = 9.99;
  assert.notEqual(clone.sequences[0].steps[0].voltage, state.sequences[0].steps[0].voltage);
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
  const bytes = encodeDump(createSyntheticState());
  const assembler = new DumpAssembler();
  for (const frame of packDump(bytes)) assembler.feed(frame);
  assert.ok(assembler.done);
  assert.equal(assembler.error, null);
  assert.deepEqual(Array.from(assembler.bytes), Array.from(bytes));
});

test("DumpAssembler tolerates out-of-order DUMP_DATA delivery", () => {
  const bytes = encodeDump(createSyntheticState());
  const frames = packDump(bytes);
  const shuffled = [...frames.slice(0, -1)].reverse().concat(frames[frames.length - 1]);
  const assembler = new DumpAssembler();
  for (const frame of shuffled) assembler.feed(frame);
  assert.deepEqual(Array.from(assembler.bytes), Array.from(bytes));
});

test("DumpAssembler flags a tampered checksum", () => {
  const bytes = encodeDump(createSyntheticState());
  const frames = packDump(bytes);
  frames[0].payload[2] ^= 0x7f; // corrupt one data byte after checksum was computed
  const assembler = new DumpAssembler();
  for (const frame of frames) assembler.feed(frame);
  assert.ok(assembler.done);
  assert.match(assembler.error, /checksum/);
});

test("DumpAssembler flags a missing packet", () => {
  const bytes = encodeDump(createSyntheticState());
  const frames = packDump(bytes);
  assert.ok(frames.length > 2, "test assumes a multi-packet dump");
  const withGap = frames.filter((_, i) => i !== 1);
  const assembler = new DumpAssembler();
  for (const frame of withGap) assembler.feed(frame);
  assert.ok(assembler.done);
  assert.ok(assembler.error);
});

console.log(`\n${passed} test(s) passed`);
if (process.exitCode) {
  console.error("SOME TESTS FAILED");
} else {
  console.log("ALL TESTS PASSED");
}
