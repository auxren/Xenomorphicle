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
// All bus work (MasterQuery and its state polling) happens in Loop(), the
// non-ISR path, mirroring PresetBusUI::Task() and Bus200eBridge.
// ---------------------------------------------------------------------------

#include "../Buchla200eModuleTable.h"
#include "../PresetBus.h"

namespace Bus200eAppNS {

enum Screen : uint8_t {
  SCR_MODULE_SELECT = 0,
  SCR_MODULE_HOME,
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
  void DrawModuleSelect() const;
  void DrawModuleHome() const;
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

FLASHMEM __attribute__((noinline))
void AppBus200e::DrawModuleHome() const {
  const char *model = Buchla200eModelForAddress(target_);

  graphics.setPrintPos(0, 15);
  graphics.printf("Target %02X", target_);
  graphics.setPrintPos(0, 26);
  if (model) graphics.printf("probably %s", model);
  else       graphics.print("unknown address");

  // --- SEAM: per-module-type handlers hang off this switch ----------------
  // Identification is by address (see header), so this is a table lookup,
  // not a protocol answer. Adding a module type = add a ModuleType enum
  // value + a case here.
  Bus200eAppNS::ModuleType type = Bus200eAppNS::MODTYPE_UNKNOWN;
  if (model && model[0] == '2' && model[1] == '5' && model[2] == '1')
    type = Bus200eAppNS::MODTYPE_251E;

  graphics.setPrintPos(0, 38);
  switch (type) {
    case Bus200eAppNS::MODTYPE_251E:
      graphics.print("251e: seq tools");
      graphics.setPrintPos(0, 47);
      graphics.print("(next phase)");
      break;
    case Bus200eAppNS::MODTYPE_UNKNOWN:
    default:
      graphics.print("no handler yet");
      break;
  }

  graphics.setPrintPos(0, 57);
  graphics.print("L:back");
}

// --- app interface ---------------------------------------------------------

FLASHMEM void AppBus200e::Init() {
  addr_ = 0x5C;
  target_ = 0x5C;
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
    if (event.control == OC::CONTROL_BUTTON_L)
      screen_ = Bus200eAppNS::SCR_MODULE_SELECT;
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
      target_ = addr_;
      screen_ = Bus200eAppNS::SCR_MODULE_HOME;
      break;
    default: break;
  }
}

FLASHMEM void AppBus200e::HandleEncoderEvent(const UI::Event &event) {
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
