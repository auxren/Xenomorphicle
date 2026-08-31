// Transient bus-master orchestration for foreign-module BACKUP/RESTORE.
// Pure logic -- no hardware includes; see Bus200eMaster.h for the contract.
#include <stddef.h>

// On target, keep this cold code out of ITCM; host builds compile it bare.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define MASTER_CODE FLASHMEM
#else
#define MASTER_CODE
#endif

#include "Bus200eMaster.h"

static const Bus200eMasterOps *ops;

static Bus200eMasterState state = BUS200E_MASTER_IDLE;
static Bus200eMasterError last_error = BUS200E_MASTER_ERR_NONE;

static uint8_t job_mod_addr;
static uint8_t job_card_lo;
static int     job_is_restore;

static uint32_t send_start_ms;          // job accepted -> SENDING timeout base
static uint32_t phase_start_ms;         // current-state entry time
static uint32_t activity_baseline;      // card_activity() sampled at send time
static uint32_t last_activity_val;
static uint32_t last_activity_change_ms;

static int ops_complete(const Bus200eMasterOps *o) {
  return o && o->now_ms && o->tx_gate_open && o->probe_card &&
         o->send_frame && o->suppress_echo && o->card_activity;
}

MASTER_CODE void Bus200eMasterInit(const Bus200eMasterOps *o) {
  ops = o;
  state = BUS200E_MASTER_IDLE;
  last_error = BUS200E_MASTER_ERR_NONE;
  job_mod_addr = 0;
  job_card_lo = 0;
  job_is_restore = 0;
  send_start_ms = 0;
  phase_start_ms = 0;
  activity_baseline = 0;
  last_activity_val = 0;
  last_activity_change_ms = 0;
}

MASTER_CODE int Bus200eMasterFindFreeCard(const uint8_t *candidates, uint8_t n,
                                           uint8_t *out_card_lo) {
  if (!ops || !ops->probe_card || !candidates || !out_card_lo) return 0;
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t addr7 = (uint8_t) ((BUS200E_CARD_BASE | candidates[i]) & 0x7F);
    if (!ops->probe_card(addr7)) {
      *out_card_lo = candidates[i];
      return 1;
    }
  }
  return 0;
}

MASTER_CODE static int start_job(uint8_t mod_addr, uint8_t card_lo, int is_restore) {
  if (!ops_complete(ops)) return -BUS200E_MASTER_ERR_BAD_ARGS;
  if (state == BUS200E_MASTER_SENDING || state == BUS200E_MASTER_WAIT_ACTIVITY ||
      state == BUS200E_MASTER_TRANSFERRING)
    return -BUS200E_MASTER_ERR_BUSY;

  job_mod_addr = mod_addr & 0x7F;
  job_card_lo = card_lo & 0x7F;
  job_is_restore = is_restore;
  last_error = BUS200E_MASTER_ERR_NONE;
  send_start_ms = ops->now_ms();
  phase_start_ms = send_start_ms;
  state = BUS200E_MASTER_SENDING;
  return 0;
}

int Bus200eMasterBackup(uint8_t mod_addr, uint8_t card_lo) {
  return start_job(mod_addr, card_lo, 0);
}
int Bus200eMasterRestore(uint8_t mod_addr, uint8_t card_lo) {
  return start_job(mod_addr, card_lo, 1);
}

MASTER_CODE void Bus200eMasterTask(void) {
  if (!ops_complete(ops)) return;
  if (state != BUS200E_MASTER_SENDING && state != BUS200E_MASTER_WAIT_ACTIVITY &&
      state != BUS200E_MASTER_TRANSFERRING)
    return;

  const uint32_t now = ops->now_ms();

  switch (state) {
    case BUS200E_MASTER_SENDING: {
      if (now - send_start_ms > BUS200E_MASTER_SEND_TIMEOUT_MS) {
        state = BUS200E_MASTER_FAILED;
        last_error = BUS200E_MASTER_ERR_SEND_TIMEOUT;
        return;
      }
      if (!ops->tx_gate_open()) return;  // retry next Task() pass

      uint8_t f[BUS200E_XFER_FRAME_LEN];
      const uint8_t op = job_is_restore ? BUS200E_OP_RESTORE : BUS200E_OP_BACKUP;
      const int n = Bus200eBuildTransferFrame(op, job_mod_addr, job_card_lo, 0,
                                               f, sizeof(f));
      if (n < 0) {  // can't happen with a valid op, but fail closed
        state = BUS200E_MASTER_FAILED;
        last_error = BUS200E_MASTER_ERR_BAD_ARGS;
        return;
      }
      if (ops->send_frame(f, (uint8_t) n) != 0) return;  // NAK/arb loss: retry

      ops->suppress_echo(f, (uint8_t) n);
      activity_baseline = ops->card_activity();
      last_activity_val = activity_baseline;
      phase_start_ms = now;
      state = BUS200E_MASTER_WAIT_ACTIVITY;
      return;
    }

    case BUS200E_MASTER_WAIT_ACTIVITY: {
      const uint32_t act = ops->card_activity();
      if (act != activity_baseline) {
        last_activity_val = act;
        last_activity_change_ms = now;
        state = BUS200E_MASTER_TRANSFERRING;
        return;
      }
      if (now - phase_start_ms > BUS200E_MASTER_ACTIVITY_TIMEOUT_MS) {
        state = BUS200E_MASTER_FAILED;
        last_error = BUS200E_MASTER_ERR_NO_RESPONSE;
      }
      return;
    }

    case BUS200E_MASTER_TRANSFERRING: {
      const uint32_t act = ops->card_activity();
      if (act != last_activity_val) {
        last_activity_val = act;
        last_activity_change_ms = now;
      }
      if (now - last_activity_change_ms > BUS200E_MASTER_QUIET_DONE_MS) {
        state = BUS200E_MASTER_DONE;
        return;
      }
      if (now - send_start_ms > BUS200E_MASTER_HARD_CAP_MS) {
        // Safety net: activity never went quiet (a chatty bus, or our
        // quiet-detection missed a gap). We saw real transfer bytes move,
        // so call it done rather than fail a job that likely succeeded.
        state = BUS200E_MASTER_DONE;
      }
      return;
    }

    default:
      return;
  }
}

Bus200eMasterState Bus200eMasterGetState(void) { return state; }
Bus200eMasterError Bus200eMasterLastError(void) { return last_error; }
uint8_t Bus200eMasterCardAddr(void) { return job_card_lo; }
uint8_t Bus200eMasterModAddr(void) { return job_mod_addr; }
int Bus200eMasterIsRestore(void) { return job_is_restore; }

void Bus200eMasterReset(void) {
  if (state == BUS200E_MASTER_DONE || state == BUS200E_MASTER_FAILED)
    state = BUS200E_MASTER_IDLE;
}
