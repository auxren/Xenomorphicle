#pragma once
// ---------------------------------------------------------------------------
// 200e preset-bus transport: LPI2C1 slave on the Xenomorpher's I2C header
// (pins 18=SDA / 19=SCL, shared with the stock Wire master).
//
// Neither stock WireIMXRT nor teensy4_i2c can listen on the I2C general
// call (SCFGR1[GCEN] unused in both) — and every 200e command broadcasts on
// address 0x00 — so this is a small direct-register slave block on LPI2C1.
// The LPI2C master and slave engines are independent; stock Wire's master
// (fully polled, no IRQ use) keeps working for ScanI2C and our QUERY reply.
//
// ISR work is minimal: bus bytes/START/STOP land in an SPSC event ring.
// Task() (loop context) drains the ring into the Bus200e parser, dispatches
// save/recall into the PresetEngine, and answers QUERY by mastering the
// reply frame when the bus has been quiet.
//
// HARDWARE PREREQUISITE: the bus is 5V with system-side pull-ups; the
// i.MX RT pins are NOT 5V tolerant. A bidirectional level shifter
// (PCA9306 / 2x BSS138) is mandatory between the header and EDAC pins 8/9.
// ---------------------------------------------------------------------------
#include <stdint.h>

#include "Bus200eMaster.h"

namespace OC {
namespace PresetBus {

struct Stats {
  uint32_t isr_count;
  uint32_t starts;
  uint32_t stops;
  uint32_t bytes;
  uint32_t ring_ovf;       // events lost (frame poisoned downstream)
  uint32_t query_replies;
  uint32_t query_retries;
  uint32_t midi_rx;        // bus MIDI messages received
  uint32_t midi_rx_ovf;    // dropped: RX ring full
  uint32_t midi_tx;        // bus MIDI frames mastered onto the bus
  uint32_t midi_tx_drop;   // dropped: TX ring full or persistent arb loss
  uint32_t midi_tx_merged; // continuous controllers folded into a pending one
  // high-water marks (selftest): worst observed ring depths
  uint32_t ring_hw;
  uint32_t midi_rx_hw;
  uint32_t midi_tx_hw;
  // bus-stuck watchdog: times the bus was declared stuck / times the
  // recovery sequence brought BBF back down
  uint32_t bus_stuck;
  uint32_t bus_recovered;
};

#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)

void Init();               // call after Wire.begin() and PhzConfig load
void Task();               // call from loop()

bool Enabled();            // Init ran (I2C_Expansion hardware present)
bool RemoteEnabled();      // bus remote-enable state (parser)
void SetModuleAddress(uint8_t a);  // payload address; persisted by caller
void SetModuleAddressRuntime(uint8_t a);  // live only, no config write
uint8_t ModuleAddress();

// ---- bus-wide preset commands (commander mode) ----
// Broadcast the same general-call SAVE/RECALL frames a preset manager sends
// ([04][00][22][02/01][n]); every module on the bus acts, and the local
// PresetEngine is dispatched too (our own TX is invisible to our slave).
// Last-wins pending command, mastered from Task() behind the quiet gate.
void BroadcastSave(uint8_t slot);
void BroadcastRecall(uint8_t slot);

// WPM / preset-manager presence: probed as a master ACK test on address
// 0x50 every few seconds when the bus is quiet. Hot plug/unplug is normal.
bool WpmPresent();

// ---- 0x50 card serving ----
// Serve the storage-card address (32K image, PBCARD.BIN) so 200e modules on
// a WPM-less bus can BACKUP/RESTORE against us. HARD-GATED: refused while a
// WPM is present, self-tested at enable, never persisted (off every boot).
// Returns 0 on success; <0 = refused (WPM present / no memory / self-test).
int CardServeEnable(bool on);
bool CardServing();

// ---- foreign-module BACKUP/RESTORE (transient master; new) ----
// Capture (BACKUP) or write back (RESTORE) another 200e module's preset
// dump -- e.g. a 251e for a browser applet -- by transiently mastering the
// bus. This composes existing mechanisms rather than adding new hardware
// surface: MasterBackup claims a card address the same way manual card
// serving does (CardServeEnable's self-test applies either way), trying
// 0x50 first and falling back to 0x51 via Bus200eMasterFindFreeCard() if a
// live WPM already holds 0x50 (0x50's own WPM-presence gate is unchanged --
// still refused outright, never contested), then hands off to
// Bus200eMaster (see Bus200eMaster.h) to send the BACKUP/RESTORE frame and
// watch the resulting card traffic. UNRESOLVED DESIGN QUESTION: this reuses
// the SAME card image/file (PBCARD.BIN) as ordinary WPM-less card serving,
// so a foreign-module
// capture will overwrite whatever local-backup image was there. The USB
// bridge in front of this (Bus200eBridge.{h,cpp}) has since taken the
// narrowest available position rather than resolve it: it writes browser
// bytes into the in-RAM image but deliberately never marks it dirty, so a
// pushed dump is staged for the RESTORE and never flushed over the user's
// PBCARD.BIN. Giving foreign dumps their own image/file is still the real
// fix, and is still open.
//
// MasterBackup: returns 0 if accepted, <0 (negated Bus200eMasterError) if
// refused outright (a job is already running, or the card claim failed --
// WPM present, no memory, self-test). Poll MasterState()/MasterError()
// afterward; once MasterState() reports BUS200E_MASTER_DONE, the captured
// bytes are at MasterCardImage()[0 .. Bus200eMasterBytesTransferred())
// (image itself is 32K, valid only while CardServing()).
//
// MasterRestore: the caller must already have (re)populated the card image
// with the bytes to write back and be CardServing() before calling; returns
// <0 immediately otherwise. Bus200eBridge.cpp is the caller that does that
// today -- it CardServeEnable()s on PUT_DUMP, writes each verified
// DUMP_DATA chunk into MasterCardImage(), and only calls this once DUMP_END
// checks out. Nothing has been run against real hardware yet.
int MasterBackup(uint8_t mod_addr);
int MasterRestore(uint8_t mod_addr);
Bus200eMasterState MasterState();
Bus200eMasterError MasterError();
uint8_t *MasterCardImage();   // the 32K card buffer; NULL unless CardServing()
void MasterReset();           // acknowledge a DONE/FAILED result, back to IDLE
// hex-dump the last completed master transfer's bytes (0..
// Bus200eMasterBytesTransferred()) from MasterCardImage() -- NOT
// bytes_written/bytes_read from BusCardGetStats(), which are lifetime
// cumulative counters, not the size of any one transfer (see PresetBus.cpp).
void DumpCard();

// ---- module identification (QUERY; transient master; new) ----
// Ask ONE module at `mod_addr` who it is (bus command 0x1A) and capture the
// version string it answers with -- the mirror image of try_query_reply(),
// which is how we answer when a preset manager asks US. This is the missing
// direct way to find out WHICH module lives at an address: a BACKUP tells
// you a module is there and how many bytes it holds, not what it is.
//
// Needs no card and claims no address (unlike MasterBackup): the exchange is
// one mastered request frame and one reply frame, so it is safe to fire at
// an unknown address on a live bus. The reply comes back through the ordinary
// slave RX path -- our own general-call listener hears the module's answer
// and routes it via the parser's query_reply hook (see PresetBus.cpp's
// cb_query_reply) -- so nothing here polls the target.
//
// MasterQuery: returns 0 if accepted, <0 (negated Bus200eMasterError) if
// refused (a query already in flight, or mod_addr 0 = broadcast). Poll
// QueryReplyReady()/MasterQueryState() afterwards, or just watch the console
// -- Task() prints the answer once, as soon as it lands.
// UNVERIFIED: never yet run against a real module. See Bus200eMaster.h.
int MasterQuery(uint8_t mod_addr);
bool QueryReplyReady();        // a reply has been captured (state == DONE)
// Copy up to `cap` captured version bytes out; returns how many were written
// (0 = none). NOT NUL-terminated: the bytes are exactly what the module sent.
uint8_t MasterQueryVersion(uint8_t *out, uint8_t cap);
Bus200eQueryState MasterQueryState();
Bus200eMasterError MasterQueryError();
void MasterQueryReset();       // acknowledge a DONE/FAILED query, back to IDLE

// ---- bus MIDI ----
// TX: queue a message for mastering onto the bus (ISR-safe; sent from
// Task() when the bus is quiet). channel 1 -> 200e bus A, 2 -> bus B,
// anything else -> both. Realtime types (>= 0xF8) ignore channel/data.
void QueueMidiTx(uint8_t type, uint8_t channel, uint8_t d1, uint8_t d2);
// RX: drain one received bus MIDI message (status keeps the 200e bus mask
// in its low nibble). Call from the active app's MIDI poll context.
bool ReadMidiRx(uint8_t &status, uint8_t &d1, uint8_t &d2);
const Stats &GetStats();
void DebugDump();          // print status + decoded-command ring to Serial
void SetVerbose(bool on);
bool Verbose();

#else

inline void Init() {}
inline void Task() {}
inline bool Enabled() { return false; }
inline bool RemoteEnabled() { return false; }
inline void SetModuleAddress(uint8_t) {}
inline void SetModuleAddressRuntime(uint8_t) {}
inline uint8_t ModuleAddress() { return 0; }
inline void QueueMidiTx(uint8_t, uint8_t, uint8_t, uint8_t) {}
inline bool ReadMidiRx(uint8_t &, uint8_t &, uint8_t &) { return false; }
inline void BroadcastSave(uint8_t) {}
inline void BroadcastRecall(uint8_t) {}
inline bool WpmPresent() { return false; }
inline int CardServeEnable(bool) { return -1; }
inline bool CardServing() { return false; }
inline int MasterBackup(uint8_t) { return -1; }
inline int MasterRestore(uint8_t) { return -1; }
inline Bus200eMasterState MasterState() { return BUS200E_MASTER_IDLE; }
inline Bus200eMasterError MasterError() { return BUS200E_MASTER_ERR_NONE; }
inline uint8_t *MasterCardImage() { return nullptr; }
inline void MasterReset() {}
inline void DumpCard() {}
inline int MasterQuery(uint8_t) { return -1; }
inline bool QueryReplyReady() { return false; }
inline uint8_t MasterQueryVersion(uint8_t *, uint8_t) { return 0; }
inline Bus200eQueryState MasterQueryState() { return BUS200E_QUERY_IDLE; }
inline Bus200eMasterError MasterQueryError() { return BUS200E_MASTER_ERR_NONE; }
inline void MasterQueryReset() {}
inline const Stats &GetStats() { static Stats s = {}; return s; }
inline void DebugDump() {}
inline void SetVerbose(bool) {}
inline bool Verbose() { return false; }

#endif

}  // namespace PresetBus
}  // namespace OC
