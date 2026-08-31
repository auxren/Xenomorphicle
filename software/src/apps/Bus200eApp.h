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
  void DrawModuleSelect() const;
  void DrawModuleHome() const;
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

// Decode slot_ out of the resident card image. False if the image is gone
// (CardServing() dropped) or the transfer was too short to contain this
// slot -- in either case the caller must NOT present stale bytes as live.
FLASHMEM __attribute__((noinline))
bool AppBus200e::DecodeSlotFromCardImage() {
#ifdef PRESET_BUS
  const uint8_t *img = OC::PresetBus::MasterCardImage();
  if (!img) return false;

  const uint32_t off = (uint32_t)slot_ * (uint32_t)kBuchla251eSlotBytes;
  const uint32_t need = off + (uint32_t)kBuchla251eSlotBytes;
  // A short transfer would otherwise decode whatever else is in the 64K
  // buffer and draw it as this module's sequence.
  if (Bus200eMasterBytesTransferred() < need) return false;

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

  // --- SEAM: per-module-type handlers hang off this switch ----------------
  // Identification is by address (see header), so this is a table lookup,
  // not a protocol answer. Adding a module type = add a ModuleType enum
  // value + a case here.
  ModuleType type = MODTYPE_UNKNOWN;
  if (model && model[0] == '2' && model[1] == '5' && model[2] == '1')
    type = MODTYPE_251E;

  // Line 1: what we are talking to, and which preset. "~" because the
  // address only implies the model by convention -- clones squat addresses.
  graphics.setPrintPos(0, 13);
  graphics.printf("~%s @%02X", model ? model : "?", target_);
  graphics.setPrintPos(80, 13);
  graphics.printf("Slot %d", slot_ + 1);          // 1-indexed for humans

  if (type != MODTYPE_251E) {
    graphics.setPrintPos(0, 30);
    graphics.print("no handler for this");
    graphics.setPrintPos(0, 40);
    graphics.print("module type yet");
    graphics.setPrintPos(0, 56);
    graphics.print("L:back");
    return;
  }

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
    switch (event.control) {
      case OC::CONTROL_BUTTON_L:
        screen_ = Bus200eAppNS::SCR_MODULE_SELECT;
        break;
      case OC::CONTROL_BUTTON_DOWN:
        // Cycle A-D. Only four values, so a button beats an encoder and
        // leaves the left encoder free to drive the action cursor.
        seq_ = (uint8_t)((seq_ + 1) % kBuchla251eSequencesPerSlot);
        break;
      case OC::CONTROL_BUTTON_R:
        // Read is the only action wired up; the others are stubs on
        // purpose (see the HomeAction comment).
        if (action_ == Bus200eAppNS::ACT_READ) StartRead();
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
      int a = (int)action_ + event.value;
      CONSTRAIN(a, 0, Bus200eAppNS::ACT_COUNT - 1);
      action_ = (uint8_t)a;
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
