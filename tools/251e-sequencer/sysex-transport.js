// sysex-transport.js
// ---------------------------------------------------------------------------
// Transport layer for the 251e sequencer applet.
//
// *** PLACEHOLDER FRAMING -- READ BEFORE TOUCHING FIRMWARE CODE ***
// The real Xenomorpher <-> host SysEx protocol for the 251e bridge is being
// designed in parallel by firmware. Nothing below is agreed. It exists so
// the web applet has *something* concrete to encode/decode/round-trip
// against while that design lands, and so that swapping in the real framing
// is a small, isolated diff (this file, plus the two constants at the
// bottom of sequence-codec.js) rather than a rewrite of the UI.
//
// For style/precedent, the framing here loosely follows the one *real*,
// shipped SysEx protocol in this repo: Captain MIDI's, documented in
// docs/hoc-midi-sysex.md and implemented in tools/hoc_sysex.py. Same
// manufacturer ID, same family byte, same ACK/NAK/DUMP_DATA/DUMP_END shape.
// The app-id byte and every command/payload layout below is invented for
// this applet and is NOT part of that spec.
//
// USB port identity: as of this writing (2026-08-30), the Xenomorpher
// firmware (software/platformio.ini, env nlm* / T41) builds with
// `-DUSB_MIDI` and does NOT ship a custom src/usb_names.h override, so it
// enumerates over USB with Teensyduino's stock MIDI descriptor:
//   manufacturer: "Teensyduino"
//   product:      "Teensy MIDI"
// (confirmed by reading PRODUCT_NAME / MANUFACTURER_NAME in
// framework-arduinoteensy/cores/teensy4/usb_desc.h for the MIDI-only USB
// type this project builds). The on-device display name "NLM Xenomorpher"
// (OC_strings.cpp, Strings::NAME_NLM) is a UI string, not the USB product
// string -- WebMIDI will NOT see "Xenomorpher" until/unless firmware adds
// a usb_names.h override. DEFAULT_PORT_NAME_CANDIDATES below lists both so
// this keeps working the day that changes; only the constant needs to move
// "Xenomorpher" ahead of "Teensy MIDI" (or drop the fallback).
// ---------------------------------------------------------------------------

export const DEFAULT_PORT_NAME_CANDIDATES = ["Xenomorpher", "Teensy MIDI"];

/** @typedef {Uint8Array} DumpBytes raw device dump payload (see sequence-codec.js) */

// ---- Frame constants (placeholder) ---------------------------------------

export const SYSEX = Object.freeze({
  START: 0xf0,
  END: 0xf7,
  MFR_ID: 0x7d, // non-commercial/educational MIDI manufacturer ID (shared w/ Captain MIDI)
  FAMILY_ID: 0x62, // "Beige Maze" family byte, reused for repo consistency
  APP_ID: 0x35, // placeholder app id for "251e sequencer bridge" -- NOT assigned by firmware
  PROTO_VERSION: 0x01,
});

export const CMD = Object.freeze({
  INFO: 0x01, // H->D, no payload
  INFO_R: 0x41, // D->H: schema n_sequences max_steps dump_len_lo dump_len_hi
  GET_DUMP: 0x04, // H->D, no payload -- request full 4-sequence dump
  PUT_DUMP: 0x05, // H->D, no payload -- announce an incoming write, device replies ACK/NAK then expects DUMP_DATA*/DUMP_END
  DUMP_DATA: 0x44, // both directions: seq total {bytes...}
  DUMP_END: 0x45, // both directions: n_packets xor7
  ACK: 0x40, // D->H: echoes the command byte (+ context bytes)
  NAK: 0x7e, // D->H: cmd errcode
});

export const NAK_ERRORS = Object.freeze({
  1: "protocol version mismatch",
  2: "unknown command",
  3: "bad dump length",
  4: "bad value / out of range",
  5: "busy",
  6: "dump checksum/packet-count mismatch",
});

// Conservative like hOC's 60-byte ceiling (F0/F7 included); keeps every
// frame well inside any USB-MIDI SysEx buffer a Teensy build is likely to
// use. Payload bytes per DUMP_DATA frame after the seq/total header.
export const MAX_FRAME_BYTES = 64;
const DUMP_DATA_HEADER_BYTES = 2; // seq, total (not counting F0/mfr/family/app/ver/cmd/F7)
const NON_PAYLOAD_BYTES = 7; // F0 mfr family app ver cmd F7
export const DUMP_CHUNK_BYTES = MAX_FRAME_BYTES - NON_PAYLOAD_BYTES - DUMP_DATA_HEADER_BYTES;

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
 * Split a raw dump byte array into DUMP_DATA payloads + a trailing
 * DUMP_END payload, mirroring the hOC dump stream shape.
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
    chunks.push({ cmd: CMD.DUMP_DATA, payload: [seq, total, ...slice] });
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
      const [seq, total, ...data] = payload;
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
   * Read the full 4-sequence dump from the device.
   * @returns {Promise<DumpBytes>}
   */
  async readDump() {
    throw new Error("not implemented");
  }

  /**
   * Write a full 4-sequence dump to the device.
   * @param {DumpBytes} bytes
   * @returns {Promise<void>}
   */
  async writeDump(bytes) {
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

// ---- WebMIDI transport ------------------------------------------------------

export class WebMidiTransport extends Transport {
  /**
   * @param {{portNameCandidates?: string[], ackTimeoutMs?: number}} [opts]
   */
  constructor(opts = {}) {
    super();
    this.portNameCandidates = opts.portNameCandidates ?? DEFAULT_PORT_NAME_CANDIDATES;
    this.ackTimeoutMs = opts.ackTimeoutMs ?? 3000;
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

  _requireConnected() {
    if (!this.connected) throw new Error("transport not connected -- call connect() first");
  }

  _send(cmd, payload) {
    const frame = buildFrame(cmd, payload);
    this._output.send(frame);
    this._log(`-> cmd 0x${cmd.toString(16)} (${payload.length}B payload)`);
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

  async readDump() {
    this._requireConnected();
    this._send(CMD.GET_DUMP, []);
    const assembler = new DumpAssembler();
    while (!assembler.done) {
      const frame = await this._waitForFrame(this.ackTimeoutMs);
      if (frame.cmd === CMD.NAK) {
        throw new Error(`device NAK: ${NAK_ERRORS[frame.payload[1]] ?? "unknown"}`);
      }
      assembler.feed(frame);
      this._log(`<- cmd 0x${frame.cmd.toString(16)}`);
    }
    if (assembler.error) throw new Error(`dump reassembly failed: ${assembler.error}`);
    return assembler.bytes;
  }

  async writeDump(bytes) {
    this._requireConnected();
    this._send(CMD.PUT_DUMP, []);
    let reply = await this._waitForFrame(this.ackTimeoutMs);
    if (reply.cmd === CMD.NAK) throw new Error(`device refused write: ${NAK_ERRORS[reply.payload[1]] ?? "unknown"}`);

    for (const { cmd, payload } of packDump(bytes)) {
      this._send(cmd, payload);
      reply = await this._waitForFrame(this.ackTimeoutMs);
      if (reply.cmd === CMD.NAK) throw new Error(`device NAK during write: ${NAK_ERRORS[reply.payload[1]] ?? "unknown"}`);
    }
    this._log("write complete (ACKed)");
  }
}

// ---- Mock transport ---------------------------------------------------------

export class MockTransport extends Transport {
  /**
   * @param {{initialBytes?: Uint8Array, latencyMs?: number}} [opts]
   */
  constructor(opts = {}) {
    super();
    this._bytes = opts.initialBytes ? Uint8Array.from(opts.initialBytes) : new Uint8Array(0);
    this._latencyMs = opts.latencyMs ?? 30;
    this._connected = false;
  }

  get connected() {
    return this._connected;
  }

  async connect() {
    await this._delay();
    this._connected = true;
    this._log("connected: Mock 251e (synthetic dump)");
  }

  async disconnect() {
    await this._delay();
    this._connected = false;
    this._log("disconnected");
  }

  _requireConnected() {
    if (!this.connected) throw new Error("transport not connected -- call connect() first");
  }

  _delay() {
    return new Promise((resolve) => setTimeout(resolve, this._latencyMs));
  }

  /**
   * Round-trips the stored bytes through the *real* packDump/DumpAssembler
   * logic instead of just handing them back, so the framing module itself
   * gets exercised on every mock read too.
   */
  async readDump() {
    this._requireConnected();
    const assembler = new DumpAssembler();
    for (const frame of packDump(this._bytes)) {
      await this._delay();
      this._log(`<- cmd 0x${frame.cmd.toString(16)} (simulated)`);
      assembler.feed(frame);
    }
    if (assembler.error) throw new Error(`internal mock framing error: ${assembler.error}`);
    return assembler.bytes;
  }

  async writeDump(bytes) {
    this._requireConnected();
    const assembler = new DumpAssembler();
    for (const frame of packDump(bytes)) {
      await this._delay();
      this._log(`-> cmd 0x${frame.cmd.toString(16)} (simulated)`);
      assembler.feed(frame);
    }
    if (assembler.error) throw new Error(`internal mock framing error: ${assembler.error}`);
    this._bytes = assembler.bytes;
    this._log("write complete (mock ACKed)");
  }
}
