// Pure UI-gating decisions for the 200e app. See the header for why these
// live outside the app class. No hardware, no Arduino.
#include "Buchla200eUiGate.h"

#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define B200E_GATE_CODE FLASHMEM
#else
#define B200E_GATE_CODE
#endif

B200E_GATE_CODE
Buchla200eReadBlock Buchla200eCheckRead(const Buchla200eReadContext &c) {
  // Order matters. "The bus is off" is not fixable by waiting, so it outranks
  // the busy cases; among those, report the app's own in-flight work before
  // background work the user may not have started deliberately.
  if (!c.bus_enabled) return BUCHLA200E_READ_BUS_OFF;
  if (c.read_active) return BUCHLA200E_READ_IN_FLIGHT;
  if (c.write_active) return BUCHLA200E_READ_WRITE_IN_FLIGHT;
  if (!c.scan_idle) return BUCHLA200E_READ_BUSY_SCAN;
  if (c.probe_active) return BUCHLA200E_READ_BUSY_PROBE;
  return BUCHLA200E_READ_OK;
}

B200E_GATE_CODE
const char *Buchla200eReadBlockText(Buchla200eReadBlock b) {
  switch (b) {
    case BUCHLA200E_READ_OK:               return "ok";
    case BUCHLA200E_READ_BUS_OFF:          return "bus off";
    case BUCHLA200E_READ_IN_FLIGHT:        return "read already running";
    case BUCHLA200E_READ_WRITE_IN_FLIGHT:  return "write running";
    // NOT "(L stops)". This text is drawn on the module home screen too, and
    // there encL means back-to-module-select, so the advice cost a silent
    // navigation and still did not stop the scan. The remedy belongs where it
    // is true: the module-select screen draws "Scan N/M encL:stop" in place.
    case BUCHLA200E_READ_BUSY_SCAN:        return "busy: scanning bus";
    case BUCHLA200E_READ_BUSY_PROBE:       return "busy: probe";
    case BUCHLA200E_READ_BAD_ADDR:         return "addr is us / zero";
    case BUCHLA200E_READ_UNSAVED_EDIT:     return "edited: Save or Read";
    default:                               return "refused";
  }
}

B200E_GATE_CODE
Buchla200eJobFate Buchla200eJobProgress(Bus200eMasterState st,
                                        uint32_t elapsed_ms,
                                        uint32_t timeout_ms) {
  if (st == BUS200E_MASTER_DONE) return BUCHLA200E_JOB_DONE;
  if (st == BUS200E_MASTER_FAILED) return BUCHLA200E_JOB_FAILED;
  // A job we started is SENDING before MasterBackup() even returns, so IDLE
  // cannot mean "not started yet" -- it means another caller reset the shared
  // FSM out from under us. Report that rather than waiting for a terminal
  // state that will never arrive.
  if (st == BUS200E_MASTER_IDLE) return BUCHLA200E_JOB_LOST;
  if (elapsed_ms >= timeout_ms) return BUCHLA200E_JOB_TIMEOUT;
  return BUCHLA200E_JOB_PENDING;
}

B200E_GATE_CODE
Buchla200eJobFate Buchla200eQueryProgress(Bus200eQueryState st,
                                          uint32_t elapsed_ms,
                                          uint32_t timeout_ms) {
  if (st == BUS200E_QUERY_DONE) return BUCHLA200E_JOB_DONE;
  if (st == BUS200E_QUERY_FAILED) return BUCHLA200E_JOB_FAILED;
  // Same reasoning as above: MasterQuery() leaves the query FSM non-idle on
  // acceptance, and StopScan()/StartProbe() both call MasterQueryReset(), so
  // one of those running mid-flight strands the other. IDLE = lost.
  if (st == BUS200E_QUERY_IDLE) return BUCHLA200E_JOB_LOST;
  if (elapsed_ms >= timeout_ms) return BUCHLA200E_JOB_TIMEOUT;
  return BUCHLA200E_JOB_PENDING;
}
