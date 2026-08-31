// sysex-transport.js
// ---------------------------------------------------------------------------
// Transport layer for the 251e sequencer applet.
//
// *** REAL FIRMWARE FRAMING -- matches software/src/Bus200eSysEx.{h,cpp} ***
// This mirrors the device-side contract bit-for-bit: same manufacturer ID,
// family byte, app ID, protocol version, command bytes, 60-byte frame
// ceiling, and 44-byte DUMP_DATA chunk size. It is the same family as this
// repo's other real, shipped SysEx protocol -- Captain MIDI's, documented in
// docs/hoc-midi-sysex.md and implemented in tools/hoc_sysex.py -- but with
// one deliberate deviation: a raw 200e card dump is genuinely 8-bit data (it
// is read verbatim off a module's FRAM/EEPROM), so unlike hOC's "all payload
// bytes are already 7-bit" rule, DUMP_DATA chunk payloads here are 7-bit-
// packed (packChunk/unpackChunk below, porting Bus200eSysExPack/Unpack).
// Every other command's payload (INFO, GET_DUMP, PUT_DUMP, STATUS, ACK, NAK,
// the DUMP_DATA seq/total header itself) stays plain 7-bit and unpacked,
// exactly like hOC.
//
// sequence-codec.js's own 251e byte-format guess is a SEPARATE, still-open
// question (that dump layout is still being reverse-engineered) -- nothing
// here assumes anything about what the packed bytes mean, only how they're
// framed/chunked/packed for the wire.
//
// USB port identity: the firmware DOES ship a product-string override --
// software/src/usb_name.c defines usb_string_product_name as
// "Phazerville" (11 chars), and it is linked into every T4x env (the link
// step warns about the -Wlto-type-mismatch against the core's own weak
// declaration, which is how you can tell it took). So WebMIDI sees a port
// named "Phazerville", NOT Teensyduino's stock "Teensy MIDI" and NOT the
// on-device display name "NLM Xenomorpher" (OC_strings.cpp,
// Strings::NAME_NLM -- a UI string, never a USB descriptor).
// DEFAULT_PORT_NAME_CANDIDATES keeps the other two as fallbacks: a bench
// unit flashed with a stock build, or a future rename, still connects.
// ---------------------------------------------------------------------------

export const DEFAULT_PORT_NAME_CANDIDATES = ["Phazerville", "Xenomorpher", "Teensy MIDI"];

// ---- module addressing -----------------------------------------------------
// GET_DUMP/PUT_DUMP both carry [mod_addr]: WHICH 200e module on the bus to
// BACKUP from / RESTORE to. It is a SysEx payload byte, so it must be 7-bit;
// the firmware console takes the same value as two hex digits ('m' / 'q' in
// software/src/Main.cpp), so the UI does too.

export const MODULE_ADDRESS_MIN = 0x00;
export const MODULE_ADDRESS_MAX = 0x7f;

/** "2e" / "0x2E" / "2E" -> 0x2e. Throws on anything else. */
export function parseModuleAddress(text) {
  const s = String(text ?? "").trim().replace(/^0x/i, "");
  if (!/^[0-9a-f]{1,2}$/i.test(s)) {
    throw new Error(`module address must be 1-2 hex digits (got "${text}")`);
  }
  const v = parseInt(s, 16);
  if (v < MODULE_ADDRESS_MIN || v > MODULE_ADDRESS_MAX) {
    throw new Error(`module address ${formatModuleAddress(v)} is not 7-bit (00-7F)`);
  }
  return v;
}

/** 0x2e -> "2E" (matches the firmware console's own %02X logging). */
export function formatModuleAddress(v) {
  return v.toString(16).toUpperCase().padStart(2, "0");
}

/** @typedef {Uint8Array} DumpBytes raw device dump payload (see sequence-codec.js) */

// ---- Frame constants (match software/src/Bus200eSysEx.h exactly) ----------

export const SYSEX = Object.freeze({
  START: 0xf0,
  END: 0xf7,
  MFR_ID: 0x7d, // non-commercial/educational MIDI manufacturer ID (shared w/ Captain MIDI)
  FAMILY_ID: 0x62, // "Beige Maze" family byte, reused for repo consistency
  APP_ID: 0x35, // BUS200E_SYSEX_APP_ID -- "251e sequencer bridge"
  PROTO_VERSION: 0x01, // BUS200E_SYSEX_PROTO_VER
});

export const CMD = Object.freeze({
  INFO: 0x01, // H->D, no payload
  INFO_R: 0x41, // D->H: schema n_sequences max_steps dump_len_lo dump_len_hi
  GET_DUMP: 0x04, // H->D, payload = [mod_addr]
  PUT_DUMP: 0x05, // H->D, payload = [mod_addr] -- device replies ACK/NAK then expects DUMP_DATA*/DUMP_END
  STATUS: 0x10, // H->D, no payload -- poll the master FSM's in-progress BACKUP/RESTORE
  DUMP_DATA: 0x44, // both directions: [seq, total, ...7-bit-PACKED chunk] -- see packChunk/unpackChunk
  DUMP_END: 0x45, // both directions: [n_packets, xor7] -- xor7 over RAW (pre-pack) bytes
  ACK: 0x40, // D->H: echoes the command byte (+ context bytes)
  STATUS_R: 0x50, // D->H: [state, error, mod_addr, is_restore] -- reply to STATUS
  NAK: 0x7e, // D->H: [cmd, errcode]
});

// Matches Bus200eSysExNakReason (Bus200eSysEx.h). 3/4 are not real codes in
// this protocol (unlike hOC's own NAK table, which this one otherwise
// mirrors) -- 7/8/9 are new, mapped from Bus200eMasterError for foreign-
// module bus-mastering failures with no hOC analog.
export const NAK_ERRORS = Object.freeze({
  1: "protocol version mismatch",
  2: "unknown command",
  5: "busy (a master job is already running)",
  6: "dump checksum/packet-count mismatch",
  7: "no free card slot",
  8: "send timeout",
  9: "no response from module",
});

// BUS200E_SYSEX_CHUNK_BYTES / BUS200E_SYSEX_MAX_MESSAGE / hOC's 60-byte
// ceiling, reused verbatim rather than re-deriving a number for the same
// hardware. A chunk's packed form (worst case, all bytes >= 0x80) is 51
// bytes (BUS200E_SYSEX_MAX_PACKED = pack(44) = 7 hibits bytes + 44 data
// bytes); MAX_FRAME_BYTES = F0 + 5-byte header + 2-byte seq/total + 51-byte
// packed chunk + F7 = 60.
export const DUMP_CHUNK_BYTES = 44; // raw bytes per DUMP_DATA chunk
// DUMP_END's n_packets is a single 7-bit field, so a transfer tops out at 127
// packets -- BUS200E_BRIDGE_MAX_PACKETS / BUS200E_BRIDGE_MAX_DUMP_BYTES in
// software/src/Bus200eBridge.h, where a bigger capture is refused (NAK 6)
// rather than truncated. 127 * 44 = 5588 raw bytes.
export const MAX_DUMP_PACKETS = 127;
export const MAX_DUMP_BYTES = MAX_DUMP_PACKETS * DUMP_CHUNK_BYTES;
export const MAX_PACKED_CHUNK_BYTES = 51; // packChunk(44 raw bytes).length, worst case
const DUMP_DATA_HEADER_BYTES = 2; // seq, total (not counting F0/mfr/family/app/ver/cmd/F7)
const NON_PAYLOAD_BYTES = 7; // F0 mfr family app ver cmd F7
export const MAX_FRAME_BYTES = NON_PAYLOAD_BYTES + DUMP_DATA_HEADER_BYTES + MAX_PACKED_CHUNK_BYTES; // 60

// ---- 7-bit packing (DUMP_DATA chunks only -- see file header) -------------
// Faithful port of Bus200eSysExPack/Unpack (software/src/Bus200eSysEx.cpp).
// Every OTHER command's payload stays plain 7-bit and unpacked.

/**
 * 7-bit-packs a raw byte array (which may contain bytes >= 0x80) into an
 * all-7-bit byte array. Groups of up to 7 raw bytes become a "hibits" byte
 * (bit j set iff raw byte j in the group had its MSB set) followed by that
 * group's bytes with their MSB stripped -- so a full 7-byte group produces
 * 8 packed bytes.
 * @param {Uint8Array|number[]} raw
 * @returns {number[]} 7-bit-clean packed bytes (length: groups + raw.length)
 */
export function packChunk(raw) {
  const out = [];
  for (let ri = 0; ri < raw.length; ri += 7) {
    const cnt = Math.min(7, raw.length - ri);
    let hibits = 0;
    for (let j = 0; j < cnt; j++) {
      if (raw[ri + j] & 0x80) hibits |= 1 << j;
    }
    out.push(hibits);
    for (let j = 0; j < cnt; j++) {
      out.push(raw[ri + j] & 0x7f);
    }
  }
  return out;
}

/**
 * Inverse of packChunk. Returns null if the packed buffer is malformed (a
 * hibits byte with no data bytes following it -- the same condition
 * Bus200eSysExUnpack fails -1 on).
 * @param {Uint8Array|number[]} packed
 * @returns {number[]|null}
 */
export function unpackChunk(packed) {
  const out = [];
  let pi = 0;
  while (pi < packed.length) {
    const hibits = packed[pi++];
    const remaining = packed.length - pi;
    const cnt = Math.min(7, remaining);
    if (cnt === 0) return null; // a hibits byte with no data byte following
    for (let j = 0; j < cnt; j++) {
      let b = packed[pi++] & 0x7f;
      if (hibits & (1 << j)) b |= 0x80;
      out.push(b);
    }
  }
  return out;
}

// ---- Frame build/parse -----------------------------------------------------

/**
 * Build one complete SysEx frame (Uint8Array, F0..F7 inclusive) for a
 * command + payload, all 7-bit bytes.
 * @param {number} cmd
 * @param {number[]|Uint8Array} payload
 */
export function buildFrame(cmd, payload = []) {
  const body = [SYSEX.MFR_ID, SYSEX.FAMILY_ID, SYSEX.APP_ID, SYSEX.PROTO_VERSION, cmd, ...payload];
  for (const b of body) {
    if (b < 0 || b > 0x7f) throw new RangeError(`SysEx payload byte 0x${b.toString(16)} is not 7-bit`);
  }
  return Uint8Array.from([SYSEX.START, ...body, SYSEX.END]);
}

/**
 * Parse one complete SysEx frame. Returns null if it isn't ours (wrong
 * header/app id) rather than throwing, matching hOC's "ignore, don't crash
 * the loop" stance for frames belonging to other apps.
 * @param {Uint8Array|number[]} bytes
 * @returns {{cmd:number, payload:number[]}|null}
 */
export function parseFrame(bytes) {
  const b = Array.from(bytes);
  if (b.length < 7) return null;
  if (b[0] !== SYSEX.START || b[b.length - 1] !== SYSEX.END) return null;
  if (b[1] !== SYSEX.MFR_ID || b[2] !== SYSEX.FAMILY_ID || b[3] !== SYSEX.APP_ID) return null;
  if (b[4] !== SYSEX.PROTO_VERSION) return null; // caller can special-case NAK 1 if needed
  const cmd = b[5];
  const payload = b.slice(6, b.length - 1);
  return { cmd, payload };
}

// ---- Dump chunking / reassembly -------------------------------------------
// Shared by WebMidiTransport and MockTransport so both exercise identical
// framing logic -- the mock differs only in how bytes travel, never in how
// they're packed.

/**
 * Split a raw dump byte array (bytes may be full 8-bit -- a captured 200e
 * dump is not guaranteed to be 7-bit-safe) into DUMP_DATA payloads + a
 * trailing DUMP_END payload, mirroring the hOC dump stream shape. Each
 * DUMP_DATA payload is [seq, total, ...packChunk(rawSlice)] -- the seq/total
 * header stays unpacked, only the chunk itself is 7-bit-packed. The DUMP_END
 * xor7 checksum is computed over the RAW (pre-pack) bytes, matching
 * Bus200eSysExXor7.
 * @param {Uint8Array} bytes
 * @returns {{cmd:number, payload:number[]}[]} frame descriptors, in order
 */
export function packDump(bytes) {
  const chunks = [];
  const total = Math.max(1, Math.ceil(bytes.length / DUMP_CHUNK_BYTES));
  let checksum = 0;
  for (let seq = 0; seq < total; seq++) {
    const start = seq * DUMP_CHUNK_BYTES;
    const slice = Array.from(bytes.slice(start, start + DUMP_CHUNK_BYTES));
    for (const v of slice) checksum ^= v;
    const packed = packChunk(slice);
    chunks.push({ cmd: CMD.DUMP_DATA, payload: [seq, total, ...packed] });
  }
  chunks.push({ cmd: CMD.DUMP_END, payload: [total, checksum & 0x7f] });
  return chunks;
}

/** Reassembles a stream of DUMP_DATA/DUMP_END frames back into raw bytes. */
export class DumpAssembler {
  constructor() {
    this.reset();
  }

  reset() {
    this._packets = new Map(); // seq -> byte[]
    this._total = null;
    this._done = false;
    this._error = null;
    this._bytes = null;
  }

  get done() {
    return this._done;
  }
  get error() {
    return this._error;
  }
  /** @returns {Uint8Array|null} */
  get bytes() {
    return this._bytes;
  }

  /**
   * Feed one parsed frame ({cmd, payload}). Returns this.done afterward.
   */
  feed({ cmd, payload }) {
    if (cmd === CMD.DUMP_DATA) {
      const [seq, total, ...packed] = payload;
      const data = unpackChunk(packed);
      if (data === null) {
        this._error = `malformed packed chunk at seq ${seq}`;
        this._done = true;
        return this._done;
      }
      this._total = total;
      this._packets.set(seq, data);
    } else if (cmd === CMD.DUMP_END) {
      const [nPackets, xor7] = payload;
      if (this._packets.size !== nPackets) {
        this._error = `expected ${nPackets} packets, received ${this._packets.size}`;
        this._done = true;
        return this._done;
      }
      const flat = [];
      let checksum = 0;
      for (let seq = 0; seq < nPackets; seq++) {
        const data = this._packets.get(seq);
        if (!data) {
          this._error = `missing packet seq ${seq}`;
          this._done = true;
          return this._done;
        }
        for (const v of data) checksum ^= v;
        flat.push(...data);
      }
      if ((checksum & 0x7f) !== xor7) {
        this._error = "checksum mismatch";
        this._done = true;
        return this._done;
      }
      this._bytes = Uint8Array.from(flat);
      this._done = true;
    }
    return this._done;
  }
}

// ---- Transport interface ---------------------------------------------------
//
// Both WebMidiTransport and MockTransport implement this same shape so the
// UI layer (app.js) never branches on which one it's holding. Treat this
// class as documentation of the contract; it's fine to duck-type instead
// of literally extending it.

export class Transport {
  /** @returns {boolean} */
  get connected() {
    throw new Error("not implemented");
  }

  /** Locate + open the device. Resolves once ready, rejects if not found. */
  async connect() {
    throw new Error("not implemented");
  }

  async disconnect() {
    throw new Error("not implemented");
  }

  /**
   * BACKUP the full dump out of the 200e module at `modAddr` and return it.
   * @param {number} modAddr 7-bit bus address of the module to read
   * @returns {Promise<DumpBytes>}
   */
  async readDump(modAddr) {
    throw new Error("not implemented");
  }

  /**
   * RESTORE a full dump into the 200e module at `modAddr`.
   * @param {DumpBytes} bytes
   * @param {number} modAddr
   * @returns {Promise<void>}
   */
  async writeDump(bytes, modAddr) {
    throw new Error("not implemented");
  }

  /**
   * Subscribe to human-readable status/log lines (connection state, frame
   * traffic, errors). Returns an unsubscribe function.
   * @param {(line:string)=>void} listener
   */
  onLog(listener) {
    (this._logListeners ??= new Set()).add(listener);
    return () => this._logListeners.delete(listener);
  }

  _log(line) {
    for (const l of this._logListeners ?? []) l(line);
  }
}

// ---- shared protocol driver -------------------------------------------------
// The request/response choreography is identical on both transports -- only
// how a frame reaches the wire differs -- so it lives here once and both
// subclasses inherit it. That is what makes the MockTransport a real test of
// the protocol rather than a stub that returns the bytes it was handed:
// a mock read/write travels through exactly this code.
//
// Mirrors software/src/Bus200eBridge.cpp's FSM, which is the other end of
// every exchange below.

function nakMessage(frame, context) {
  const [cmd, err] = frame.payload;
  const why = NAK_ERRORS[err] ?? `unknown NAK code ${err}`;
  return `${context}: ${why} (NAK on cmd 0x${(cmd ?? 0).toString(16)})`;
}

export class SysExProtocolTransport extends Transport {
  /**
   * @param {{ackTimeoutMs?: number, jobTimeoutMs?: number}} [opts]
   */
  constructor(opts = {}) {
    super();
    // A reply to a framing-level request (ACK/NAK/INFO_R/STATUS_R).
    this.ackTimeoutMs = opts.ackTimeoutMs ?? 3000;
    // A reply that waits on the 200e bus itself. A foreign-module BACKUP or
    // RESTORE is a transient bus-master job measured in seconds --
    // Bus200eMaster.h's own BUS200E_MASTER_HARD_CAP_MS is 15s -- so anything
    // gated on one gets its own, much longer budget.
    this.jobTimeoutMs = opts.jobTimeoutMs ?? 20000;
  }

  // --- subclass hooks ---
  /** @param {Uint8Array} _frame full F0..F7 frame */
  _sendFrame(_frame) {
    throw new Error("not implemented");
  }
  /** @returns {Promise<{cmd:number,payload:number[]}>} */
  _waitForFrame(_timeoutMs) {
    throw new Error("not implemented");
  }

  _requireConnected() {
    if (!this.connected) throw new Error("transport not connected -- call connect() first");
  }

  _send(cmd, payload) {
    this._sendFrame(buildFrame(cmd, payload));
    this._log(`-> cmd 0x${cmd.toString(16)} (${payload.length}B payload)`);
  }

  _modAddr(modAddr) {
    const v = typeof modAddr === "number" ? modAddr : parseModuleAddress(modAddr);
    if (!Number.isInteger(v) || v < MODULE_ADDRESS_MIN || v > MODULE_ADDRESS_MAX) {
      throw new Error(`module address must be 0x00-0x7F, got ${modAddr}`);
    }
    return v;
  }

  /** INFO -> INFO_R. Firmware reports 0/0 for n_sequences/max_steps on purpose. */
  async info() {
    this._requireConnected();
    this._send(CMD.INFO, []);
    const frame = await this._waitForFrame(this.ackTimeoutMs);
    if (frame.cmd === CMD.NAK) throw new Error(nakMessage(frame, "INFO refused"));
    const p = frame.payload;
    return {
      schema: p[0],
      // 0 = "firmware does not decode the 251e byte layout" -- by design; the
      // dump is captured verbatim and sequence-codec.js owns what it means.
      nSequences: p[1],
      maxSteps: p[2],
      lastDumpBytes: (p[3] ?? 0) | ((p[4] ?? 0) << 7),
      chunkBytes: p[5],
      maxPackets: p[6],
      cardServing: !!p[7],
      raw: p,
    };
  }

  /** STATUS -> STATUS_R [state, error, mod_addr, is_restore, bridge_state]. */
  async status() {
    this._requireConnected();
    this._send(CMD.STATUS, []);
    const frame = await this._waitForFrame(this.ackTimeoutMs);
    if (frame.cmd === CMD.NAK) throw new Error(nakMessage(frame, "STATUS refused"));
    const [state, error, modAddr, isRestore, bridgeState] = frame.payload;
    return { state, error, modAddr, isRestore: !!isRestore, bridgeState, raw: frame.payload };
  }

  /**
   * GET_DUMP: the device masters a BACKUP against `modAddr` and streams the
   * captured bytes back as a DUMP_DATA run terminated by DUMP_END.
   * @param {number|string} modAddr
   */
  async readDump(modAddr) {
    this._requireConnected();
    const addr = this._modAddr(modAddr);
    this._send(CMD.GET_DUMP, [addr]);

    const ack = await this._waitForFrame(this.ackTimeoutMs);
    if (ack.cmd === CMD.NAK) throw new Error(nakMessage(ack, `read from module ${formatModuleAddress(addr)} refused`));

    const assembler = new DumpAssembler();
    // The first DUMP_DATA only arrives once the BACKUP has actually run on
    // the bus; everything after it is back-to-back USB traffic.
    let budget = this.jobTimeoutMs;
    while (!assembler.done) {
      const frame = await this._waitForFrame(budget);
      if (frame.cmd === CMD.NAK) throw new Error(nakMessage(frame, "capture failed"));
      if (frame.cmd === CMD.DUMP_DATA || frame.cmd === CMD.DUMP_END) {
        assembler.feed(frame);
        budget = this.ackTimeoutMs;
      }
      this._log(`<- cmd 0x${frame.cmd.toString(16)}`);
    }
    if (assembler.error) throw new Error(`dump reassembly failed: ${assembler.error}`);
    return assembler.bytes;
  }

  /**
   * PUT_DUMP: stage `bytes` in the device's card image, then have it master a
   * RESTORE into `modAddr`.
   * @param {DumpBytes} bytes
   * @param {number|string} modAddr
   */
  async writeDump(bytes, modAddr) {
    this._requireConnected();
    const addr = this._modAddr(modAddr);
    this._send(CMD.PUT_DUMP, [addr]);
    let reply = await this._waitForFrame(this.ackTimeoutMs);
    if (reply.cmd === CMD.NAK)
      throw new Error(nakMessage(reply, `write to module ${formatModuleAddress(addr)} refused`));

    for (const { cmd, payload } of packDump(bytes)) {
      this._send(cmd, payload);
      reply = await this._waitForFrame(this.ackTimeoutMs);
      if (reply.cmd === CMD.NAK) throw new Error(nakMessage(reply, "write rejected mid-transfer"));
    }

    // DUMP_END's ACK only means "checksum good, RESTORE accepted". The bus
    // job itself then runs for seconds; the device sends a final
    // ACK{PUT_DUMP, mod_addr} when it lands, or a NAK if it didn't. Waiting
    // for that is the difference between "the browser sent some bytes" and
    // "the module took them".
    const done = await this._waitForFrame(this.jobTimeoutMs);
    if (done.cmd === CMD.NAK) throw new Error(nakMessage(done, "restore failed"));
    this._log(`restore of module ${formatModuleAddress(addr)} complete`);
  }
}

// ---- WebMIDI transport ------------------------------------------------------

export class WebMidiTransport extends SysExProtocolTransport {
  /**
   * @param {{portNameCandidates?: string[], ackTimeoutMs?: number, jobTimeoutMs?: number}} [opts]
   */
  constructor(opts = {}) {
    super(opts);
    this.portNameCandidates = opts.portNameCandidates ?? DEFAULT_PORT_NAME_CANDIDATES;
    this._input = null;
    this._output = null;
    this._midiAccess = null;
  }

  get connected() {
    return !!(this._input && this._output);
  }

  async connect() {
    if (!navigator.requestMIDIAccess) {
      throw new Error("WebMIDI is not available in this browser (needs Chrome/Edge, and a secure context)");
    }
    this._midiAccess = await navigator.requestMIDIAccess({ sysex: true });
    const input = this._findPort(this._midiAccess.inputs);
    const output = this._findPort(this._midiAccess.outputs);
    if (!input || !output) {
      const names = [
        ...Array.from(this._midiAccess.inputs.values()).map((p) => p.name),
        ...Array.from(this._midiAccess.outputs.values()).map((p) => p.name),
      ];
      throw new Error(
        `no MIDI port matching [${this.portNameCandidates.join(", ")}]; ` +
          `available: ${names.length ? names.join(", ") : "(none)"}`
      );
    }
    this._input = input;
    this._output = output;
    await this._input.open();
    await this._output.open();
    this._log(`connected: ${input.name}`);
  }

  async disconnect() {
    await this._input?.close();
    await this._output?.close();
    this._input = null;
    this._output = null;
    this._log("disconnected");
  }

  _findPort(portMap) {
    const ports = Array.from(portMap.values());
    for (const candidate of this.portNameCandidates) {
      const hit = ports.find((p) => p.name?.toLowerCase().includes(candidate.toLowerCase()));
      if (hit) return hit;
    }
    return null;
  }

  _sendFrame(frame) {
    this._output.send(frame);
  }

  /** Waits for the next frame belonging to our app, with a timeout. */
  _waitForFrame(timeoutMs) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this._input.removeEventListener("midimessage", onMsg);
        reject(new Error("timeout waiting for device reply"));
      }, timeoutMs);
      const onMsg = (ev) => {
        const frame = parseFrame(ev.data);
        if (!frame) return; // not ours; keep waiting
        clearTimeout(timer);
        this._input.removeEventListener("midimessage", onMsg);
        resolve(frame);
      };
      this._input.addEventListener("midimessage", onMsg);
    });
  }
}

// ---- Mock device + transport -------------------------------------------------
//
// MockDevice is a frame-level model of software/src/Bus200eBridge.cpp: same
// commands, same ACK/NAK codes, same session rules (one job at a time, a
// PUT_DUMP must precede its DUMP_DATA, a bad checksum or a missing packet
// NAKs 6 and never "restores"). It exists so the whole pipeline -- UI edit ->
// encodeDump -> packDump/packChunk -> frames -> reassembly -> stored bytes,
// and the same path backwards -- is exercisable in Node with no hardware.
//
// What it deliberately does NOT model: bus timing. A real BACKUP/RESTORE
// masters the 200e bus for seconds and can fail with NAK 7/8/9; the mock
// answers instantly and never fails that way. Those paths are covered on the
// firmware side instead (software/test/test_bus200e_bridge.cpp).

export class MockDevice {
  /**
   * @param {{initialBytes?: Uint8Array}} [opts]
   */
  constructor(opts = {}) {
    this.bytes = opts.initialBytes ? Uint8Array.from(opts.initialBytes) : new Uint8Array(0);
    this.lastModAddr = null;
    this.restores = 0;
    this.captures = 0;
    this._session = null; // { modAddr, assembler }
  }

  _nak(cmd, err) {
    return [{ cmd: CMD.NAK, payload: [cmd, err] }];
  }

  /**
   * Feed one host->device frame; returns the device->host frames it answers
   * with, in order.
   * @param {{cmd:number, payload:number[]}} frame
   */
  handle({ cmd, payload }) {
    switch (cmd) {
      case CMD.INFO:
        return [
          {
            cmd: CMD.INFO_R,
            payload: [
              1,
              0, // n_sequences: firmware does not decode the 251e layout
              0, // max_steps: ditto
              this.bytes.length & 0x7f,
              (this.bytes.length >> 7) & 0x7f,
              DUMP_CHUNK_BYTES,
              MAX_DUMP_PACKETS,
              1,
            ],
          },
        ];

      case CMD.STATUS:
        return [
          {
            cmd: CMD.STATUS_R,
            payload: [0, 0, this.lastModAddr ?? 0, 0, this._session ? 3 : 0],
          },
        ];

      case CMD.GET_DUMP: {
        if (this._session) return this._nak(cmd, 5);
        if (payload.length < 1) return this._nak(cmd, 2); // no default module, ever
        this.lastModAddr = payload[0];
        this.captures++;
        return [{ cmd: CMD.ACK, payload: [CMD.GET_DUMP, payload[0]] }, ...packDump(this.bytes)];
      }

      case CMD.PUT_DUMP: {
        if (this._session) return this._nak(cmd, 5);
        if (payload.length < 1) return this._nak(cmd, 2);
        this.lastModAddr = payload[0];
        this._session = { modAddr: payload[0], assembler: new DumpAssembler() };
        return [{ cmd: CMD.ACK, payload: [CMD.PUT_DUMP, payload[0]] }];
      }

      case CMD.DUMP_DATA: {
        if (!this._session) return this._nak(cmd, 2);
        const [seq, total] = payload;
        if (total === 0 || seq >= total || total > MAX_DUMP_PACKETS) {
          this._session = null;
          return this._nak(cmd, 6);
        }
        this._session.assembler.feed({ cmd, payload });
        if (this._session.assembler.error) {
          this._session = null;
          return this._nak(cmd, 6);
        }
        return [{ cmd: CMD.ACK, payload: [CMD.DUMP_DATA, seq] }];
      }

      case CMD.DUMP_END: {
        if (!this._session) return this._nak(cmd, 2);
        const session = this._session;
        this._session = null;
        session.assembler.feed({ cmd, payload });
        if (session.assembler.error || !session.assembler.bytes) return this._nak(cmd, 6);
        this.bytes = session.assembler.bytes;
        this.restores++;
        return [
          { cmd: CMD.ACK, payload: [CMD.DUMP_END, payload[0]] },
          // the "the RESTORE finished on the bus" message the firmware's
          // pump_restore() sends once the master FSM reports DONE
          { cmd: CMD.ACK, payload: [CMD.PUT_DUMP, session.modAddr] },
        ];
      }

      default:
        return this._nak(cmd, 2);
    }
  }
}

export class MockTransport extends SysExProtocolTransport {
  /**
   * @param {{initialBytes?: Uint8Array, latencyMs?: number, device?: MockDevice}} [opts]
   */
  constructor(opts = {}) {
    super(opts);
    this.device = opts.device ?? new MockDevice({ initialBytes: opts.initialBytes });
    this._latencyMs = opts.latencyMs ?? 0;
    this._connected = false;
    this._inbox = [];
  }

  get connected() {
    return this._connected;
  }

  async connect() {
    await this._delay();
    this._connected = true;
    this._log("connected: Mock 251e (in-process Bus200eBridge model)");
  }

  async disconnect() {
    await this._delay();
    this._connected = false;
    this._log("disconnected");
  }

  _delay() {
    if (!this._latencyMs) return Promise.resolve();
    return new Promise((resolve) => setTimeout(resolve, this._latencyMs));
  }

  // The mock wire: a frame goes out as real bytes, comes back through
  // parseFrame, and the device's replies queue up the same way inbound MIDI
  // messages would. Nothing shortcuts the framing.
  _sendFrame(frame) {
    const parsed = parseFrame(frame);
    if (!parsed) return; // a frame the device wouldn't recognize is simply dropped
    for (const reply of this.device.handle(parsed)) {
      const wire = buildFrame(reply.cmd, reply.payload);
      this._inbox.push(parseFrame(wire));
    }
  }

  async _waitForFrame() {
    await this._delay();
    if (!this._inbox.length) throw new Error("timeout waiting for device reply");
    const frame = this._inbox.shift();
    this._log(`<- cmd 0x${frame.cmd.toString(16)} (simulated)`);
    return frame;
  }
}
