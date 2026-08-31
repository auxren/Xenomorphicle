// app.js -- UI controller for the 251e sequencer applet.
//
// Talks to the device only through the Transport interface
// (sysex-transport.js) and only understands sequence data through the
// codec (sequence-codec.js). It does not know or care whether it's holding
// a MockTransport or a WebMidiTransport -- same methods either way.
import { MAX_STEPS, MAX_VOLTAGE, SEQUENCE_LABELS, createSyntheticState, cloneState, decodeDump, encodeDump } from "./sequence-codec.js";
import { MockTransport, WebMidiTransport, DEFAULT_PORT_NAME_CANDIDATES } from "./sysex-transport.js";

const els = {
  grid: document.getElementById("grid"),
  transportMode: document.getElementById("transport-mode"),
  connectBtn: document.getElementById("connect-btn"),
  readBtn: document.getElementById("read-btn"),
  writeBtn: document.getElementById("write-btn"),
  status: document.getElementById("status"),
  log: document.getElementById("log"),
  dirtyBadge: document.getElementById("dirty-badge"),
};

/** @type {import('./sysex-transport.js').Transport} */
let transport = null;
let state = createSyntheticState(); // working copy the UI edits
let lastReadBytes = encodeDump(state); // what the device last reported / last write -- for dirty tracking
let dirty = false;

function logLine(text) {
  const time = new Date().toLocaleTimeString();
  const div = document.createElement("div");
  div.className = "log-line";
  div.textContent = `[${time}] ${text}`;
  els.log.prepend(div);
}

function setStatus(text, kind = "info") {
  els.status.textContent = text;
  els.status.className = `status status-${kind}`;
}

function markDirty(isDirty) {
  dirty = isDirty;
  els.dirtyBadge.hidden = !isDirty;
}

// ---- transport lifecycle ---------------------------------------------------

function buildTransport(mode) {
  if (mode === "webmidi") {
    return new WebMidiTransport({ portNameCandidates: DEFAULT_PORT_NAME_CANDIDATES });
  }
  return new MockTransport({ initialBytes: encodeDump(createSyntheticState()) });
}

async function connect() {
  const mode = els.transportMode.value;
  transport?.onLog && (transport._logListeners = null); // drop old listeners
  transport = buildTransport(mode);
  transport.onLog(logLine);
  setStatus("connecting...", "info");
  try {
    await transport.connect();
    setStatus(`connected (${mode === "webmidi" ? "WebMIDI" : "mock"})`, "ok");
    els.readBtn.disabled = false;
    els.writeBtn.disabled = false;
    els.connectBtn.textContent = "Disconnect";
  } catch (err) {
    setStatus(`connect failed: ${err.message}`, "error");
    logLine(`connect error: ${err.message}`);
    transport = null;
  }
}

async function disconnect() {
  if (!transport) return;
  await transport.disconnect();
  transport = null;
  els.readBtn.disabled = true;
  els.writeBtn.disabled = true;
  els.connectBtn.textContent = "Connect";
  setStatus("disconnected", "info");
}

async function readFromDevice() {
  if (!transport) return;
  setStatus("reading...", "info");
  try {
    const bytes = await transport.readDump();
    lastReadBytes = bytes;
    state = decodeDump(bytes);
    markDirty(false);
    renderGrid();
    setStatus("read complete", "ok");
    logLine(`read ${bytes.length} bytes, decoded ${state.sequences.length} sequences`);
  } catch (err) {
    setStatus(`read failed: ${err.message}`, "error");
    logLine(`read error: ${err.message}`);
  }
}

async function writeToDevice() {
  if (!transport) return;
  setStatus("writing...", "info");
  try {
    const bytes = encodeDump(state);
    await transport.writeDump(bytes);
    lastReadBytes = bytes;
    markDirty(false);
    setStatus("write complete", "ok");
    logLine(`wrote ${bytes.length} bytes`);
  } catch (err) {
    setStatus(`write failed: ${err.message}`, "error");
    logLine(`write error: ${err.message}`);
  }
}

// ---- grid rendering ---------------------------------------------------------

function voltageToPercent(v) {
  return (v / MAX_VOLTAGE) * 100;
}

function renderGrid() {
  els.grid.innerHTML = "";
  state.sequences.forEach((seq, seqIndex) => {
    const row = document.createElement("section");
    row.className = "sequence-row";

    const header = document.createElement("div");
    header.className = "sequence-header";
    header.innerHTML = `
      <span class="sequence-label">${SEQUENCE_LABELS[seqIndex]}</span>
      <label class="length-control">
        length
        <input type="number" min="1" max="${MAX_STEPS}" value="${seq.length}" data-seq="${seqIndex}" class="length-input" />
      </label>
    `;
    row.appendChild(header);

    const stepsEl = document.createElement("div");
    stepsEl.className = "steps";
    seq.steps.forEach((step, stepIndex) => {
      const cell = document.createElement("div");
      cell.className = "step";
      cell.classList.toggle("step-inactive", stepIndex >= seq.length);
      cell.classList.toggle("step-gate-on", step.gateOn);

      const bar = document.createElement("div");
      bar.className = "step-bar";
      bar.style.height = `${voltageToPercent(step.voltage)}%`;
      cell.appendChild(bar);

      const num = document.createElement("input");
      num.type = "number";
      num.className = "step-voltage";
      num.min = "0";
      num.max = String(MAX_VOLTAGE);
      num.step = "0.05";
      num.value = step.voltage.toFixed(2);
      num.addEventListener("input", () => {
        step.voltage = clampNum(parseFloat(num.value), 0, MAX_VOLTAGE);
        bar.style.height = `${voltageToPercent(step.voltage)}%`;
        markDirty(true);
      });
      cell.appendChild(num);

      const gateBtn = document.createElement("button");
      gateBtn.className = "step-gate-toggle";
      gateBtn.type = "button";
      gateBtn.textContent = step.gateOn ? "gate" : "off";
      gateBtn.addEventListener("click", () => {
        step.gateOn = !step.gateOn;
        gateBtn.textContent = step.gateOn ? "gate" : "off";
        cell.classList.toggle("step-gate-on", step.gateOn);
        markDirty(true);
      });
      cell.appendChild(gateBtn);

      cell.addEventListener("click", (ev) => {
        if (ev.target === num || ev.target === gateBtn) return;
      });

      stepsEl.appendChild(cell);
    });
    row.appendChild(stepsEl);
    els.grid.appendChild(row);
  });

  // wire up length inputs after render (event delegation would also work)
  els.grid.querySelectorAll(".length-input").forEach((input) => {
    input.addEventListener("change", () => {
      const seqIndex = Number(input.dataset.seq);
      const len = clampNum(Math.round(Number(input.value)), 1, MAX_STEPS);
      input.value = String(len);
      state.sequences[seqIndex].length = len;
      markDirty(true);
      renderGrid();
    });
  });
}

function clampNum(v, lo, hi) {
  if (Number.isNaN(v)) return lo;
  return Math.min(hi, Math.max(lo, v));
}

// ---- wiring -----------------------------------------------------------------

els.connectBtn.addEventListener("click", () => {
  if (transport?.connected) {
    disconnect();
  } else {
    connect();
  }
});
els.readBtn.addEventListener("click", readFromDevice);
els.writeBtn.addEventListener("click", writeToDevice);

// URL flag ?transport=webmidi|mock preselects the dropdown (mock is default
// and is what this applet is verified against -- see README block in
// index.html for why real hardware is out of scope right now).
const urlMode = new URLSearchParams(location.search).get("transport");
if (urlMode === "webmidi" || urlMode === "mock") {
  els.transportMode.value = urlMode;
}

renderGrid();
setStatus("idle -- press Connect", "info");
