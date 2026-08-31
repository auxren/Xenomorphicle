// sequence-codec.js
// ---------------------------------------------------------------------------
// *** PROVISIONAL / PLACEHOLDER BYTE FORMAT -- NOT THE REAL 251E FORMAT ***
//
// The actual Buchla 251e "Quad Sequential Voltage Source" byte layout is
// being reverse-engineered in parallel and is not yet known to this applet.
// Everything in this file -- field choices, byte widths, value ranges,
// scaling -- is an invented, clearly-bounded stand-in so the rest of the
// app (UI, transport) has a concrete shape to work against.
//
// ALL format-specific knowledge lives in this one file. When the real
// format lands, only decodeDump/encodeDump/the constants below need to
// change -- app.js and sysex-transport.js talk in terms of the decoded
// {sequences: [...]} shape and raw bytes respectively, never in terms of
// individual field offsets.
//
// Provisional schema:
//   - 4 independent sequences (A-D), one per 251e output channel.
//   - Each sequence holds up to MAX_STEPS steps; `length` (1..MAX_STEPS)
//     says how many are active/played (matches "sequential" hardware that
//     runs stages 1..N and wraps).
//   - Each step has:
//       voltage    0.00 .. 10.00 V (plausible for a Buchla control voltage
//                  source; encoded as one 7-bit byte, ~0.0787V/LSB)
//       gateOn     whether this step fires its gate/trigger output
//       gateLength 0..100, gate width as % of the step's time slot
//   - Every dump byte is a plain 7-bit value (0..127), same rule Captain
//     MIDI's real protocol uses ("no 8-bit packing, no struct dumps") --
//     these bytes get sliced directly into SysEx payloads by
//     sysex-transport.js's packDump, which does NOT do 8-to-7-bit
//     repacking, so nothing in this file may produce a byte >= 0x80.
//     (gateOn therefore gets its own byte instead of a spare high bit.)
//   - Byte layout per sequence: [length, step0.voltageByte, step0.gateOn,
//     step0.gateLength, step1.voltageByte, step1.gateOn, step1.gateLength,
//     ...].
//   - Full dump = 4 sequences back-to-back, no header/footer (the
//     transport layer's SysEx framing carries length/checksum instead).
// ---------------------------------------------------------------------------

export const NUM_SEQUENCES = 4;
export const MAX_STEPS = 16;
export const MIN_VOLTAGE = 0;
export const MAX_VOLTAGE = 10;
export const SEQUENCE_LABELS = ["A", "B", "C", "D"];

const SEVEN_BIT_MAX = 127;
const BYTES_PER_STEP = 3; // voltage, gateOn, gateLength -- all 7-bit
const BYTES_PER_SEQUENCE = 1 + MAX_STEPS * BYTES_PER_STEP; // length byte + steps
export const DUMP_LENGTH = NUM_SEQUENCES * BYTES_PER_SEQUENCE; // 4 * 49 = 196

const VOLTAGE_TO_BYTE = SEVEN_BIT_MAX / MAX_VOLTAGE;
const BYTE_TO_VOLTAGE = MAX_VOLTAGE / SEVEN_BIT_MAX;

function clamp(v, lo, hi) {
  return Math.min(hi, Math.max(lo, v));
}

/**
 * @typedef {{voltage: number, gateOn: boolean, gateLength: number}} Step
 * @typedef {{length: number, steps: Step[]}} Sequence
 * @typedef {{sequences: Sequence[]}} SequencerState
 */

/** Creates a step with sane defaults. */
function makeStep(voltage = 0, gateOn = false, gateLength = 50) {
  return { voltage, gateOn, gateLength };
}

/** Creates an all-zero, all-off state: 4 sequences, all MAX_STEPS long. */
export function createEmptyState() {
  return {
    sequences: Array.from({ length: NUM_SEQUENCES }, () => ({
      length: MAX_STEPS,
      steps: Array.from({ length: MAX_STEPS }, () => makeStep()),
    })),
  };
}

/**
 * Creates a musically-plausible synthetic state for the mock transport and
 * for UI development -- NOT device data, just enough variety to exercise
 * the codec and UI (different lengths, voltages, gate patterns per row).
 */
export function createSyntheticState() {
  const state = createEmptyState();

  // Sequence A: ascending 8-step "scale" in 1V/oct-ish steps, gates on.
  const seqA = state.sequences[0];
  seqA.length = 8;
  for (let i = 0; i < seqA.length; i++) {
    seqA.steps[i] = makeStep((i % 8) * (5 / 7), true, 40);
  }

  // Sequence B: descending 6-step, alternating gates, longer gate length.
  const seqB = state.sequences[1];
  seqB.length = 6;
  for (let i = 0; i < seqB.length; i++) {
    seqB.steps[i] = makeStep(8 - i * 1.3, i % 2 === 0, 70);
  }

  // Sequence C: 16-step random-ish CV pattern with sparse gates.
  const seqC = state.sequences[2];
  seqC.length = 16;
  const cPattern = [0, 2, 1, 5, 3, 7, 2, 6, 4, 8, 1, 3, 9, 0, 6, 2];
  for (let i = 0; i < seqC.length; i++) {
    seqC.steps[i] = makeStep(cPattern[i], i % 3 === 0, 25);
  }

  // Sequence D: 4-step drone, all same voltage, full gates.
  const seqD = state.sequences[3];
  seqD.length = 4;
  for (let i = 0; i < seqD.length; i++) {
    seqD.steps[i] = makeStep(3.75, true, 95);
  }

  return state;
}

/** Deep clone -- editors should mutate a clone, never the live/last-read state directly. */
export function cloneState(state) {
  return {
    sequences: state.sequences.map((seq) => ({
      length: seq.length,
      steps: seq.steps.map((s) => ({ ...s })),
    })),
  };
}

/**
 * Decodes a raw device dump into a SequencerState.
 * @param {Uint8Array} bytes
 * @returns {SequencerState}
 */
export function decodeDump(bytes) {
  if (bytes.length !== DUMP_LENGTH) {
    throw new Error(`expected a ${DUMP_LENGTH}-byte dump, got ${bytes.length} bytes`);
  }
  const sequences = [];
  let offset = 0;
  for (let s = 0; s < NUM_SEQUENCES; s++) {
    const length = clamp(bytes[offset], 1, MAX_STEPS);
    offset += 1;
    const steps = [];
    for (let i = 0; i < MAX_STEPS; i++) {
      const voltageByte = bytes[offset];
      const gateOnByte = bytes[offset + 1];
      const gateLengthByte = bytes[offset + 2];
      offset += 3;
      steps.push({
        voltage: Math.round(voltageByte * BYTE_TO_VOLTAGE * 1000) / 1000,
        gateOn: gateOnByte !== 0,
        gateLength: clamp(gateLengthByte, 0, 100),
      });
    }
    sequences.push({ length, steps });
  }
  return { sequences };
}

/**
 * Encodes a SequencerState back into a raw device dump.
 * @param {SequencerState} state
 * @returns {Uint8Array}
 */
export function encodeDump(state) {
  if (!state?.sequences || state.sequences.length !== NUM_SEQUENCES) {
    throw new Error(`state must have exactly ${NUM_SEQUENCES} sequences`);
  }
  const bytes = new Uint8Array(DUMP_LENGTH);
  let offset = 0;
  for (let s = 0; s < NUM_SEQUENCES; s++) {
    const seq = state.sequences[s];
    if (!seq || !Array.isArray(seq.steps)) {
      throw new Error(`sequence ${s} is missing a steps array`);
    }
    bytes[offset] = clamp(Math.round(seq.length ?? MAX_STEPS), 1, MAX_STEPS);
    offset += 1;
    for (let i = 0; i < MAX_STEPS; i++) {
      const step = seq.steps[i] ?? makeStep();
      const voltage = clamp(step.voltage ?? 0, MIN_VOLTAGE, MAX_VOLTAGE);
      const voltageByte = clamp(Math.round(voltage * VOLTAGE_TO_BYTE), 0, SEVEN_BIT_MAX);
      const gateLength = clamp(Math.round(step.gateLength ?? 0), 0, 100);
      bytes[offset] = voltageByte;
      bytes[offset + 1] = step.gateOn ? 1 : 0;
      bytes[offset + 2] = gateLength;
      offset += 3;
    }
  }
  return bytes;
}
