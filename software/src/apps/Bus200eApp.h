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
// READ ONLY, DELIBERATELY: this pass reaches MasterBackup and nothing else.
// MasterRestore is not reachable from any code path here. Writing
// permanently overwrites a real module's presets and gets its own pass with
// the arm-then-confirm gesture the console's `x` command established.
// ---------------------------------------------------------------------------

#include "../Buchla200eModuleTable.h"
#include "../Buchla251eSlotCodec.h"
#include "../Buchla259eSlotCodec.h"
#include "../PresetBus.h"

namespace Bus200eAppNS {

enum Screen : uint8_t {
  SCR_MODULE_SELECT = 0,
  SCR_MODULE_HOME,
};

// Home-screen action row. Only READ is implemented this pass; the rest are
// deliberate stubs so the row's shape (and the muscle memory) is settled
// before the phases that fill them in.
enum HomeAction : uint8_t {
  ACT_READ = 0,
  ACT_EDIT,
  ACT_GEN,
  ACT_REC,
  ACT_COUNT,
};

// A whole-bank MasterBackup, pumped from Loop().
enum ReadState : uint8_t {
  READ_NONE = 0,   // nothing read this session
  READ_ACTIVE,     // transfer in flight
  READ_OK,         // working_slot_ decoded from a completed read
  READ_FAIL,       // see read_err_
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
  "Read", "Edit", "Gen", "Rec"
};
static const uint8_t kActionWidths[ACT_COUNT] = {4, 4, 3, 3};  // chars

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
  // ISR context: nothing to do. Deliberately empty -- see header comment.
  void Controller() final {}
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
  Bus200eAppNS::ModuleType CurrentModuleType() const;
  void DrawModuleSelect() const;
  void DrawModuleHome() const;
  void DrawModule251e() const;
  void DrawModule259e() const;
  void DrawRow259e(int row, int y) const;
  void DrawTenths(int tenths, const char *suffix) const;
  void ScrollRows(int delta);
  void DrawStageStrip() const;
  void DrawReadState() const;
  int SeqEndStage() const;     // 0-indexed stage holding the end marker, or -1
  uint8_t SeqPeakRaw() const;
  int FoundIndexToTableIndex(int nth) const;
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
}

FLASHMEM __attribute__((noinline))
void AppBus200e::StopScan() {
  // Leave found_/found_count_ intact: an aborted scan keeps what it found.
  scan_state_ = Bus200eAppNS::SCAN_IDLE;
#ifdef PRESET_BUS
  OC::PresetBus::MasterQueryReset();
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
    return;
  }

  const Buchla200eModuleEntry *e = Buchla200eModuleAt(scan_index_);
  if (!e) { scan_state_ = Bus200eAppNS::SCAN_IDLE; return; }
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
    graphics.printf("Scan %d/%d  L:stop", scan_index_,
                    Buchla200eModuleCount());
  } else if (probe_active_) {
    graphics.printf("Probe %02X ...", probe_addr_);
  } else if (probe_result_ >= 0) {
    graphics.printf("Probe %02X %s", probe_addr_,
                    probe_result_ ? "answered" : "silent");
  } else {
    graphics.printf("L:scan D:probe  %d hit", found_count_);
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

// Provenance line. This is the whole reason the screen is trustworthy: the
// target module's own panel will happily keep showing something else.
FLASHMEM __attribute__((noinline))
void AppBus200e::DrawReadState() const {
  graphics.setPrintPos(0, 46);
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
  graphics.print("L:back");
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

  // Action row. Only Read does anything this pass.
  int x = 0;
  for (int i = 0; i < ACT_COUNT; ++i) {
    graphics.setPrintPos(x, 56);
    graphics.print(kActionNames[i]);
    const int w = (int)kActionWidths[i] * 6;
    if (i == action_) graphics.invertRect(x - 1, 55, w + 2, 10);
    x += w + 6;
  }
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
  graphics.print("L:back  encL:scroll");
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
  row_cursor_ = 0;
  row_top_ = 0;
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
  }
}

void AppBus200e::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

FLASHMEM void AppBus200e::Loop() {
  PumpScan();
  PumpProbe();
  PumpRead();
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
  if (screen_ == Bus200eAppNS::SCR_MODULE_HOME) DrawModuleHome();
  else                                          DrawModuleSelect();
}

FLASHMEM void AppBus200e::DrawScreensaver() const { DrawMenu(); }

FLASHMEM void AppBus200e::HandleButtonEvent(const UI::Event &event) {
  if (event.type != UI::EVENT_BUTTON_PRESS) return;

  if (screen_ == Bus200eAppNS::SCR_MODULE_HOME) {
    const bool is259 = (CurrentModuleType() == Bus200eAppNS::MODTYPE_259E);
    switch (event.control) {
      case OC::CONTROL_BUTTON_L:
        screen_ = Bus200eAppNS::SCR_MODULE_SELECT;
        break;
      case OC::CONTROL_BUTTON_DOWN:
        // Cycle A-D. Only four values, so a button beats an encoder and
        // leaves the left encoder free to drive the action cursor. The
        // 259e has no sequences, so this does nothing there rather than
        // being repurposed into a gesture nothing on screen advertises.
        if (!is259)
          seq_ = (uint8_t)((seq_ + 1) % kBuchla251eSequencesPerSlot);
        break;
      case OC::CONTROL_BUTTON_R:
        // Read is the only action wired up; the others are stubs on
        // purpose (see the HomeAction comment). On a 259e it is the only
        // action that exists at all.
        if (is259 || action_ == Bus200eAppNS::ACT_READ) StartRead();
        break;
      default: break;
    }
    return;
  }

  switch (event.control) {
    case OC::CONTROL_BUTTON_L:
      if (scan_state_ != Bus200eAppNS::SCAN_IDLE) StopScan();
      else                                        StartScan();
      break;
    case OC::CONTROL_BUTTON_DOWN:
      StartProbe();
      break;
    case OC::CONTROL_BUTTON_R:
      if (addr_ != target_) {
        // New module: anything previously read belongs to the old one.
        read_state_ = Bus200eAppNS::READ_NONE;
        edited_ = false;
      }
      target_ = addr_;
      screen_ = Bus200eAppNS::SCR_MODULE_HOME;
      break;
    default: break;
  }
}

FLASHMEM void AppBus200e::HandleEncoderEvent(const UI::Event &event) {
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
