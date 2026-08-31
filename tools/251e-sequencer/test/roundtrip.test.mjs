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
//   3. module addressing: parse/format, and that GET_DUMP/PUT_DUMP actually
//      carry [mod_addr] on the wire (they used to send an empty payload).
//   4. the WHOLE pipeline against MockDevice -- an in-process model of
//      software/src/Bus200eBridge.cpp: read -> decode -> edit -> encode ->
//      write -> read back, plus the refusal paths (no address, bad checksum,
//      DUMP_DATA with no session, busy).
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
  // payload = [seq, total, hibits, data0, data1, ...] -- index 2 is the
  // packed group's hibits byte (whose low 7 bits, once unpacked, only ever
  // touch the high/8th bit of each raw byte, which xor7 deliberately
  // discards via its own "& 0x7F" fold -- see Bus200eSysExXor7 -- so
  // corrupting it wouldn't actually move the checksum). Index 3 is the
  // first packed *data* byte -- flipping its low 7 bits corrupts the
  // reconstructed raw byte's checksum-relevant bits.
  frames[0].payload[3] ^= 0x7f;
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

// ---- transport: header/command constants match firmware exactly -----------
// (software/src/Bus200eSysEx.h -- the "THIS IS THE CONTRACT" header)

test("SYSEX header constants match Bus200eSysEx.h", () => {
  assert.equal(SYSEX.MFR_ID, 0x7d);
  assert.equal(SYSEX.FAMILY_ID, 0x62);
  assert.equal(SYSEX.APP_ID, 0x35);
  assert.equal(SYSEX.PROTO_VERSION, 0x01);
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

test("packChunk(44 raw bytes) hits BUS200E_SYSEX_MAX_PACKED (51 bytes) exactly", () => {
  const raw44 = new Array(DUMP_CHUNK_BYTES).fill(0);
  const packed = packChunk(raw44);
  assert.equal(packed.length, MAX_PACKED_CHUNK_BYTES);
  assert.equal(packed.length, 51);
  // + seq/total(2) + header/footer(7) == the 60-byte ceiling (test_dump_data_full_chunk_hits_60_byte_ceiling)
  assert.equal(2 + 7 + packed.length, MAX_FRAME_BYTES);
  assert.equal(MAX_FRAME_BYTES, 60);
});

test("unpackChunk returns null (malformed) for a lone hibits byte with no data following", () => {
  // test_unpack_malformed: a single 0x00 byte unpacks to -1 in C++; here
  // that's represented as null rather than an exception/sentinel, per this
  // module's existing "return null, don't throw, for a recognizable-but-bad
  // input" convention (see parseFrame).
  assert.equal(unpackChunk([0x00]), null);
});

test("packDump/DumpAssembler round-trip a dump with full 8-bit bytes (>= 0x80)", () => {
  // Unlike sequence-codec.js's current placeholder (which only ever emits
  // 7-bit-safe bytes), a real captured 200e dump is not guaranteed to be --
  // this is the scenario Bus200eSysExPack/Unpack exist for. Spread across
  // 0-255 and several DUMP_DATA chunks, mirroring test_full_dump_stream_shape.
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
  // seq=0, total=1, packed chunk = a lone hibits byte with nothing after it
  const result = assembler.feed({ cmd: CMD.DUMP_DATA, payload: [0, 1, 0x00] });
  assert.equal(result, true);
  assert.ok(assembler.done);
  assert.match(assembler.error, /malformed packed chunk/);
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
  assert.equal(MAX_DUMP_PACKETS, 127); // DUMP_END's n_packets is one 7-bit byte
  assert.equal(MAX_DUMP_BYTES, 127 * DUMP_CHUNK_BYTES);
  assert.equal(MAX_DUMP_BYTES, 5588);
});

// ---- end-to-end against the MockDevice -------------------------------------
// MockDevice models software/src/Bus200eBridge.cpp at the frame level, so
// these exercise the REAL path: encodeDump -> packDump -> packChunk ->
// buildFrame -> (wire) -> parseFrame -> DumpAssembler -> stored bytes.

async function connectedMock(opts = {}) {
  const t = new MockTransport({ latencyMs: 0, ...opts });
  await t.connect();
  return t;
}

atest("GET_DUMP carries the module address on the wire", async () => {
  const sent = [];
  const device = new MockDevice({ initialBytes: encodeDump(createSyntheticState()) });
  const realHandle = device.handle.bind(device);
  device.handle = (frame) => {
    sent.push(frame);
    return realHandle(frame);
  };
  const t = await connectedMock({ device });
  await t.readDump(0x2e);
  const get = sent.find((f) => f.cmd === CMD.GET_DUMP);
  assert.ok(get, "no GET_DUMP reached the device");
  assert.deepEqual(get.payload, [0x2e], "GET_DUMP payload must be [mod_addr]");
  assert.equal(device.lastModAddr, 0x2e);
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
  await t.writeDump(encodeDump(createSyntheticState()), 0x3c);
  const put = sent.find((f) => f.cmd === CMD.PUT_DUMP);
  assert.ok(put, "no PUT_DUMP reached the device");
  assert.deepEqual(put.payload, [0x3c], "PUT_DUMP payload must be [mod_addr]");
  assert.equal(device.lastModAddr, 0x3c);
  assert.equal(device.restores, 1, "a clean DUMP_END must trigger exactly one restore");
});

atest("a hex-string module address works the same as a number (the UI passes text)", async () => {
  const t = await connectedMock({ initialBytes: encodeDump(createSyntheticState()) });
  const bytes = await t.readDump("2e");
  assert.equal(t.device.lastModAddr, 0x2e);
  assert.equal(bytes.length, DUMP_LENGTH);
});

atest("an out-of-range module address is refused before anything hits the wire", async () => {
  const t = await connectedMock();
  const before = t.device.captures;
  await assert.rejects(() => t.readDump(0x80), /0x00-0x7F/);
  await assert.rejects(() => t.writeDump(encodeDump(createSyntheticState()), -1), /0x00-0x7F/);
  assert.equal(t.device.captures, before, "nothing should have been sent");
});

atest("full pipeline: read -> decode -> edit -> encode -> write -> read back", async () => {
  // Seed the device with something other than what we'll write, so a
  // pass-through bug can't fake this.
  const seeded = encodeDump(createEmptyState());
  const t = await connectedMock({ initialBytes: seeded });

  // 1. read
  const readBytes = await t.readDump(0x2e);
  assert.deepEqual(Array.from(readBytes), Array.from(seeded));

  // 2. decode + edit exactly the way the UI does
  const state = decodeDump(readBytes);
  state.sequences[0].length = 5;
  state.sequences[0].steps[0].voltage = 7.5;
  state.sequences[0].steps[0].gateOn = true;
  state.sequences[0].steps[0].gateLength = 88;
  state.sequences[3].steps[15].voltage = 10;

  // 3. write
  const written = encodeDump(state);
  assert.notDeepEqual(Array.from(written), Array.from(seeded));
  await t.writeDump(written, 0x2e);

  // 4. read back -- byte-for-byte, and the edits survive a decode
  const back = await t.readDump(0x2e);
  assert.deepEqual(Array.from(back), Array.from(written));
  const backState = decodeDump(back);
  assert.equal(backState.sequences[0].length, 5);
  assert.equal(backState.sequences[0].steps[0].gateOn, true);
  assert.equal(backState.sequences[0].steps[0].gateLength, 88);
  assert.ok(Math.abs(backState.sequences[0].steps[0].voltage - 7.5) <= 10 / 127 + 1e-9);
  assert.ok(Math.abs(backState.sequences[3].steps[15].voltage - 10) <= 10 / 127 + 1e-9);
});

atest("the round trip survives full 8-bit dump bytes (a REAL 200e capture)", async () => {
  // sequence-codec.js's placeholder layout is 7-bit-safe, but a genuine FRAM
  // capture is not -- this is what Bus200eSysExPack/packChunk exist for, and
  // it must survive the whole transport, not just the packer's unit test.
  const raw = Uint8Array.from({ length: 517 }, (_, i) => (i * 37 + 11) % 256);
  const t = await connectedMock();
  await t.writeDump(raw, 0x2e);
  const back = await t.readDump(0x2e);
  assert.deepEqual(Array.from(back), Array.from(raw));
});

atest("device NAKs a DUMP_END whose checksum doesn't match, and never restores", async () => {
  const device = new MockDevice();
  const t = await connectedMock({ device });
  // drive the frames by hand so we can corrupt one mid-stream
  const bytes = encodeDump(createSyntheticState());
  const frames = packDump(bytes);
  frames[0].payload[3] ^= 0x7f; // corrupt a packed data byte (see the tamper test above)
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
  const reply = device.handle({ cmd: CMD.DUMP_DATA, payload: [0, 1, 0x00, 0x01] });
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
  const t = await connectedMock({ initialBytes: encodeDump(createSyntheticState()) });
  const info = await t.info();
  assert.equal(info.schema, 1);
  assert.equal(info.nSequences, 0);
  assert.equal(info.maxSteps, 0);
  assert.equal(info.chunkBytes, DUMP_CHUNK_BYTES);
  assert.equal(info.maxPackets, MAX_DUMP_PACKETS);
  assert.equal(info.lastDumpBytes, DUMP_LENGTH); // 14-bit lo/hi split reassembles
});

atest("a transport that isn't connected refuses to send anything", async () => {
  const t = new MockTransport();
  await assert.rejects(() => t.readDump(0x2e), /not connected/);
  await assert.rejects(() => t.writeDump(encodeDump(createSyntheticState()), 0x2e), /not connected/);
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

  els["module-addr"].value = "2e";
  els["module-addr"].dispatch("input");
  assert.equal(els["module-addr"].value, "2E", "the field uppercases as you type");
  assert.equal(els["read-btn"].disabled, false);
  assert.equal(els["write-btn"].disabled, false);
  assert.equal(els["module-addr"].classes.has("invalid"), false);
});

atest("app.js: Read from device sends the entered address and decodes the reply", async () => {
  const { els, store } = await loadAppWithFakeDom();
  els["connect-btn"].dispatch("click");
  await settle();
  els["module-addr"].value = "2E";
  els["module-addr"].dispatch("input");

  els["read-btn"].dispatch("click");
  await settle();
  await settle();

  assert.equal(els.status.textContent, "read complete");
  assert.match(logText(els), new RegExp(`read ${DUMP_LENGTH} bytes from module 2E`));
  // the address the user typed is what got persisted for next time
  assert.equal(store.get("251e-sequencer.module-addr"), "2E");
});

atest("app.js: Write to device reports the module it wrote to", async () => {
  const { els } = await loadAppWithFakeDom();
  els["connect-btn"].dispatch("click");
  await settle();
  els["module-addr"].value = "3C";
  els["module-addr"].dispatch("input");

  els["write-btn"].dispatch("click");
  await settle();
  await settle();

  assert.equal(els.status.textContent, "write complete");
  assert.match(logText(els), new RegExp(`wrote ${DUMP_LENGTH} bytes to module 3C`));
  assert.equal(els["dirty-badge"].hidden, true, "a successful write clears the dirty badge");
});

atest("app.js: a remembered address is restored, and ?module= overrides it", async () => {
  const remembered = await loadAppWithFakeDom({ storedAddr: "2E" });
  assert.equal(remembered.els["module-addr"].value, "2E");
  assert.equal(remembered.els["read-btn"].disabled, true, "still needs a connection");

  const overridden = await loadAppWithFakeDom({ storedAddr: "2E", search: "?module=3c" });
  assert.equal(overridden.els["module-addr"].value, "3C");

  const junk = await loadAppWithFakeDom({ storedAddr: "2E", search: "?module=nope" });
  assert.equal(junk.els["module-addr"].value, "2E", "a junk ?module= must not clobber the field");
});

await runAsyncTests();

console.log(`\n${passed} test(s) passed`);
if (process.exitCode) {
  console.error("SOME TESTS FAILED");
} else {
  console.log("ALL TESTS PASSED");
}
