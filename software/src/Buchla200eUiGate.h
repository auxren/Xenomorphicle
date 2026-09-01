#ifndef BUCHLA200EUIGATE_H_
#define BUCHLA200EUIGATE_H_

#include <stdint.h>

#include "Bus200eMaster.h"

// ---------------------------------------------------------------------------
// Two decisions the 200e app's UI has to make, pulled out of the app so they
// can be tested without hardware -- same shape and reasoning as
// Buchla200eWriteGuard.h.
//
// Both exist because of one bug: pressing Read did nothing, said nothing, and
// once the app was in that state it never came back. Two separate defects fed
// it, and each gets a function here.
//
// 1. REFUSALS WERE SILENT. StartRead() had three bare `return`s -- scan
//    running, probe running, bus disabled. Pressing the button produced no
//    message, no state change, nothing on screen. A button press must always
//    produce a visible result, so the refusal is now a value the UI prints.
//
// 2. AN IN-FLIGHT JOB COULD STICK FOREVER. The pumps only acted on the two
//    terminal states (DONE/FAILED) and had no timeout, so any other outcome
//    left the app "in flight" permanently -- which then tripped (1) and made
//    the button dead. The live trigger is that the master FSM is SHARED: the
//    console 'm'/'x' commands and the USB SysEx bridge call the same
//    MasterBackup/MasterReset. A reset from any of them drops the FSM to IDLE
//    underneath a job the app believes it owns. Observed on the bench.
//
//    Bus200eMasterBackup() sets SENDING before returning 0 (see start_job in
//    Bus200eMaster.cpp), so the app never legitimately sees IDLE while it
//    holds an accepted job. IDLE therefore means "lost", not "not started
//    yet", and can be reported honestly rather than waited on.
// ---------------------------------------------------------------------------

// Why a Read was refused. Ordered most-fundamental-first, like the write
// guard: tell the user the thing they must fix, not a downstream symptom.
enum Buchla200eReadBlock : uint8_t {
  BUCHLA200E_READ_OK = 0,
  BUCHLA200E_READ_BUS_OFF,          // preset bus not enabled on this hardware
  BUCHLA200E_READ_IN_FLIGHT,        // this app already has a read running
  BUCHLA200E_READ_WRITE_IN_FLIGHT,  // a write is running; do not disturb it
  BUCHLA200E_READ_BUSY_SCAN,        // the scan owns the master FSM
  BUCHLA200E_READ_BUSY_PROBE,       // a single-address probe is in flight
  BUCHLA200E_READ_BAD_ADDR,         // address 0, or our own module address
};

struct Buchla200eReadContext {
  bool bus_enabled;
  bool read_active;
  bool write_active;
  bool scan_idle;
  bool probe_active;
};

Buchla200eReadBlock Buchla200eCheckRead(const Buchla200eReadContext &c);

// Short enough for a 128px line.
const char *Buchla200eReadBlockText(Buchla200eReadBlock b);

// What has become of a job the app started and is waiting on.
enum Buchla200eJobFate : uint8_t {
  BUCHLA200E_JOB_PENDING = 0,  // still running; keep waiting
  BUCHLA200E_JOB_DONE,
  BUCHLA200E_JOB_FAILED,       // the FSM reported failure; read its error
  BUCHLA200E_JOB_TIMEOUT,      // never reached a terminal state in time
  BUCHLA200E_JOB_LOST,         // FSM went idle underneath us (see header)
};

// The app's own backstop, deliberately LONGER than the master's internal
// safety net (BUS200E_MASTER_HARD_CAP_MS) so that in the normal case the
// master fails the job itself and the app reports that real error. This only
// fires when the master never gets to speak at all -- i.e. it stopped being
// pumped, or its state was moved by another caller in a way that is neither
// terminal nor idle. Derived from the master's constant so the two cannot
// drift apart.
#define BUCHLA200E_JOB_TIMEOUT_MS (BUS200E_MASTER_HARD_CAP_MS + 5000u)

// Query jobs (scan/probe) are far shorter than a whole-bank transfer: the
// send gate plus one bus turnaround. Same reasoning, sized to that path.
#define BUCHLA200E_QUERY_TIMEOUT_MS \
  (BUS200E_MASTER_QUERY_SEND_TIMEOUT_MS + BUS200E_MASTER_QUERY_REPLY_TIMEOUT_MS + 2000u)

Buchla200eJobFate Buchla200eJobProgress(Bus200eMasterState st,
                                        uint32_t elapsed_ms,
                                        uint32_t timeout_ms);

Buchla200eJobFate Buchla200eQueryProgress(Bus200eQueryState st,
                                          uint32_t elapsed_ms,
                                          uint32_t timeout_ms);

#endif  // BUCHLA200EUIGATE_H_
