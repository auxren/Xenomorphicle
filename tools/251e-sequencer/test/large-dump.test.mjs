// Standalone Node test (no framework, no deps): run with
//   node tools/251e-sequencer/test/large-dump.test.mjs
//
// ---------------------------------------------------------------------------
// THE CROSS-CHECK AGAINST FIRMWARE, NOT A SECOND SELF-CONSISTENCY SUITE.
//
// Every literal in the "SHARED WORKED EXAMPLES" section below also appears,
// as the same literal, in software/test/test_bus200e_sysex.cpp (and the
// packet/ACK shapes in software/test/test_bus200e_bridge.cpp). Two
// independent implementations of one wire format agreeing with each other on
// concrete bytes is the only evidence that matters here; "each side round
// trips its own output" is not. If you change a byte on one side, the other
// side's suite must fail until you change it there too.
//
// What is under test is protocol v2's whole reason for existing: a real 251e
// preset bank -- 30 records x 2104 = 63120 bytes, confirmed four independent
// ways including a live MasterBackup(0x5C) (Buchla_FW/docs/
// 251e-SEQUENCE-FORMAT.md) -- crossing the wire in ONE session. Protocol v1's
// single-7-bit-byte packet counters capped a transfer at 127 packets * 44
// bytes = 5588, so the firmware NAK 6'd exactly the transfer this bridge was
// built for. v2 widens every counter to 14 bits (two 7-bit septets, low
// first), which costs 2 bytes of DUMP_DATA header and therefore 2 bytes of
// chunk (44 -> 42), and removes the ceiling entirely.
//
// This file deliberately does NOT import sequence-codec.js: what the dump
// bytes MEAN is that module's business and is being reverse-engineered
// separately. Everything here is transport -- how bytes move, not what they
// say -- and its synthetic "bank" is a byte ramp chosen to exercise the
// packing, not to resemble a real preset.
// ---------------------------------------------------------------------------
import assert from "node:assert/strict";
import {
  SYSEX,
  CMD,
  buildFrame,
  parseFrame,
  packChunk,
  unpackChunk,
  packDump,
  DumpAssembler,
  septetPair,
  fromSeptetPair,
  MAX_COUNT_14BIT,
  DUMP_CHUNK_BYTES,
  MAX_PACKED_CHUNK_BYTES,
  MAX_FRAME_BYTES,
  MAX_DUMP_PACKETS,
  MAX_DUMP_BYTES,
  MockDevice,
  MockTransport,
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

// ---- SHARED WORKED EXAMPLES ------------------------------------------------
// Mirrors of test_bus200e_sysex.cpp's WORKED_* defines and kWorkedFrameSeq1234.

const WORKED_BANK_BYTES = 63120; // 30 x 2104, a real 251e preset bank
const WORKED_BANK_PACKETS = 1503; // ceil(63120 / 42)
const WORKED_BANK_LAST_CHUNK = 36; // 63120 - 1502*42
const WORKED_BANK_XOR7 = 0x10; // xor of all 63120 raw bytes, folded to 7 bits
const WORKED_DUMP_DATA_WIRE_BYTES = 88671; // every DUMP_DATA frame, F0..F7 inclusive

/** The synthetic bank both suites use. Not 7-bit-safe -- that is the point. */
const workedBankByte = (i) => (i * 37 + 11) & 0xff;

function workedBank() {
  const b = new Uint8Array(WORKED_BANK_BYTES);
  for (let i = 0; i < WORKED_BANK_BYTES; i++) b[i] = workedBankByte(i);
  return b;
}

// One complete DUMP_DATA frame from the middle of that bank -- seq 1234 of
// 1503, raw offset 1234*42 = 51828 -- byte for byte, F0/F7 excluded (they
// belong to the MIDI layer, not to Bus200eSysExBuildMessage's output).
// Bytes [5..8]: 0x52 0x09 = seq 1234 low-septet-first, 0x5F 0x0B = total
// 1503. Neither number could have ridden a v1 single-byte field at all.
const WORKED_FRAME_SEQ_1234 = [
  0x7d, 0x62, 0x35, 0x02, 0x44, 0x52, 0x09, 0x5f, 0x0b, 0x63, 0x4f, 0x74,
  0x19, 0x3e, 0x63, 0x08, 0x2d, 0x63, 0x52, 0x77, 0x1c, 0x41, 0x66, 0x0b,
  0x30, 0x63, 0x55, 0x7a, 0x1f, 0x44, 0x69, 0x0e, 0x33, 0x63, 0x58, 0x7d,
  0x22, 0x47, 0x6c, 0x11, 0x36, 0x61, 0x5b, 0x00, 0x25, 0x4a, 0x6f, 0x14,
  0x39, 0x61, 0x5e, 0x03, 0x28, 0x4d, 0x72, 0x17, 0x3c,
];

// DUMP_END for the whole bank, same convention (no F0/F7):
// mfr, family, app, ver, cmd, n_lo=95, n_hi=11, xor7=0x10.
const WORKED_DUMP_END_FRAME = [0x7d, 0x62, 0x35, 0x02, 0x45, 95, 11, 0x10];

// ---- frame geometry: identical numbers to Bus200eSysEx.h -------------------

test("v2 frame geometry matches Bus200eSysEx.h exactly", () => {
  assert.equal(SYSEX.PROTO_VERSION, 0x02, "BUS200E_SYSEX_PROTO_VER");
  assert.equal(DUMP_CHUNK_BYTES, 42, "BUS200E_SYSEX_CHUNK_BYTES");
  assert.equal(MAX_PACKED_CHUNK_BYTES, 48, "BUS200E_SYSEX_MAX_PACKED");
  // 5-byte header + 4-byte seq/total + 48-byte packed chunk = 57
  // (BUS200E_SYSEX_MAX_MESSAGE), + F0 + F7 = 59.
  assert.equal(MAX_FRAME_BYTES, 59);
  assert.ok(MAX_FRAME_BYTES <= 60, "must stay inside hOC's 60-byte frame rule");
});

test("42 is the largest chunk that fits: 43 would overflow the 60-byte ceiling", () => {
  // The reason v2's chunk is 42 and not "44 minus the 2 header bytes it lost":
  // packChunk grows in 8-byte groups, so nothing lands exactly on the ceiling.
  assert.equal(packChunk(new Array(42).fill(0)).length, 48);
  assert.equal(packChunk(new Array(43).fill(0)).length, 50);
  assert.equal(7 + 4 + 48, 59); // fits
  assert.equal(7 + 4 + 50, 61); // does not
});

test("MAX_DUMP_PACKETS/MAX_DUMP_BYTES now come from the card image, not the wire", () => {
  // The 14-bit counters would carry 16383 packets; the device's 64 KB card
  // image is what actually bounds a transfer (BUS200E_BRIDGE_MAX_PACKETS).
  assert.equal(MAX_DUMP_BYTES, 65536);
  assert.equal(MAX_DUMP_PACKETS, 1561); // ceil(65536 / 42)
  assert.ok(MAX_DUMP_PACKETS < MAX_COUNT_14BIT, "the wire must not be the binding limit");
  assert.ok(WORKED_BANK_PACKETS < MAX_DUMP_PACKETS, "a real 251e bank must fit with room over");
});

// ---- septet pairs (BUS200E_SYSEX_LO7/HI7/FROM14) ---------------------------

test("septetPair/fromSeptetPair match the C++ macros' worked values", () => {
  // test_septet_pair_helpers in test_bus200e_sysex.cpp asserts these same two.
  assert.deepEqual(septetPair(1234), [82, 9]);
  assert.deepEqual(septetPair(1503), [95, 11]);
  assert.equal(MAX_COUNT_14BIT, 16383);
  for (const v of [0, 1, 127, 128, 1234, 1435, 1503, 1561, 16383]) {
    const [lo, hi] = septetPair(v);
    assert.ok(lo >= 0 && lo <= 0x7f && hi >= 0 && hi <= 0x7f, `${v} produced a non-7-bit septet`);
    assert.equal(fromSeptetPair(lo, hi), v);
  }
});

test("septetPair refuses a value past 14 bits rather than truncating it", () => {
  assert.throws(() => septetPair(16384), RangeError);
  assert.throws(() => septetPair(-1), RangeError);
});

// ---- the worked frame, byte for byte ---------------------------------------

test("packDump emits test_bus200e_sysex.cpp's worked seq-1234 frame byte for byte", () => {
  const frames = packDump(workedBank());
  assert.equal(frames.length, WORKED_BANK_PACKETS + 1, "1503 DUMP_DATA + 1 DUMP_END");

  const wire = Array.from(buildFrame(CMD.DUMP_DATA, frames[1234].payload));
  // strip the F0/F7 the C++ side never sees (Bus200eSysExBuildMessage emits
  // only the bytes BETWEEN them)
  assert.equal(wire[0], SYSEX.START);
  assert.equal(wire[wire.length - 1], SYSEX.END);
  assert.deepEqual(wire.slice(1, -1), WORKED_FRAME_SEQ_1234);
  assert.equal(wire.length, 59, "a full-chunk frame is 59 bytes on the wire");

  // and the header septets are exactly what the firmware built
  assert.deepEqual(frames[1234].payload.slice(0, 4), [0x52, 0x09, 0x5f, 0x0b]);
  assert.equal(fromSeptetPair(0x52, 0x09), 1234);
  assert.equal(fromSeptetPair(0x5f, 0x0b), WORKED_BANK_PACKETS);
});

test("the worked frame parses back to the bank bytes at offset 51828", () => {
  const parsed = parseFrame(Uint8Array.from([SYSEX.START, ...WORKED_FRAME_SEQ_1234, SYSEX.END]));
  assert.ok(parsed, "the C++-authored frame must parse as one of ours");
  assert.equal(parsed.cmd, CMD.DUMP_DATA);
  const [seqLo, seqHi, totalLo, totalHi, ...packed] = parsed.payload;
  assert.equal(fromSeptetPair(seqLo, seqHi), 1234);
  assert.equal(fromSeptetPair(totalLo, totalHi), WORKED_BANK_PACKETS);
  const raw = unpackChunk(packed);
  assert.equal(raw.length, DUMP_CHUNK_BYTES);
  const off = 1234 * DUMP_CHUNK_BYTES;
  assert.equal(off, 51828);
  assert.deepEqual(
    raw,
    Array.from({ length: DUMP_CHUNK_BYTES }, (_, i) => workedBankByte(off + i))
  );
});

test("DUMP_END for the whole bank is [95, 11, 0x10] -- the same bytes firmware sends", () => {
  const frames = packDump(workedBank());
  const end = frames[frames.length - 1];
  assert.equal(end.cmd, CMD.DUMP_END);
  assert.deepEqual(end.payload, [95, 11, WORKED_BANK_XOR7]);
  const wire = Array.from(buildFrame(CMD.DUMP_END, end.payload));
  assert.deepEqual(wire.slice(1, -1), WORKED_DUMP_END_FRAME);
});

// ---- the whole 63 KB transfer ----------------------------------------------

test("a whole 63120-byte 251e bank chunks, packs and reassembles losslessly", () => {
  const bank = workedBank();
  const frames = packDump(bank);
  assert.equal(frames.length - 1, WORKED_BANK_PACKETS);

  let wireBytes = 0;
  for (let seq = 0; seq < WORKED_BANK_PACKETS; seq++) {
    const f = frames[seq];
    assert.equal(f.cmd, CMD.DUMP_DATA);
    assert.deepEqual(f.payload.slice(0, 2), septetPair(seq), `seq ${seq} header`);
    assert.deepEqual(f.payload.slice(2, 4), septetPair(WORKED_BANK_PACKETS));
    for (const b of f.payload) assert.ok(b >= 0 && b <= 0x7f, "every payload byte must be 7-bit");
    const frame = buildFrame(f.cmd, f.payload);
    assert.ok(frame.length <= 60, `frame ${seq} is ${frame.length} bytes, over hOC's ceiling`);
    wireBytes += frame.length;
  }
  // the last packet is a SHORT one -- 63120 is not a multiple of 42
  assert.equal(
    unpackChunk(frames[WORKED_BANK_PACKETS - 1].payload.slice(4)).length,
    WORKED_BANK_LAST_CHUNK
  );
  // ...and the C++ suite counts exactly the same wire bytes
  assert.equal(wireBytes, WORKED_DUMP_DATA_WIRE_BYTES);

  const assembler = new DumpAssembler();
  for (const f of frames) assembler.feed(f);
  assert.ok(assembler.done);
  assert.equal(assembler.error, null);
  assert.equal(assembler.bytes.length, WORKED_BANK_BYTES);
  assert.ok(Buffer.from(assembler.bytes).equals(Buffer.from(bank)), "bank must survive byte for byte");
});

test("packDump refuses a dump larger than the device's card image", () => {
  // Better here than 1561 frames later with a NAK 6.
  const tooBig = new Uint8Array(MAX_DUMP_BYTES + 1);
  assert.throws(() => packDump(tooBig), RangeError);
  // ...and exactly at the limit it is accepted
  const atLimit = packDump(new Uint8Array(MAX_DUMP_BYTES));
  assert.equal(atLimit.length - 1, MAX_DUMP_PACKETS);
});

test("DumpAssembler survives out-of-order delivery of a 1503-packet bank", () => {
  const bank = workedBank();
  const frames = packDump(bank);
  const shuffled = [...frames.slice(0, -1)].reverse().concat(frames[frames.length - 1]);
  const assembler = new DumpAssembler();
  for (const f of shuffled) assembler.feed(f);
  assert.equal(assembler.error, null);
  assert.ok(Buffer.from(assembler.bytes).equals(Buffer.from(bank)));
});

test("a single corrupted byte anywhere in a 63 KB bank is caught by xor7", () => {
  const frames = packDump(workedBank());
  // payload = [seqLo, seqHi, totalLo, totalHi, hibits, data0, ...] -- index 5
  // is the first packed DATA byte. Index 4 is the group's hibits byte, whose
  // bits only set the 8th bit of each raw byte, which xor7 folds away (see
  // Bus200eSysExXor7's & 0x7F) -- corrupting THAT would not move the checksum.
  frames[900].payload[5] ^= 0x7f;
  const assembler = new DumpAssembler();
  for (const f of frames) assembler.feed(f);
  assert.ok(assembler.done);
  assert.match(assembler.error, /checksum/);
});

test("a missing packet in a 63 KB bank is caught by the packet count", () => {
  const frames = packDump(workedBank()).filter((_, i) => i !== 742);
  const assembler = new DumpAssembler();
  for (const f of frames) assembler.feed(f);
  assert.ok(assembler.done);
  assert.ok(assembler.error);
});

// ---- against MockDevice (the frame-level model of Bus200eBridge.cpp) --------

test("MockDevice mirrors the firmware's 3-byte septet-pair ACK for DUMP_DATA", () => {
  // Bus200eBridge.cpp's send_ack14: a seq no longer fits one context byte, so
  // ACK{DUMP_DATA, ...} and ACK{DUMP_END, ...} carry a lo/hi pair.
  const device = new MockDevice();
  device.handle({ cmd: CMD.PUT_DUMP, payload: [0x5c] });
  const [seqLo, seqHi] = septetPair(1234);
  const reply = device.handle({
    cmd: CMD.DUMP_DATA,
    payload: [seqLo, seqHi, ...septetPair(WORKED_BANK_PACKETS), 0x00, 0x01],
  });
  assert.equal(reply[0].cmd, CMD.ACK);
  assert.deepEqual(reply[0].payload, [CMD.DUMP_DATA, seqLo, seqHi]);
});

test("MockDevice refuses a 14-bit seq past the card image's packet count", () => {
  // Same guard as Bus200eBridge.cpp: seq indexes the firmware's seen-bitmap.
  const device = new MockDevice();
  device.handle({ cmd: CMD.PUT_DUMP, payload: [0x5c] });
  const reply = device.handle({
    cmd: CMD.DUMP_DATA,
    payload: [...septetPair(MAX_DUMP_PACKETS), ...septetPair(MAX_COUNT_14BIT), 0x00, 0x01],
  });
  assert.equal(reply[0].cmd, CMD.NAK);
  assert.equal(reply[0].payload[1], 6);
  assert.equal(device.restores, 0);
});

atest("the full 63120-byte bank survives a write -> read round trip end to end", async () => {
  const bank = workedBank();
  const t = new MockTransport({ latencyMs: 0 });
  await t.connect();
  await t.writeDump(bank, 0x5c); // the 251e's real bus address
  assert.equal(t.device.restores, 1);
  assert.equal(t.device.lastModAddr, 0x5c);
  const back = await t.readDump(0x5c);
  assert.equal(back.length, WORKED_BANK_BYTES);
  assert.ok(Buffer.from(back).equals(Buffer.from(bank)), "63 KB must survive both directions");
});

atest("INFO_R reports a 63120-byte dump length and the real packet ceiling", async () => {
  // v1 split the length over TWO septets, which stops at 16383 -- it could
  // not have reported this number at all. v2 uses three.
  const t = new MockTransport({ device: new MockDevice({ initialBytes: workedBank() }) });
  await t.connect();
  const info = await t.info();
  assert.equal(info.schema, 2);
  assert.equal(info.lastDumpBytes, WORKED_BANK_BYTES);
  assert.ok(WORKED_BANK_BYTES > MAX_COUNT_14BIT, "the point: two septets would have wrapped");
  assert.equal(info.chunkBytes, DUMP_CHUNK_BYTES);
  assert.equal(info.maxPackets, MAX_DUMP_PACKETS);
  assert.equal(info.cardServing, true);
});

atest("the per-packet log is throttled, not one line per 1503 frames", async () => {
  // app.js appends a DOM node per log line; 3000 of them for one bank would
  // be a UI problem this transport layer would have created.
  const t = new MockTransport({ device: new MockDevice({ initialBytes: workedBank() }) });
  const lines = [];
  t.onLog((l) => lines.push(l));
  await t.connect();
  await t.readDump(0x5c);
  assert.ok(
    lines.length < 40,
    `expected a throttled log, got ${lines.length} lines for a 1503-packet dump`
  );
  assert.ok(
    lines.some((l) => /dump complete: 1503 packets, 63120 bytes/.test(l)),
    "the summary line must still report the real totals"
  );
});

await runAsyncTests();

console.log(`\n${passed} test(s) passed`);
if (process.exitCode) {
  console.error("SOME TESTS FAILED");
} else {
  console.log("ALL TESTS PASSED");
}
