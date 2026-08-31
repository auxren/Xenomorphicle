#ifndef BUCHLA200EMODULETABLE_H_
#define BUCHLA200EMODULETABLE_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// Buchla 200e bus address -> module model, the ONLY identification mechanism
// this bus actually has.
//
// There is no type-discovery command. A QUERY (0x1A -> 0x1C reply) proves a
// module is present and answering at an address and nothing more: both a real
// 251e and a real 259e answer with the identical single payload byte 0xFF
// (bench-confirmed 2026-08-31, and not an echo -- cycling the request
// argument left the reply unchanged). Identification is therefore a lookup,
// exactly as Studio H's own WPM does it: `getDisplayMessage()` in
// 2Wireless.ino switches on the module address to produce the model name,
// and their GetPresets.html duplicates the same table as a dropdown. Their
// sendQuery() is dead code and they never parse the 0x1C reply at all.
//
// Recipe this supports: QUERY-scan the address space for presence, then map
// each responder through this table for a model name.
//
// THREE CAVEATS, all real:
//   1. An address says what a module PRESENTS AS, not what it is. Studio H's
//      own clone modules deliberately squat the address of whatever they
//      emulate (their DPO answers at 0x28 as a 259e; their 254e at
//      0x34/0x35 as a 256e; their 255e at 0x10/0x11 as a 257e; CSR at 0x20
//      as a 210e). Treat the model name as a strong hint, never a proof --
//      anything that must be certain (e.g. picking a preset-bank codec)
//      should still sanity-check the data it gets back.
//   2. This Xenomorpher's own default module address is 0x3C, which this
//      table calls a 281e C1. That collision is real and worth remembering
//      on a bus that has an actual 281e: our address is runtime-settable
//      (PresetBus::SetModuleAddress) precisely so it can be moved.
//   3. 0x22 is deliberately absent -- a 225s there would collide with the
//      preset manager's own source address, and Studio H commented it out of
//      their own dropdown for that reason.
//
// Table transcribed from Studio H's 2Wireless.ino getDisplayMessage() switch.
// Multiple instances of one model take consecutive addresses, suffixed A/B/C/D.
// ---------------------------------------------------------------------------

struct Buchla200eModuleEntry {
  uint8_t addr;
  char name[10];  // longest real entry is "285 BM A" (8 chars + NUL)
};

// Model name for a bus address, or nullptr if the address isn't in the table
// (which means "not a documented 200e module address" -- NOT "nothing is
// there". A responding address absent from this table is a real and
// interesting result: an undocumented or third-party module).
const char *Buchla200eModelForAddress(uint8_t addr);

// Whole-table access, for a scan UI that wants to walk known addresses
// rather than probe all 128.
int Buchla200eModuleCount();
const Buchla200eModuleEntry *Buchla200eModuleAt(int index);

#endif  // BUCHLA200EMODULETABLE_H_
