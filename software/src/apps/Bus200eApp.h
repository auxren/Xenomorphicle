#pragma once
// ---------------------------------------------------------------------------
// 200e Modules -- work on other Buchla 200e modules from this one, over the
// preset bus.
//
// This pass is the shell: pick a target module, then land on a per-module-type
// home screen. The per-type handlers (251e sequence view/edit, generate,
// record-from-MIDI) hook into DrawModuleHome()'s switch; see the SEAM comment.
//
// IDENTIFICATION, and why it looks the way it does: this bus has no
// type-discovery command. A QUERY (0x1A -> 0x1C reply) proves only that a
// module is present and answering -- a real 251e and a real 259e both reply
// with the identical literal 0xFF (bench-confirmed 2026-08-31, and confirmed
// again in the 251e's own firmware where that byte is a compile-time
// constant). Identification is therefore the address->model lookup in
// Buchla200eModuleTable.h, exactly as Studio H's own preset manager does it.
// So the UI says "probably", never "is": clone modules deliberately squat the
// address of whatever they emulate.
//
// SCAN COST: a silent address costs the full QUERY reply timeout (1000ms), so
// walking all 61 known addresses can take ~a minute. The scan is therefore
// progressive and abortable, pumped one query at a time from Loop(), never
// blocking. Manual address entry always works and never depends on it.
//
// THREADING: Controller() runs in the core timer ISR -- it stays empty here.
// All bus work (MasterQuery/MasterBackup and their state polling) happens in
// Loop(), the non-ISR path, mirroring PresetBusUI::Task() and Bus200eBridge.
//
// PROVENANCE: the 251e's own front panel does NOT repaint when its stored
// state is changed over the bus (confirmed three independent ways), so this
// screen is the only honest account of what the module holds. The home
// screen therefore always states where its data came from and whether it
// still matches the module -- NO DATA / LIVE / EDITED* -- rather than showing
// stage values with no provenance.
//
// WRITING REWRITES THE WHOLE BANK. MasterRestore transfers all 30 slots --
// the card image is the unit of transfer -- so Save is guarded by
// Buchla200eCheckWrite() (its own file, its own tests) and an arm-then-confirm
// gesture, mirroring the console's `x` command. The confirm screen states the
// whole-bank consequence and the changed-byte count outright, because "save
// this preset" is what the user thinks they are doing and it is not what
// happens on the wire.
//
// CONTROLS (the panel has A, B, X, Y and both encoder pushes; CONTROL_BUTTON_M
// /"Z" is NOT wired on this hardware -- the grey clock button fires no UI
// control, confirmed by watching events on the bench, so nothing binds to it):
//   encR push = confirm/enter     encL push = back/cancel
//   A = primary action (Save)     B = cycle sequence A-D
//   X / Y = per-screen context
// ---------------------------------------------------------------------------

#include "../Buchla200eModuleTable.h"
#include "../Buchla200eWriteGuard.h"
#include "../Buchla251eGenerator.h"
#include "../Buchla251eNoteMap.h"
#include "../Buchla251eRecorder.h"
#include "../Buchla251eSlotCodec.h"
#include "../Buchla259eSlotCodec.h"
#include "../HSMIDI.h"
#include "../PresetBus.h"

namespace Bus200eAppNS {

enum Screen : uint8_t {
  SCR_MODULE_SELECT = 0,
  SCR_MODULE_HOME,
  SCR_WRITE_CONFIRM,   // armed; nothing has gone on the wire yet
  SCR_EDIT,            // per-stage editor over the selected sequence
  SCR_GEN,             // Euclidean generator parameters
  SCR_REC,             // record stages from incoming MIDI
};

// Generator parameter cursor. Pitches are carried as MIDI note numbers, not
// volts: on this module raw stage value == note number exactly (1.2V/oct,
// note 0 = 0V), and showing volts here would invite the 1V/oct assumption
// that this format does NOT follow. See Buchla251eNoteMap.h.
enum GenParam : uint8_t {
  GEN_LENGTH = 0,
  GEN_FILL,
  GEN_ROTATION,
  GEN_NOTE,      // pitch of an active (pulse) stage
  GEN_REST,      // pitch of an inactive stage
  GEN_COUNT,
};

// bjorklund.h returns a uint32_t mask, so a Euclidean pattern cannot exceed
// 32 steps. The format holds 50, reachable by hand-editing or recording --
// the UI states the cap rather than silently clamping a user who typed 40.
static constexpr int kGenMaxLength = 32;
static constexpr int kGenMinLength = 2;

// Home-screen action row. READ and SAVE are implemented; Edit/Gen/Rec are
// deliberate stubs so the row's shape (and the muscle memory) is settled
// before the phases that fill them in.
enum HomeAction : uint8_t {
  ACT_READ = 0,
  ACT_EDIT,
  ACT_GEN,
  ACT_REC,
  ACT_SAVE,
  ACT_COUNT,
};

// A whole-bank MasterBackup, pumped from Loop().
enum ReadState : uint8_t {
  READ_NONE = 0,   // nothing read this session
  READ_ACTIVE,     // transfer in flight
  READ_OK,         // working_slot_ decoded from a completed read
  READ_FAIL,       // see read_err_
};

// A whole-bank MasterRestore, pumped from Loop(). Mirrors ReadState so the
// provenance line can speak about writes in the same voice it speaks about
// reads -- and so a failed write can never be drawn as a successful one.
enum WriteState : uint8_t {
  WRITE_NONE = 0,
  WRITE_ACTIVE,
  WRITE_OK,
  WRITE_FAIL,
};

// Module types this app has a handler for. Identification is by address (see
// the header comment), so MODTYPE_251E means "the table calls this address a
// 251". Add a type here and a case in DrawModuleHome() to grow the app.
enum ModuleType : uint8_t {
  MODTYPE_UNKNOWN = 0,
  MODTYPE_251E,
  MODTYPE_259E,
};

// Scan pacing. One query in flight at a time; the FSM below is the whole
// scheduler.
enum ScanState : uint8_t {
  SCAN_IDLE = 0,
  SCAN_NEXT,     // pick the next address and fire a query
  SCAN_WAITING,  // a query is in flight
};

static constexpr int kFoundBytes = 8;   // 64 bits >= Buchla200eModuleCount()
static constexpr int kListRows = 3;     // responders visible at once

// The 251e's bank is 30 slots. Displayed 1-30, carried on the wire 0-29 --
// this app keeps the WIRE value in slot_ and adds 1 only when drawing, which
// is the direction that fails safe: an off-by-one shows up as a wrong label
// rather than a read of the wrong preset.
static constexpr int kSlotCount = 30;

// Stage strip geometry. 50 stages at 2px each = 100px, left-aligned.
static constexpr int kStripX = 4;
static constexpr int kStripTop = 31;
static constexpr int kStripH = 12;
static constexpr int kStripBase = kStripTop + kStripH;   // bars grow up from here

// Auto-scale floor for the strip, in raw units (12 = 1.2V = one octave).
// Without it a sequence that only wobbles by 0.1V would be drawn as though
// it swept the full range.
static constexpr int kStripMinFullScale = 12;

// Action labels. Fixed-size char arrays rather than a pointer array, and
// PROGMEM, so they stay in flash -- on Teensy 4 .rodata (pointer array
// included) lands in DTCM by default, which this build cannot spare. Same
// pattern as Buchla200eModuleTable.cpp.
static const char kActionNames[ACT_COUNT][5] PROGMEM = {
  "Read", "Edit", "Gen", "Rec", "Save"
};
static const uint8_t kActionWidths[ACT_COUNT] = {4, 4, 3, 3, 4};  // chars

// --- 259e parameter list ---------------------------------------------------
// How a value is rendered, which is NOT a property of its width: a 12-bit
// field can be a signed attenuverter, an exponential pitch, or a scan-width
// percentage with a floor, and showing any of them as a bare 0-4095 count
// would be actively misleading. See Buchla_FW/docs/259e-PRESET-FORMAT.md.
enum RowKind : uint8_t {
  ROW_PITCH = 0,   // exponential, 512 counts/octave
  ROW_BIPOLAR,     // offset-binary attenuverter, signed percent
  ROW_UNIPOLAR,    // plain percent of range
  ROW_MODFREQ,     // absolute, or a tracking interval when mod_freq_mode == 2
  ROW_DUAL,        // meaning switches on the paired timbre index
  ROW_MORPH,       // linear crossfade, red -> green
  ROW_WARP,        // has a floor: knob spans 20%..60% of scan width
  ROW_ENGINE,      // 4-way modulator sync, one state of which can be inert
  ROW_MODDEST,     // 3-bit destination mask
  ROW_WAVEFORM,
  ROW_FREQMODE,
  ROW_WAVEBTN,
  ROW_TIMBRE,
};

// `idx` is a param index for the 12-bit kinds, and a record byte offset for
// the discrete ones. ROW_DUAL draws its own label (that is the whole point of
// the dual use), so its table label is only a placeholder.
struct Row259e {
  char label[9];
  uint8_t kind;
  uint8_t idx;
};

// Grouped by what the musician is thinking about -- principal voice, then
// shaping, then timbre, then the modulator, then FM -- not by byte order.
static const Row259e kRows259e[] PROGMEM = {
  {"Pitch",    ROW_PITCH,    1},
  {"PitchCV",  ROW_BIPOLAR,  0},
  {"Morph",    ROW_MORPH,    8},
  {"MorphCV",  ROW_BIPOLAR,  6},
  {"Warp",     ROW_WARP,     9},
  {"WarpCV",   ROW_BIPOLAR,  7},
  {"TimbreR",  ROW_TIMBRE,  31},
  {"TimbreG",  ROW_TIMBRE,  32},
  {"WaveBtn",  ROW_WAVEBTN, 30},
  {"ModFreq",  ROW_MODFREQ,  3},
  {"ModFrqCV", ROW_BIPOLAR,  2},
  {"ModWave",  ROW_WAVEFORM,28},
  {"ModRate",  ROW_FREQMODE,29},
  {"Sync",     ROW_ENGINE,  26},
  {"FM Index", ROW_UNIPOLAR,10},
  {"FM IdxCV", ROW_BIPOLAR, 11},
  {"ModDest",  ROW_MODDEST, 27},
  {"dual4",    ROW_DUAL,     4},
  {"dual5",    ROW_DUAL,     5},
};
static constexpr int kRow259eCount =
    (int)(sizeof(kRows259e) / sizeof(kRows259e[0]));

// Three rows fit between the header and the provenance line at y=46.
static constexpr int kRows259eVisible = 3;
static constexpr int kRow259eY0 = 21;
static constexpr int kRow259eDY = 8;
static constexpr int kRow259eValueX = 74;   // char column 12 of 21

}  // namespace Bus200eAppNS

OC_APP_CLASS(AppBus200e, TWOCCS("2E"), "200e Modules", "200e"),
  public HSApplication {
public:
  OC_APP_INTERFACE_DECLARE(AppBus200e, 2);

  void Start() final {}
  void Resume() final {}

  // ISR context (core timer, ~16.6kHz). Does nothing at all unless a
  // recording is armed -- and when it is, the only work is draining the MIDI
  // interfaces into a fixed-size array. No heap, no blocking, no Serial.
  //
  // This is the ONE part of this app that must stay in ITCM: it is ISR-hot,
  // so nothing below it is FLASHMEM.
  void Controller() final {
#if defined(ARDUINO_TEENSY41)
    if (!rec_armed_) return;
    rec_timeout_ = 0;
    RecPollDevice(usbMIDI);          // USB device (a computer)
    RecPollDevice(usbHostMIDI[0]);   // USB host port -- the k-board lives here
    RecPollDevice(usbHostMIDI[1]);
    RecPollDevice(MIDI1);            // DIN on Serial8
    RecPollBusMidi();                // 200e bus MIDI
#endif
  }

  void View() const final { DrawMenu(); }

private:
  uint8_t screen_ = Bus200eAppNS::SCR_MODULE_SELECT;
  uint8_t addr_ = 0x5C;        // known-good 251e; manual entry always works
  uint8_t target_ = 0x5C;      // the address the home screen is working on

  // scan state
  uint8_t scan_state_ = Bus200eAppNS::SCAN_IDLE;
  int scan_index_ = 0;         // index into the module table
  int found_count_ = 0;
  int list_top_ = 0;           // scroll offset into the responder list
  uint8_t found_[Bus200eAppNS::kFoundBytes] = {0};

  // single-address probe (works for off-table addresses too, unlike the scan)
  uint8_t probe_addr_ = 0;
  int8_t probe_result_ = -1;   // -1 none, 0 silent, 1 answered
  bool probe_active_ = false;

  // --- 251e home screen ---
  uint8_t slot_ = 0;           // WIRE index 0-29; displayed as slot_+1
  uint8_t seq_ = 0;            // 0-3 = A-D
  uint8_t action_ = Bus200eAppNS::ACT_READ;
  uint8_t read_state_ = Bus200eAppNS::READ_NONE;
  Bus200eMasterError read_err_ = BUS200E_MASTER_ERR_NONE;
  uint32_t read_ms_ = 0;       // millis() when the bank read completed
  bool edited_ = false;        // working_slot_ diverges from the module

  // Which module the resident card image actually came from. Without these a
  // read of 0x5C followed by a retarget to 0x28 would look writable, and
  // would push a 251e bank at a 259e.
  uint8_t read_addr_ = 0;
  uint8_t read_type_ = Bus200eAppNS::MODTYPE_UNKNOWN;

  // write path
  uint8_t write_state_ = Bus200eAppNS::WRITE_NONE;
  Bus200eMasterError write_err_ = BUS200E_MASTER_ERR_NONE;
  int pending_changes_ = 0;    // diff size shown on the confirm screen
  Buchla200eWriteBlock write_block_ = BUCHLA200E_WRITE_OK;

  // The decoded slot under the cursor. 2104 bytes, and deliberately a MEMBER:
  // app instances live in RAM2 (see the DMAMEM app_container in _config.h),
  // where 2KB is free, whereas a file-scope static would land in DTCM, which
  // is critically tight -- the debug build already cannot boot for want of
  // stack.
  Buchla251eSlot working_slot_;

  // --- 259e home screen ---
  // 33 bytes, so unlike the 251e slot this one is cheap wherever it lives --
  // kept a member for the same reason regardless (RAM2, not DTCM).
  Buchla259eSlot working_259e_;
  uint8_t row_cursor_ = 0;     // index into kRows259e
  uint8_t row_top_ = 0;        // scroll offset

  // --- edit screen ---
  uint8_t edit_stage_ = 0;     // 0-49, cursor into the selected sequence

  // --- generator screen ---
  uint8_t gen_cursor_ = Bus200eAppNS::GEN_LENGTH;
  uint8_t gen_len_ = 16;
  uint8_t gen_fill_ = 5;
  uint8_t gen_rot_ = 0;
  uint8_t gen_note_ = 60;      // MIDI note for an active stage
  uint8_t gen_rest_ = 0;       // MIDI note for a rest stage

  // --- record screen ---
  // recorder_ points into working_slot_ once armed, so it must never outlive
  // an arm; Init(), disarm and suspend all clear rec_armed_.
  Buchla251eRecorder recorder_;
  volatile bool rec_armed_ = false;      // written by UI, read by the ISR
  volatile uint8_t rec_count_ = 0;       // mirrored out of the ISR for drawing
  volatile uint8_t rec_last_note_ = 0;
  volatile bool rec_any_ = false;        // anything at all arrived yet
#if defined(ARDUINO_TEENSY41)
  elapsedMicros rec_timeout_;            // bounds the ISR drain, as Quadrants does
#endif

  // ISR-hot, deliberately in-class (so NOT FLASHMEM) and deliberately
  // template-per-device, matching Quadrants::ProcessMIDI. Only note-ons are
  // of interest; everything else is left for the apps that own it.
#if defined(ARDUINO_TEENSY41)
  template <typename T1>
  void RecPollDevice(T1 &device) {
    while (rec_timeout_ < 60 && device.read()) {
      if (device.getType() == HEM_MIDI_NOTE_ON)
        RecNote(device.getData1(), device.getData2());
    }
  }

  void RecPollBusMidi() {
#ifdef PRESET_BUS
    uint8_t status, d1, d2;
    while (rec_timeout_ < 60 && OC::PresetBus::ReadMidiRx(status, d1, d2)) {
      // Realtime bytes are whole status bytes; channel messages carry the
      // 200e bus mask in the low nibble, so mask it off before comparing.
      if (status < 0xF8 && (status & 0xF0) == HEM_MIDI_NOTE_ON)
        RecNote(d1 & 0x7F, d2 & 0x7F);
    }
#endif
  }

  // The one place a note becomes a stage. Velocity-0 is a note-off by MIDI
  // convention; the recorder already ignores it, so this does not re-handle
  // it -- it only mirrors state out for the display.
  void RecNote(uint8_t note, uint8_t vel) {
    if (recorder_.NoteOn(note, vel)) {
      rec_count_ = recorder_.count();
      rec_last_note_ = note;
      rec_any_ = true;
    } else if (vel > 0) {
      // A real note that did not fit: the sequence is full. Surfaced on
      // screen rather than dropped silently.
      rec_any_ = true;
    }
  }
#endif  // ARDUINO_TEENSY41

  static bool IsFound(const uint8_t *bits, int i) {
    return (bits[i >> 3] >> (i & 7)) & 1;
  }
  static void SetFound(uint8_t *bits, int i) {
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
  }

  void StartScan();
  void StopScan();
  void PumpScan();
  void PumpProbe();
  void StartProbe();
  void StartRead();
  void PumpRead();
  bool DecodeSlotFromCardImage();
  uint32_t ExpectedBankBytes() const;
  int ComputeSlotDiff();       // changed bytes for slot_, or <0 on overflow
  Buchla200eWriteBlock CheckWrite(int changed) const;
  void ArmWrite();
  void CommitWrite();
  void PumpWrite();
  void DrawWriteConfirm() const;
  Bus200eAppNS::ModuleType CurrentModuleType() const;
  void DrawModuleSelect() const;
  void DrawModuleHome() const;
  void DrawModule251e() const;
  void DrawModule259e() const;
  void DrawRow259e(int row, int y) const;
  void DrawTenths(int tenths, const char *suffix) const;
  void ScrollRows(int delta);
  void DrawStageStrip() const;
  void DrawStageStripCursor() const;
  void DrawReadState() const;
  int SeqEndStage() const;     // 0-indexed stage holding the end marker, or -1
  uint8_t SeqPeakRaw() const;
  int FoundIndexToTableIndex(int nth) const;

  // edit / gen / rec
  void DrawEdit() const;
  void DrawGen() const;
  void DrawRec() const;
  void EditNudge(int delta);
  void EditToggleEnd();
  void GenAdjust(int delta);
  void GenApply();
  void RecArm();
  void RecStop();
  bool HaveSequence() const;   // a decoded 251e slot is on screen
};

// ---------------------------------------------------------------------------
// Every definition below is out-of-class and explicitly FLASHMEM+noinline.
// LTO silently drops FLASHMEM from in-class (implicitly-inline) definitions,
// which is how this repo has repeatedly tipped a 32KB ITCM bank boundary.
// ---------------------------------------------------------------------------

FLASHMEM __attribute__((noinline))
void AppBus200e::StartScan() {
  for (int i = 0; i < Bus200eAppNS::kFoundBytes; ++i) found_[i] = 0;
  found_count_ = 0;
  list_top_ = 0;
  scan_index_ = 0;
  scan_state_ = Bus200eAppNS::SCAN_NEXT;
#ifdef PRESET_BUS
  // Silence the per-address console line. It is not free: Teensy's
  // usb_serial_write spins in `while (!tx_available)` for up to 120ms when
  // the USB TX buffer is full and the host is not draining, and a scan emits
  // ~59 of those lines. That is what starved loop() to a 57ms poll gap on the
  // bench -- the display stopped updating and the abort press went unseen.
  // The results are on screen anyway; the console 'q' command is unaffected.
  OC::PresetBus::MasterQuerySetQuiet(true);
#endif
}

FLASHMEM __attribute__((noinline))
void AppBus200e::StopScan() {
  // Leave found_/found_count_ intact: an aborted scan keeps what it found.
  scan_state_ = Bus200eAppNS::SCAN_IDLE;
#ifdef PRESET_BUS
  OC::PresetBus::MasterQueryReset();
  OC::PresetBus::MasterQuerySetQuiet(false);
#endif
}

FLASHMEM __attribute__((noinline))
void AppBus200e::StartProbe() {
#ifdef PRESET_BUS
  if (scan_state_ != Bus200eAppNS::SCAN_IDLE) return;  // scan owns the FSM
  if (!OC::PresetBus::Enabled()) return;
  if (addr_ == 0 || addr_ == OC::PresetBus::ModuleAddress()) return;
  OC::PresetBus::MasterQueryReset();
  if (OC::PresetBus::MasterQuery(addr_) == 0) {
    probe_addr_ = addr_;
    probe_result_ = -1;
    probe_active_ = true;
  }
#endif
}

FLASHMEM __attribute__((noinline))
void AppBus200e::PumpProbe() {
#ifdef PRESET_BUS
  if (!probe_active_) return;
  const Bus200eQueryState st = OC::PresetBus::MasterQueryState();
  if (st == BUS200E_QUERY_DONE) {
    probe_result_ = 1;
  } else if (st == BUS200E_QUERY_FAILED) {
    probe_result_ = 0;
  } else {
    return;  // still in flight
  }
  OC::PresetBus::MasterQueryReset();
  probe_active_ = false;
#endif
}

// One query in flight at a time, advanced across Loop() calls. Never blocks.
FLASHMEM __attribute__((noinline))
void AppBus200e::PumpScan() {
#ifdef PRESET_BUS
  if (scan_state_ == Bus200eAppNS::SCAN_IDLE) return;

  const int count = Buchla200eModuleCount();

  if (scan_state_ == Bus200eAppNS::SCAN_WAITING) {
    const Bus200eQueryState st = OC::PresetBus::MasterQueryState();
    if (st == BUS200E_QUERY_DONE) {
      SetFound(found_, scan_index_);
      ++found_count_;
    } else if (st != BUS200E_QUERY_FAILED) {
      return;  // SENDING or WAITING -- come back next Loop()
    }
    OC::PresetBus::MasterQueryReset();
    ++scan_index_;
    scan_state_ = Bus200eAppNS::SCAN_NEXT;
  }

  if (scan_state_ != Bus200eAppNS::SCAN_NEXT) return;

  // Skip our own address -- querying ourselves proves nothing and the reply
  // path is our own slave.
  const uint8_t self = OC::PresetBus::ModuleAddress();
  while (scan_index_ < count) {
    const Buchla200eModuleEntry *e = Buchla200eModuleAt(scan_index_);
    if (e && e->addr != self && e->addr != 0) break;
    ++scan_index_;
  }
  if (scan_index_ >= count) {
    scan_state_ = Bus200eAppNS::SCAN_IDLE;
    OC::PresetBus::MasterQuerySetQuiet(false);
    return;
  }

  const Buchla200eModuleEntry *e = Buchla200eModuleAt(scan_index_);
  if (!e) {
    scan_state_ = Bus200eAppNS::SCAN_IDLE;
    OC::PresetBus::MasterQuerySetQuiet(false);
    return;
  }
  if (OC::PresetBus::MasterQuery(e->addr) == 0) {
    scan_state_ = Bus200eAppNS::SCAN_WAITING;
  } else {
    // Refused (bus busy, or a job already running) -- skip rather than spin.
    ++scan_index_;
  }
#endif
}

// --- reading a bank -------------------------------------------------------
// MasterBackup pulls the WHOLE 63120-byte bank, not one slot, so a single
// read serves all 30 slots: changing slot_ afterwards re-decodes out of the
// resident card image with no further bus traffic. That is why
// DecodeSlotFromCardImage() is separate from PumpRead().

FLASHMEM __attribute__((noinline))
void AppBus200e::StartRead() {
#ifdef PRESET_BUS
  if (read_state_ == Bus200eAppNS::READ_ACTIVE) return;
  if (scan_state_ != Bus200eAppNS::SCAN_IDLE || probe_active_) return;
  if (!OC::PresetBus::Enabled()) return;

  // Clear any DONE/FAILED left over from a previous job, otherwise the
  // master FSM refuses the new one as busy.
  OC::PresetBus::MasterReset();
  const int rc = OC::PresetBus::MasterBackup(target_);
  if (rc == 0) {
    read_state_ = Bus200eAppNS::READ_ACTIVE;
    read_err_ = BUS200E_MASTER_ERR_NONE;
  } else {
    read_state_ = Bus200eAppNS::READ_FAIL;
    read_err_ = (Bus200eMasterError)(-rc);
  }
#endif
}

// Identification is the address->model table, not a protocol answer (see the
// header comment). Matching on the model string keeps every instance of a
// model working: the table carries 259 A..D at 0x28-0x2B and 251 A at 0x5C.
FLASHMEM __attribute__((noinline))
Bus200eAppNS::ModuleType AppBus200e::CurrentModuleType() const {
  const char *model = Buchla200eModelForAddress(target_);
  if (!model) return Bus200eAppNS::MODTYPE_UNKNOWN;
  if (model[0] != '2' || model[1] != '5') return Bus200eAppNS::MODTYPE_UNKNOWN;
  if (model[2] == '1') return Bus200eAppNS::MODTYPE_251E;
  if (model[2] == '9') return Bus200eAppNS::MODTYPE_259E;
  return Bus200eAppNS::MODTYPE_UNKNOWN;
}

// Decode slot_ out of the resident card image. False if the image is gone
// (CardServing() dropped) or the transfer was too short to contain this
// slot -- in either case the caller must NOT present stale bytes as live.
//
// The record size is per module type and they are wildly different (2104 vs
// 33). Getting this wrong would not fail loudly -- it would decode the wrong
// bytes and draw them as parameters -- so the size comes from the same type
// decision the drawing does, never from a default.
FLASHMEM __attribute__((noinline))
bool AppBus200e::DecodeSlotFromCardImage() {
#ifdef PRESET_BUS
  const uint8_t *img = OC::PresetBus::MasterCardImage();
  if (!img) return false;

  const Bus200eAppNS::ModuleType type = CurrentModuleType();
  const uint32_t rec = (type == Bus200eAppNS::MODTYPE_259E)
                           ? (uint32_t)kBuchla259eRecordBytes
                           : (uint32_t)kBuchla251eSlotBytes;
  const uint32_t off = (uint32_t)slot_ * rec;
  // A short transfer would otherwise decode whatever else is in the 64K
  // buffer and draw it as this module's data. One real capture during this
  // module's bring-up came back exactly one record short while still
  // reporting success, so this check is not theoretical.
  if (Bus200eMasterBytesTransferred() < off + rec) return false;

  if (type == Bus200eAppNS::MODTYPE_259E)
    Buchla259eDecodeSlot(img + off, working_259e_);
  else
    Buchla251eDecodeSlot(img + off, working_slot_);
  return true;
#else
  return false;
#endif
}

FLASHMEM __attribute__((noinline))
void AppBus200e::PumpRead() {
#ifdef PRESET_BUS
  if (read_state_ != Bus200eAppNS::READ_ACTIVE) return;

  const Bus200eMasterState st = OC::PresetBus::MasterState();
  if (st == BUS200E_MASTER_DONE) {
    if (DecodeSlotFromCardImage()) {
      read_state_ = Bus200eAppNS::READ_OK;
      read_ms_ = millis();
      edited_ = false;
      // Stamp WHOSE bank is now resident. The write guard refuses unless this
      // still matches the target at Save time.
      read_addr_ = target_;
      read_type_ = (uint8_t)CurrentModuleType();
    } else {
      // Transfer reported success but the bytes aren't usable. Say so --
      // a failed read must never be shown as a good one.
      read_state_ = Bus200eAppNS::READ_FAIL;
      read_err_ = BUS200E_MASTER_ERR_NO_RESPONSE;
    }
    OC::PresetBus::MasterReset();
  } else if (st == BUS200E_MASTER_FAILED) {
    read_state_ = Bus200eAppNS::READ_FAIL;
    read_err_ = OC::PresetBus::MasterError();
    OC::PresetBus::MasterReset();
  }
#endif
}

// --- writing a bank -------------------------------------------------------
// MasterRestore sends the WHOLE bank. Everything below exists to make that
// safe: size it, diff it, check it, arm it, then send it -- and verify after.

FLASHMEM __attribute__((noinline))
uint32_t AppBus200e::ExpectedBankBytes() const {
  switch (CurrentModuleType()) {
    case Bus200eAppNS::MODTYPE_251E:
      return (uint32_t)Bus200eAppNS::kSlotCount * kBuchla251eSlotBytes;
    case Bus200eAppNS::MODTYPE_259E:
      return (uint32_t)Bus200eAppNS::kSlotCount * kBuchla259eRecordBytes;
    default:
      return 0;   // unknown type: the guard treats 0 as "cannot size" = refuse
  }
}

// Changed bytes between the resident image's copy of this slot and the
// working copy. <0 means the diff overflowed its buffer, which the guard
// treats as unverifiable rather than as zero.
FLASHMEM __attribute__((noinline))
int AppBus200e::ComputeSlotDiff() {
#ifdef PRESET_BUS
  if (CurrentModuleType() != Bus200eAppNS::MODTYPE_251E) return 0;
  const uint8_t *img = OC::PresetBus::MasterCardImage();
  if (!img) return 0;
  const uint32_t off = (uint32_t)slot_ * kBuchla251eSlotBytes;
  if (Bus200eMasterBytesTransferred() < off + kBuchla251eSlotBytes) return 0;
  // Only the count is wanted here; the patch buffer is a scratch the caller
  // never reads, so a small one is fine -- overflow reports -1, which the
  // guard blocks on.
  Buchla251eBytePatch patches[32];
  return Buchla251eDiffSlot(img + off, working_slot_, patches, 32);
#else
  return 0;
#endif
}

FLASHMEM __attribute__((noinline))
Buchla200eWriteBlock AppBus200e::CheckWrite(int changed) const {
  Buchla200eWriteContext c;
  c.have_read = (read_state_ == Bus200eAppNS::READ_OK);
  c.read_addr = read_addr_;
  c.read_type = read_type_;
  c.target_addr = target_;
  c.target_type = (uint8_t)CurrentModuleType();
  c.expected_bank_bytes = ExpectedBankBytes();
  c.changed_bytes = changed;
#ifdef PRESET_BUS
  c.bytes_transferred = Bus200eMasterBytesTransferred();
  c.card_serving = OC::PresetBus::CardServing();
  c.image_valid = (OC::PresetBus::MasterCardImage() != nullptr);
  c.master_idle = (scan_state_ == Bus200eAppNS::SCAN_IDLE) && !probe_active_ &&
                  (read_state_ != Bus200eAppNS::READ_ACTIVE) &&
                  (write_state_ != Bus200eAppNS::WRITE_ACTIVE);
#else
  c.bytes_transferred = 0;
  c.card_serving = false;
  c.image_valid = false;
  c.master_idle = false;
#endif
  return Buchla200eCheckWrite(c);
}

// Step 1 of 2. Computes the diff and runs the guard, but touches nothing on
// the bus -- if this refuses, the reason is shown and no wire activity has
// happened.
FLASHMEM __attribute__((noinline))
void AppBus200e::ArmWrite() {
  pending_changes_ = ComputeSlotDiff();
  write_block_ = CheckWrite(pending_changes_);
  screen_ = Bus200eAppNS::SCR_WRITE_CONFIRM;
}

// Step 2 of 2. Re-runs the guard rather than trusting the arm-time verdict:
// the card image can be dropped, or a transfer started, in between.
FLASHMEM __attribute__((noinline))
void AppBus200e::CommitWrite() {
#ifdef PRESET_BUS
  const int changed = ComputeSlotDiff();
  write_block_ = CheckWrite(changed);
  if (write_block_ != BUCHLA200E_WRITE_OK) {
    pending_changes_ = changed;
    return;   // stay on the confirm screen showing why
  }

  uint8_t *img = OC::PresetBus::MasterCardImage();
  if (!img) { write_block_ = BUCHLA200E_WRITE_NO_IMAGE; return; }

  // Encode the edited slot into the resident image. Only this slot's bytes
  // move; the other 29 stay exactly as they were read, which is what makes a
  // whole-bank transfer non-destructive to them.
  Buchla251eEncodeSlot(working_slot_, img + (uint32_t)slot_ * kBuchla251eSlotBytes);

  OC::PresetBus::MasterReset();
  const int rc = OC::PresetBus::MasterRestore(target_);
  if (rc == 0) {
    write_state_ = Bus200eAppNS::WRITE_ACTIVE;
    write_err_ = BUS200E_MASTER_ERR_NONE;
  } else {
    write_state_ = Bus200eAppNS::WRITE_FAIL;
    write_err_ = (Bus200eMasterError)(-rc);
  }
  screen_ = Bus200eAppNS::SCR_MODULE_HOME;
#endif
}

FLASHMEM __attribute__((noinline))
void AppBus200e::PumpWrite() {
#ifdef PRESET_BUS
  if (write_state_ != Bus200eAppNS::WRITE_ACTIVE) return;

  const Bus200eMasterState st = OC::PresetBus::MasterState();
  if (st == BUS200E_MASTER_DONE) {
    write_state_ = Bus200eAppNS::WRITE_OK;
    // The module now holds what working_slot_ holds, so this is no longer a
    // divergence. Verification is a deliberate follow-up Read, not an
    // automatic one -- see the comment on the confirm screen.
    edited_ = false;
    OC::PresetBus::MasterReset();
  } else if (st == BUS200E_MASTER_FAILED) {
    write_state_ = Bus200eAppNS::WRITE_FAIL;
    write_err_ = OC::PresetBus::MasterError();
    OC::PresetBus::MasterReset();
  }
#endif
}

// --- sequence facts -------------------------------------------------------

// The loop point: first stage carrying the confirmed "end: Always" marker.
FLASHMEM __attribute__((noinline))
int AppBus200e::SeqEndStage() const {
  const Buchla251eSequence &s = working_slot_.sequences[seq_];
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
    if (Buchla251eHasEndMarker(s.stages[i])) return i;
  return -1;
}

FLASHMEM __attribute__((noinline))
uint8_t AppBus200e::SeqPeakRaw() const {
  const Buchla251eSequence &s = working_slot_.sequences[seq_];
  uint8_t peak = 0;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
    if (s.stages[i].value > peak) peak = s.stages[i].value;
  return peak;
}

// Edit/Gen/Rec all mutate working_slot_, which only means anything once a
// bank has actually been read. Without this they would happily let the user
// sculpt a buffer full of nothing and then offer to write it.
FLASHMEM __attribute__((noinline))
bool AppBus200e::HaveSequence() const {
  return read_state_ == Bus200eAppNS::READ_OK &&
         CurrentModuleType() == Bus200eAppNS::MODTYPE_251E;
}

// --- edit ------------------------------------------------------------------

FLASHMEM __attribute__((noinline))
void AppBus200e::EditNudge(int delta) {
  if (!HaveSequence()) return;
  Buchla251eStage &st = working_slot_.sequences[seq_].stages[edit_stage_];
  int v = (int)st.value + delta;
  CONSTRAIN(v, 0, 255);
  if ((uint8_t)v == st.value) return;
  st.value = (uint8_t)v;
  edited_ = true;
}

// The loop point. Given its own button rather than a chord because it is the
// single most consequential edit on this screen: it decides where the
// sequence restarts, and a sequence with no marker runs all 50 stages.
FLASHMEM __attribute__((noinline))
void AppBus200e::EditToggleEnd() {
  if (!HaveSequence()) return;
  Buchla251eSequence &s = working_slot_.sequences[seq_];
  const bool had = Buchla251eHasEndMarker(s.stages[edit_stage_]);
  if (had) {
    Buchla251eSetEndMarker(s.stages[edit_stage_], false);
  } else {
    // Exactly one loop point: clear all 50 first, matching the same
    // clear-all-then-set-one discipline the generator and recorder use.
    for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
      Buchla251eSetEndMarker(s.stages[i], false);
    Buchla251eSetEndMarker(s.stages[edit_stage_], true);
  }
  edited_ = true;
}

// --- generator -------------------------------------------------------------

FLASHMEM __attribute__((noinline))
void AppBus200e::GenAdjust(int delta) {
  using namespace Bus200eAppNS;
  int v;
  switch (gen_cursor_) {
    case GEN_LENGTH:
      v = (int)gen_len_ + delta;
      CONSTRAIN(v, kGenMinLength, kGenMaxLength);
      gen_len_ = (uint8_t)v;
      break;
    case GEN_FILL:
      v = (int)gen_fill_ + delta;
      CONSTRAIN(v, 0, (int)gen_len_);
      gen_fill_ = (uint8_t)v;
      break;
    case GEN_ROTATION:
      v = (int)gen_rot_ + delta;
      CONSTRAIN(v, 0, (int)gen_len_ - 1);
      gen_rot_ = (uint8_t)v;
      break;
    case GEN_NOTE:
      v = (int)gen_note_ + delta;
      CONSTRAIN(v, 0, 127);
      gen_note_ = (uint8_t)v;
      break;
    case GEN_REST:
      v = (int)gen_rest_ + delta;
      CONSTRAIN(v, 0, 127);
      gen_rest_ = (uint8_t)v;
      break;
    default: break;
  }
  // Enforce the generator's own invariants here, while the user is looking
  // at the numbers -- shrinking length has to drag fill/rotation down with
  // it, and letting the generator do that silently at apply time would show
  // the user a fill it never used.
  Buchla251eClampEuclidParams(gen_len_, gen_fill_, gen_rot_);
}

// Explicit, not live-on-every-turn: applying overwrites stages, and the user
// should pick the moment. Returns to the home screen so the strip shows what
// was produced rather than leaving them on a parameter screen guessing.
FLASHMEM __attribute__((noinline))
void AppBus200e::GenApply() {
  if (!HaveSequence()) return;
  Buchla251eEuclidParams p;
  p.length = gen_len_;
  p.fill = gen_fill_;
  p.rotation = gen_rot_;
  // The generator takes volts; raw == note number and raw == volts*10, so
  // note/10 is the volts that round-trips back to exactly this note.
  p.base_volts = (float)Buchla251eNoteToRaw(gen_note_) / 10.0f;
  p.rest_volts = (float)Buchla251eNoteToRaw(gen_rest_) / 10.0f;
  Buchla251eGenerateEuclid(p, working_slot_.sequences[seq_]);
  edited_ = true;
  edit_stage_ = 0;
  screen_ = Bus200eAppNS::SCR_MODULE_HOME;
}

// --- recorder --------------------------------------------------------------

// Arming is cheap and freely repeatable: it touches no bus and carries none
// of the write path's confirm ceremony, because it only mutates the local
// working buffer. The expensive gesture belongs on Save, not here.
FLASHMEM __attribute__((noinline))
void AppBus200e::RecArm() {
  if (!HaveSequence()) return;
  recorder_.Reset(working_slot_.sequences[seq_]);
  rec_count_ = 0;
  rec_last_note_ = 0;
  rec_any_ = false;
#if defined(ARDUINO_TEENSY41)
  rec_timeout_ = 0;
#endif
  rec_armed_ = true;   // last: the ISR starts polling the instant this is set
}

FLASHMEM __attribute__((noinline))
void AppBus200e::RecStop() {
  if (!rec_armed_) return;
  rec_armed_ = false;  // first: stop the ISR before touching what it writes
  // Stop() places the end marker at the last recorded stage, and is a
  // complete no-op when nothing was recorded -- so an arm-then-stop with no
  // notes is a safe cancel, not a silent wipe of the existing markers.
  recorder_.Stop();
  if (rec_count_ > 0) edited_ = true;
}

// Move the 259e list cursor, dragging the 3-row window along with it.
FLASHMEM __attribute__((noinline))
void AppBus200e::ScrollRows(int delta) {
  using namespace Bus200eAppNS;
  int c = (int)row_cursor_ + delta;
  CONSTRAIN(c, 0, kRow259eCount - 1);
  row_cursor_ = (uint8_t)c;

  int top = (int)row_top_;
  if (c < top) top = c;
  if (c >= top + kRows259eVisible) top = c - kRows259eVisible + 1;
  CONSTRAIN(top, 0, kRow259eCount - kRows259eVisible);
  row_top_ = (uint8_t)top;
}

// nth responder -> its index in the module table, or -1.
FLASHMEM __attribute__((noinline))
int AppBus200e::FoundIndexToTableIndex(int nth) const {
  const int count = Buchla200eModuleCount();
  int seen = 0;
  for (int i = 0; i < count; ++i) {
    if (!IsFound(found_, i)) continue;
    if (seen == nth) return i;
    ++seen;
  }
  return -1;
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawModuleSelect() const {
  const char *model = Buchla200eModelForAddress(addr_);

  graphics.setPrintPos(0, 15);
  graphics.printf("Addr %02X", addr_);
  graphics.invertRect(28, 14, 14, 10);   // the encoder target has focus
  graphics.setPrintPos(48, 15);
  // "probably": address is a convention, not a proof -- clones squat it.
  if (model) graphics.printf("~%s", model);
  else       graphics.print("~unknown");

  graphics.setPrintPos(0, 26);
  if (scan_state_ != Bus200eAppNS::SCAN_IDLE) {
    graphics.printf("Scan %d/%d encL:stop", scan_index_,
                    Buchla200eModuleCount());
  } else if (probe_active_) {
    graphics.printf("Probe %02X ...", probe_addr_);
  } else if (probe_result_ >= 0) {
    graphics.printf("Probe %02X %s", probe_addr_,
                    probe_result_ ? "answered" : "silent");
  } else {
    graphics.printf("encL:scan B:probe %d", found_count_);
  }

  // responder list
  if (found_count_ == 0) {
    graphics.setPrintPos(0, 40);
    graphics.print(scan_state_ != Bus200eAppNS::SCAN_IDLE ? "..."
                                                          : "no responders yet");
    return;
  }
  for (int row = 0; row < Bus200eAppNS::kListRows; ++row) {
    const int nth = list_top_ + row;
    if (nth >= found_count_) break;
    const int ti = FoundIndexToTableIndex(nth);
    if (ti < 0) break;
    const Buchla200eModuleEntry *e = Buchla200eModuleAt(ti);
    if (!e) break;
    const int y = 36 + row * 9;
    graphics.setPrintPos(4, y);
    graphics.printf("%02X ~%s", e->addr, e->name);
    if (e->addr == addr_) graphics.drawRect(0, y + 2, 3, 3);
  }
}

// 50 stages across 100px. Bar height is the stage's raw value (volts*10)
// auto-scaled to this sequence's own peak, floored at kStripMinFullScale so
// a nearly-flat sequence isn't drawn as a full-range sweep and an all-zero
// one draws no bars at all. Absolute pitch is carried by the numeric peak on
// the line above; the strip is a SHAPE preview -- what you are about to push.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawStageStrip() const {
  using namespace Bus200eAppNS;
  const Buchla251eSequence &s = working_slot_.sequences[seq_];

  // Baseline always drawn: an all-zero sequence must read as "flat", which
  // is a fact about the data, not as "empty", which would look like no data.
  graphics.drawHLine(kStripX, kStripBase, kBuchla251eStagesPerSequence * 2);

  const uint8_t peak = SeqPeakRaw();
  const int full = (peak < kStripMinFullScale) ? kStripMinFullScale : (int)peak;

  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    const int x = kStripX + i * 2;
    const int bh = ((int)s.stages[i].value * kStripH) / full;
    if (bh > 0) graphics.drawVLine(x, kStripBase - bh, bh);
    // The loop marker crosses BELOW the baseline, which a data bar never
    // does, so it cannot be misread as a very tall stage.
    if (Buchla251eHasEndMarker(s.stages[i]))
      graphics.drawVLine(x, kStripTop - 2, kStripH + 5);
  }
}

// Editor cursor: a caret UNDER the strip's baseline. Deliberately not an
// inverted column -- the end marker already owns the below-baseline region's
// full height, and a solid bar there would be indistinguishable from it.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawStageStripCursor() const {
  using namespace Bus200eAppNS;
  const int x = kStripX + (int)edit_stage_ * 2;
  graphics.drawHLine(x - 1, kStripBase + 3, 3);
}

// Provenance line. This is the whole reason the screen is trustworthy: the
// target module's own panel will happily keep showing something else.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawReadState() const {
  graphics.setPrintPos(0, 46);

  // A write in flight or just finished outranks the read line: it is the more
  // recent, and more consequential, thing that happened to the module.
  switch (write_state_) {
    case Bus200eAppNS::WRITE_ACTIVE:
      graphics.printf("WRITING %02X ...", target_);
      graphics.invertRect(0, 45, 128, 10);
      return;
    case Bus200eAppNS::WRITE_OK:
      // Not "verified" -- only that the transfer completed. Re-Read to check.
      graphics.print("WROTE ok - Read to chk");
      return;
    case Bus200eAppNS::WRITE_FAIL:
      graphics.printf("WRITE FAILED (err %d)", (int)write_err_);
      graphics.invertRect(0, 45, 128, 10);
      return;
    default: break;
  }

  switch (read_state_) {
    case Bus200eAppNS::READ_ACTIVE:
      graphics.printf("reading %02X ...", target_);
      break;
    case Bus200eAppNS::READ_OK:
      if (edited_) {
        // Loud on purpose: these bytes are NOT what the module holds.
        graphics.printf("EDITED*      wire %d", slot_);
        graphics.invertRect(0, 45, 46, 10);
      } else {
        graphics.printf("LIVE %lus ago  wire %d",
                        (unsigned long)((millis() - read_ms_) / 1000u), slot_);
      }
      break;
    case Bus200eAppNS::READ_FAIL:
      switch (read_err_) {
        case BUS200E_MASTER_ERR_NO_FREE_CARD:
          graphics.print("READ FAIL: no card"); break;
        case BUS200E_MASTER_ERR_SEND_TIMEOUT:
          graphics.print("READ FAIL: bus busy"); break;
        case BUS200E_MASTER_ERR_NO_RESPONSE:
          graphics.print("READ FAIL: no answer"); break;
        default:
          graphics.printf("READ FAIL (err %d)", (int)read_err_); break;
      }
      graphics.invertRect(0, 45, 128, 10);
      break;
    case Bus200eAppNS::READ_NONE:
    default:
      graphics.print("NO DATA - Read first");
      break;
  }
}

// The confirm screen. Nothing has touched the bus yet when this is drawn.
//
// It says "rewrites all 30" out loud because the user's mental model is
// "save this preset", and that is not what goes on the wire -- the card image
// is the unit of transfer. Being vague here would be the single most
// expensive omission in the app.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawWriteConfirm() const {
  graphics.setPrintPos(0, 13);
  graphics.printf("WRITE to %02X slot %d", target_, slot_ + 1);
  graphics.invertRect(0, 12, 128, 10);

  if (write_block_ != BUCHLA200E_WRITE_OK) {
    graphics.setPrintPos(0, 26);
    graphics.print("BLOCKED:");
    graphics.setPrintPos(0, 36);
    graphics.print(Buchla200eWriteBlockText(write_block_));
    graphics.setPrintPos(0, 56);
    graphics.print("encL:back");
    return;
  }

  graphics.setPrintPos(0, 26);
  graphics.printf("%d byte%s change", pending_changes_,
                  pending_changes_ == 1 ? "" : "s");
  graphics.setPrintPos(0, 36);
  graphics.print("Rewrites ALL 30 slots");
  graphics.setPrintPos(0, 46);
  graphics.print("from what was read.");

  graphics.setPrintPos(0, 56);
  graphics.print("encR:CONFIRM  encL:no");
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawModuleHome() const {
  using namespace Bus200eAppNS;
  const char *model = Buchla200eModelForAddress(target_);

  // Line 1: what we are talking to, and which preset. "~" because the
  // address only implies the model by convention -- clones squat addresses.
  graphics.setPrintPos(0, 13);
  graphics.printf("~%s @%02X", model ? model : "?", target_);
  graphics.setPrintPos(80, 13);
  graphics.printf("Slot %d", slot_ + 1);          // 1-indexed for humans

  // --- SEAM: per-module-type handlers hang off this switch ----------------
  // Identification is by address (see header), so this is a table lookup,
  // not a protocol answer. Adding a module type = add a ModuleType enum
  // value, a case here, and its own Draw function.
  switch (CurrentModuleType()) {
    case MODTYPE_251E: DrawModule251e(); return;
    case MODTYPE_259E: DrawModule259e(); return;
    default: break;
  }

  graphics.setPrintPos(0, 30);
  graphics.print("no handler for this");
  graphics.setPrintPos(0, 40);
  graphics.print("module type yet");
  graphics.setPrintPos(0, 56);
  graphics.print("encL:back");
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawModule251e() const {
  using namespace Bus200eAppNS;

  // Line 2: sequence, loop point, peak. Peak comes from the raw byte
  // (volts*10 exactly), so no float formatting is needed.
  graphics.setPrintPos(0, 22);
  graphics.printf("Seq %c", 'A' + seq_);
  if (read_state_ == READ_OK) {
    const int end = SeqEndStage();
    graphics.setPrintPos(42, 22);
    if (end >= 0) graphics.printf("end@%d", end + 1);   // 1-indexed
    else          graphics.print("end:--");
    const uint8_t peak = SeqPeakRaw();
    graphics.setPrintPos(88, 22);
    graphics.printf("^%d.%dV", peak / 10, peak % 10);
  }

  if (read_state_ == READ_OK) {
    DrawStageStrip();
  } else {
    graphics.setPrintPos(kStripX, kStripTop + 4);
    graphics.print("(no sequence read)");
  }

  DrawReadState();

  // Action row. Read and Save are wired; Edit/Gen/Rec are stubs.
  // 4px gaps, not 6: five entries at 6px spill to 132px on a 128px screen.
  int x = 0;
  for (int i = 0; i < ACT_COUNT; ++i) {
    graphics.setPrintPos(x, 56);
    graphics.print(kActionNames[i]);
    const int w = (int)kActionWidths[i] * 6;
    if (i == action_) graphics.invertRect(x - 1, 55, w + 2, 10);
    x += w + 4;
  }
}

// --- edit / gen / rec screens ---------------------------------------------

// The stage editor. Reuses the home screen's strip so the shape the user was
// just looking at is the shape they are now editing, with a cursor added.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawEdit() const {
  using namespace Bus200eAppNS;

  graphics.setPrintPos(0, 13);
  graphics.printf("Edit %c  stage %d", 'A' + seq_, edit_stage_ + 1);  // 1-indexed

  if (!HaveSequence()) {
    graphics.setPrintPos(0, 30);
    graphics.print("Read a bank first.");
    graphics.setPrintPos(0, 56);
    graphics.print("encL:back");
    return;
  }

  const Buchla251eSequence &s = working_slot_.sequences[seq_];
  const uint8_t raw = s.stages[edit_stage_].value;

  // Note number AND volts: the note is what a musician reasons in, the volts
  // are what the module actually receives, and on this format they are the
  // same number scaled -- showing both keeps the 1.2V/oct mapping visible
  // instead of hiding it behind a note name.
  graphics.setPrintPos(0, 22);
  graphics.printf("Note %d  %d.%dV", raw, raw / 10, raw % 10);
  if (Buchla251eHasEndMarker(s.stages[edit_stage_])) {
    graphics.setPrintPos(96, 22);
    graphics.print("END");
    graphics.invertRect(95, 21, 22, 10);
  }

  DrawStageStrip();
  DrawStageStripCursor();

  graphics.setPrintPos(0, 47);
  graphics.print("A:end  X/Y:oct");
  graphics.setPrintPos(0, 56);
  graphics.print("encL:stg encR:val");
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawGen() const {
  using namespace Bus200eAppNS;

  graphics.setPrintPos(0, 13);
  graphics.printf("Gen Seq %c", 'A' + seq_);
  // The cap is stated, not enforced silently: the format holds 50 stages and
  // a user who expects 50 deserves to know why they cannot have them.
  graphics.setPrintPos(66, 13);
  graphics.print("(max 32)");

  if (!HaveSequence()) {
    graphics.setPrintPos(0, 30);
    graphics.print("Read a bank first.");
    graphics.setPrintPos(0, 56);
    graphics.print("encL:back");
    return;
  }

  // Two columns of parameters, cursor inverted.
  const int xs[GEN_COUNT] = {0, 64, 0, 64, 0};
  const int ys[GEN_COUNT] = {24, 24, 34, 34, 44};
  for (int i = 0; i < GEN_COUNT; ++i) {
    graphics.setPrintPos(xs[i], ys[i]);
    switch (i) {
      case GEN_LENGTH:   graphics.printf("Len  %d", gen_len_); break;
      case GEN_FILL:     graphics.printf("Fill %d", gen_fill_); break;
      case GEN_ROTATION: graphics.printf("Rot  %d", gen_rot_); break;
      case GEN_NOTE:     graphics.printf("Note %d", gen_note_); break;
      case GEN_REST:     graphics.printf("Rest %d", gen_rest_); break;
      default: break;
    }
    if (i == gen_cursor_) graphics.invertRect(xs[i] - 1, ys[i] - 1, 56, 10);
  }

  graphics.setPrintPos(0, 56);
  graphics.print("encR:APPLY encL:back");
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawRec() const {
  using namespace Bus200eAppNS;

  graphics.setPrintPos(0, 13);
  graphics.printf("Rec Seq %c", 'A' + seq_);

  if (!HaveSequence()) {
    graphics.setPrintPos(0, 30);
    graphics.print("Read a bank first.");
    graphics.setPrintPos(0, 56);
    graphics.print("encL:back");
    return;
  }

  if (rec_armed_) {
    graphics.setPrintPos(84, 13);
    graphics.print("ARMED");
    graphics.invertRect(83, 12, 34, 10);
  }

  const uint8_t n = rec_count_;
  graphics.setPrintPos(0, 26);
  graphics.printf("Notes %d/%d", n, kBuchla251eStagesPerSequence);
  if (n >= kBuchla251eStagesPerSequence) {
    graphics.setPrintPos(72, 26);
    graphics.print("FULL");
    graphics.invertRect(71, 25, 28, 10);
  }

  // The live view IS the diagnostic: if the k-board is on a port nothing
  // polls, this line never changes and that is the symptom to report.
  graphics.setPrintPos(0, 36);
  if (rec_any_) {
    const uint8_t note = rec_last_note_;
    graphics.printf("Last %d  %d.%dV", note, note / 10, note % 10);
  } else if (rec_armed_) {
    graphics.print("waiting for MIDI...");
  } else {
    graphics.print("nothing recorded");
  }

  graphics.setPrintPos(0, 46);
  graphics.print("USB dev/host, DIN, bus");

  graphics.setPrintPos(0, 56);
  if (rec_armed_) graphics.print("encR:STOP");
  else            graphics.print("encR:ARM  encL:back");
}

// --- 259e parameter viewer -------------------------------------------------

// Signed tenths as "-12.3st". Printed by hand because the fractional digit
// of a negative value is not -(t%10) once the whole part is 0: -0.5 would
// come out "0.5" and silently flip the sign of a CV inversion.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawTenths(int tenths, const char *suffix) const {
  const int whole = tenths / 10;
  const int frac = (tenths < 0 ? -tenths : tenths) % 10;
  const bool needs_sign = (tenths < 0 && whole == 0);
  graphics.printf("%s%d.%d%s", needs_sign ? "-" : "", whole, frac, suffix);
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawRow259e(int row, int y) const {
  using namespace Bus200eAppNS;
  const Row259e &r = kRows259e[row];
  const Buchla259eSlot &s = working_259e_;
  const bool have = (read_state_ == READ_OK);

  // Label. ROW_DUAL overrides it: which of the two meanings is live depends
  // on the paired timbre index, so a fixed label would be wrong half the
  // time -- and wrong in a way that reads as authoritative.
  graphics.setPrintPos(2, y);
  if (r.kind == ROW_DUAL) {
    const bool skew = (r.idx == 4) ? Buchla259eParam4IsSkew(s)
                                    : Buchla259eParam5IsSkew(s);
    if (r.idx == 4) graphics.print(skew ? "RedSkew" : "ModCVat");
    else            graphics.print(skew ? "GrnSkew" : "PrnCVat");
  } else {
    graphics.print(r.label);
  }

  graphics.setPrintPos(kRow259eValueX, y);
  if (!have) { graphics.print("--"); return; }

  const uint16_t w = (r.idx < kBuchla259eParamCount) ? s.param[r.idx] : 0;

  switch (r.kind) {
    case ROW_PITCH:
      DrawTenths(Buchla259eSemitoneTenths(w), "st");
      break;

    case ROW_BIPOLAR:
      // Signed: negative means the CV is inverted, not merely turned down.
      graphics.printf("%+d%%", Buchla259eBipolarPercent(w));
      break;

    case ROW_UNIPOLAR:
      graphics.printf("%d%%", Buchla259eUnipolarPercent(w));
      break;

    case ROW_MODFREQ:
      // Absolute, unless the modulator is tracking the principal, in which
      // case the same bytes are an interval either side of unison.
      if (s.mod_freq_mode == 2) {
        const int t = Buchla259eIntervalTenths(w);
        if (t > 0) graphics.print("+");
        DrawTenths(t, "st");
      } else {
        graphics.printf("%d", (int)Buchla259eParam12(w));
      }
      break;

    case ROW_DUAL: {
      const bool skew = (r.idx == 4) ? Buchla259eParam4IsSkew(s)
                                      : Buchla259eParam5IsSkew(s);
      if (skew) graphics.printf("b%lu", (unsigned long)Buchla259eSkewBase(w));
      else      graphics.printf("%d%%", Buchla259eUnipolarPercent(w));
      break;
    }

    case ROW_MORPH:
      // A crossfade between the two timbre tables, so name the destination
      // rather than leaving a bare percentage pointing nowhere.
      graphics.printf("%d%%grn", Buchla259eUnipolarPercent(w));
      break;

    case ROW_WARP:
      // The real scan width, not the stored count: the knob bottoms out at
      // 20%, so showing 0 here would read as "off" when it is not.
      graphics.printf("%d%%scan", Buchla259eWarpScanPercent(w));
      break;

    case ROW_ENGINE:
      if (Buchla259eEngineModeIsInert(s)) {
        // Mode 1 does nothing while the modulator is slow. Say so.
        graphics.print("1 INERT");
      } else {
        switch (s.engine_mode) {
          case 0:  graphics.print("0 off"); break;
          case 1:  graphics.print("1 mirror"); break;
          case 2:  graphics.print("2 sync"); break;
          default: graphics.print("3 free"); break;
        }
      }
      break;

    case ROW_MODDEST: {
      // 3-bit mask, drawn as present/absent letters so all three states are
      // visible at once instead of hidden behind a number.
      const uint8_t m = s.mod_dest_mask;
      graphics.printf("%c%c%c",
                      (m & kBuchla259eModDestFreq)  ? 'F' : '-',
                      (m & kBuchla259eModDestWarp)  ? 'W' : '-',
                      (m & kBuchla259eModDestMorph) ? 'M' : '-');
      break;
    }

    case ROW_WAVEFORM:
      switch (s.mod_waveform) {
        case 0:  graphics.print("Tri"); break;
        case 1:  graphics.print("Sqr"); break;
        case 2:  graphics.print("Saw"); break;
        default: graphics.printf("?%d", s.mod_waveform); break;
      }
      break;

    case ROW_FREQMODE:
      switch (s.mod_freq_mode) {
        case 0:  graphics.print("slow"); break;
        case 1:  graphics.print("norm"); break;
        case 2:  graphics.print("track"); break;
        default: graphics.printf("?%d", s.mod_freq_mode); break;
      }
      break;

    case ROW_WAVEBTN:
      graphics.print(s.wave_button_target ? "red" : "green");
      break;

    case ROW_TIMBRE:
      graphics.printf("%d", (r.idx == 31) ? s.red_timbre : s.green_timbre);
      break;

    default:
      graphics.print("?");
      break;
  }
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawModule259e() const {
  using namespace Bus200eAppNS;

  for (int i = 0; i < kRows259eVisible; ++i) {
    const int row = row_top_ + i;
    if (row >= kRow259eCount) break;
    const int y = kRow259eY0 + i * kRow259eDY;
    DrawRow259e(row, y);
    if (row == row_cursor_) graphics.invertRect(0, y - 1, 124, kRow259eDY + 1);
  }

  // Scrollbar: 19 rows in 3 makes position worth showing, and a bar costs no
  // characters on a line that has none to spare. The guard is for the day
  // someone trims the table to 3 rows or fewer -- the span would be 0 and
  // this would divide by it.
  const int span = kRow259eCount - kRows259eVisible;
  if (span > 0) {
    const int track_h = kRows259eVisible * kRow259eDY;
    const int knob_h = (track_h * kRows259eVisible) / kRow259eCount + 1;
    const int knob_y =
        kRow259eY0 - 1 + (track_h - knob_h) * (int)row_top_ / span;
    graphics.drawVLine(126, kRow259eY0 - 1, track_h);
    graphics.drawRect(125, knob_y, 3, knob_h);
  }

  DrawReadState();

  // Only Read applies to a 259e: there is no editor, generator or MIDI
  // recorder for it. Drawing the other three would advertise nothing.
  graphics.setPrintPos(0, 56);
  graphics.print("Read");
  graphics.invertRect(-1, 55, 26, 10);
  graphics.setPrintPos(36, 56);
  graphics.print("encL:back/scroll");
}

// --- app interface ---------------------------------------------------------

FLASHMEM void AppBus200e::Init() {
  addr_ = 0x5C;
  target_ = 0x5C;
  slot_ = 0;
  seq_ = 0;
  action_ = Bus200eAppNS::ACT_READ;
  read_state_ = Bus200eAppNS::READ_NONE;
  read_err_ = BUS200E_MASTER_ERR_NONE;
  edited_ = false;
  read_addr_ = 0;
  read_type_ = Bus200eAppNS::MODTYPE_UNKNOWN;
  write_state_ = Bus200eAppNS::WRITE_NONE;
  write_err_ = BUS200E_MASTER_ERR_NONE;
  pending_changes_ = 0;
  write_block_ = BUCHLA200E_WRITE_OK;
  row_cursor_ = 0;
  row_top_ = 0;
  edit_stage_ = 0;
  gen_cursor_ = Bus200eAppNS::GEN_LENGTH;
  gen_len_ = 16;
  gen_fill_ = 5;
  gen_rot_ = 0;
  gen_note_ = 60;
  gen_rest_ = 0;
  // recorder_ holds a pointer into working_slot_; clearing the arm flag is
  // what makes that pointer unreachable from the ISR.
  rec_armed_ = false;
  rec_count_ = 0;
  rec_last_note_ = 0;
  rec_any_ = false;
}

FLASHMEM size_t AppBus200e::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  stream_buffer.Write<uint8_t>(addr_);
  stream_buffer.Write<uint8_t>(target_);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppBus200e::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  const uint8_t a = stream_buffer.Read<uint8_t>();
  const uint8_t t = stream_buffer.Read<uint8_t>();
  addr_ = (a <= 0x7F) ? a : 0x5C;
  target_ = (t <= 0x7F) ? t : 0x5C;
  return stream_buffer.underflow() ? 0 : stream_buffer.read();
}

FLASHMEM void AppBus200e::HandleAppEvent(OC::AppEvent event) {
  // A scan owns the bus master FSM; never leave one running in the
  // background when the app goes away.
  if (event == OC::APP_EVENT_SUSPEND || event == OC::APP_EVENT_SCREENSAVER_ON) {
    if (scan_state_ != Bus200eAppNS::SCAN_IDLE) StopScan();
    // Never leave a write armed across a suspend or a screensaver: the
    // confirm prompt would be gone but the next encR press would still
    // commit a whole-bank write.
    if (screen_ == Bus200eAppNS::SCR_WRITE_CONFIRM)
      screen_ = Bus200eAppNS::SCR_MODULE_HOME;
    // Likewise never leave the recorder armed: its ISR poll would keep
    // consuming note-ons that the user is playing at some other app, and
    // recorder_ holds a pointer into working_slot_.
    if (rec_armed_) RecStop();
  }
}

void AppBus200e::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

FLASHMEM void AppBus200e::Loop() {
  PumpScan();
  PumpProbe();
  PumpRead();
  PumpWrite();
}

FLASHMEM void AppBus200e::DrawMenu() const {
  gfxHeader("200e Modules");
#ifdef PRESET_BUS
  if (!OC::PresetBus::Enabled()) {
    graphics.setPrintPos(0, 25);
    graphics.print("preset bus disabled");
    return;
  }
#endif
  switch (screen_) {
    case Bus200eAppNS::SCR_WRITE_CONFIRM: DrawWriteConfirm(); return;
    case Bus200eAppNS::SCR_MODULE_HOME:   DrawModuleHome();   return;
    case Bus200eAppNS::SCR_EDIT:          DrawEdit();         return;
    case Bus200eAppNS::SCR_GEN:           DrawGen();          return;
    case Bus200eAppNS::SCR_REC:           DrawRec();          return;
    default:                              DrawModuleSelect(); return;
  }
}

FLASHMEM void AppBus200e::DrawScreensaver() const { DrawMenu(); }

FLASHMEM void AppBus200e::HandleButtonEvent(const UI::Event &event) {
  if (event.type != UI::EVENT_BUTTON_PRESS) return;

  // --- armed write: only two answers, and one of them is "no" -------------
  if (screen_ == Bus200eAppNS::SCR_WRITE_CONFIRM) {
    switch (event.control) {
      case OC::CONTROL_BUTTON_R:      // encR = confirm
        if (write_block_ == BUCHLA200E_WRITE_OK) CommitWrite();
        break;
      case OC::CONTROL_BUTTON_L:      // encL = back/cancel
        screen_ = Bus200eAppNS::SCR_MODULE_HOME;
        break;
      default:
        // Everything else is deliberately inert here. A stray A/B/X/Y must
        // not be able to commit a whole-bank write.
        break;
    }
    return;
  }

  // --- stage editor --------------------------------------------------------
  if (screen_ == Bus200eAppNS::SCR_EDIT) {
    switch (event.control) {
      case OC::CONTROL_BUTTON_L:      // encL = back
      case OC::CONTROL_BUTTON_R:      // encR = done (edits are already applied)
        screen_ = Bus200eAppNS::SCR_MODULE_HOME;
        break;
      case OC::CONTROL_BUTTON_UP:     // A = toggle the loop point
        EditToggleEnd();
        break;
      case OC::CONTROL_BUTTON_DOWN:   // B = cycle sequence, as everywhere else
        seq_ = (uint8_t)((seq_ + 1) % kBuchla251eSequencesPerSlot);
        break;
      case OC::CONTROL_BUTTON_UP2:    // X = down an octave (12 raw = 1.2V)
        EditNudge(-12);
        break;
      case OC::CONTROL_BUTTON_DOWN2:  // Y = up an octave
        EditNudge(12);
        break;
      default: break;
    }
    return;
  }

  // --- generator -----------------------------------------------------------
  if (screen_ == Bus200eAppNS::SCR_GEN) {
    switch (event.control) {
      case OC::CONTROL_BUTTON_L:      // encL = back without applying
        screen_ = Bus200eAppNS::SCR_MODULE_HOME;
        break;
      case OC::CONTROL_BUTTON_R:      // encR = confirm = apply
      case OC::CONTROL_BUTTON_UP:     // A = primary action, same thing
        GenApply();
        break;
      case OC::CONTROL_BUTTON_DOWN:   // B = cycle sequence
        seq_ = (uint8_t)((seq_ + 1) % kBuchla251eSequencesPerSlot);
        break;
      default: break;
    }
    return;
  }

  // --- recorder ------------------------------------------------------------
  if (screen_ == Bus200eAppNS::SCR_REC) {
    switch (event.control) {
      case OC::CONTROL_BUTTON_R:      // encR = arm, or stop if armed
        if (rec_armed_) {
          RecStop();
          screen_ = Bus200eAppNS::SCR_MODULE_HOME;
        } else {
          RecArm();
        }
        break;
      case OC::CONTROL_BUTTON_L:      // encL = leave; stops cleanly if armed
        // Not called "cancel": notes already recorded have already changed
        // the working buffer, and Stop() at least leaves a coherent loop
        // point rather than a half-recorded sequence with a stale marker.
        RecStop();
        screen_ = Bus200eAppNS::SCR_MODULE_HOME;
        break;
      case OC::CONTROL_BUTTON_DOWN:   // B = cycle sequence, but not mid-record
        if (!rec_armed_)
          seq_ = (uint8_t)((seq_ + 1) % kBuchla251eSequencesPerSlot);
        break;
      default: break;
    }
    return;
  }

  if (screen_ == Bus200eAppNS::SCR_MODULE_HOME) {
    const bool is259 = (CurrentModuleType() == Bus200eAppNS::MODTYPE_259E);
    switch (event.control) {
      case OC::CONTROL_BUTTON_L:      // encL = back
        screen_ = Bus200eAppNS::SCR_MODULE_SELECT;
        break;
      case OC::CONTROL_BUTTON_DOWN:   // B = cycle sequence A-D
        // The 259e has no sequences, so this does nothing there rather than
        // being repurposed into a gesture nothing on screen advertises.
        if (!is259)
          seq_ = (uint8_t)((seq_ + 1) % kBuchla251eSequencesPerSlot);
        break;
      case OC::CONTROL_BUTTON_UP:     // A = primary action (Save)
        // Arms only; nothing reaches the bus until the confirm screen is
        // answered. A 259e has no write path yet, so A does nothing there.
        if (!is259) ArmWrite();
        break;
      case OC::CONTROL_BUTTON_R:      // encR = confirm/enter = run the action
        if (is259) {
          StartRead();               // the only action a 259e page has
        } else if (action_ == Bus200eAppNS::ACT_READ) {
          StartRead();
        } else if (action_ == Bus200eAppNS::ACT_SAVE) {
          ArmWrite();
        } else if (action_ == Bus200eAppNS::ACT_EDIT) {
          edit_stage_ = 0;
          screen_ = Bus200eAppNS::SCR_EDIT;
        } else if (action_ == Bus200eAppNS::ACT_GEN) {
          screen_ = Bus200eAppNS::SCR_GEN;
        } else if (action_ == Bus200eAppNS::ACT_REC) {
          screen_ = Bus200eAppNS::SCR_REC;
        }
        break;
      default: break;
    }
    return;
  }

  switch (event.control) {
    case OC::CONTROL_BUTTON_L:        // encL = start/stop scan
      if (scan_state_ != Bus200eAppNS::SCAN_IDLE) StopScan();
      else                                        StartScan();
      break;
    case OC::CONTROL_BUTTON_DOWN:     // B = probe one address
      StartProbe();
      break;
    case OC::CONTROL_BUTTON_R:        // encR = confirm/enter = pick target
      if (addr_ != target_) {
        // New module: anything previously read belongs to the old one, and
        // the write guard must not be able to mistake it for this one's.
        read_state_ = Bus200eAppNS::READ_NONE;
        edited_ = false;
        read_addr_ = 0;
        read_type_ = Bus200eAppNS::MODTYPE_UNKNOWN;
        write_state_ = Bus200eAppNS::WRITE_NONE;
      }
      target_ = addr_;
      screen_ = Bus200eAppNS::SCR_MODULE_HOME;
      break;
    default: break;
  }
}

FLASHMEM void AppBus200e::HandleEncoderEvent(const UI::Event &event) {
  // An armed write ignores the encoders outright: changing the slot or the
  // action cursor under a confirm prompt would make the prompt describe
  // something other than what a confirm would do.
  if (screen_ == Bus200eAppNS::SCR_WRITE_CONFIRM) return;

  // Stage editor: encL walks the 50 stages, encR edits the one under the
  // cursor. Clamped rather than wrapped, matching every other cursor in this
  // app -- and a wrap from stage 50 back to 1 mid-edit is a good way to
  // change the wrong stage without noticing.
  if (screen_ == Bus200eAppNS::SCR_EDIT) {
    if (!HaveSequence()) return;
    if (event.control == OC::CONTROL_ENCODER_L) {
      int s = (int)edit_stage_ + event.value;
      CONSTRAIN(s, 0, kBuchla251eStagesPerSequence - 1);
      edit_stage_ = (uint8_t)s;
    } else if (event.control == OC::CONTROL_ENCODER_R) {
      EditNudge(event.value);
    }
    return;
  }

  if (screen_ == Bus200eAppNS::SCR_GEN) {
    if (event.control == OC::CONTROL_ENCODER_L) {
      int c = (int)gen_cursor_ + event.value;
      CONSTRAIN(c, 0, Bus200eAppNS::GEN_COUNT - 1);
      gen_cursor_ = (uint8_t)c;
    } else if (event.control == OC::CONTROL_ENCODER_R) {
      GenAdjust(event.value);
    }
    return;
  }

  // The recorder takes no encoder input: while armed the only thing that
  // should change the sequence is the keyboard.
  if (screen_ == Bus200eAppNS::SCR_REC) return;

  if (screen_ == Bus200eAppNS::SCR_MODULE_HOME) {
    if (event.control == OC::CONTROL_ENCODER_R) {
      int s = (int)slot_ + event.value;
      CONSTRAIN(s, 0, Bus200eAppNS::kSlotCount - 1);
      if ((uint8_t)s != slot_) {
        slot_ = (uint8_t)s;
        // The bank read covers all 30 slots, so browsing costs no bus
        // traffic -- but if the card image is gone the old slot's bytes
        // must not be left on screen labelled as this slot's.
        if (read_state_ == Bus200eAppNS::READ_OK && !DecodeSlotFromCardImage()) {
          read_state_ = Bus200eAppNS::READ_NONE;
          edited_ = false;
        }
      }
    } else if (event.control == OC::CONTROL_ENCODER_L) {
      // On a 259e the action row is a single entry, so the left encoder is
      // free for what that page actually needs: scrolling 19 parameters
      // through a 3-row window.
      if (CurrentModuleType() == Bus200eAppNS::MODTYPE_259E) {
        ScrollRows(event.value);
      } else {
        int a = (int)action_ + event.value;
        CONSTRAIN(a, 0, Bus200eAppNS::ACT_COUNT - 1);
        action_ = (uint8_t)a;
      }
    }
    return;
  }

  if (screen_ != Bus200eAppNS::SCR_MODULE_SELECT) return;

  if (event.control == OC::CONTROL_ENCODER_R) {
    int a = (int)addr_ + event.value;
    CONSTRAIN(a, 0, 0x7F);
    addr_ = (uint8_t)a;
    return;
  }

  if (event.control == OC::CONTROL_ENCODER_L && found_count_ > 0) {
    int sel = list_top_ + event.value;
    CONSTRAIN(sel, 0, found_count_ - 1);
    list_top_ = sel;
    // scrolling the list also picks the target -- one gesture, not two
    const int ti = FoundIndexToTableIndex(sel);
    const Buchla200eModuleEntry *e = (ti >= 0) ? Buchla200eModuleAt(ti) : nullptr;
    if (e) addr_ = e->addr;
  }
}

FLASHMEM void AppBus200e::GetIOConfig(OC::IOConfig &ioconfig) const {
  ioconfig.outputs[0].set("CH1", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[1].set("CH2", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[2].set("CH3", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[3].set("CH4", OC::OUTPUT_MODE_UNI);
}

FLASHMEM void AppBus200e::DrawDebugInfo() const {}
