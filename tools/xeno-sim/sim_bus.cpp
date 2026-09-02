#include "sim_bus.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "Buchla251eSlotCodec.h"
#include "Buchla259eSlotCodec.h"
#include "PresetBus.h"     // real declarations; the definitions below are ours
#include "PresetEngine.h"  // where a bus-wide SAVE/RECALL lands here
#include "sim_host.h"      // SimNowMs, SimLog
#include "sim_capture.h"

namespace {

// The card image PresetBus serves. 64K, matching BUSCARD_SIZE on target.
constexpr size_t kCardSize = 65536;
uint8_t g_card[kCardSize];
bool g_serving = false;
bool g_bus_enabled = true;
bool g_quiet = false;
bool g_real_timing = false;
bool g_synthetic = false;

// Our own address on the bus: the Xenomorpher's default. The app skips it
// during a scan, so it has to be a real value, not 0.
constexpr uint8_t kSelfAddr = 0x3C;
uint8_t g_self_addr = kSelfAddr;

// --- the modules on the other end of the wire ------------------------------

struct SimModule {
  uint8_t addr;
  const char *label;
  std::vector<uint8_t> bank;   // empty: answers QUERY but serves no bank
};

std::vector<SimModule> g_modules;

// How the simulated modules treat a RESTORE. Faithful unless a test or the
// operator asks otherwise -- see SimBusSetWriteFault.
SimWriteFault g_write_fault = SIM_WRITE_FAITHFUL;
// See the note at the clear site: the fault applies to the first RESTORE only.
bool g_write_fault_once = false;

// Armed by a RESTORE under SIM_WRITE_SHORT_READBACK and consumed by the very
// next BACKUP, which is the firmware's post-write read-back. Only that one
// transfer is truncated: the user's FIRST read has to succeed or there is
// nothing to edit, and a re-Read after the failure has to succeed or the test
// could not tell "invalidated, ask again" from "wedged".
bool g_short_next_backup = false;

SimModule *FindModule(uint8_t addr) {
  for (auto &m : g_modules)
    if (m.addr == addr) return &m;
  return nullptr;
}

// --- fake wire state -------------------------------------------------------

bool g_query_pending = false;
uint32_t g_query_at = 0;
uint8_t g_query_from = 0;

bool g_xfer_pending = false;
bool g_xfer_restore = false;
uint32_t g_xfer_start = 0;      // when the target starts touching the card
uint32_t g_xfer_end = 0;        // when it stops
uint32_t g_xfer_bytes = 0;
// The card's byte counters, shaped exactly like the BusCardStats pair the
// firmware reads on hardware (see master_card_activity in PresetBus.cpp):
// FREE-RUNNING and cumulative, one for each direction, because the master
// takes a baseline when a job starts and works in deltas.
//
// This used to be a single counter that restarted at 0 for every transfer.
// Nothing did two transfers in a row until the write path started reading its
// own result back, so the second job's delta -- baseline 63120, counter back
// at 0 -- came out as 0 bytes, and a good write verified as BAD. The fake was
// wrong, not the firmware.
uint32_t g_card_bytes_read = 0;     // the target READ our card (a RESTORE)
uint32_t g_card_bytes_written = 0;  // the target WROTE our card (a BACKUP)
uint32_t g_xfer_base = 0;           // that counter when this transfer began

// Bench-shaped pacing for the fake modules. These are the SIMULATOR's guesses
// about how a real module behaves, not measured values -- the firmware's own
// timeouts (which they must stay inside) are the real constants and are linked,
// not copied. See README: transfer pacing is NOT faithful.
constexpr uint32_t kQueryReplyMs = 20;     // bus turnaround for a live module
constexpr uint32_t kXferStartMs = 200;     // command -> first card touch
constexpr uint32_t kXferBytesPerMs = 30;   // ~2.1s for a 63120-byte 251e bank

// --- bus MIDI RX ring ------------------------------------------------------

struct MidiMsg { uint8_t status, d1, d2; };
MidiMsg g_midi_rx[16];
int g_midi_head = 0, g_midi_tail = 0;

// --- log -------------------------------------------------------------------


// --- Bus200eMasterOps ------------------------------------------------------

uint32_t ops_now() { return SimNowMs(); }

// The simulator has no other bus traffic, so the TX gate is always open. A real
// bus is not this polite; see README.
int ops_gate_open() { return 1; }

// 0x50 is free (no WPM on the simulated bus).
int ops_probe_card(uint8_t) { return 0; }

void ops_suppress_echo(const uint8_t *, uint8_t) {}

// Same choice of counter the firmware makes on hardware, and for the same
// reason: which one moves depends on the job's direction.
uint32_t ops_card_activity() {
  return Bus200eMasterIsRestore() ? g_card_bytes_read : g_card_bytes_written;
}

int ops_send_frame(const uint8_t *b, uint8_t n) {
  // QUERY: [len=4][destAddr][0x22][0x1A][0xFF]
  if (n == 5 && b[3] == 0x1A) {
    const uint8_t dest = b[1];
    SimModule *m = FindModule(dest);
    if (m) {
      g_query_pending = true;
      g_query_at = SimNowMs() + kQueryReplyMs;
      g_query_from = dest;
    }
    // Absent address: nothing scheduled, so the firmware's own
    // BUS200E_MASTER_QUERY_REPLY_TIMEOUT_MS runs its full course.
    return 0;
  }

  // BACKUP/RESTORE: [len=7][0x00][0x22][0x04|0x05][mod][card_lo][off_lo][off_hi]
  if (n == 8 && (b[3] == 0x04 || b[3] == 0x05)) {
    const bool restore = (b[3] == 0x05);
    const uint8_t mod = b[4];
    SimModule *m = FindModule(mod);
    if (!m || m->bank.empty()) {
      // No such module, or one with no bank to serve: it never touches the
      // card, and the firmware's ACTIVITY_TIMEOUT reports NO_RESPONSE.
      SimLog("%s %02X: target never answers (no bank simulated)",
             restore ? "RESTORE" : "BACKUP", mod);
      return 0;
    }
    const uint32_t bytes = (uint32_t)m->bank.size();
    // How much of the bank this transfer actually moves. Equal to the bank
    // except under SIM_WRITE_SHORT_READBACK, below.
    uint32_t moved = bytes;
    if (restore) {
      // The simulated module STORES what the master sent, because that is what
      // a real one does -- and because the firmware now reads the bank straight
      // back to verify it. A restore that changed nothing here would make every
      // write in the simulator report BAD, which would be a lie about the
      // firmware rather than a fact about the module.
      const uint32_t rec = (bytes % kBuchla251eSlotBytes == 0)
                               ? (uint32_t)kBuchla251eSlotBytes
                               : (uint32_t)kBuchla259eRecordBytes;
      switch (g_write_fault) {
        case SIM_WRITE_IGNORE:
          break;
        case SIM_WRITE_DROP_TAIL:
          memcpy(m->bank.data(), g_card, bytes - rec);
          break;
        case SIM_WRITE_FLIP_FIRST:
          memcpy(m->bank.data(), g_card, bytes);
          m->bank[0] ^= 0x01;
          break;
        case SIM_WRITE_FLIP_LAST:
          memcpy(m->bank.data(), g_card, bytes);
          m->bank[bytes - 1] ^= 0x01;
          break;
        case SIM_WRITE_SHORT_READBACK:
          // The STORE is perfect. What breaks is the read-back that follows.
          memcpy(m->bank.data(), g_card, bytes);
          g_short_next_backup = true;
          break;
        case SIM_WRITE_FAITHFUL:
        default:
          memcpy(m->bank.data(), g_card, bytes);
          break;
      }
      SimLog("RESTORE %02X: %u bytes stored (%s)", mod, (unsigned)bytes,
             SimWriteFaultName(g_write_fault));
      // A module that mishandles ONE write and then behaves. Without this, a
      // fault is permanent and the UNDO that the pre-write snapshot exists for
      // can never be seen to work: the recovery write goes out through the
      // same broken module and lands as BAD again, so the one thing worth
      // proving -- that the bank really does come back -- is unreachable.
      // A transient is also the honest shape of the bug this recovers from.
      if (g_write_fault_once) g_write_fault = SIM_WRITE_FAITHFUL;
      SimLog("  SIMULATED ONLY. No hardware was written.");
    } else {
      if (g_short_next_backup) {
        // One record, then silence -- and NO error. The module simply stops
        // touching the card, so the master's activity watcher sees the wire
        // go quiet and calls the job DONE, exactly as it would after a
        // complete transfer. A short DONE is a real observed failure: see the
        // note in AppBus200e::DecodeSlotFromCardImage about a bench capture
        // that came back one record shy while reporting success.
        //
        // Only the first record lands, so the rest of the card image is still
        // the bytes the RESTORE was built from. That is the trap: the
        // untouched 97% agrees with the intent by construction, so a
        // whole-bank hash of the image says "the module holds what we sent"
        // on the strength of bytes the module never sent.
        g_short_next_backup = false;
        moved = (bytes % kBuchla251eSlotBytes == 0)
                    ? (uint32_t)kBuchla251eSlotBytes
                    : (uint32_t)kBuchla259eRecordBytes;
        if (moved > bytes) moved = bytes;
        memcpy(g_card, m->bank.data(), moved);
        SimLog("BACKUP %02X: served only %u of %u bytes, then went quiet "
               "(fault: read-back truncated)",
               mod, (unsigned)moved, (unsigned)bytes);
      } else {
        // The target writes its bank into our card image.
        memcpy(g_card, m->bank.data(), bytes);
        SimLog("BACKUP %02X: served %u bytes from capture", mod, (unsigned)bytes);
      }
    }
    g_xfer_pending = true;
    g_xfer_restore = restore;
    g_xfer_base = restore ? g_card_bytes_read : g_card_bytes_written;
    g_xfer_start = SimNowMs() + kXferStartMs;
    g_xfer_end = g_xfer_start + moved / kXferBytesPerMs + 1;
    g_xfer_bytes = moved;
    return 0;
  }

  return 0;
}

const Bus200eMasterOps kOps = {
  ops_now, ops_gate_open, ops_probe_card, ops_send_frame, ops_suppress_echo,
  ops_card_activity,
};

// --- synthetic fallback banks ----------------------------------------------

std::vector<uint8_t> SyntheticBank251e() {
  std::vector<uint8_t> bank((size_t)30 * kBuchla251eSlotBytes, 0);
  for (int slot = 0; slot < 30; ++slot) {
    Buchla251eSlot s;
    Buchla251eDecodeSlot(bank.data() + (size_t)slot * kBuchla251eSlotBytes, s);
    for (int q = 0; q < kBuchla251eSequencesPerSlot; ++q) {
      for (int i = 0; i < kBuchla251eStagesPerSequence; ++i)
        s.sequences[q].stages[i].value =
            (uint8_t)(36 + ((i * 7 + q * 5 + slot) % 25));
      Buchla251eSetEndMarker(s.sequences[q].stages[7 + q], true);
    }
    Buchla251eEncodeSlot(s, bank.data() + (size_t)slot * kBuchla251eSlotBytes);
  }
  return bank;
}

std::vector<uint8_t> SyntheticBank259e() {
  std::vector<uint8_t> bank((size_t)30 * kBuchla259eRecordBytes, 0);
  for (int slot = 0; slot < 30; ++slot) {
    Buchla259eSlot s;
    Buchla259eDecodeSlot(bank.data() + (size_t)slot * kBuchla259eRecordBytes, s);
    for (int p = 0; p < kBuchla259eParamCount; ++p)
      s.param[p] = (uint16_t)(((p * 211 + slot * 97) & 0x0FFF) << 4);
    s.mod_waveform = (uint8_t)(slot % 3);
    s.mod_freq_mode = (uint8_t)(slot % 3);
    s.engine_mode = (uint8_t)(slot % 4);
    s.mod_dest_mask = (uint8_t)(slot % 8);
    s.wave_button_target = (uint8_t)(slot & 1);
    s.red_timbre = (uint8_t)(slot % 10);
    s.green_timbre = (uint8_t)((slot + 5) % 10);
    Buchla259eEncodeSlot(s, bank.data() + (size_t)slot * kBuchla259eRecordBytes);
  }
  return bank;
}

}  // namespace

// ---------------------------------------------------------------------------
// public surface
// ---------------------------------------------------------------------------

// SimLog/SimLogLines now live in sim_host.cpp: one log for the whole
// simulator, so a bus line and a firmware Serial line interleave in order.


bool SimBusUsingSyntheticBanks() { return g_synthetic; }

void SimBusSetRealTiming(bool on) { g_real_timing = on; }
bool SimBusRealTiming() { return g_real_timing; }

void SimBusSetWriteFault(SimWriteFault f) { g_write_fault = f; }
void SimBusSetWriteFaultOnce(bool once) { g_write_fault_once = once; }
SimWriteFault SimBusWriteFault() { return g_write_fault; }

const char *SimWriteFaultName(SimWriteFault f) {
  switch (f) {
    case SIM_WRITE_IGNORE:      return "fault: stores nothing";
    case SIM_WRITE_DROP_TAIL:   return "fault: last record dropped";
    case SIM_WRITE_FLIP_FIRST:  return "fault: 1 byte wrong, first slot";
    case SIM_WRITE_FLIP_LAST:   return "fault: 1 byte wrong, last slot";
    case SIM_WRITE_SHORT_READBACK: return "fault: read-back truncated";
    case SIM_WRITE_FAITHFUL:
    default:                    return "faithful";
  }
}

void SimBusInit(const SimBusConfig &cfg) {
  g_bus_enabled = cfg.bus_enabled;
  g_real_timing = cfg.real_timing;
  memset(g_card, 0, sizeof(g_card));
  g_serving = false;
  g_short_next_backup = false;
  g_modules.clear();

  std::vector<uint8_t> b251, b259;
  std::string err;
  bool ok251 = SimLoadHexDump(cfg.capture_251e, b251, err);
  if (!ok251) {
    SimLog("251e capture unavailable: %s", err.c_str());
    b251 = SyntheticBank251e();
    g_synthetic = true;
  } else {
    SimLog("251e bank: %u bytes from %s", (unsigned)b251.size(),
           cfg.capture_251e.c_str());
  }
  bool ok259 = SimLoadHexDump(cfg.capture_259e, b259, err);
  if (!ok259) {
    SimLog("259e capture unavailable: %s", err.c_str());
    b259 = SyntheticBank259e();
    g_synthetic = true;
  } else {
    SimLog("259e bank: %u bytes from %s", (unsigned)b259.size(),
           cfg.capture_259e.c_str());
  }
  if (g_synthetic)
    SimLog("SYNTHETIC BANKS IN USE -- these bytes are invented, not captured.");

  g_modules.push_back({0x20, "210", {}});
  g_modules.push_back({0x28, "259 A", b259});
  g_modules.push_back({0x5C, "251 A", b251});

  Bus200eMasterInit(&kOps);
}

void SimBusTask() {
  const uint32_t now = SimNowMs();

  if (g_query_pending && (int32_t)(now - g_query_at) >= 0) {
    g_query_pending = false;
    // Every real 200e module answers with the same literal 0xFF -- a QUERY
    // proves presence, never identity. See Buchla200eModuleTable.h.
    const uint8_t ver = 0xFF;
    Bus200eMasterQueryReply(g_query_from, &ver, 1);
  }

  if (g_xfer_pending && (int32_t)(now - g_xfer_start) >= 0) {
    const uint32_t span = g_xfer_end - g_xfer_start;
    const uint32_t done = (now >= g_xfer_end) ? span : (now - g_xfer_start);
    const uint32_t moved = span ? (uint32_t)((uint64_t)g_xfer_bytes * done / span)
                                : g_xfer_bytes;
    (g_xfer_restore ? g_card_bytes_read : g_card_bytes_written) =
        g_xfer_base + moved;
    if (now >= g_xfer_end) g_xfer_pending = false;
  }

  Bus200eMasterTask();
  Bus200eMasterQueryTask();
}

bool SimBusBusy() {
  if (g_query_pending || g_xfer_pending) return true;
  const Bus200eMasterState s = Bus200eMasterGetState();
  if (s != BUS200E_MASTER_IDLE && s != BUS200E_MASTER_DONE &&
      s != BUS200E_MASTER_FAILED)
    return true;
  const Bus200eQueryState q = Bus200eMasterQueryGetState();
  return q == BUS200E_QUERY_SENDING || q == BUS200E_QUERY_WAITING;
}

void SimBusInjectMidiNote(uint8_t note, uint8_t vel) {
  const int next = (g_midi_head + 1) % 16;
  if (next == g_midi_tail) return;
  // Channel messages carry the 200e bus mask in the low nibble, which is why
  // AppBus200e::RecPollBusMidi() masks with 0xF0 before comparing.
  g_midi_rx[g_midi_head] = {(uint8_t)(0x90 | 0x01), note, vel};
  g_midi_head = next;
}

std::string SimBusStatusLine() {
  static const char *kMaster[] = {"idle", "find-card", "sending", "wait-act",
                                  "xfer", "DONE", "FAILED"};
  static const char *kQuery[] = {"idle", "sending", "waiting", "DONE", "FAILED"};
  char buf[160];
  snprintf(buf, sizeof(buf),
           "t=%us  master=%s(%u B)  query=%s  card=%s  timing=%s",
           (unsigned)(SimNowMs() / 1000u), kMaster[(int)Bus200eMasterGetState()],
           (unsigned)Bus200eMasterBytesTransferred(),
           kQuery[(int)Bus200eMasterQueryGetState()], g_serving ? "served" : "-",
           g_real_timing ? "real" : "fast");
  return buf;
}

// ---------------------------------------------------------------------------
// OC::PresetBus -- the shim. Declarations come from the real src/PresetBus.h;
// only the bodies are ours. Anything the app does not call is left undefined
// on purpose, so a new dependency shows up as a link error rather than as a
// silently wrong answer.
// ---------------------------------------------------------------------------

namespace OC {
namespace PresetBus {

// Boot and per-loop hooks. The simulator's own bus lives in SimBusTask(),
// which the runtime calls next to this one; these exist so the real call sites
// in the boot sequence and the loop stay where they are.
void Init() { SimLog("PresetBus::Init (simulated bus, self addr 0x%02X)", g_self_addr); }
void Task() {}

bool Enabled() { return g_bus_enabled; }

// The bus parser's remote-enable latch. Nothing here can send the frame that
// sets it, so the Setup app's bus page always shows it off.
bool RemoteEnabled() { return false; }

uint8_t ModuleAddress() { return g_self_addr; }
void SetModuleAddress(uint8_t a) { g_self_addr = a; SimLog("PresetBus: module address -> 0x%02X (not persisted)", a); }
void SetModuleAddressRuntime(uint8_t a) { g_self_addr = a; }

// MIDI out over the bus. Counted and logged like every other transmit path in
// the simulator, then dropped -- there is no wire.
void QueueMidiTx(uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2) {
  static unsigned n = 0;
  if (++n <= 8)
    SimLog("bus MIDI TX: type %02X ch%u %u %u (goes nowhere)", type, channel, d1, d2);
  else if (n == 9)
    SimLog("bus MIDI TX: ...further messages not logged");
}
bool CardServing() { return g_serving; }

int MasterQuery(uint8_t mod_addr) { return Bus200eMasterQuery(mod_addr); }
Bus200eQueryState MasterQueryState() { return Bus200eMasterQueryGetState(); }
Bus200eMasterError MasterQueryError() { return Bus200eMasterQueryLastError(); }
void MasterQueryReset() { Bus200eMasterQueryReset(); }
void MasterQuerySetQuiet(bool on) { g_quiet = on; }
bool MasterQueryQuiet() { return g_quiet; }

int MasterBackup(uint8_t mod_addr) {
  if (!g_bus_enabled) return -BUS200E_MASTER_ERR_BAD_ARGS;
  const uint8_t candidates[] = {0, 1};
  uint8_t card_lo = 0;
  if (!Bus200eMasterFindFreeCard(candidates, 2, &card_lo))
    return -BUS200E_MASTER_ERR_NO_FREE_CARD;
  g_serving = true;
  return Bus200eMasterBackup(mod_addr, card_lo);
}

int MasterRestore(uint8_t mod_addr) {
  if (!g_bus_enabled) return -BUS200E_MASTER_ERR_BAD_ARGS;
  // Mirrors the real one: the caller must already be serving a populated card.
  if (!g_serving) return -BUS200E_MASTER_ERR_NO_FREE_CARD;
  return Bus200eMasterRestore(mod_addr, Bus200eMasterCardAddr());
}

Bus200eMasterState MasterState() { return Bus200eMasterGetState(); }
Bus200eMasterError MasterError() { return Bus200eMasterLastError(); }
uint8_t *MasterCardImage() { return g_serving ? g_card : nullptr; }
void MasterReset() { Bus200eMasterReset(); }

// The preset overlay's two bus-wide actions. On the module these master a
// general-call SAVE/RECALL frame that every 200e in the case obeys, and
// dispatch the local PresetEngine as well. Only the local half exists here --
// nothing is put on any wire, because there is no wire -- but that half is the
// REAL PresetEngine, running against the RAM-backed file system, exactly as
// PresetBus.cpp:177-178 and :300-301 dispatch it.
void BroadcastSave(uint8_t slot) { PresetEngine::RequestSave(slot); }
void BroadcastRecall(uint8_t slot) { PresetEngine::RequestRecall(slot); }
bool BroadcastQueued() { return false; }   // the shim never waits for a wire

// No preset manager on the simulated bus, so the overlay's WPM dot stays
// hollow. Inventing one would change what STORE means -- with a WPM present
// the module is not the one holding the presets.
bool WpmPresent() { return false; }

// The I2C slave's counters. Every one of them is a count of something that
// happens on a real wire -- ISR entries, arbitration losses, ring overflows --
// so all of them are zero here, and the Setup app's bus statistics page is
// blank rather than plausible.
const Stats &GetStats() {
  static const Stats zero{};
  return zero;
}

bool ReadMidiRx(uint8_t &status, uint8_t &d1, uint8_t &d2) {
  if (g_midi_tail == g_midi_head) return false;
  status = g_midi_rx[g_midi_tail].status;
  d1 = g_midi_rx[g_midi_tail].d1;
  d2 = g_midi_rx[g_midi_tail].d2;
  g_midi_tail = (g_midi_tail + 1) % 16;
  return true;
}

}  // namespace PresetBus
}  // namespace OC
