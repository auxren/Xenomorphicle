// app.js -- UI controller for the 251e sequencer applet.
//
// Talks to the device only through the Transport interface
// (sysex-transport.js) and only understands sequence data through the
// codec (sequence-codec.js). It does not know or care whether it's holding
// a MockTransport or a WebMidiTransport -- same methods either way.
import { MAX_STEPS, MAX_VOLTAGE, SEQUENCE_LABELS, createSyntheticState, decodeDump, encodeDump } from "./sequence-codec.js";
import {
  MockTransport,
  WebMidiTransport,
  DEFAULT_PORT_NAME_CANDIDATES,
  parseModuleAddress,
  formatModuleAddress,
} from "./sysex-transport.js";

const els = {
  grid: document.getElementById("grid"),
  transportMode: document.getElementById("transport-mode"),
  connectBtn: document.getElementById("connect-btn"),
  moduleAddr: document.getElementById("module-addr"),
  readBtn: document.getElementById("read-btn"),
  writeBtn: document.getElementById("write-btn"),
  status: document.getElementById("status"),
  log: document.getElementById("log"),
  dirtyBadge: document.getElementById("dirty-badge"),
};

const MODULE_ADDR_STORAGE_KEY = "251e-sequencer.module-addr";

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

// ---- module address ---------------------------------------------------------
// GET_DUMP/PUT_DUMP both carry [mod_addr]: which 200e module on the bus to
// BACKUP from / RESTORE to. There is deliberately NO default -- the firmware
// NAKs an addressless GET_DUMP/PUT_DUMP rather than guess (Bus200eBridge.cpp),
// and neither should the UI. Read/Write stay disabled until a valid address
// is entered.

/** @returns {number|null} the parsed 7-bit address, or null if the field is empty/invalid */
function currentModuleAddr() {
  const raw = els.moduleAddr.value.trim();
  if (!raw) return null;
  try {
    return parseModuleAddress(raw);
  } catch {
    return null;
  }
}

function updateActionButtons() {
  const ready = !!transport?.connected && currentModuleAddr() !== null;
  els.readBtn.disabled = !ready;
  els.writeBtn.disabled = !ready;
  const raw = els.moduleAddr.value.trim();
  els.moduleAddr.classList.toggle("invalid", raw !== "" && currentModuleAddr() === null);
  if (!ready && transport?.connected) {
    els.readBtn.title = els.writeBtn.title = "enter a module address (2 hex digits) first";
  } else {
    els.readBtn.title = els.writeBtn.title = "";
  }
}

function loadStoredModuleAddr() {
  try {
    const saved = localStorage.getItem(MODULE_ADDR_STORAGE_KEY);
    if (saved) els.moduleAddr.value = saved;
  } catch {
    // private windows / blocked site data: the field just starts empty
  }
}

function storeModuleAddr(addr) {
  try {
    localStorage.setItem(MODULE_ADDR_STORAGE_KEY, formatModuleAddress(addr));
  } catch {
    // non-fatal
  }
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
  transport = buildTransport(mode);
  transport.onLog(logLine);
  setStatus("connecting...", "info");
  try {
    await transport.connect();
    setStatus(`connected (${mode === "webmidi" ? "WebMIDI" : "mock"})`, "ok");
    els.connectBtn.textContent = "Disconnect";
  } catch (err) {
    setStatus(`connect failed: ${err.message}`, "error");
    logLine(`connect error: ${err.message}`);
    transport = null;
  }
  updateActionButtons();
}

async function disconnect() {
  if (!transport) return;
  await transport.disconnect();
  transport = null;
  els.connectBtn.textContent = "Connect";
  setStatus("disconnected", "info");
  updateActionButtons();
}

async function readFromDevice() {
  const addr = currentModuleAddr();
  if (!transport || addr === null) return;
  setStatus(`reading module ${formatModuleAddress(addr)}...`, "info");
  try {
    // The device masters a BACKUP on the 200e bus, which takes real seconds
    // -- the transport's own jobTimeoutMs covers that, not this call.
    const bytes = await transport.readDump(addr);
    storeModuleAddr(addr);
    logLine(`read ${bytes.length} bytes from module ${formatModuleAddress(addr)}`);
    lastReadBytes = bytes;
    state = decodeDump(bytes);
    markDirty(false);
    renderGrid();
    setStatus("read complete", "ok");
  } catch (err) {
    setStatus(`read failed: ${err.message}`, "error");
    logLine(`read error: ${err.message}`);
    // A real 251e dump is very unlikely to be exactly DUMP_LENGTH bytes: the
    // codec's layout is an admitted placeholder (see sequence-codec.js), so
    // say so rather than let "expected a 196-byte dump" read as a bug in the
    // transport, which just delivered whatever the module actually holds.
    if (/expected a \d+-byte dump/.test(err.message)) {
      logLine(
        "note: the bytes arrived fine -- it's sequence-codec.js's PLACEHOLDER " +
          "251e layout that doesn't match them. The real byte format is still " +
          "being reverse-engineered."
      );
    }
  }
}

async function writeToDevice() {
  const addr = currentModuleAddr();
  if (!transport || addr === null) return;
  setStatus(`writing module ${formatModuleAddress(addr)}...`, "info");
  try {
    const bytes = encodeDump(state);
    await transport.writeDump(bytes, addr);
    storeModuleAddr(addr);
    lastReadBytes = bytes;
    markDirty(false);
    setStatus("write complete", "ok");
    logLine(`wrote ${bytes.length} bytes to module ${formatModuleAddress(addr)}`);
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
els.moduleAddr.addEventListener("input", () => {
  // uppercase as you type, so the field reads like the firmware console's
  // own %02X logging
  const caret = els.moduleAddr.selectionStart;
  els.moduleAddr.value = els.moduleAddr.value.toUpperCase();
  els.moduleAddr.setSelectionRange(caret, caret);
  updateActionButtons();
});

// Last bench session's module address, if the browser kept it. Loaded FIRST
// so an explicit ?module= in the URL still wins.
loadStoredModuleAddr();

// URL flag ?transport=webmidi|mock preselects the dropdown (mock is default
// and is what this applet is verified against -- see README block in
// index.html for why real hardware is out of scope right now).
const urlParams = new URLSearchParams(location.search);
const urlMode = urlParams.get("transport");
if (urlMode === "webmidi" || urlMode === "mock") {
  els.transportMode.value = urlMode;
}
// ?module=2e preseeds the address field (a bench session usually targets the
// same module all afternoon; the last valid one is remembered anyway).
const urlModule = urlParams.get("module");
if (urlModule) {
  try {
    els.moduleAddr.value = formatModuleAddress(parseModuleAddress(urlModule));
  } catch {
    // ignore a junk ?module= rather than blocking the whole applet on it
  }
}

updateActionButtons();
renderGrid();
setStatus("idle -- press Connect", "info");
