#ifndef BUS200EBRIDGE_H_
#define BUS200EBRIDGE_H_

#include <stdint.h>

#include "Bus200eMaster.h"  // Bus200eMasterState / Bus200eMasterError
#include "Bus200eSysEx.h"

// ---------------------------------------------------------------------------
// The session layer that finally JOINS the two halves of the browser <-> 200e
// preset pipeline:
//
//   Bus200eSysEx.{h,cpp}   framing only -- pack/unpack/build/parse, no state
//   Bus200eMaster.{h,cpp}  bus mastering only -- BACKUP/RESTORE against a
//                          foreign module, no idea where the bytes come from
//   PresetBus.{h,cpp}      the card image those bytes live in (32K, valid
//                          only while CardServing())
//
// Before this file there was nothing in between: PresetBus.h's own comment on
// MasterRestore said "the caller must already have (re)populated the card
// image with the bytes to write back ... (there is no USB-bridge wiring here
// to do that yet)", and Bus200eSysEx.h's said "wiring a live device-side USB
// MIDI SysEx RX handler that calls into this is NOT done here". This module
// is exactly that missing middle: it owns the multi-message session state
// (PUT_DUMP -> DUMP_DATA* -> DUMP_END -> MasterRestore; GET_DUMP ->
// MasterBackup -> DUMP_DATA* -> DUMP_END) and the card-image reads/writes
// that neither neighbour would be right to own.
//
// Deliberately BSP-free and host-testable, exactly like Bus200eMaster: no
// USB, no MIDI library, no Arduino, no Wire. Everything platform-specific
// arrives through the Bus200eBridgeOps callback table below.
// Bus200eBridgeUsb.cpp supplies the real usbMIDI/OC::PresetBus-backed
// implementation on target; test_bus200e_bridge.cpp exercises every path
// here against a fake ops table.
//
// ---- what this is NOT ------------------------------------------------------
// This never touches PresetEngine::SaveSlot()/RecallSlot() or PresetBus's
// bus-wide broadcast SAVE/RECALL. Foreign-module BACKUP/RESTORE via card
// serving is an architecturally separate path (a transient master + a card
// image, not a general-call preset command), and it stays separate here.
//
// ---- protocol shape (the contract is Bus200eSysEx.h; this is the FSM) -----
//
//   host: GET_DUMP [mod_addr]
//   dev:  ACK [GET_DUMP, mod_addr]            (or NAK)
//         ... MasterBackup(mod_addr) runs over real bus time ...
//         DUMP_DATA [0, total, <packed>] ... DUMP_DATA [total-1, ...]
//         DUMP_END  [total, xor7]             (or NAK if it failed)
//
//   host: PUT_DUMP [mod_addr]
//   dev:  ACK [PUT_DUMP, mod_addr]            (or NAK)
//   host: DUMP_DATA [0, total, <packed>]  -> dev: ACK [DUMP_DATA, 0]
//         ...
//   host: DUMP_END [total, xor7]
//   dev:  ACK [DUMP_END, total]               (or NAK 6 on a bad transfer)
//         ... MasterRestore(mod_addr) runs over real bus time; the host
//             polls STATUS to learn how it ended ...
//
// DUMP_END's n_packets is a single 7-bit field, so a transfer is capped at
// 127 packets = BUS200E_BRIDGE_MAX_DUMP_BYTES raw bytes. That is far smaller
// than the 64K card image; a capture bigger than the cap is refused (NAK 6)
// rather than silently truncated. Raising it means a protocol version bump,
// not a constant change here. Concretely: a real 251e bank is 63120 bytes,
// so a whole-bank 251e dump does NOT fit this transport and will be NAK 6'd
// -- the bridge is for small banks until that version bump happens.
//
// ---- The card-image offset assumption: now CONFIRMED zero ------------------
// This module reads a capture from card image offset 0 and writes a pushed
// dump back to offset 0, inherited from PresetBus.cpp's DumpCard(). That was
// an untested guess; a real 251e BACKUP (bus 0x5C, 63120 bytes) has since
// settled it. Its 2104-byte record lattice lands at card addresses that are
// exact multiples of 2104 with no remainder -- the only base consistent with
// the observed bytes is 0 (strictly: base mod 32768 == 0, measured through a
// 32K image; a re-capture through the now-64K image pins it outright).
// So a 200e module doing a BACKUP does start at card address 0, and no base
// offset needs threading through here or DumpCard().
//
// NOTHING BELOW HAS BEEN RUN AGAINST REAL HARDWARE. The framing and this
// FSM are host-tested; the bus side inherits Bus200eMaster.h's own UNVERIFIED
// timing caveats on top of the offset question above.
// ---------------------------------------------------------------------------

// DUMP_END carries n_packets in one 7-bit byte.
#define BUS200E_BRIDGE_MAX_PACKETS 127
#define BUS200E_BRIDGE_MAX_DUMP_BYTES \
  (BUS200E_BRIDGE_MAX_PACKETS * BUS200E_SYSEX_CHUNK_BYTES)  // 5588

// A half-finished PUT_DUMP would otherwise pin the session forever (and lock
// out every later GET_DUMP): a browser tab closing mid-transfer is normal.
#define BUS200E_BRIDGE_RX_TIMEOUT_MS 5000

// How many DUMP_DATA frames one Bus200eBridgeTask() call may push out. The
// whole stream is a few KB and usbMIDI's TX path is shared with the audio/
// MIDI foundation requirement, so it is metered across loop passes instead
// of blasted in one go.
#define BUS200E_BRIDGE_SEND_BUDGET 4

typedef enum {
  BUS200E_BRIDGE_IDLE = 0,
  BUS200E_BRIDGE_CAPTURING,  // GET_DUMP accepted; MasterBackup running
  BUS200E_BRIDGE_SENDING,    // capture done; streaming DUMP_DATA out
  BUS200E_BRIDGE_RECEIVING,  // PUT_DUMP accepted; collecting DUMP_DATA
  BUS200E_BRIDGE_RESTORING,  // DUMP_END was clean; MasterRestore running
} Bus200eBridgeState;

// Everything platform-specific. All fields must be non-NULL except
// card_mark_dirty (optional: builds with no persistence for the card image
// simply leave it out).
typedef struct {
  uint32_t (*now_ms)(void);

  // Card serving (PresetBus.h): the 32K image is the staging area for both
  // directions, and is only valid/addressable while CardServing().
  int (*card_serving)(void);            // 1 = serving
  int (*card_serve_enable)(int on);     // 0 = success (OC::PresetBus::CardServeEnable)
  uint8_t *(*card_image)(void);         // NULL unless serving
  uint32_t (*card_size)(void);          // BUSCARD_SIZE
  void (*card_mark_dirty)(void);        // may be NULL

  // Foreign-module bus mastering (PresetBus.h / Bus200eMaster.h). backup and
  // restore return 0 if accepted, <0 = negated Bus200eMasterError.
  int (*master_backup)(uint8_t mod_addr);
  int (*master_restore)(uint8_t mod_addr);
  Bus200eMasterState (*master_state)(void);
  Bus200eMasterError (*master_error)(void);
  uint32_t (*master_bytes)(void);       // Bus200eMasterBytesTransferred()
  void (*master_reset)(void);

  // Send one built message payload (the bytes BETWEEN F0 and F7 -- adding
  // those, and everything else about USB, belongs to the caller).
  void (*send_message)(const uint8_t *payload, uint32_t len);
} Bus200eBridgeOps;

// Store `ops` and reset all session state. Safe to call again to re-arm.
void Bus200eBridgeInit(const Bus200eBridgeOps *ops);

// Feed one complete received SysEx message. `sysex` may include the leading
// F0 and trailing F7 (as usbMIDI.getSysExArray() hands them over) or not --
// both are accepted. Messages that aren't ours (manufacturer/family/app ID
// mismatch) are ignored silently, matching hOC's stance for other apps'
// traffic. Loop context only: this parses, touches the card image, and can
// send replies. Never call it from an ISR.
void Bus200eBridgeHandleSysEx(const uint8_t *sysex, uint32_t len);

// Pump the session: watches the master FSM for a capture/restore finishing,
// streams outbound DUMP_DATA, and times out a stalled inbound transfer. Call
// every loop tick; cheap while IDLE.
void Bus200eBridgeTask(void);

Bus200eBridgeState Bus200eBridgeGetState(void);
uint8_t Bus200eBridgeModAddr(void);    // target module of the active/last session
uint32_t Bus200eBridgeDumpBytes(void); // raw length of the last dump sent/received
uint8_t Bus200eBridgeLastNak(void);    // last Bus200eSysExNakReason sent (0 = none)

// Drop any in-flight session and return to IDLE without touching the master
// FSM (it has its own Reset). For the console/bench and for tests.
void Bus200eBridgeAbort(void);

#endif  // BUS200EBRIDGE_H_
