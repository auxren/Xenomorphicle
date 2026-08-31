#ifndef BUCHLA200EWRITEGUARD_H_
#define BUCHLA200EWRITEGUARD_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// Whether it is safe to push the resident card image back to a 200e module.
//
// WHY THIS IS ITS OWN FILE, AND PURE: MasterRestore transfers the ENTIRE bank
// -- all 30 slots -- not the one slot being edited. The card image is the unit
// of transfer. So a write built on a stale, short, or wrong-module image
// destroys 29 presets the user never touched, and there is no undo. That makes
// the permit/refuse decision the highest-consequence logic in the app, so it
// lives here as a pure function over a plain struct, with no bus, no UI and no
// hardware, and is exercised by test_buchla200e_write_guard.cpp.
//
// The short-read case is not theoretical: one real MasterBackup during
// bring-up returned 957 bytes instead of 990 -- exactly one record short --
// while reporting DONE with error=NONE. Harmless on a read. Unrecoverable on
// a write.
// ---------------------------------------------------------------------------

enum Buchla200eWriteBlock : uint8_t {
  BUCHLA200E_WRITE_OK = 0,
  BUCHLA200E_WRITE_NO_READ,       // nothing read this session
  BUCHLA200E_WRITE_WRONG_MODULE,  // image belongs to a different module
  BUCHLA200E_WRITE_SHORT_READ,    // transfer < a full bank
  BUCHLA200E_WRITE_NO_IMAGE,      // card image gone (CardServing dropped)
  BUCHLA200E_WRITE_BUSY,          // a scan/probe/read/write is in flight
  BUCHLA200E_WRITE_NO_CHANGES,    // diff is empty; nothing to send
};

// Everything the decision depends on, gathered by the caller. Deliberately
// plain data: the point is that this can be constructed in a test.
struct Buchla200eWriteContext {
  bool     have_read;            // a read COMPLETED this session
  uint8_t  read_addr;            // module address that read came from
  uint8_t  read_type;            // module type that read was decoded as
  uint8_t  target_addr;          // module we are about to write
  uint8_t  target_type;
  uint32_t bytes_transferred;    // Bus200eMasterBytesTransferred()
  uint32_t expected_bank_bytes;  // slots * record size for this module type
  bool     card_serving;         // CardServing()
  bool     image_valid;          // MasterCardImage() != NULL
  bool     master_idle;          // no scan/probe/read/write in flight
  int      changed_bytes;        // Buchla251eDiffSlot() result; <0 = overflow
};

// The order matters: report the most fundamental problem first, so the user
// is told "you have not read this module" rather than "nothing changed".
Buchla200eWriteBlock Buchla200eCheckWrite(const Buchla200eWriteContext &ctx);

// Short enough for a 128px line.
const char *Buchla200eWriteBlockText(Buchla200eWriteBlock b);

#endif  // BUCHLA200EWRITEGUARD_H_
