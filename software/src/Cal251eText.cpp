// The 251e run's Failed screen text: see Cal251eText.h.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define C251T_CODE FLASHMEM
#else
#define C251T_CODE
#endif

#include "Cal251eText.h"

#include <stdio.h>

C251T_CODE void Cal251eFailText(Cal251eFlow::FailCode code, int cr, int id_ch, int id_in,
                                Cal251eFailRows *out) {
  using F = Cal251eFlow;
  const char *l1 = "", *l2 = "";
  char l1b[32], l2b[32], r3[24], r4[24];  // l1b/l2b: "%d" of an int is up to 11 chars
  snprintf(r3, sizeof(r3), "Nothing was written.");
  snprintf(r4, sizeof(r4), "Outputs released.");
  switch (code) {
    case F::F_NO_SELF_CAL: l1 = "Run Outputs first."; break;
    case F::F_INPUT_UNCAL: l1 = "Run Inputs first."; l2 = "(Xeno in 1-4 uncal)"; break;
    case F::F_NO_251E: l1 = "No 251e answered at"; l2 = "5C. On 30.7C4?"; break;
    case F::F_PATCH: l1 = "A loopback cable is"; l2 = "missing or wrong."; break;
    case F::F_LOOPBACK: l1 = "Xeno out vs in differ"; l2 = "by > 2.5 mV: no go."; break;
    case F::F_IDENTITY:
      if (id_ch >= 0) {
        snprintf(l1b, sizeof(l1b), "CV%d did not move", id_ch + 1);
        l1 = l1b;
        if (id_in >= 0) snprintf(l2b, sizeof(l2b), "input %d did instead.", id_in + 1);
        else snprintf(l2b, sizeof(l2b), "any input: no cable?");
        l2 = l2b;
      } else {
        l1 = "Patch check failed.";
      }
      break;
    case F::F_NO_OVERRIDE: l1 = "251e took no hold"; l2 = "(status bit1). 30.7C4?"; break;
    case F::F_NOISY: l1 = "Input reading noisy"; l2 = "(loose cable?)"; break;
    case F::F_DRIFT: l1 = "Readings drift over"; l2 = "1.5 mV: warm up more."; break;
    case F::F_INPUT_RANGE: l1 = "An input is too near"; l2 = "its top: not written."; break;
    case F::F_SCALE: l1 = "Output scale is > 4%"; l2 = "off 383.1 codes/V."; snprintf(r4, sizeof(r4), "See CALR,251LINE."); break;
    case F::F_FIT: l1 = "The fit failed (gain"; l2 = "or offset out of range)"; break;
    case F::F_NONLIN: l1 = "Non-linearity is over"; l2 = "3 mV (DAC INL)."; break;
    case F::F_RECORD: l1 = "Record fails the 10 V"; l2 = "check: not sent."; break;
    case F::F_VERIFY: l1 = "Corrected output is"; l2 = "over 4 mV off target."; break;
    case F::F_EXPIRED: l1 = "Readings over 10 min"; l2 = "old: measure again."; break;
    case F::F_INTERNAL: l1 = "Internal check failed"; l2 = "(nothing was sent)."; break;
    case F::F_NO_INPUT: l1 = "No input samples"; l2 = "(ADC silent)."; break;
    case F::F_BUS: l1 = "200e bus error or"; l2 = "no reply from 251e."; break;
    case F::F_COMMIT_REFUSED:
      l1 = "The 251e refused the"; l2 = "COMMIT (see below).";
      snprintf(r3, sizeof(r3), "Its page may be torn.");
      snprintf(r4, sizeof(r4), "Read =5Cs by hand.");
      break;
    case F::F_COMMIT_UNKNOWN: l1 = "COMMIT unknown: read"; l2 = "=5Cs by hand."; break;
    case F::F_READBACK: l1 = "Read-back differs:"; l2 = "read =5Cs by hand."; break;
    case F::F_CANCELLED: l1 = "Cancelled."; break;
    case F::F_WRITE_CAP: l1 = "20 COMMITs since"; l2 = "power-up: power-cycle."; break;
    default: break;
  }
  if (code != F::F_COMMIT_REFUSED) {
    if (cr == 1 && code != F::F_READBACK) {
      snprintf(r3, sizeof(r3), "Record committed.");
    } else if (cr == E_COMMIT_REFUSED) {
      snprintf(r3, sizeof(r3), "Its page may be torn.");
      snprintf(r4, sizeof(r4), "Read =5Cs by hand.");
    } else if (cr != 0 && cr != E_NO_SLOT && cr != E_BUS_BUSY && cr != E_INTERNAL) {
      snprintf(r3, sizeof(r3), "Written: UNKNOWN.");
    }
    // F_READBACK: the COMMIT reported OK but the record read back is not the one put, so what
    // the module holds is unknown ("Record committed." would be a lie).
  }
  snprintf(out->row[0], sizeof(out->row[0]), "%.21s", F::FailName(code));
  snprintf(out->row[1], sizeof(out->row[1]), "%.21s", l1);
  snprintf(out->row[2], sizeof(out->row[2]), "%.21s", l2);
  snprintf(out->row[3], sizeof(out->row[3]), "%.21s", r3);
  snprintf(out->row[4], sizeof(out->row[4]), "%.21s", r4);
}
