#ifndef BUCHLA200EFWVERSIONS_H_
#define BUCHLA200EFWVERSIONS_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// Firmware versions the 200e modules have SHOWN us, by bus address.
//
// There is no bus command that asks a module for its firmware revision. The
// one frame that carries it is the module's firmware-display broadcast,
// command 0x13 -- [0A 22 own 13] followed by seven text bytes, four version
// characters and three spaces ("30.7   ") -- and both module firmwares we
// have disassembled build it from exactly one place: a front-panel button
// held for ~3.5 s (251e: event 63 off timer slot 4; 259e: the same, and only
// while polling mode is on). Nothing on the bus can draw it out. The QUERY
// (0x1A) a scan uses gets 0x1C with a literal 0xFF back, and that is all.
//
// Studio H's WPM handles this the only way it can be handled: it listens.
// 2Wireless.ino receiveEvent() matches the DISPLAY_EVENT prefix 0x0A220013,
// stores the module address and the seven bytes in FRAM, and the homepage
// shows "Module 251 A" / "Firmware 30.7" -- the LAST module heard, one at a
// time. This cache does the same with a memory of more than one module, so
// the responder list can show every version it has ever been shown, and a
// user who wants to see a version holds the button on that module.
//
// Pure state, no hardware, host-tested. Fed from the bus layer's fw_version
// hook (PresetBus.cpp), read by the 200e Modules app, which also persists it
// beside the scan set and restores it on RESUME -- so like the WPM's FRAM
// copy a version survives a power cycle.
// ---------------------------------------------------------------------------

// What a real module sends: 4 version chars + 3 spaces.
#define BUCHLA200E_FWVER_LEN   7
// Distinct addresses remembered at once. A full 200e case is well under
// this; past it the entry heard longest ago is replaced.
#define BUCHLA200E_FWVER_SLOTS 16

// Record a version frame from `addr` (7-bit). `ver` need not be
// NUL-terminated; `n` bytes are taken, truncated to BUCHLA200E_FWVER_LEN and
// padded with spaces. n == 0 is ignored (a frame with no text says nothing).
// Last-wins per address. Bumps the generation only when something changed.
void Buchla200eFwVersionNote(uint8_t addr, const uint8_t *ver, uint8_t n);

// Copy the version for `addr` into `out` as a NUL-terminated string with
// trailing spaces trimmed and any non-printable byte shown as '.'. Returns
// the number of characters written (not counting the NUL), 0 when nothing
// has been heard from that address (out[0] is then NUL, if cap > 0).
int Buchla200eFwVersionGet(uint8_t addr, char *out, int cap);

bool Buchla200eFwVersionKnown(uint8_t addr);
int  Buchla200eFwVersionCount(void);

// Incremented on every Note() that changed the cache. A consumer that wants
// to react (persist, repaint, mark the address found) compares it with the
// value it last saw; Restore() does NOT bump it, so a load from storage is
// never mistaken for news from the bus.
uint32_t Buchla200eFwVersionGeneration(void);

// Storage form: one 64-bit word per address -- the seven bytes as sent, in
// the low 56 bits, and a non-zero marker in the top byte so a known entry
// is never 0. Pack() returns 0 for an unknown address. Restore() refuses a 0
// or unmarked word, and refuses to overwrite an address already heard from
// in this session: what the bus said since power-up beats what storage
// remembers. Returns true when the entry was taken.
uint64_t Buchla200eFwVersionPack(uint8_t addr);
bool     Buchla200eFwVersionRestore(uint8_t addr, uint64_t packed);

void Buchla200eFwVersionReset(void);

#endif  // BUCHLA200EFWVERSIONS_H_
