#ifndef CAL251ETEXT_H_
#define CAL251ETEXT_H_

#include <stdint.h>

#include "Cal251eFlow.h"

// The 251e run's Failed screen as plain text (the app draws it; the host test checks it). Five
// rows of at most 21 characters: row 0 the fail name, rows 1 and 2 the cause, row 3 what became
// of the 251e's record, row 4 the last word.
//
// commit_result is Cal251eFlow::commit_result() (0 no COMMIT verdict, 1 the module's status shows
// this record, else a Cal251eBus Error); identity_channel / identity_input are the flow's
// (F_IDENTITY: the channel whose own input did not follow, and the input that rose instead, -1
// none). What each commit_result means for the record:
//   0, E_BUS_BUSY, E_INTERNAL   nothing was programmed (the last two: the COMMIT frame never went
//                               on the wire): "Nothing was written."
//   1                           "Record committed." (not on F_READBACK, where the record read back
//                               differs)
//   E_COMMIT_REFUSED            the module refused, maybe after programming: its one page may be torn
//   anything else               "Written: UNKNOWN."
struct Cal251eFailRows {
  char row[5][24];
};

void Cal251eFailText(Cal251eFlow::FailCode code, int commit_result, int identity_channel,
                     int identity_input, Cal251eFailRows *out);

#endif  // CAL251ETEXT_H_
