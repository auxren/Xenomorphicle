// app.js -- UI controller for the 251e sequencer applet.
//
// Talks to the device only through the Transport interface
// (sysex-transport.js) and only understands sequence data through the
// codec (sequence-codec.js). It does not know or care whether it's holding
// a MockTransport or a WebMidiTransport -- same methods either way.
//
// The unit both sides move is a whole 63120-byte BANK: 30 slots x 4 sequences
// x 50 stages. The UI edits one slot/sequence at a time, but the bank in
// memory is always complete, so a write puts back all 30 slots with every
// untouched byte -- including the fields nobody has decoded yet -- intact.
import {
  BANK_BYTES,
  SLOTS_PER_BANK,
  SEQUENCES_PER_SLOT,
  STAGES_PER_SEQUENCE,
  SEQUENCE_LABELS,
  STAGE_VALUE_MAX,
  STAGE_TIME_MAX,
  createSyntheticBank,
  decodeBank,
  encodeBank,
  getSequenceParam,
  setSequenceParam,
} from "./sequence-codec.js";
import {
  MockTransport,
  WebMidiTransport,
  DEFAULT_PORT_NAME_CANDIDATES,
  MAX_DUMP_BYTES,
  parseModuleAddress,
  formatModuleAddress,
} from "./sysex-transport.js";

const els = {
  grid: document.getElementById("grid"),
  slotSelect: document.getElementById("slot-select"),
  seqTabs: document.getElementById("seq-tabs"),
  slotMeta: document.getElementById("slot-meta"),
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
let bank = createSyntheticBank(); // working copy the UI edits (a full 30-slot bank)
let slotIndex = 0;
let seqIndex = 0;
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

function currentSlot() {
  return bank.slots[slotIndex];
}

function currentSequence() {
  return currentSlot().sequences[seqIndex];
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

// ---- transport capability check --------------------------------------------
// A 251e BACKUP/RESTORE is one continuous 63120-byte transfer -- the module
// cannot be asked for a single slot. Protocol v2's 14-bit packet counters
// carry a whole bank in one session (1503 packets x 42 raw bytes), so this
// check is normally inert. It exists for the case where the device's card
// image can't stage a bank at all: then saying so up front beats letting the
// stream die inside packDump/buildFrame with an error that reads like a codec
// bug, and beats sending a truncated bank to a module that would take it.

/** @returns {string|null} why a bank transfer can't run, or null if it can */
function bankTransferBlocker() {
  if (BANK_BYTES <= MAX_DUMP_BYTES) return null;
  return (
    `a full 251e bank is ${BANK_BYTES} bytes, which exceeds this transport's ` +
    `${MAX_DUMP_BYTES}-byte per-session limit -- refusing rather than sending a ` +
    `partial bank (the codec and UI are ready for the full one)`
  );
}

// ---- transport lifecycle ---------------------------------------------------

function buildTransport(mode) {
  if (mode === "webmidi") {
    return new WebMidiTransport({ portNameCandidates: DEFAULT_PORT_NAME_CANDIDATES });
  }
  return new MockTransport({ initialBytes: encodeBank(createSyntheticBank()) });
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
  // The user expressed which module they mean; remember it whether or not the
  // transfer itself gets off the ground.
  storeModuleAddr(addr);

  const blocker = bankTransferBlocker();
  if (blocker) {
    setStatus("read unavailable -- bank exceeds transport limit", "error");
    logLine(`read not attempted: ${blocker}`);
    return;
  }

  setStatus(`reading module ${formatModuleAddress(addr)}...`, "info");
  try {
    // The device masters a BACKUP on the 200e bus, which takes real seconds
    // -- the transport's own jobTimeoutMs covers that, not this call.
    const bytes = await transport.readDump(addr);
    logLine(`read ${bytes.length} bytes from module ${formatModuleAddress(addr)}`);
    bank = decodeBank(bytes);
    slotIndex = Math.min(slotIndex, SLOTS_PER_BANK - 1);
    markDirty(false);
    renderAll();
    setStatus("read complete", "ok");
  } catch (err) {
    setStatus(`read failed: ${err.message}`, "error");
    logLine(`read error: ${err.message}`);
    if (/expected a \d+-byte 251e bank/.test(err.message)) {
      logLine(
        `note: the bytes arrived fine -- they just aren't a whole ${BANK_BYTES}-byte ` +
          "251e bank (30 slots x 2104). A short read usually means the capture buffer " +
          "wrapped or the transfer was truncated, not that the layout is wrong."
      );
    }
  }
}

async function writeToDevice() {
  const addr = currentModuleAddr();
  if (!transport || addr === null) return;
  storeModuleAddr(addr);

  const blocker = bankTransferBlocker();
  if (blocker) {
    setStatus("write unavailable -- bank exceeds transport limit", "error");
    logLine(`write not attempted: ${blocker}`);
    return;
  }

  setStatus(`writing module ${formatModuleAddress(addr)}...`, "info");
  try {
    // A RESTORE always sends the whole bank -- all 30 slots, every byte the
    // user didn't touch included.
    const bytes = encodeBank(bank);
    await transport.writeDump(bytes, addr);
    markDirty(false);
    setStatus("write complete", "ok");
    logLine(`wrote ${bytes.length} bytes to module ${formatModuleAddress(addr)}`);
  } catch (err) {
    setStatus(`write failed: ${err.message}`, "error");
    logLine(`write error: ${err.message}`);
  }
}

// ---- rendering --------------------------------------------------------------

function renderSlotSelect() {
  els.slotSelect.innerHTML = "";
  for (let i = 0; i < SLOTS_PER_BANK; i++) {
    const opt = document.createElement("option");
    opt.value = String(i);
    // The module's own front panel numbers presets 1..30.
    opt.textContent = `slot ${i + 1}`;
    els.slotSelect.appendChild(opt);
  }
  els.slotSelect.value = String(slotIndex);
}

function renderSeqTabs() {
  els.seqTabs.innerHTML = "";
  for (let s = 0; s < SEQUENCES_PER_SLOT; s++) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "seq-tab";
    btn.classList.toggle("seq-tab-active", s === seqIndex);
    btn.textContent = SEQUENCE_LABELS[s];
    btn.addEventListener("click", () => {
      seqIndex = s;
      renderSeqTabs();
      renderSlotMeta();
      renderGrid();
    });
    els.seqTabs.appendChild(btn);
  }
}

function numberField(labelText, value, { min, max, step, title }, onChange) {
  const label = document.createElement("label");
  label.className = "meta-field";
  label.title = title ?? "";
  const span = document.createElement("span");
  span.textContent = labelText;
  label.appendChild(span);
  const input = document.createElement("input");
  input.type = "number";
  input.value = String(value);
  if (min !== undefined) input.min = String(min);
  if (max !== undefined) input.max = String(max);
  if (step !== undefined) input.step = String(step);
  input.addEventListener("change", () => onChange(Number(input.value), input));
  label.appendChild(input);
  return label;
}

function renderSlotMeta() {
  const slot = currentSlot();
  const seq = currentSequence();
  els.slotMeta.innerHTML = "";

  // The two big-endian header floats. Confirmed as floats; their MEANING
  // (offset/scale? output range?) is not confirmed, so they are labelled by
  // position, not by a job.
  slot.header.floats.forEach((value, i) => {
    els.slotMeta.appendChild(
      numberField(
        `header float ${i}`,
        value,
        { step: "any", title: "IEEE-754 big-endian float at slot header byte " + i * 4 + ". Confirmed as a float; its meaning is not decoded (defaults 0.0 / 1.0)." },
        (v) => {
          if (!Number.isFinite(v)) return;
          slot.header.floats[i] = v;
          markDirty(true);
        }
      )
    );
  });

  els.slotMeta.appendChild(
    numberField(
      `seq ${SEQUENCE_LABELS[seqIndex]} param`,
      getSequenceParam(seq),
      { min: 0, max: 255, step: 1, title: "Block-trailer byte +1: a per-sequence parameter, 120 by default. What it controls is not decoded." },
      (v) => {
        setSequenceParam(seq, v);
        markDirty(true);
      }
    )
  );
}

function valuePercent(v) {
  return (v / STAGE_VALUE_MAX) * 100;
}

function renderGrid() {
  const seq = currentSequence();
  els.grid.innerHTML = "";

  const row = document.createElement("section");
  row.className = "sequence-row";

  const header = document.createElement("div");
  header.className = "sequence-header";
  const label = document.createElement("span");
  label.className = "sequence-label";
  label.textContent = SEQUENCE_LABELS[seqIndex];
  header.appendChild(label);
  const caption = document.createElement("span");
  caption.className = "sequence-caption";
  caption.textContent = `slot ${slotIndex + 1} / sequence ${SEQUENCE_LABELS[seqIndex]} -- ${STAGES_PER_SEQUENCE} stages`;
  header.appendChild(caption);
  row.appendChild(header);

  const stepsEl = document.createElement("div");
  stepsEl.className = "steps";
  seq.stages.forEach((stage, stageIndex) => {
    const cell = document.createElement("div");
    cell.className = "step";

    const bar = document.createElement("div");
    bar.className = "step-bar";
    bar.style.height = `${valuePercent(stage.value)}%`;
    cell.appendChild(bar);

    const idx = document.createElement("div");
    idx.className = "step-index";
    idx.textContent = String(stageIndex + 1);
    cell.appendChild(idx);

    // Stage byte 0: raw 0..255. Very likely the stage CV, but no
    // volts-per-LSB has ever been measured, so the UI shows the byte.
    const valueInput = document.createElement("input");
    valueInput.type = "number";
    valueInput.className = "step-value";
    valueInput.min = "0";
    valueInput.max = String(STAGE_VALUE_MAX);
    valueInput.step = "1";
    valueInput.title = "stage byte 0 (raw 0-255; likely the stage CV, units unconfirmed)";
    valueInput.value = String(stage.value);
    valueInput.addEventListener("input", () => {
      stage.value = clampNum(Math.round(Number(valueInput.value)), 0, STAGE_VALUE_MAX);
      bar.style.height = `${valuePercent(stage.value)}%`;
      markDirty(true);
    });
    cell.appendChild(valueInput);

    // Stage bytes 2-3: little-endian uint16, default 4.
    const timeInput = document.createElement("input");
    timeInput.type = "number";
    timeInput.className = "step-time";
    timeInput.min = "0";
    timeInput.max = String(STAGE_TIME_MAX);
    timeInput.step = "1";
    timeInput.title = "stage bytes 2-3 (LE uint16, default 4; reads like a time/duration, units unconfirmed)";
    timeInput.value = String(stage.time);
    timeInput.addEventListener("input", () => {
      stage.time = clampNum(Math.round(Number(timeInput.value)), 0, STAGE_TIME_MAX);
      markDirty(true);
    });
    cell.appendChild(timeInput);

    stepsEl.appendChild(cell);
  });
  row.appendChild(stepsEl);
  els.grid.appendChild(row);
}

function renderAll() {
  els.slotSelect.value = String(slotIndex);
  renderSeqTabs();
  renderSlotMeta();
  renderGrid();
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
els.slotSelect.addEventListener("change", () => {
  slotIndex = clampNum(Math.round(Number(els.slotSelect.value)), 0, SLOTS_PER_BANK - 1);
  renderSlotMeta();
  renderGrid();
});
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
// ?slot=1..30 opens straight on a slot (the bench notes refer to slots by
// their front-panel number).
const urlSlot = Number(urlParams.get("slot"));
if (Number.isFinite(urlSlot) && urlSlot >= 1 && urlSlot <= SLOTS_PER_BANK) {
  slotIndex = Math.round(urlSlot) - 1;
}

updateActionButtons();
markDirty(false);
renderSlotSelect();
renderAll();
setStatus("idle -- press Connect", "info");
