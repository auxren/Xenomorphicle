// The 259e run's Failed screen text: see Cal259eText.h.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define C259T_CODE FLASHMEM
#else
#define C259T_CODE
#endif

#include "Cal259eText.h"

#include <stdio.h>

#include "Cal259eBackup.h"

C259T_CODE void Cal259eFailText(Cal259eFlow::FailCode code, int cr, BackupWhy why, uint8_t target,
                                Cal259eFailRows *out, CalRec::Sanity sanity) {
  using F = Cal259eFlow;
  const char *l1 = "", *l2 = "";
  char r3[24], r4[24], l2b[24];
  const unsigned t = unsigned(target);
  snprintf(r3, sizeof(r3), "Nothing was written.");
  snprintf(r4, sizeof(r4), "Output A released.");
  switch (code) {
    case F::F_NO_259E: l1 = "No 259e answered at"; l2 = "28-2B. On 31.2C4?"; break;
    case F::F_SEVERAL_MOVED: l1 = "More than one 259e"; l2 = "moved: patch one."; break;
    case F::F_IDENTITY: l1 = "No 259e saw A rise."; l2 = "Wrong jack patched?"; break;
    case F::F_PATCH: l1 = "Meter and held volts"; l2 = "differ by > 50 mV."; break;
    case F::F_NOISY: l1 = "Meter reading noisy"; l2 = "(loose lead?)"; break;
    case F::F_SCALE:
      snprintf(l2b, sizeof(l2b), "%s", CalRec::SanityName(sanity));
      l1 = "ADC span check:"; l2 = l2b;
      snprintf(r3, sizeof(r3), "Scale T is unproven.");
      break;
    case F::F_FIT: l1 = "The fit failed (an"; l2 = "offset out of range)."; break;
    case F::F_VERIFY: l1 = "Verify residual"; l2 = "is over 3 cents."; break;
    case F::F_NONLIN: l1 = "Fit-point nonlinear-"; l2 = "ity is over 5 cents."; break;
    case F::F_EXPIRED: l1 = "Readings over 10 min"; l2 = "old: measure again."; break;
    case F::F_INTERNAL: l1 = "Internal check failed"; l2 = "(nothing was sent)."; break;
    case F::F_BACKUP:
      // Why the preset backup did not happen: three rows, the last "Nothing was
      // written." (or, with the firmware card served, how to recover).
      l1 = Cal259eBackupText(why, 0);
      l2 = Cal259eBackupText(why, 1);
      snprintf(r3, sizeof(r3), "%.21s", Cal259eBackupText(why, 2));
      break;
    case F::F_BUS: l1 = "200e bus error or"; l2 = "no reply from 259e."; break;
    case F::F_NO_SLOT:
      l1 = "All 8 calibration"; l2 = "slots are used;";
      snprintf(r3, sizeof(r3), "compaction disabled.");
      snprintf(r4, sizeof(r4), "Nothing was written.");
      break;
    case F::F_COMMIT_REFUSED:
      l1 = "The 259e refused the"; l2 = "COMMIT (see below).";
      snprintf(r3, sizeof(r3), "A slot may be used.");
      snprintf(r4, sizeof(r4), "Read =%02Xs and =%02Xr.", t, t);
      break;
    case F::F_COMMIT_UNKNOWN:
      snprintf(l2b, sizeof(l2b), "=%02Xs and =%02Xr.", t, t);
      l1 = "COMMIT unknown: read"; l2 = l2b;
      break;
    case F::F_READBACK:
      snprintf(l2b, sizeof(l2b), "read =%02Xr by hand.", t);
      l1 = "Read-back differs:"; l2 = l2b;
      break;
    case F::F_CANCELLED: l1 = "Cancelled."; break;
    case F::F_METER_LOST: l1 = "The meter was lost."; break;
    case F::F_METER_CONFIG: l1 = "The meter refused a"; l2 = "setup command."; break;
    case F::F_METER_TIMEOUT: l1 = "The meter stopped"; l2 = "answering."; break;
    case F::F_NO_METER: l1 = "No meter."; break;
    case F::F_NO_SELF_CAL: l1 = "Run Outputs first."; break;
    default: break;
  }
  if (code != F::F_BACKUP && code != F::F_NO_SLOT && code != F::F_COMMIT_REFUSED) {
    if (cr == 1 && code != F::F_READBACK) {
      snprintf(r3, sizeof(r3), "Record committed.");
    } else if (cr == E_COMMIT_REFUSED) {
      snprintf(r3, sizeof(r3), "A slot may be used.");
      snprintf(r4, sizeof(r4), "Read =%02Xs and =%02Xr.", t, t);
    } else if (cr != 0 && cr != E_NO_SLOT && cr != E_BUS_BUSY && cr != E_INTERNAL) {
      // (E_BUS_BUSY, E_INTERNAL: the COMMIT frame never went out, nothing was sent.)
      snprintf(r3, sizeof(r3), "Written: UNKNOWN.");
    }
    // F_READBACK: the COMMIT reported OK but the record read back is not the one
    // put, so what the module holds is unknown ("Record committed." would be a lie).
  }
  snprintf(out->row[0], sizeof(out->row[0]), "%.21s", F::FailName(code));
  snprintf(out->row[1], sizeof(out->row[1]), "%.21s", l1);
  snprintf(out->row[2], sizeof(out->row[2]), "%.21s", l2);
  snprintf(out->row[3], sizeof(out->row[3]), "%.21s", r3);
  snprintf(out->row[4], sizeof(out->row[4]), "%.21s", r4);
}
