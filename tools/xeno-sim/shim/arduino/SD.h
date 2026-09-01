#ifndef XENOSIM_SD_H_
#define XENOSIM_SD_H_
// RAM-backed stand-in for the SD card (see FS.h).
//
// The card is ABSENT by default, which is the path a module with an empty slot
// takes, and `--sd-card` seats one. Both halves matter: the preset engine's
// storage routing used to depend on `SDcard_Ready`, so inserting a card made
// all 30 slots read "Empty preset" and pulling it brought them back. A
// simulator that can only ever be one of those two machines cannot see that
// class of bug at all -- the whole point is that the two must agree.
//
// The volume itself is FS's std::map either way; presence only decides whether
// begin() succeeds, i.e. whether the firmware sets SDcard_Ready.
#include <FS.h>

// Teensyduino's name for the T4.1's own card socket, which is the argument
// Main.cpp:522 passes. The value is not used here -- there is one socket.
#define BUILTIN_SDCARD 254

// Whether a card is seated. Read by begin()/mediaPresent(), set from the
// command line before boot -- never by firmware, which has no such control.
void SimSetCardPresent(bool present);
bool SimCardPresent();

class SimSDClass : public FS {
public:
  bool begin(uint8_t = 0) { return SimCardPresent(); }
  bool mediaPresent() { return SimCardPresent(); }
};
extern SimSDClass SD;
#endif
