// Pure permit/refuse logic for a 200e whole-bank write. See the header for
// why this is separated out and tested on its own.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define B200E_GUARD_CODE FLASHMEM
#else
#define B200E_GUARD_CODE
#endif

#include "Buchla200eWriteGuard.h"

B200E_GUARD_CODE
Buchla200eWriteBlock Buchla200eCheckWrite(const Buchla200eWriteContext &ctx) {
  // 4. Nothing else may be using the master FSM. Checked first because a
  // write started mid-transfer corrupts both.
  if (!ctx.master_idle) return BUCHLA200E_WRITE_BUSY;

  // 1. There must be a completed read this session...
  if (!ctx.have_read) return BUCHLA200E_WRITE_NO_READ;

  // ...OF THIS MODULE. Read 0x5C, retarget to 0x28, and the resident image is
  // a 251e bank about to be pushed into a 259e. Address alone is not enough:
  // a clone squatting an address could change type under the same number.
  if (ctx.read_addr != ctx.target_addr || ctx.read_type != ctx.target_type)
    return BUCHLA200E_WRITE_WRONG_MODULE;

  // 2. The read must have been complete. A short transfer means every record
  // after the truncation point is whatever else was in the 64K buffer.
  if (ctx.expected_bank_bytes == 0 ||
      ctx.bytes_transferred < ctx.expected_bank_bytes)
    return BUCHLA200E_WRITE_SHORT_READ;

  // 3. The image must still be there at write time -- CardServing() can drop
  // between the read and the Save.
  if (!ctx.card_serving || !ctx.image_valid)
    return BUCHLA200E_WRITE_NO_IMAGE;

  // A negative count is a diff that overflowed its patch buffer: treat it as
  // "too many to verify", never as "none".
  if (ctx.changed_bytes < 0) return BUCHLA200E_WRITE_SHORT_READ;

  // Nothing to do. Refuse rather than transfer 63,120 bytes for no reason --
  // every needless whole-bank write is another chance to corrupt one.
  if (ctx.changed_bytes == 0) return BUCHLA200E_WRITE_NO_CHANGES;

  return BUCHLA200E_WRITE_OK;
}

B200E_GUARD_CODE
const char *Buchla200eWriteBlockText(Buchla200eWriteBlock b) {
  switch (b) {
    case BUCHLA200E_WRITE_OK:           return "ok";
    case BUCHLA200E_WRITE_NO_READ:      return "Read the module first";
    case BUCHLA200E_WRITE_WRONG_MODULE: return "Other module's data";
    case BUCHLA200E_WRITE_SHORT_READ:   return "Read was incomplete";
    case BUCHLA200E_WRITE_NO_IMAGE:     return "Image gone - reread";
    case BUCHLA200E_WRITE_BUSY:         return "Bus busy";
    case BUCHLA200E_WRITE_NO_CHANGES:   return "No changes to write";
    default:                            return "blocked";
  }
}
