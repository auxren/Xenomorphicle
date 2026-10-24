#ifndef CAL259ETEXT_H_
#define CAL259ETEXT_H_

#include <stdint.h>

#include "Cal259eFlow.h"

// The 259e run's Failed screen as plain text (the app draws it; the host test
// checks it). Five rows of at most 21 characters: row 0 the fail name, rows 1 and
// 2 the cause, row 3 what became of the 259e's record, row 4 the last word.
//
// commit_result is Cal259eFlow::commit_result() (0 no COMMIT verdict, 1 the module's
// status shows this record, else a Cal259eBus Error). What each means for the
// record:
//   0 or E_NO_SLOT  nothing was programmed: "Nothing was written."
//   1               "Record committed." (the status named the record put); but
//                   not on F_READBACK, where the record read back differs
//   E_COMMIT_REFUSED the module refused AFTER programming (31.2C4 checks the
//                   read-back last): a slot was consumed and may be torn
//   anything else   "Written: UNKNOWN."
struct Cal259eFailRows {
  char row[5][24];
};

void Cal259eFailText(Cal259eFlow::FailCode code, int commit_result, BackupWhy why,
                     uint8_t target, Cal259eFailRows *out);

#endif  // CAL259ETEXT_H_
