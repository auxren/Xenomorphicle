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
#include "../Buchla200eUiGate.h"
#include "../Buchla200eWriteGuard.h"
#include "../Buchla251eGenerator.h"
#include "../Buchla251eNoteMap.h"
#include "../Buchla251eRecorder.h"
#include "../Buchla251eSlotCodec.h"
#include "../Buchla259eSlotCodec.h"
#include "../HSMIDI.h"
#include "../PresetBus.h"
// SnapshotBank/LoadSnapshot for the pre-write undo. Included explicitly rather
// than relied on transitively: this compiled in full builds only because
// Quadrants.h happens to be included first and pulls it in, which is not a
// dependency this header should have.
#include "../PresetEngine.h"
#include "../PhzConfig.h"    // the scan set lives in GLOBALS.CFG, not EEPROM

namespace Bus200eAppNS {

enum Screen : uint8_t {
  SCR_MODULE_SELECT = 0,
  SCR_MODULE_HOME,
  SCR_WRITE_CONFIRM,   // armed; nothing has gone on the wire yet
  SCR_EDIT,            // per-stage editor over the selected sequence
  SCR_GEN,             // Euclidean generator parameters
  SCR_REC,             // record stages from incoming MIDI
  SCR_SNAP_CONFIRM,    // armed: put the pre-write bank back
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

// Home-screen action row.
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
  WRITE_VERIFYING,  // restore done; reading the bank back to check it landed
  WRITE_OK,         // read back and matched, byte for byte
  WRITE_FAIL,
  WRITE_BAD,        // read back and did NOT match: module contents unknown
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

// How long the write-confirm screen ignores a "yes" after it appears. Long
// enough that no fumbled chord or double-tap can cross it, short enough that a
// deliberate press never feels refused: a considered look at a new screen is
// well over a third of a second, and the preset overlay already asks 500 ms
// for a STORE, which is a far less consequential act than this.
static constexpr uint32_t kConfirmDeadMs = 350;

// The scan set's home: GLOBALS.CFG, in the namespace the preset bus already
// owns (PRESETBUS_KEY = 8 << 8, shared with the module address and the slot
// manifests). Two keys -- the 64-bit found bitmap, and addr/target/table-size.
static constexpr uint16_t kScanSetKey  = (8 << 8) | 0x20;
static constexpr uint16_t kScanMetaKey = (8 << 8) | 0x21;

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
  // Back to 2 bytes: addr_ and target_ only.
  //
  // The scan set briefly lived here, which grew this chunk 6 -> 16 bytes and
  // put it against an EEPROM budget nobody has measured on real hardware --
  // and BuildAppData's overflow behaviour is to silently drop a RANDOMLY
  // ROTATED app per save, so exceeding it corrupts a different app each time
  // and would be near-impossible to diagnose. It now lives in GLOBALS.CFG
  // under the PRESETBUS namespace instead, where a 64-bit value holds the
  // whole bitmap exactly and costs no EEPROM at all.
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
  bool landed_ = false;        // first RESUME after power-up has chosen a screen
  uint8_t found_[Bus200eAppNS::kFoundBytes] = {0};
  // What found_ held when the current scan started, so a scan that hears
  // NOBODY can hand it back instead of replacing it. See KeepRememberedIfEmpty.
  uint8_t remembered_[Bus200eAppNS::kFoundBytes] = {0};
  int remembered_count_ = 0;
  bool scan_kept_ = false;     // the last scan heard nobody; showing the old set
  void KeepRememberedIfEmpty();

  // single-address probe (works for off-table addresses too, unlike the scan)
  uint8_t probe_addr_ = 0;
  int8_t probe_result_ = -1;   // -1 none, 0 silent, 1 answered
  bool probe_active_ = false;
  uint32_t probe_ms_ = 0;      // millis() the probe was fired; bounds it
  uint32_t scan_step_ms_ = 0;  // millis() the current scan query was fired

  // Why the last button press did nothing. A refusal the user cannot see is
  // indistinguishable from a dead button -- which is exactly how the Read
  // bug presented. Cleared when an action actually starts.
  Buchla200eReadBlock last_refusal_ = BUCHLA200E_READ_OK;

  // --- 251e home screen ---
  uint8_t slot_ = 0;           // WIRE index 0-29; displayed as slot_+1
  uint8_t seq_ = 0;            // 0-3 = A-D
  uint8_t action_ = Bus200eAppNS::ACT_READ;
  uint8_t read_state_ = Bus200eAppNS::READ_NONE;
  Bus200eMasterError read_err_ = BUS200E_MASTER_ERR_NONE;
  uint32_t read_ms_ = 0;       // millis() when the bank read completed
  uint32_t read_started_ms_ = 0;   // millis() the transfer was accepted
  uint32_t armed_ms_ = 0;          // millis() the confirm screen appeared
  bool found_dirty_ = false;       // scan result awaiting a safe write
  bool snap_here_ = false;         // a snapshot exists for target_
  uint8_t recover_cursor_ = 0;     // 0 = keep, 1 = undo; safe end first
  uint32_t write_started_ms_ = 0;  // ditto for a restore
  // Set when a job vanished instead of finishing, so the status line can say
  // "lost" rather than blaming the module for an error it never reported.
  bool read_lost_ = false;
  bool write_lost_ = false;
  bool edited_ = false;        // working_slot_ diverges from the module

  // Which module the resident card image actually came from. Without these a
  // read of 0x5C followed by a retarget to 0x28 would look writable, and
  // would push a 251e bank at a 259e.
  uint8_t read_addr_ = 0;
  uint8_t read_type_ = Bus200eAppNS::MODTYPE_UNKNOWN;

  // CRC-32 of the whole bank as it was when the read completed. The card
  // image is shared with the console 'w' patcher and the USB bridge, so this
  // is the only thing that can tell, at Save time, whether the bytes about to
  // go on the wire are still the ones that were read.
  uint32_t read_hash_ = 0;

  // write path
  uint8_t write_state_ = Bus200eAppNS::WRITE_NONE;
  Bus200eMasterError write_err_ = BUS200E_MASTER_ERR_NONE;
  int pending_changes_ = 0;    // diff size shown on the confirm screen
  Buchla200eWriteBlock write_block_ = BUCHLA200E_WRITE_OK;
  // What the bank hashed to once the patch was applied and checked. The
  // read-back after the restore is compared against this: equal means the
  // module holds exactly what was intended, and nothing else does.
  uint32_t intended_hash_ = 0;
  uint32_t intended_outside_ = 0;
  // The slot window that was actually committed, latched at CommitWrite.
  // PumpWrite used to re-derive the hole from the live slot_, which the user
  // can change with the encoder while the write is in flight -- so a genuine
  // mismatch could be attributed to the wrong slot and reported as
  // "BAD: OTHER PRESETS!" when only the edited slot was wrong.
  uint32_t committed_off_ = 0;
  uint32_t verify_started_ms_ = 0;
  int verify_diff_ = 0;         // differing bytes in the read-back of our slot
  bool verify_outside_ok_ = true;  // the other 29 presets came back unchanged
  // Whether the read-back covered the whole bank. Separate from
  // verify_outside_ok_ on purpose: "the other 29 came back wrong" and "we
  // never saw the other 29" are different facts and need different words.
  bool verify_covered_ = true;

  // Freshly encoded copy of the slot being written, built at commit time.
  // Its whole job is to be compared against: once against the image after
  // patching (proving the patch list was neither too wide nor too narrow),
  // and once against the read-back (proving the module took it). A member,
  // not a stack local -- 2104 bytes of stack in Loop() is not something this
  // build can spend, and app instances live in RAM2 where it is free.
  uint8_t intended_slot_[kBuchla251eSlotBytes];

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

  // The target as a person names it. Every status line used to say the bare
  // address -- "reading 5C ..." -- which means nothing at the panel: the
  // module is a 251e, and that is what the case says on its front. The
  // address stays on the home title beside the model, where it earns its
  // place (two 259es differ only by it); it is the FALLBACK here, "@5C",
  // for an address the table does not know.
  const char *TargetName() const {
    const char *model = Buchla200eModelForAddress(target_);
    if (model) return model;
    static char buf[4];
    snprintf(buf, sizeof(buf), "@%02X", target_);
    return buf;
  }

  void StartScan();
  void ConsumeScanDirty();
  void LoadScanSet();
  void RefreshSnapshotFlag();
  void CommitSnapshotRestore();
  void DrawSnapConfirm() const;
  void StopScan();
  void PumpScan();
  void PumpProbe();
  void StartProbe();
  void StartRead();
  void PumpRead();
  bool DecodeSlotFromCardImage();
  uint32_t ExpectedBankBytes() const;
  int ComputeSlotDiff();       // changed bytes for slot_, or <0 on overflow
  Buchla200eWriteBlock CheckWrite(int changed, uint32_t image_hash) const;
  void ArmWrite();
  void CommitWrite();
  void PumpWrite();
  uint32_t HashResidentBank() const;   // CRC-32 over the full expected bank
  bool SlotWindowInBank(uint32_t *off_out) const;  // slot_ lies inside the bank
  // A restore is in flight OR its read-back is: both own the master FSM, and
  // both must keep every other bus user out.
  bool WriteBusy() const {
    return write_state_ == Bus200eAppNS::WRITE_ACTIVE ||
           write_state_ == Bus200eAppNS::WRITE_VERIFYING;
  }
  void DrawWriteConfirm() const;
  Bus200eAppNS::ModuleType CurrentModuleType() const;
  void DrawModuleSelect() const;
  void DrawModuleHome() const;
  void DrawModule251e() const;
  void DrawModule259e() const;
  void DrawRow259e(int row, int y) const;
  void DrawTenths(int tenths, const char *suffix) const;
  void DrawAge(uint32_t ms) const;
  void ScrollRows(int delta);
  void DrawStageStrip() const;
  void DrawStageStripCursor() const;
  void DrawReadState() const;
  void DrawXferTail(const char *verb) const;  // " ..." or " NN%"
  int SeqEndStage() const;     // 0-indexed stage holding the end marker, or -1
  uint8_t SeqPeakRaw() const;
  int FoundIndexToTableIndex(int nth) const;
  void SyncListToAddr();       // list cursor onto addr_'s row, if it responded

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
#ifdef PRESET_BUS
  // A scan drives the same query FSM a probe uses and calls
  // MasterQueryReset() as it goes, so starting one on top of a probe (or a
  // transfer) stranded the other job. Refuse visibly instead.
  Buchla200eReadContext rc;
  rc.bus_enabled = OC::PresetBus::Enabled();
  rc.read_active = (read_state_ == Bus200eAppNS::READ_ACTIVE);
  rc.write_active = WriteBusy();
  rc.scan_idle = true;   // starting one is what we are here to do
  rc.probe_active = probe_active_;
  last_refusal_ = Buchla200eCheckRead(rc);
  if (last_refusal_ != BUCHLA200E_READ_OK) return;
#endif
  for (int i = 0; i < Bus200eAppNS::kFoundBytes; ++i) {
    remembered_[i] = found_[i];
    found_[i] = 0;
  }
  remembered_count_ = found_count_;
  found_count_ = 0;
  scan_kept_ = false;
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
  // Leave found_/found_count_ intact: an aborted scan keeps what it found --
  // unless that is nothing, in which case the set it started from is better.
  scan_state_ = Bus200eAppNS::SCAN_IDLE;
#ifdef PRESET_BUS
  OC::PresetBus::MasterQueryReset();
  OC::PresetBus::MasterQuerySetQuiet(false);
#endif
  KeepRememberedIfEmpty();
}

// A scan that heard nobody must not replace a set that named somebody.
//
// The remembered set is the answer to a 49-second question and it is only
// wrong when the case physically changes. A scan that finds nothing is far
// more often the bench than the case: this module runs from USB on a desk,
// and one encL press with the bus unplugged (or the case off) used to sweep
// the table, find nobody, and -- via ConsumeScanDirty -- persist an all-zero
// set over the good one in GLOBALS.CFG. The next real session then opened
// on the scan page with no responders and had to spend the 49 s again. That
// is the "scan set lost" the bench kept seeing.
//
// Keeping a stale set costs nothing: a module that is no longer there simply
// does not answer when targeted, and the next scan that DOES find modules
// replaces the set as before. Only the empty result is refused.
FLASHMEM __attribute__((noinline))
void AppBus200e::KeepRememberedIfEmpty() {
  if (found_count_ != 0 || remembered_count_ == 0) return;
  for (int i = 0; i < Bus200eAppNS::kFoundBytes; ++i) found_[i] = remembered_[i];
  found_count_ = remembered_count_;
  found_dirty_ = false;   // nothing new to write; the file already holds this
  scan_kept_ = true;      // and say so on the status line, not only here
  serial_printf("200e: scan found nobody; keeping the %d remembered\n",
                found_count_);
}

FLASHMEM __attribute__((noinline))
void AppBus200e::StartProbe() {
#ifdef PRESET_BUS
  // Same treatment as StartRead: no silent refusals. A probe also may not
  // start on top of a read/write, because MasterQueryReset() below would
  // disturb a job those own.
  Buchla200eReadContext rc;
  rc.bus_enabled = OC::PresetBus::Enabled();
  rc.read_active = (read_state_ == Bus200eAppNS::READ_ACTIVE);
  rc.write_active = WriteBusy();
  rc.scan_idle = (scan_state_ == Bus200eAppNS::SCAN_IDLE);
  rc.probe_active = probe_active_;
  last_refusal_ = Buchla200eCheckRead(rc);
  if (last_refusal_ == BUCHLA200E_READ_OK &&
      (addr_ == 0 || addr_ == OC::PresetBus::ModuleAddress()))
    last_refusal_ = BUCHLA200E_READ_BAD_ADDR;
  if (last_refusal_ != BUCHLA200E_READ_OK) return;

  OC::PresetBus::MasterQueryReset();
  if (OC::PresetBus::MasterQuery(addr_) == 0) {
    probe_addr_ = addr_;
    probe_result_ = -1;
    probe_active_ = true;
    scan_kept_ = false;
    probe_ms_ = millis();
  } else {
    // Refused by the query FSM itself. Previously this left no trace at all.
    last_refusal_ = BUCHLA200E_READ_BUSY_PROBE;
  }
#endif
}

FLASHMEM __attribute__((noinline))
void AppBus200e::PumpProbe() {
#ifdef PRESET_BUS
  if (!probe_active_) return;
  // StopScan() and StartProbe() both call MasterQueryReset(), so a scan
  // started while a probe was in flight used to strand probe_active_ true
  // forever -- and a stuck probe silently blocked every subsequent Read.
  // Treat idle-underneath-us and overrun as terminal, not as "still waiting".
  const Bus200eQueryState st = OC::PresetBus::MasterQueryState();
  const Buchla200eJobFate fate = Buchla200eQueryProgress(
      st, millis() - probe_ms_, BUCHLA200E_QUERY_TIMEOUT_MS);
  if (fate == BUCHLA200E_JOB_PENDING) return;

  if (fate == BUCHLA200E_JOB_DONE)        probe_result_ = 1;
  else if (fate == BUCHLA200E_JOB_FAILED) probe_result_ = 0;
  else                                    probe_result_ = -1;  // lost/timeout

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
    // Bounded, so a query that never terminates advances the sweep instead
    // of parking the scan (and with it the whole app) on one address.
    const Bus200eQueryState st = OC::PresetBus::MasterQueryState();
    const Buchla200eJobFate fate = Buchla200eQueryProgress(
        st, millis() - scan_step_ms_, BUCHLA200E_QUERY_TIMEOUT_MS);
    if (fate == BUCHLA200E_JOB_PENDING) return;  // come back next Loop()
    if (fate == BUCHLA200E_JOB_DONE) {
      SetFound(found_, scan_index_);
      ++found_count_;
    }
    // FAILED / LOST / TIMEOUT all mean "no answer recorded here" -- move on.
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
    // Mark the result for persistence -- but do NOT write it from here.
    //
    // The set is worth keeping: the case only changes when someone physically
    // changes it, and the answer costs ~49 s to obtain. But OC::SaveAppData()
    // is a heavier hammer than it looks from this call site. It calls
    // SaveGlobalSettings() (which loads GLOBALS.CFG into PhzConfig's SHARED
    // map and never hands it back -- persist_cur_slot documents that handback
    // as mandatory), it rewrites 000.SCL-003.SCL on the SD card when one is
    // present, and it holds __disable_irq() across each flash erase/program
    // window. That last one masks our own I2C slave: if a foreign master is
    // mid-transfer through the card we serve, its transfer can tear. Doing
    // all that as a side effect of a scan finishing is the wrong trade.
    //
    // ConsumeScanDirty() below writes it at a quiescent moment instead.
    found_dirty_ = true;
    serial_printf("200e: scan complete, %d modules found\n", found_count_);
    KeepRememberedIfEmpty();   // an empty result never replaces a real one
    return;
  }

  const Buchla200eModuleEntry *e = Buchla200eModuleAt(scan_index_);
  if (!e) {
    scan_state_ = Bus200eAppNS::SCAN_IDLE;
    OC::PresetBus::MasterQuerySetQuiet(false);
    KeepRememberedIfEmpty();
    return;
  }
  if (OC::PresetBus::MasterQuery(e->addr) == 0) {
    scan_state_ = Bus200eAppNS::SCAN_WAITING;
    scan_step_ms_ = millis();
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
  // Every refusal is now a value the status line prints. These used to be
  // three bare returns, which made a blocked Read indistinguishable from a
  // dead button -- the bug this app was reported with.
  Buchla200eReadContext rc;
  rc.bus_enabled = OC::PresetBus::Enabled();
  rc.read_active = (read_state_ == Bus200eAppNS::READ_ACTIVE);
  rc.write_active = WriteBusy();
  rc.scan_idle = (scan_state_ == Bus200eAppNS::SCAN_IDLE);
  rc.probe_active = probe_active_;
  last_refusal_ = Buchla200eCheckRead(rc);
  if (last_refusal_ != BUCHLA200E_READ_OK) return;

  // The address check StartProbe has always had, and this never did.
  // Address 0 is the bus's GENERAL CALL destination: a BACKUP addressed
  // there is not a read of "module zero", it is an invitation to every
  // module on the bus at once. And backing up our own address means asking
  // our own slave to answer our own master. Neither is a write, so the
  // mandate is not at stake -- but the first is real bus disruption in
  // someone else's case, and the address encoder can reach both.
  if (target_ == 0 || target_ == OC::PresetBus::ModuleAddress()) {
    last_refusal_ = BUCHLA200E_READ_BAD_ADDR;
    return;
  }

  // Clear any DONE/FAILED left over from a previous job, otherwise the
  // master FSM refuses the new one as busy.
  OC::PresetBus::MasterReset();
  const int rc2 = OC::PresetBus::MasterBackup(target_);
  if (rc2 == 0) {
    read_state_ = Bus200eAppNS::READ_ACTIVE;
    read_err_ = BUS200E_MASTER_ERR_NONE;
    read_started_ms_ = millis();   // bounds the wait; see PumpRead
    // A read in flight is now the most recent thing that happened to this
    // module, so it -- not the previous write -- owns the provenance line.
    // DrawReadState() gives write_state_ priority, and nothing used to clear
    // it, so after any write the line was pinned to WROTE + VERIFIED for the
    // rest of the session: pressing Read re-read all 63120 bytes and the
    // screen did not change by a pixel, which is indistinguishable from a
    // dead button. It also meant the freshness clock never restarted and the
    // banner was drawn over slots it did not describe.
    // Only cleared once the job is actually accepted, so a REFUSED read
    // leaves the previous write's verdict standing.
    write_state_ = Bus200eAppNS::WRITE_NONE;
  } else {
    read_state_ = Bus200eAppNS::READ_FAIL;
    read_err_ = (Bus200eMasterError)(-rc2);
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

  // The master FSM is shared with the console commands and the USB bridge.
  // Waiting only on DONE/FAILED meant any other outcome hung here forever,
  // which then made the Read button silently refuse. Bounded and explicit.
  const Bus200eMasterState st = OC::PresetBus::MasterState();
  const Buchla200eJobFate fate = Buchla200eJobProgress(
      st, millis() - read_started_ms_, BUCHLA200E_JOB_TIMEOUT_MS);
  if (fate == BUCHLA200E_JOB_PENDING) return;

  if (fate == BUCHLA200E_JOB_LOST || fate == BUCHLA200E_JOB_TIMEOUT) {
    read_state_ = Bus200eAppNS::READ_FAIL;
    read_err_ = BUS200E_MASTER_ERR_NONE;   // nobody reported one; don't invent
    read_lost_ = true;
    OC::PresetBus::MasterReset();
    return;
  }
  read_lost_ = false;

  if (st == BUS200E_MASTER_DONE) {
    if (DecodeSlotFromCardImage()) {
      read_state_ = Bus200eAppNS::READ_OK;
      read_ms_ = millis();
      edited_ = false;
      // Stamp WHOSE bank is now resident. The write guard refuses unless this
      // still matches the target at Save time.
      read_addr_ = target_;
      read_type_ = (uint8_t)CurrentModuleType();
      // ...and WHAT it contained, so a later Save can tell whether anything
      // else has written into the shared image since.
      read_hash_ = HashResidentBank();
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

// Where slot_ lives in the bank, and whether that window is entirely inside
// the bank we actually hold. This is the range check the guard asks about:
// the transfer is whole-bank, so an out-of-range slot index does not fail --
// it silently lands on a neighbouring preset, or past the end of the buffer.
FLASHMEM __attribute__((noinline))
bool AppBus200e::SlotWindowInBank(uint32_t *off_out) const {
#ifdef PRESET_BUS
  if (off_out) *off_out = 0;
  if (CurrentModuleType() != Bus200eAppNS::MODTYPE_251E) return false;
  if (slot_ < 0 || slot_ >= Bus200eAppNS::kSlotCount) return false;
  const uint32_t off = (uint32_t)slot_ * kBuchla251eSlotBytes;
  const uint32_t end = off + kBuchla251eSlotBytes;
  if (end > ExpectedBankBytes()) return false;
  if (end > Bus200eMasterBytesTransferred()) return false;
  if (off_out) *off_out = off;
  return true;
#else
  (void)off_out;
  return false;
#endif
}

// Encodes the working slot into intended_slot_ and returns how many bytes it
// differs from the resident image's copy.
//
// This is the number the user consents to on the confirm screen, so it is
// computed from the same bytes that will actually be sent -- encode once into
// a member, compare, and later memcpy that same member. The previous form
// called Buchla251eDiffSlot(), which encodes into a 2104-byte STACK buffer;
// on a build whose stack overflow was already once a crash-loop, doing that
// from Loop() was a risk taken for nothing.
FLASHMEM __attribute__((noinline))
int AppBus200e::ComputeSlotDiff() {
#ifdef PRESET_BUS
  uint32_t off = 0;
  if (!SlotWindowInBank(&off)) return 0;
  const uint8_t *img = OC::PresetBus::MasterCardImage();
  if (!img) return 0;

  Buchla251eEncodeSlot(working_slot_, intended_slot_);
  int count = 0;
  for (uint32_t i = 0; i < kBuchla251eSlotBytes; ++i)
    if (intended_slot_[i] != img[off + i]) ++count;
  return count;
#else
  return 0;
#endif
}

// CRC-32 over the full expected bank in the resident image. Zero when there
// is nothing to hash, which never collides with a real answer being trusted:
// every caller pairs it with the guard's image/short-read checks.
FLASHMEM __attribute__((noinline))
uint32_t AppBus200e::HashResidentBank() const {
#ifdef PRESET_BUS
  const uint8_t *img = OC::PresetBus::MasterCardImage();
  const uint32_t len = ExpectedBankBytes();
  if (!img || len == 0) return 0;
  if (Bus200eMasterBytesTransferred() < len) return 0;
  return Buchla200eCrc32(img, len);
#else
  return 0;
#endif
}

FLASHMEM __attribute__((noinline))
Buchla200eWriteBlock AppBus200e::CheckWrite(int changed, uint32_t image_hash) const {
  Buchla200eWriteContext c;
  c.have_read = (read_state_ == Bus200eAppNS::READ_OK);
  c.read_addr = read_addr_;
  c.read_type = read_type_;
  c.target_addr = target_;
  c.target_type = (uint8_t)CurrentModuleType();
  c.expected_bank_bytes = ExpectedBankBytes();
  c.changed_bytes = changed;
  c.patches_in_range = SlotWindowInBank(nullptr);
#ifdef PRESET_BUS
  c.bytes_transferred = Bus200eMasterBytesTransferred();
  c.card_serving = OC::PresetBus::CardServing();
  c.image_valid = (OC::PresetBus::MasterCardImage() != nullptr);
  c.master_idle = (scan_state_ == Bus200eAppNS::SCAN_IDLE) && !probe_active_ &&
                  (read_state_ != Bus200eAppNS::READ_ACTIVE) && !WriteBusy();
  // image_hash is the caller's freshly taken CRC of the resident bank. This
  // is the only check that looks at the bytes rather than at a counter: the
  // console 'w' command patches this same buffer, Bus200eBridge writes browser
  // SysEx into it, and any other MasterBackup replaces it wholesale. "We read
  // it, therefore it is still ours" was never true.
  c.image_matches_read = (read_state_ == Bus200eAppNS::READ_OK) &&
                         (read_hash_ != 0) && (image_hash == read_hash_);
#else
  (void)image_hash;
  c.bytes_transferred = 0;
  c.card_serving = false;
  c.image_valid = false;
  c.master_idle = false;
  c.image_matches_read = false;
#endif
  return Buchla200eCheckWrite(c);
}

// Step 1 of 2. Computes the diff and runs the guard, but touches nothing on
// the bus -- if this refuses, the reason is shown and no wire activity has
// happened.
FLASHMEM __attribute__((noinline))
void AppBus200e::ArmWrite() {
  pending_changes_ = ComputeSlotDiff();
  const uint32_t h = HashResidentBank();
  write_block_ = CheckWrite(pending_changes_, h);
  screen_ = Bus200eAppNS::SCR_WRITE_CONFIRM;
  armed_ms_ = millis();
}

// Step 2 of 2, and the only place in this app that puts bytes on the wire in
// the destructive direction.
//
// The sequence below is deliberately paranoid, because the failure it guards
// against is silent and total: MasterRestore sends 63,120 bytes and the module
// takes them. Nothing rejects a bad bank. So, in order:
//
//   1. Re-diff and re-run the full guard -- never trust the arm-time verdict,
//      since the image can be dropped or replaced while the confirm screen is
//      up. The guard's image_matches_read check is fed a CRC taken right here.
//   2. Hash the bank with the target slot punched out, so we know what the
//      OTHER 29 presets look like before we touch anything.
//   3. Copy in the exact bytes the diff counted -- the same intended_slot_
//      buffer, not a re-encode that might differ.
//   4. Prove the copy: the slot must equal intended_slot_ byte for byte, and
//      everything outside it must still hash to what it did in step 2. A
//      memcpy that ran long, or a slot offset off by one record, fails here
//      instead of on the module.
//   5. Remember the intended whole-bank hash, then send. PumpWrite reads the
//      bank back and compares against it.
//
// If step 4 fails the image is already modified and cannot be trusted, so the
// read is invalidated and nothing is sent.
FLASHMEM __attribute__((noinline))
void AppBus200e::CommitWrite() {
#ifdef PRESET_BUS
  const int changed = ComputeSlotDiff();      // also fills intended_slot_
  const uint32_t bank_len = ExpectedBankBytes();
  uint8_t *img = OC::PresetBus::MasterCardImage();

  write_block_ = CheckWrite(changed, img ? Buchla200eCrc32(img, bank_len) : 0);
  if (write_block_ != BUCHLA200E_WRITE_OK) {
    pending_changes_ = changed;
    return;   // stay on the confirm screen showing why
  }
  if (!img) { write_block_ = BUCHLA200E_WRITE_NO_IMAGE; return; }

  // The guard passing means the window is in range, so this cannot fail --
  // but it is what produces `off`, and a bad `off` is the whole danger here.
  uint32_t off = 0;
  if (!SlotWindowInBank(&off)) {
    write_block_ = BUCHLA200E_WRITE_PATCH_RANGE;
    return;
  }

  // 2a. SNAPSHOT, before a single byte is modified or sent.
  //
  // This is the step that turns "we can tell you it went wrong" into "we can
  // put it back". Everything downstream -- the hash of the other 29, the
  // byte-for-byte proof, the read-back -- only ever DETECTED damage. The
  // module keeps no undo and this app kept only hashes, so the honest reading
  // of the old confirm screen was: we will check, and if it is wrong we will
  // say so, and that is all. One 64 KB block changes that.
  //
  // Taken here rather than earlier because this is the last moment the image
  // is provably the module's own contents: the guard above has just re-checked
  // it against read_hash_, and the memcpy below is the first thing to touch it.
  //
  // A snapshot that fails to write does NOT block the write. It is a safety
  // net, not a precondition, and refusing an edit the user has already
  // confirmed because a filesystem hiccuped would be its own kind of wrong --
  // but say so on the wire log, because the net is not there.
  // ...but NOT over an existing one taken before a write that went BAD.
  //
  // This nearly recreated, one level up, the exact failure the snapshot was
  // built to end. Write, get BAD, then do the obvious thing and retry: the
  // image is now the module's CORRUPT contents, and an unconditional snapshot
  // would overwrite the pristine bank with it. The undo would then faithfully
  // restore the damage, and the last good copy would once again have been
  // destroyed by the recovery machinery itself. Worse in the OTHER-PRESETS
  // case, where the retry also re-sends the 29 damaged presets as its baseline.
  //
  // So: while a BAD verdict for this target stands and a snapshot for it
  // survives, keep the one we have. It is older than the image and that is
  // precisely why it is worth more.
  const bool keep_existing =
      (write_state_ == Bus200eAppNS::WRITE_BAD) && snap_here_;
  if (keep_existing) {
    serial_printf("200e: keeping the pre-BAD snapshot for %02X\n", target_);
  } else if (!OC::PresetEngine::SnapshotBank(target_, img, bank_len,
                                            Buchla200eCrc32(img, bank_len))) {
    serial_printf("200e: WARNING no snapshot taken for %02X\n", target_);
  }

  const Buchla200eBankHash before =
      Buchla200eHashBank(img, bank_len, off, kBuchla251eSlotBytes);

  memcpy(img + off, intended_slot_, kBuchla251eSlotBytes);

  const Buchla200eBankHash after =
      Buchla200eHashBank(img, bank_len, off, kBuchla251eSlotBytes);
  if (after.outside != before.outside ||
      memcmp(img + off, intended_slot_, kBuchla251eSlotBytes) != 0) {
    // The image no longer holds what we meant to build. Send nothing, and
    // make the user re-Read: the buffer is now neither the module's bank nor
    // the intended one.
    write_block_ = BUCHLA200E_WRITE_BUILD_FAILED;
    read_state_ = Bus200eAppNS::READ_FAIL;
    read_hash_ = 0;
    return;
  }

  intended_hash_ = after.whole;
  intended_outside_ = after.outside;
  committed_off_ = off;   // the hole this write owns, for the verify pass

  OC::PresetBus::MasterReset();
  const int rc = OC::PresetBus::MasterRestore(target_);
  if (rc == 0) {
    write_state_ = Bus200eAppNS::WRITE_ACTIVE;
    write_err_ = BUS200E_MASTER_ERR_NONE;
    write_started_ms_ = millis();
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
  if (!WriteBusy()) return;

  const bool verifying = (write_state_ == Bus200eAppNS::WRITE_VERIFYING);

  // Same bound as the read path. A write that vanishes is worse than one that
  // fails: the module's state is then unknown, so say so and make the user
  // re-Read rather than leaving a hopeful "writing..." on screen forever.
  const Bus200eMasterState st = OC::PresetBus::MasterState();
  const Buchla200eJobFate fate = Buchla200eJobProgress(
      st, millis() - (verifying ? verify_started_ms_ : write_started_ms_),
      BUCHLA200E_JOB_TIMEOUT_MS);
  if (fate == BUCHLA200E_JOB_PENDING) return;

  const bool bad = (fate == BUCHLA200E_JOB_LOST ||
                    fate == BUCHLA200E_JOB_TIMEOUT ||
                    st == BUS200E_MASTER_FAILED);

  if (bad) {
    // A restore that failed leaves the module in an unknown state, and so
    // does a read-back that never arrived -- we cannot say the write landed,
    // and we must not say it did not. Both keep edited_ set so the user's
    // work survives and the retry path stays open.
    if (verifying) {
      write_state_ = Bus200eAppNS::WRITE_BAD;
      read_state_ = Bus200eAppNS::READ_FAIL;
      read_hash_ = 0;
    } else {
      write_state_ = Bus200eAppNS::WRITE_FAIL;
    }
    const bool reported = (st == BUS200E_MASTER_FAILED);
    write_lost_ = !reported;   // nobody said why; don't invent an error code
    write_err_ = reported ? OC::PresetBus::MasterError() : BUS200E_MASTER_ERR_NONE;
    OC::PresetBus::MasterReset();
    return;
  }
  write_lost_ = false;
  if (st != BUS200E_MASTER_DONE) return;

  if (!verifying) {
    // The restore reported DONE. That only means the bytes went out -- it says
    // nothing about what the module stored. Read the whole bank straight back
    // and compare it against the hash of what we built. Until that lands,
    // edited_ STAYS SET: claiming success before checking is exactly how a
    // failed write gets mistaken for a good one.
    OC::PresetBus::MasterReset();
    const int rc = OC::PresetBus::MasterBackup(target_);
    if (rc != 0) {
      write_state_ = Bus200eAppNS::WRITE_BAD;
      write_err_ = (Bus200eMasterError)(-rc);
      read_state_ = Bus200eAppNS::READ_FAIL;
      read_hash_ = 0;
      return;
    }
    write_state_ = Bus200eAppNS::WRITE_VERIFYING;
    write_err_ = BUS200E_MASTER_ERR_NONE;
    verify_started_ms_ = millis();
    return;
  }

  // Read-back complete. The image now holds whatever the module actually has.
  //
  // MasterReset() comes LAST, after every question about this job has been
  // asked -- how many bytes it moved, and what they were. PumpRead has the
  // same ordering. Retiring the job first would mean checking the result of
  // something already declared over.
  const uint32_t bank_len = ExpectedBankBytes();
  const uint8_t *img = OC::PresetBus::MasterCardImage();
  const uint32_t off = committed_off_;   // the hole THIS write patched

  // The read-back must have covered the WHOLE bank, not merely the edited
  // slot's window.
  //
  // This is the difference between VERIFIED meaning something and meaning
  // nothing. The read-back lands in the same image the restore was sourced
  // from, so any byte the module did not send back still holds the value we
  // intended -- and the whole-bank hash matches it happily. Requiring only
  // the slot window (as SlotWindowInBank does) let a read-back that stopped
  // after 2104 of 63120 bytes earn "WROTE + VERIFIED" on the strength of 3%
  // of the evidence, and then stamp read_hash_ with it, poisoning every
  // later diff. A short DONE is real: see the note in PumpRead about a
  // capture that came back exactly one record short while reporting success.
  //
  // HashResidentBank() has guarded the read path this way all along; the
  // verify path simply never got the same line.
  const bool covered =
      img && bank_len && Bus200eMasterBytesTransferred() >= bank_len &&
      (uint64_t)off + kBuchla251eSlotBytes <= (uint64_t)bank_len;

  const Buchla200eBankHash back =
      covered ? Buchla200eHashBank(img, bank_len, off, kBuchla251eSlotBytes)
              : Buchla200eBankHash{0, 0};

  verify_diff_ = 0;
  verify_covered_ = covered;
  verify_outside_ok_ = (covered && back.outside == intended_outside_);
  if (covered) {
    for (uint32_t i = 0; i < kBuchla251eSlotBytes; ++i)
      if (img[off + i] != intended_slot_[i]) ++verify_diff_;
  }

  if (covered && back.whole == intended_hash_) {
    // Every byte of all 30 slots is what we intended. This is the only path
    // that clears the edit flag.
    write_state_ = Bus200eAppNS::WRITE_OK;
    edited_ = false;
    read_state_ = Bus200eAppNS::READ_OK;
    read_ms_ = millis();
    read_addr_ = target_;
    read_type_ = (uint8_t)CurrentModuleType();
    read_hash_ = back.whole;
    DecodeSlotFromCardImage();
  } else {
    // The module does not hold what we sent. Say so loudly and keep edited_
    // set -- the working copy is still the user's intent, and it is now the
    // only place that intent exists.
    write_state_ = Bus200eAppNS::WRITE_BAD;
    if (covered) {
      read_state_ = Bus200eAppNS::READ_OK;
      read_ms_ = millis();
      read_addr_ = target_;
      read_type_ = (uint8_t)CurrentModuleType();
      read_hash_ = back.whole;   // truth, so a retry is diffed against reality
    } else {
      // Not enough of the bank came back to say anything about it. The image
      // is a blend of what the module sent and what we intended, so it is not
      // a usable baseline: invalidate the read and make the user re-Read.
      read_state_ = Bus200eAppNS::READ_FAIL;
      read_hash_ = 0;
    }
  }
  OC::PresetBus::MasterReset();
  // The verdict decides whether a recovery is on offer, so ask now rather
  // than opening the snapshot file on every draw.
  RefreshSnapshotFlag();
  // Always back to `keep` when a verdict lands. A cursor that remembered
  // UNDO would turn the next reflex encR into a 63,120-byte write.
  recover_cursor_ = 0;
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
  // Stay on the Gen screen. The loop here is "generate, look, adjust,
  // regenerate" -- twenty laps is normal -- and bouncing to the home screen
  // cost two presses of pure transport per lap, 40 for that session. The
  // round trip existed only because the home screen drew the stage strip and
  // this one did not; DrawGen now draws it in the two blank rows it already
  // had, so there is nothing left to go back for.
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

// The list cursor (list_top_) and the target (addr_) are one thing while
// encL scrolls, but they drift apart whenever addr_ is set some other way --
// entering the list from a module's home page, the remembered set landing
// straight on the home page at boot, encR editing the address by hand. Left
// unsynced, the first detent moved relative to row 0: from the 251e's home
// (last of three) one click down landed on the FIRST module, not the middle
// one. Bench 2026-09-02.
FLASHMEM __attribute__((noinline))
void AppBus200e::SyncListToAddr() {
  const int count = Buchla200eModuleCount();
  int nth = 0;
  for (int i = 0; i < count; ++i) {
    if (!IsFound(found_, i)) continue;
    if (Buchla200eModuleAt(i)->addr == addr_) { list_top_ = nth; return; }
    ++nth;
  }
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawModuleSelect() const {
  const char *model = Buchla200eModelForAddress(addr_);

  graphics.setPrintPos(0, 15);
  graphics.printf("Addr %02X", addr_);
  graphics.invertRect(28, 14, 14, 10);   // the encoder target has focus
  graphics.setPrintPos(48, 15);
  // The model is what the address means BY CONVENTION (header): a clone
  // squatting 0x28 lists as a 259 A. This used to be said with a '?' in
  // front of every name, here and on each responder row -- which, being on
  // every row, distinguished nothing and read as "unknown" / display fault
  // (Oren, at the bench: "there are ? in front of the module names"). A
  // worded hedge does not fit either: 13 columns remain after "Addr 5C" and
  // "285 FS A" alone is 8. So the name is plain and the address printed
  // beside it is the disclosure -- "5C 251 A" says exactly what is known,
  // which is also how Studio H's own manager presents it. Only an address
  // the table has no name for says so.
  graphics.print(model ? model : "unknown");

  graphics.setPrintPos(0, 26);
  if (scan_state_ != Bus200eAppNS::SCAN_IDLE) {
    graphics.printf("Scan %d/%d encL:stop", scan_index_,
                    Buchla200eModuleCount());
  } else if (probe_active_) {
    graphics.printf("Probe %02X ...", probe_addr_);
  } else if (probe_result_ >= 0) {
    graphics.printf("Probe %02X %s", probe_addr_,
                    probe_result_ ? "answered" : "silent");
  } else if (scan_kept_) {
    // Otherwise the list below looks exactly as it did before the scan and
    // the press appears to have done nothing.
    graphics.printf("Scan: silent, kept %d", found_count_);
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
    graphics.printf("%02X %s", e->addr, e->name);
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
    //
    // It starts at kStripTop-1, not -2: the text line above occupies rows
    // 22-29 and the old span began at row 29, so stage 1's marker (x=4, which
    // is column 0 of that text) ate the first character -- "Seq A" rendered
    // as "eq A" on the home screen and "Note 4" as "ote 4" in the editor.
    // Bars top out at kStripTop, so this still clears the tallest of them.
    if (Buchla251eHasEndMarker(s.stages[i]))
      graphics.drawVLine(x, kStripTop - 1, kStripH + 4);
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

// The tail of an in-flight line: " ..." until the first byte moves, then the
// bank's percentage. A real 251e read is ~11 s from the tap to the result
// (measured 2026-09-02: bytes done at 6.7 s, then the master waits for the
// module to fall quiet) and a write is that twice over; a line that does not
// change for 11 s reads as a hang, and the panel's only other feedback is
// the 251e's own display. The percent is bytes moved over the whole bank
// (Bus200eMasterBytesTransferred rebaselines per job, so the verify pass
// counts from 0 again), clamped, so "100%" means "every byte is in, waiting
// for the module to finish" -- which is exactly what that last stretch is.
//
// Sized for the longest name: "reading 285 FS A 100%" is 21 columns, the
// screen's width. Nothing else on this line gets longer.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawXferTail(const char *verb) const {
  uint32_t pct = 0;
#ifdef PRESET_BUS
  const uint32_t total = ExpectedBankBytes();
  const uint32_t moved = Bus200eMasterBytesTransferred();
  if (total && moved) pct = moved >= total ? 100 : moved * 100 / total;
#endif
  if (pct) graphics.printf("%s %s %lu%%", verb, TargetName(), (unsigned long)pct);
  else     graphics.printf("%s %s ...", verb, TargetName());
}

// Provenance line. This is the whole reason the screen is trustworthy: the
// target module's own panel will happily keep showing something else.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawReadState() const {
  graphics.setPrintPos(0, 46);

  // A refusal outranks everything: the user just pressed a button and needs
  // to know why nothing happened. It clears as soon as an action starts.
  if (last_refusal_ != BUCHLA200E_READ_OK) {
    graphics.print(Buchla200eReadBlockText(last_refusal_));
    graphics.invertRect(0, 45, 128, 10);
    return;
  }

  // A write in flight or just finished outranks the read line: it is the more
  // recent, and more consequential, thing that happened to the module.
  //
  // With ONE exception, below: a finished write does not outrank a live edit.
  // WRITE_OK means "the module holds what we sent", and the moment the user
  // edits again that claim is false -- the working copy and the module differ,
  // which is exactly what EDITED* exists to say. Leaving WROTE + VERIFIED up
  // suppressed that warning for the rest of the session, so the screen
  // asserted a verified match while ArmWrite was simultaneously reporting
  // "8 bytes change". It also froze the staleness clock, so LIVE Ns ago never
  // came back after the first write of a session.
  //
  // WRITE_BAD deliberately still outranks: a known-bad module is worth
  // shouting about even mid-edit, and its whole point is that it survives.
  const bool stale_ok = (write_state_ == Bus200eAppNS::WRITE_OK && edited_);

  switch (stale_ok ? Bus200eAppNS::WRITE_NONE : write_state_) {
    case Bus200eAppNS::WRITE_ACTIVE:
      DrawXferTail("WRITING");
      graphics.invertRect(0, 45, 128, 10);
      return;
    case Bus200eAppNS::WRITE_VERIFYING:
      // The bytes went out; this is the read-back. Named as its own phase so
      // "done sending" is never mistaken on screen for "confirmed stored".
      // "VERIFY", not "VERIFYING": the longest model name (285 FS A) plus
      // the dots has to fit 21 columns.
      DrawXferTail("VERIFY");
      graphics.invertRect(0, 45, 128, 10);
      return;
    case Bus200eAppNS::WRITE_OK:
      // Earned, not assumed: the whole bank was read back and hashed equal to
      // what was sent. This is the only state that says the write is done.
      graphics.print("WROTE + VERIFIED");
      return;
    case Bus200eAppNS::WRITE_BAD:
      // The worst outcome the app can report, and the only one where the
      // module's contents are known to be wrong rather than merely unknown.
      // "We could not check" comes FIRST and is its own message. A short
      // read-back leaves verify_outside_ok_ false, but that flag then means
      // "we never saw the other 29", not "the other 29 are damaged" -- and
      // reporting OTHER PRESETS! would send the owner auditing 29 presets
      // that are almost certainly fine, for a cause that was never observed.
      // The module's contents are UNKNOWN here, which is its own bad news.
      if (!verify_covered_)         graphics.print("BAD: read-back short");
      else if (!verify_outside_ok_) graphics.print("BAD: OTHER PRESETS!");
      else if (verify_diff_ == 1)   graphics.print("BAD: 1 byte wrong");
      else if (verify_diff_ > 0)    graphics.printf("BAD: %d bytes wrong", verify_diff_);
      else                          graphics.print("BAD: verify failed");
      graphics.invertRect(0, 45, 128, 10);
      return;
    case Bus200eAppNS::WRITE_FAIL:
      // "lost" is not the same as "the module rejected it": the transfer
      // vanished, so what the module now holds is unknown. Say that.
      if (write_lost_) graphics.print("WRITE LOST - reread");
      else             graphics.printf("WRITE FAILED (err %d)", (int)write_err_);
      graphics.invertRect(0, 45, 128, 10);
      return;
    default: break;
  }

  switch (read_state_) {
    case Bus200eAppNS::READ_ACTIVE:
      DrawXferTail("reading");
      break;
    case Bus200eAppNS::READ_OK:
      if (edited_) {
        // Loud on purpose: these bytes are NOT what the module holds.
        graphics.print("EDITED*");
        graphics.invertRect(0, 45, 46, 10);
      } else {
        // No "wire %d" any more, and the age is clamped to two characters.
        //
        // Both halves were the same bug. The old format's fixed overhead was
        // 17 of the 21 columns, so a 3-digit age (anything past 1m40) plus a
        // 2-digit wire index overflowed and the LAST character fell off the
        // screen -- turning "wire 10" into "wire 1", which is not obviously
        // truncated because it is a perfectly well-formed slot number. The
        // header already says "Slot 11" 33 pixels above; printing the same
        // preset's 0-indexed twin below it was a trap rather than a
        // disclosure, and it was the screen that precedes a 30-slot rewrite.
        DrawAge(millis() - read_ms_);
      }
      break;
    case Bus200eAppNS::READ_FAIL:
      if (read_lost_) {
        // Nobody reported an error -- the job was reset out from under us
        // (console command, USB bridge). Retrying is the right move.
        graphics.print("READ LOST - try again");
        graphics.invertRect(0, 45, 128, 10);
        break;
      }
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
  // Model at the left, slot at the right, as the home title lays them out.
  // "WRITE 285 FS A" is the widest the left side gets (14 columns, ends at
  // x=84); the slot starts at 86 so the two can never run together.
  graphics.setPrintPos(0, 13);
  graphics.printf("WRITE %s", TargetName());
  graphics.setPrintPos(86, 13);
  graphics.printf("Slot %d", slot_ + 1);
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
  // The other 29 are re-sent verbatim from the read, and the read-back after
  // the transfer proves all 30 landed. Both halves of that promise are stated
  // here because this is the screen where consent is actually given.
  //
  // States the blast radius and nothing more.
  //
  // This line has been wrong in both directions. It used to spend four columns
  // on "chkd" -- the only abbreviation of its kind in the firmware, on the one
  // screen that can least afford to be skimmed -- promising DETECTION, which a
  // reader hears as PROTECTION. It was then changed to "no undo", which was
  // true at the time and became false the moment the pre-write snapshot
  // landed: there IS an undo now, offered as encR:UNDO when a write ends BAD.
  //
  // It does not promise one either, because at the instant this screen is
  // drawn the snapshot does not exist yet -- CommitWrite takes it, after this
  // prompt is answered. Promising recovery before securing it is exactly the
  // kind of claim this codebase treats as a bug. The recovery offer belongs on
  // the BAD screen, where snap_here_ makes it a checked fact.
  graphics.print("29 others re-sent");

  graphics.setPrintPos(0, 56);
  graphics.print("encR:CONFIRM  encL:no");
}

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawModuleHome() const {
  using namespace Bus200eAppNS;
  const char *model = Buchla200eModelForAddress(target_);

  // Line 1: what we are talking to, and which preset. The model is the
  // address->table lookup (header); the address is printed beside it, which
  // is the honest statement -- "259e @30" says exactly what is known.
  //
  // History of the hedge glyph, so it is not re-added: it was "~", which is
  // NOT IN THE FONT (gfx_font_6x8.h holds 92 glyphs from 0x20 to '{', so
  // '~' indexed past the table and drew a noise block); then '?', which is
  // in the font but, on every screen, read as "unknown". See
  // DrawModuleSelect for why no glyph at all is the right amount of hedge.
  graphics.setPrintPos(0, 13);
  graphics.printf("%s @%02X", model ? model : "?", target_);
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

  // In the BAD state the ordinary action row is the wrong offer. The module
  // is known to hold something other than what was sent, and if a pre-write
  // snapshot survives, the only two sensible next moves are "put it back" and
  // "leave it". Edit/Gen/Rec are noise at that moment, and there is no room
  // for a sixth entry anyway -- the five already reach x=124 of 128.
  //
  // So the row becomes the recovery prompt, and it appears exactly when it is
  // actionable: never when there is nothing to restore, never on a good write.
  if (write_state_ == WRITE_BAD && snap_here_) {
    // Drawn as an action row of two, because that is what it replaces and
    // this app teaches exactly one grammar: encL is where you are, encR is
    // what it is. Reusing it means the recovery needs no new binding, puts
    // two DIFFERENT controls in the path to an irreversible 63,120-byte
    // write, and -- since the cursor starts on `keep` every time the verdict
    // lands -- makes the reflex encR press the harmless one.
    graphics.setPrintPos(0, 56);
    graphics.print("keep");
    if (recover_cursor_ == 0) graphics.invertRect(-1, 55, 26, 10);
    graphics.setPrintPos(36, 56);
    graphics.print("UNDO");
    if (recover_cursor_ == 1) graphics.invertRect(35, 55, 26, 10);
    return;
  }

  // Action row. 4px gaps, not 6: five entries at 6px spill to 132px on a
  // 128px screen.
  int x = 0;
  for (int i = 0; i < ACT_COUNT; ++i) {
    graphics.setPrintPos(x, 56);
    graphics.print(kActionNames[i]);
    const int w = (int)kActionWidths[i] * 6;
    if (i == action_) graphics.invertRect(x - 1, 55, w + 2, 10);
    x += w + 4;
  }
}

// Is there a snapshot for the module we are pointed at? Cheap enough to call
// on state changes, not cheap enough for every draw -- it opens a file.
FLASHMEM __attribute__((noinline))
void AppBus200e::RefreshSnapshotFlag() {
#ifdef PRESET_BUS
  uint8_t a = 0;
  uint32_t len = 0;
  snap_here_ = OC::PresetEngine::SnapshotInfo(&a, &len) && a == target_ &&
               len == ExpectedBankBytes();
#else
  snap_here_ = false;
#endif
}

// The recovery prompt. Same shape as the write confirm because it IS a write:
// 63,120 bytes go back on the wire, all 30 slots are rewritten again, and the
// module cannot refuse them. The only difference is which bytes.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawSnapConfirm() const {
  graphics.setPrintPos(0, 13);
  graphics.printf("UNDO write %s", TargetName());
  graphics.invertRect(0, 12, 128, 10);
  graphics.setPrintPos(0, 26);
  graphics.print("Puts back the bank");
  graphics.setPrintPos(0, 36);
  graphics.print("read before the write");
  graphics.setPrintPos(0, 46);
  graphics.print("Rewrites ALL 30 slots");
  graphics.setPrintPos(0, 56);
  graphics.print("encR:CONFIRM  encL:no");
}

// Load the snapshot back into the card image and send it. Deliberately reuses
// the ordinary write machinery from the send onward, so the read-back and the
// verdict work exactly as they do for any other write -- an undo that cannot
// be verified would be no better than the damage it is undoing.
FLASHMEM __attribute__((noinline))
void AppBus200e::CommitSnapshotRestore() {
#ifdef PRESET_BUS
  uint8_t *img = OC::PresetBus::MasterCardImage();
  const uint32_t bank_len = ExpectedBankBytes();
  if (!img || !bank_len) {
    // Say so and leave. Returning while still on the UNDO prompt made encR do
    // nothing visible, forever -- "a refusal the user cannot see is
    // indistinguishable from a dead button", which is the bug this whole app
    // was reported with.
    write_block_ = BUCHLA200E_WRITE_NO_IMAGE;
    write_state_ = Bus200eAppNS::WRITE_FAIL;
    write_lost_ = false;
    write_err_ = BUS200E_MASTER_ERR_NONE;
    screen_ = Bus200eAppNS::SCR_MODULE_HOME;
    return;
  }

  uint32_t got = 0;
  if (!OC::PresetEngine::LoadSnapshot(target_, img, bank_len, &got) ||
      got != bank_len) {
    // The snapshot is gone, truncated, or belongs to another address. Say so
    // and change nothing: a partial bank is worse than the damage.
    snap_here_ = false;
    write_state_ = Bus200eAppNS::WRITE_FAIL;
    write_lost_ = false;
    write_err_ = BUS200E_MASTER_ERR_NONE;
    screen_ = Bus200eAppNS::SCR_MODULE_HOME;
    return;
  }

  // From here it is an ordinary write: the image IS the intent.
  committed_off_ = 0;
  memcpy(intended_slot_, img, kBuchla251eSlotBytes);
  const Buchla200eBankHash h =
      Buchla200eHashBank(img, bank_len, 0, kBuchla251eSlotBytes);
  intended_hash_ = h.whole;
  intended_outside_ = h.outside;

  OC::PresetBus::MasterReset();
  if (OC::PresetBus::MasterRestore(target_) != 0) {
    write_state_ = Bus200eAppNS::WRITE_FAIL;
    write_lost_ = true;
    screen_ = Bus200eAppNS::SCR_MODULE_HOME;
    return;
  }
  write_state_ = Bus200eAppNS::WRITE_ACTIVE;
  write_started_ms_ = millis();
  screen_ = Bus200eAppNS::SCR_MODULE_HOME;
  serial_printf("200e: undo -- restoring %lu snapshot bytes to %02X\n",
                (unsigned long)bank_len, target_);
#endif
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

  // A preview of the sequence these parameters produce, in the band that was
  // blank (y=48..54). APPLY now stays on this screen, so this is the only
  // place the result is visible -- without it "generate, look, adjust" would
  // have nothing to look at. Compact geometry rather than DrawStageStrip's,
  // whose y=31..43 band is occupied here by the parameter rows.
  const Buchla251eSequence &s = working_slot_.sequences[seq_];
  const int base = 54, h = 6;
  graphics.drawHLine(kStripX, base, kBuchla251eStagesPerSequence * 2);
  uint8_t peak = 0;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
    if (s.stages[i].value > peak) peak = s.stages[i].value;
  const int full = (peak < kStripMinFullScale) ? kStripMinFullScale : (int)peak;
  for (int i = 0; i < kBuchla251eStagesPerSequence; ++i) {
    const int x = kStripX + i * 2;
    const int bh = ((int)s.stages[i].value * h) / full;
    if (bh > 0) graphics.drawVLine(x, base - bh, bh);
    // Same convention as the full strip: the loop marker crosses BELOW the
    // baseline, which a data bar never does. Starts AT base-h (row 48), not
    // above it: the GEN_REST row's glyphs occupy rows 44..51 and its cursor
    // invert 43..52, so a marker starting at row 47 -- which the default
    // 16-step pattern puts at x=34, right under "Rest 0" -- cut into the
    // text. Two rows below the baseline is still unmistakably not a bar.
    if (Buchla251eHasEndMarker(s.stages[i]))
      graphics.drawVLine(x, base - h, h + 2);
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
  // 20 chars, not 22: the old string was clipped mid-word to "...DIN, bu"
  // by the right edge -- losing the very word the line exists to say.
  graphics.print("USB host/dev DIN bus");

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

// "LIVE 45s ago" / "LIVE 2m ago" / "LIVE 9m+ ago" -- never more than 12
// columns, whatever the age. The bank's freshness is the one thing on this
// screen the user cannot check any other way, so the field must not be able
// to overflow the 21-column row and drop characters off the right edge.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawAge(uint32_t ms) const {
  const uint32_t s = ms / 1000u;
  if (s < 100)            graphics.printf("LIVE %lus ago", (unsigned long)s);
  else if (s < 600)       graphics.printf("LIVE %lum ago", (unsigned long)(s / 60u));
  else                    graphics.print("LIVE 9m+ ago");
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
    // Exactly the cursor row's 8-pixel cell -- not y-1 and not one row taller.
    // The old band stole the bottom pixel row of the cell ABOVE, which at
    // scroll 0 is the module identity line: it got a full-width white rule
    // welded to its baseline and "259 A @28 / Slot 1" vanished entirely. Any
    // label with a descender above the cursor (Morph, Warp, Sync) lost its
    // tail into the bar and read as "Morp" with a stray speck below.
    if (row == row_cursor_) graphics.invertRect(0, y, 124, kRow259eDY);
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
  // x=28, not 36: 28 + 16*6 = 124, inside the panel. At x=36 the string
  // ran to 132 and rendered as "encL:back/scrol".
  graphics.setPrintPos(28, 56);
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
  // Resume is where the scan set comes back: the config map is loaded by
  // then, which it is not when apps are constructed.
  if (event == OC::APP_EVENT_RESUME) LoadScanSet();

  if (event == OC::APP_EVENT_SUSPEND || event == OC::APP_EVENT_SCREENSAVER_ON) {
    if (scan_state_ != Bus200eAppNS::SCAN_IDLE) StopScan();
    // Never leave a write armed across a suspend or a screensaver: the
    // confirm prompt would be gone but the next encR press would still
    // commit a whole-bank write.
    // Both confirm screens, not just the write one. SCR_SNAP_CONFIRM commits
    // an equally destructive 63,120-byte transfer, and leaving it armed across
    // a suspend meant coming back to a prompt whose 350ms dead window had
    // expired hours ago -- so the FIRST encR press would ship it. The two
    // screens were built by different hands and only one of them had learned
    // the lesson stated in this comment.
    if (screen_ == Bus200eAppNS::SCR_WRITE_CONFIRM ||
        screen_ == Bus200eAppNS::SCR_SNAP_CONFIRM)
      screen_ = Bus200eAppNS::SCR_MODULE_HOME;
    // Likewise never leave the recorder armed: its ISR poll would keep
    // consuming note-ons that the user is playing at some other app, and
    // recorder_ holds a pointer into working_slot_.
    if (rec_armed_) RecStop();
  }
}

void AppBus200e::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

// Write the scan result out, once, at a moment when doing so is cheap and
// safe. See the note in PumpScan for why this is not done there.
//
// Conditions, all of them necessary:
//   - nothing of ours is on the wire (a scan, probe, read or write in flight
//     would be interrupted by the flash windows SaveAppData holds irq off for)
//   - we are not serving a card to a foreign master, so no third party's
//     transfer can tear while our I2C slave is masked
//   - the app ISR is quiet during the write, matching every other
//     SaveAppData() call site in the firmware
//   - PhzConfig's shared map is handed back afterwards, because
//     SaveGlobalSettings leaves GLOBALS.CFG resident and does not restore it
FLASHMEM void AppBus200e::ConsumeScanDirty() {
#ifdef PRESET_BUS
  if (!found_dirty_) return;
  if (scan_state_ != Bus200eAppNS::SCAN_IDLE || probe_active_) return;
  if (read_state_ == Bus200eAppNS::READ_ACTIVE || WriteBusy()) return;
  if (OC::PresetBus::CardServing()) return;

  found_dirty_ = false;

  // Two keys in GLOBALS.CFG, under the namespace the preset bus already owns.
  // A PhzConfig value is 8 bytes and kFoundBytes is 8, so the whole bitmap is
  // one value exactly -- no packing, no truncation, nothing to get wrong.
  uint64_t bits = 0;
  for (int i = 0; i < Bus200eAppNS::kFoundBytes; ++i)
    bits |= (uint64_t)found_[i] << (8 * i);

  // found_ indexes the MODULE TABLE, not raw addresses, so it only means
  // anything against the table it was built from. Store the table size beside
  // it and refuse the set if a firmware update changed it, rather than
  // silently reinterpreting old bits as different modules.
  // Only the table-size stamp. addr_/target_ are the app-data chunk's job;
  // duplicating them here gave RESUME a way to overwrite the live values.
  const uint64_t meta = (uint64_t)(uint8_t)Buchla200eModuleCount();

  OC::CORE::app_isr_enabled = false;
  PhzConfig::load_config();            // own GLOBALS.CFG before mutating it
  // Re-state the global settings before writing, exactly as every other
  // writer of this file does (Setup's bus-address and invert-display commits,
  // SaveGlobalSettings, persist_cur_slot).
  //
  // Without it, a scan on a VIRGIN module wrote a GLOBALS.CFG containing only
  // the two scan keys -- no METADATA_KEY, so global_settings.valid was false
  // on the next boot and the factory-reset prompt opened on every power-up
  // thereafter. The hole predates this code: it was masked because the scan
  // used to persist via SaveAppData(), which calls SaveGlobalSettings() as a
  // side effect. Moving off that sledgehammer removed the mask, which is
  // exactly the kind of thing a lighter-weight path has to pay attention to.
  OC::BuildGlobalSettingsValues();
  PhzConfig::setValue(Bus200eAppNS::kScanSetKey, bits);
  PhzConfig::setValue(Bus200eAppNS::kScanMetaKey, meta);
  PhzConfig::save_config();
  OC::CORE::app_isr_enabled = true;

  // Hand the shared map back. PhzConfig has ONE in-RAM map and every writer
  // is expected to leave it belonging to the active app; the next app to save
  // a file would otherwise inherit GLOBALS.
  OC::app_switcher.current_app()->DispatchAppEvent(OC::APP_EVENT_RESUME);
  serial_printf("200e: scan set persisted (%d modules)\n", found_count_);
#endif
}

// Read the scan set back. Called from Resume rather than Init because the
// config map is not loaded yet when apps are constructed.
FLASHMEM void AppBus200e::LoadScanSet() {
#ifdef PRESET_BUS
  // Own GLOBALS.CFG before reading it. This must NOT trust whatever map
  // happens to be resident.
  //
  // APP_EVENT_RESUME does not mean "your config is loaded" -- it is dispatched
  // from at least five places that each mean something different by it. Switch
  // in from Quadrants and the resident map is Quadrants' BANK file; arrive via
  // a preset recall and it is that slot's G section; persist_cur_slot,
  // PresetBusUI::persist_assignments and the clock pump all dispatch it after
  // re-owning GLOBALS for their own writes. Reading blind was worse than
  // useless: keys 0x820/0x821 decode as ordinary bank keys, so getValue can
  // SUCCEED on Quadrants data and hand back a garbage address.
  PhzConfig::load_config();

  // Claimed before the early returns: a set that is not there yet (or was
  // built in this session) must not make a LATER background RESUME "first".
  const bool first_resume = !landed_;
  landed_ = true;

  uint64_t bits = 0, meta = 0;
  if (!PhzConfig::getValue(Bus200eAppNS::kScanSetKey, bits)) return;
  if (!PhzConfig::getValue(Bus200eAppNS::kScanMetaKey, meta)) return;
  // found_ indexes the module TABLE, so the set only means anything against
  // the table it was built from.
  if ((uint8_t)(meta & 0xFF) != (uint8_t)Buchla200eModuleCount()) return;

  // addr_ and target_ are deliberately NOT restored here. They live in the
  // app-data chunk, which is per-preset and is the one source of truth for
  // them. Storing them here too meant every background RESUME -- a preset
  // recall settling, a trigger assignment being saved, a clock jack moving --
  // silently retargeted the app to whatever address was current when the scan
  // was last persisted, under a user who was aiming at something else.
  int count = 0;
  const int n = Buchla200eModuleCount();
  for (int i = 0; i < Bus200eAppNS::kFoundBytes; ++i)
    found_[i] = (uint8_t)(bits >> (8 * i));
  for (int i = 0; i < n; ++i) if (IsFound(found_, i)) ++count;
  found_count_ = count;
  list_top_ = 0;
  SyncListToAddr();
  if (count)
    serial_printf("200e: %d modules remembered; no scan needed\n", count);

  // Land on the module we were working on, not on the scan. The scan list is
  // the landing page only for a setup that has never been scanned; once a set
  // is remembered and the app-data target is in it, the home screen is where
  // the user left off. Once per power-up: RESUME is dispatched from many
  // places (see above), and a preset recall settling in the background must
  // not pull the user off the screen they are on.
  if (first_resume && count && screen_ == Bus200eAppNS::SCR_MODULE_SELECT
      && scan_state_ == Bus200eAppNS::SCAN_IDLE) {
    for (int i = 0; i < n; ++i) {
      if (Buchla200eModuleAt(i)->addr != target_ || !IsFound(found_, i)) continue;
      RefreshSnapshotFlag();
      screen_ = Bus200eAppNS::SCR_MODULE_HOME;
      break;
    }
  }
#endif
}

FLASHMEM void AppBus200e::Loop() {
  PumpScan();
  PumpProbe();
  PumpRead();
  PumpWrite();
  ConsumeScanDirty();   // no-op unless a scan just finished
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
    case Bus200eAppNS::SCR_SNAP_CONFIRM: DrawSnapConfirm(); return;
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

  // A refusal belongs to the press that caused it. Clear it here so the next
  // press starts clean -- the Start* calls below set it again if they refuse,
  // and a stale reason on screen would be its own kind of lie.
  last_refusal_ = BUCHLA200E_READ_OK;

  // --- armed undo: same two answers, same dead window ---------------------
  if (screen_ == Bus200eAppNS::SCR_SNAP_CONFIRM) {
    switch (event.control) {
      case OC::CONTROL_BUTTON_R:
        if (millis() - armed_ms_ < Bus200eAppNS::kConfirmDeadMs) break;
        CommitSnapshotRestore();
        break;
      case OC::CONTROL_BUTTON_L:
        screen_ = Bus200eAppNS::SCR_MODULE_HOME;
        break;
      default: break;   // face buttons inert, as on the write confirm
    }
    return;
  }

  // --- armed write: only two answers, and one of them is "no" -------------
  if (screen_ == Bus200eAppNS::SCR_WRITE_CONFIRM) {
    switch (event.control) {
      case OC::CONTROL_BUTTON_R:      // encR = confirm
        // The screen must have been visible long enough to read before it
        // will accept a yes.
        //
        // Without this the two-step guard was not a guard at all. The app
        // switcher is "hold A, press encR"; on the module home A ALONE arms
        // the write and here encR commits it, so the two halves of the most
        // common navigation chord are, in order, arm and commit. A fumbled
        // chord -- releasing A a few ms early -- committed 63,120 bytes to
        // another module with the confirm screen on screen for 51 ms.
        //
        // A second hole used to open from the other side: encR is "activate
        // the highlighted action" on the screen that opens this one, and
        // ACT_SAVE used to reach ArmWrite() from THAT same encR press --
        // making two presses of one button a completed write, arm and
        // confirm both on encR. Fixed by making the action row's Save entry
        // arm only through A (see CONTROL_BUTTON_UP under SCR_MODULE_HOME),
        // same as every other route into this screen: whichever door you
        // came in, arm is A and confirm is encR, never the same button twice.
        //
        // The window below still guards the surviving hazard, the fumbled
        // A+encR chord. Deliberately silent: a slip inside the window should
        // feel like nothing happened, and the user reads the screen and
        // presses again.
        if (millis() - armed_ms_ < Bus200eAppNS::kConfirmDeadMs) break;
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
    // A module we have no handler for gets NO consequential controls.
    // DrawModuleHome's default branch draws only "no handler for this module
    // type yet / encL:back", but this switch used to route the full 251e set
    // underneath it: encR launched a real whole-bank BACKUP with nothing at
    // all on screen to show for it, and A opened a WRITE dialog for a module
    // the app had just said it could not handle. is259 was the only exclusion,
    // so MODTYPE_UNKNOWN fell straight through to the 251e write path.
    const bool known = (CurrentModuleType() != Bus200eAppNS::MODTYPE_UNKNOWN);
    switch (event.control) {
      case OC::CONTROL_BUTTON_L:      // encL = back

        // Leaving is refused while a write is on the wire: PumpWrite bails on
        // !WriteBusy(), so navigating away would let the transfer finish with
        // no read-back, no verdict, and no record that it happened.
        if (WriteBusy()) {
          last_refusal_ = BUCHLA200E_READ_WRITE_IN_FLIGHT;
          break;
        }
        // Same reasoning, one step earlier: a write that ended BAD with a
        // snapshot still on flash is an open safety decision, and "keep" /
        // "UNDO" (the recovery row this screen draws in place of the
        // ordinary actions) is the ONLY place that decision gets made --
        // there is no other menu entry that reaches SCR_SNAP_CONFIRM.
        // Picking a different module resets write_state_ to WRITE_NONE (see
        // the target_ switch below), which makes the recovery row vanish
        // and leaves PBSNAP.BIN -- ONE snapshot slot, not a history -- ready
        // to be silently overwritten by the very next write, to any module.
        // Leaving without an answer must not be indistinguishable from
        // answering "keep".
        if (write_state_ == Bus200eAppNS::WRITE_BAD && snap_here_) {
          last_refusal_ = BUCHLA200E_READ_UNRESOLVED_WRITE;
          break;
        }
        screen_ = Bus200eAppNS::SCR_MODULE_SELECT;
        SyncListToAddr();
        break;
      case OC::CONTROL_BUTTON_DOWN:   // B = cycle sequence A-D
        // The 259e has no sequences, so this does nothing there rather than
        // being repurposed into a gesture nothing on screen advertises.
        if (!is259 && known)
          seq_ = (uint8_t)((seq_ + 1) % kBuchla251eSequencesPerSlot);
        break;
      case OC::CONTROL_BUTTON_UP:     // A = primary action (Save)
        // Arms only; nothing reaches the bus until the confirm screen is
        // answered. A 259e has no write path yet, so A does nothing there.
        if (!is259 && known) ArmWrite();
        break;
      case OC::CONTROL_BUTTON_R:      // encR = confirm/enter = run the action
        // While the recovery prompt is up it owns both encoder buttons --
        // the action row it replaced is not on screen, so running one of its
        // entries from a press the user aimed at "UNDO" would be acting on
        // something they cannot see.
        if (write_state_ == Bus200eAppNS::WRITE_BAD && snap_here_) {
          if (recover_cursor_ == 1) {
            screen_ = Bus200eAppNS::SCR_SNAP_CONFIRM;
            armed_ms_ = millis();
          } else {
            // keep: accept the module as it stands, ordinary action row back.
            // The snapshot is deliberately NOT discarded -- the user may
            // change their mind, and the block is already spent.
            write_state_ = Bus200eAppNS::WRITE_NONE;
          }
          break;
        }
        // Gated on `known` like A and B above. Without this the unknown-module
        // screen -- which draws only "no handler for this module type yet /
        // encL:back" -- still ran the full 251e action set underneath: encR
        // launched a real whole-bank BACKUP with nothing on screen to show
        // for it, and encL could walk action_ round to ACT_SAVE and reach
        // ArmWrite. The write guard did refuse that (zero expected bank
        // bytes), but it refused it as "Read was incomplete", which names
        // the wrong reason.
        if (!known) break;
        if (is259) {
          StartRead();               // the only action a 259e page has
        } else if (action_ == Bus200eAppNS::ACT_READ) {
          StartRead();
        } else if (action_ == Bus200eAppNS::ACT_EDIT) {
          // edit_stage_ is deliberately NOT reset here. The core loop is
          // Edit -> home -> Save -> Edit, and forgetting the cursor meant
          // re-walking it on every lap: stage 30 cost 29 encL detents each
          // time, with no acceleration, no wrap and no jump. The sequence
          // letter already persisted; the stage now does too.
          screen_ = Bus200eAppNS::SCR_EDIT;
        } else if (action_ == Bus200eAppNS::ACT_GEN) {
          screen_ = Bus200eAppNS::SCR_GEN;
        } else if (action_ == Bus200eAppNS::ACT_REC) {
          screen_ = Bus200eAppNS::SCR_REC;
        }
        // ACT_SAVE has no case here on purpose. Arming a whole-bank write
        // must come from a DIFFERENT physical input than the one that
        // confirms it (see the note above CONTROL_BUTTON_R under
        // SCR_WRITE_CONFIRM). A is the sole arm button, unconditionally, a
        // few lines up under CONTROL_BUTTON_UP -- so highlighting Save with
        // encL and pressing encR here is inert; A arms it regardless of
        // where this cursor sits, and encR only ever commits, on the confirm
        // screen A opens. Letting encR arm too, when the cursor happened to
        // be on Save, was exactly the one-button arm-and-confirm hole this
        // app cannot afford.
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
      // AFTER target_ moves, not before: RefreshSnapshotFlag asks whether a
      // snapshot exists for target_, so running it first answered about the
      // module we are leaving. It was self-correcting only by accident --
      // write_state_ is cleared on the same branch and PumpWrite refreshes
      // again -- which is not a property to rely on.
      RefreshSnapshotFlag();
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
        // An unsaved edit is the only place the user's intent exists, and
        // changing slot re-decodes working_slot_ from the card image, which
        // destroys it. One detent of the encoder you also PUSH to confirm --
        // the most likely accidental gesture on this screen -- used to erase
        // an edit silently, and worse, left edited_ set so the screen kept
        // claiming EDITED* over bytes identical to the module's.
        //
        // Refuse instead, and name both ways out. Nothing is lost by asking.
        if (edited_) {
          last_refusal_ = BUCHLA200E_READ_UNSAVED_EDIT;
          return;
        }
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
      // While the recovery prompt stands in for the action row, encL drives
      // ITS cursor -- same control, same meaning, different row.
      if (write_state_ == Bus200eAppNS::WRITE_BAD && snap_here_) {
        int c = (int)recover_cursor_ + (event.value > 0 ? 1 : -1);
        CONSTRAIN(c, 0, 1);
        recover_cursor_ = (uint8_t)c;
        return;
      }
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
    SyncListToAddr();   // dialing onto a responder puts the cursor on its row
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
