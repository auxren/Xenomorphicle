#ifndef BUS200EMASTER_H_
#define BUS200EMASTER_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// Transient bus-master orchestration for card-based BACKUP/RESTORE against a
// FOREIGN 200e module (e.g. capturing a 251e's preset dump for a browser
// applet). Nothing in PresetBus200e.cpp issues these commands -- it only
// ever reacts to them. This module is the piece that does: pick a card
// address nothing else on the bus owns, master the BACKUP/RESTORE command
// naming that address as the target module's destination, and watch the
// resulting card traffic (served by PresetBusCard, elsewhere) to know when
// the transfer is done.
//
// BSP-free and host-testable, same split as PresetBus200e/PresetBusCard: no
// hardware access here. The Bus200eMasterOps callbacks below are the entire
// transport surface; PresetBus.cpp supplies the real Wire/BusCardStats-backed
// implementation (see MasterBackup/MasterRestore there) and is the only
// caller compiled against real hardware. Every FSM decision -- frame
// building, retry/timeout bookkeeping, activity-based done-detection -- is
// exercised on host in test_bus200e_master.cpp against a fake ops table.
//
// The bus's electrical safety (level shifting) has since been verified on
// real hardware, and this project's existing tx_gate_open()/suppress-echo
// master pattern (PresetBus.cpp: pump_broadcast, pump_midi_tx, the QUERY
// reply) is proven-obeyed by the physical case -- notably the sibling
// Orin_Fun rig's ember.py masters 0xFA/0xFC realtime bytes at the 251e's
// sequencer B transport through this exact QueueMidiTx/pump_midi_tx path,
// "proven obeyed" per its own commit history (2026-08-28). That is real
// corroboration for the gating/echo-suppression *mechanism* this module
// reuses. It is NOT corroboration for BACKUP/RESTORE specifically -- a
// different opcode, never bench-traced -- so this module's own timing
// constants below remain UNVERIFIED against a live BACKUP/RESTORE exchange:
//   - the timing constants below (how fast a real 225e/251e starts touching
//     the card after a BACKUP/RESTORE command, and how long its own
//     internal per-preset write/read gaps run). Generous and reviewable;
//     tune from a bench trace before relying on them against a live case --
//     this module was written and tested host-side only, never bench-run.
//   - the address model: today PresetBus.cpp's card-serving hardware can
//     only claim 0x50 (card_lo 0) -- SAMR ADDR1 is hardcoded to
//     BUS200E_CARD_BASE in slave_reconfig(). Bus200eMasterFindFreeCard()
//     here is written address-agnostic (any card_lo candidate list) so it
//     is ready if/when that hardware gate is parameterized; the real
//     adapter today only ever offers it the single candidate {0}.
// ---------------------------------------------------------------------------

#include "PresetBus200e.h"  // BUS200E_OP_BACKUP / BUS200E_OP_RESTORE

// FSM states, exposed for status reporting (UI, USB bridge, tests).
typedef enum {
  BUS200E_MASTER_IDLE = 0,
  BUS200E_MASTER_FINDING_CARD,   // probing candidate card addresses
  BUS200E_MASTER_SENDING,        // waiting for a quiet bus to master the cmd
  BUS200E_MASTER_WAIT_ACTIVITY,  // command sent; waiting for the target to
                                  // start touching the card
  BUS200E_MASTER_TRANSFERRING,   // card activity seen; waiting for it to
                                  // go quiet (transfer done)
  BUS200E_MASTER_DONE,
  BUS200E_MASTER_FAILED,
} Bus200eMasterState;

// Bus200eMasterLastError() values while state == BUS200E_MASTER_FAILED.
typedef enum {
  BUS200E_MASTER_ERR_NONE = 0,
  BUS200E_MASTER_ERR_BUSY,          // a job was already active
  BUS200E_MASTER_ERR_BAD_ARGS,      // ops incomplete, or a bad request
  BUS200E_MASTER_ERR_NO_FREE_CARD,  // every candidate card address is claimed
  BUS200E_MASTER_ERR_SEND_TIMEOUT,  // bus never went quiet / send kept failing
  BUS200E_MASTER_ERR_NO_RESPONSE,   // command sent, target never touched the card
} Bus200eMasterError;

typedef struct {
  // Millisecond clock, shared with the rest of the transport (PresetBus.cpp
  // feeds Bus200eSetNow() from the same source on target).
  uint32_t (*now_ms)(void);

  // True if the bus is quiet enough to safely master a frame right now
  // (mirrors PresetBus.cpp's tx_gate_open(): no RX in flight, no card
  // transfer window open, bus not wedged).
  int (*tx_gate_open)(void);

  // Probe one 7-bit card address (BUS200E_CARD_BASE | card_lo): 1 = already
  // ACKed by something else on the bus (claimed), 0 = free. May be a cached
  // presence flag (e.g. WpmPresent()) rather than a fresh bus probe -- the
  // caller decides the risk/staleness tradeoff; this module only consumes
  // the answer.
  int (*probe_card)(uint8_t card_addr7);

  // Master `n` bytes onto the general call (Wire.beginTransmission(0) +
  // write + endTransmission(), on target). Returns 0 on a clean ACK'd send,
  // nonzero on any I2C error (NAK, arbitration loss, ...).
  int (*send_frame)(const uint8_t *bytes, uint8_t n);

  // Register a just-sent frame for self-echo suppression (Bus200eSuppressFrame
  // on target) so our own slave doesn't re-parse what we just mastered.
  void (*suppress_echo)(const uint8_t *bytes, uint8_t n);

  // Monotonically increasing count of bytes the local card slave has moved
  // in the direction relevant to the active job (bytes_written for a
  // BACKUP -- the target is writing into our card; bytes_read for a
  // RESTORE -- the target is reading out of it). Only the delta matters;
  // this module never inspects the absolute value.
  uint32_t (*card_activity)(void);
} Bus200eMasterOps;

// Reset all state and store `ops`. All fields of `ops` must be non-NULL
// (NULL = untested/unsupported here, unlike PresetBus200e's log-only mode --
// there's no meaningful "master with no transport" behaviour).
void Bus200eMasterInit(const Bus200eMasterOps *ops);

// Probe candidates[0..n) via ops->probe_card(BUS200E_CARD_BASE | candidates[i])
// in order; the first one that comes back free (0) is written to *out_card_lo
// and this returns 1. Returns 0 if every candidate is claimed (or n == 0).
// Pure lookup -- does not touch FSM state, safe to call any time.
int Bus200eMasterFindFreeCard(const uint8_t *candidates, uint8_t n,
                               uint8_t *out_card_lo);

// Start a BACKUP (target module -> our card at card_addr) or RESTORE (our
// card -> target module) job. card_lo must already be a card address this
// module is (about to be) serving -- callers choose it via
// Bus200eMasterFindFreeCard() beforehand, or a known-good constant while the
// hardware only offers one candidate. Returns 0 if accepted (job now
// running), <0 (a Bus200eMasterError, negated) if refused outright: busy,
// or ops incomplete.
int Bus200eMasterBackup(uint8_t mod_addr, uint8_t card_lo);
int Bus200eMasterRestore(uint8_t mod_addr, uint8_t card_lo);

// Pump the FSM. Call every loop tick (or every host-test tick) once a job is
// running; a no-op while IDLE/DONE/FAILED.
void Bus200eMasterTask(void);

Bus200eMasterState Bus200eMasterGetState(void);
Bus200eMasterError Bus200eMasterLastError(void);
uint8_t Bus200eMasterCardAddr(void);   // card_lo of the active/last job
uint8_t Bus200eMasterModAddr(void);    // target module of the active/last job
int Bus200eMasterIsRestore(void);      // 1 = RESTORE, 0 = BACKUP, of the active/last job

// Acknowledge a DONE/FAILED result and return to IDLE. Starting a new job
// (Backup/Restore) also implicitly does this once the FSM allows it (only
// from DONE/FAILED/IDLE -- see Bus200eMasterError::BUSY).
void Bus200eMasterReset(void);

// Timing constants (see the UNVERIFIED note above). Exposed so tests and
// callers can reason about worst-case job duration without duplicating them.
#define BUS200E_MASTER_SEND_TIMEOUT_MS     2000  // give up trying to grab the bus
#define BUS200E_MASTER_ACTIVITY_TIMEOUT_MS 3000  // target must start touching
                                                  // the card within this long
#define BUS200E_MASTER_QUIET_DONE_MS       1500  // this much silence after
                                                  // activity started = done
                                                  // (matches PresetBus.cpp's
                                                  // own card-transfer holdoff)
#define BUS200E_MASTER_HARD_CAP_MS        15000  // absolute safety net

#endif  // BUS200EMASTER_H_
