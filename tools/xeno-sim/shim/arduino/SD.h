#ifndef XENOSIM_SD_H_
#define XENOSIM_SD_H_
// RAM-backed stand-in for the SD card (see FS.h). SDcard_Ready is left false
// by the simulator's boot, so the firmware takes its no-card path -- the same
// one a module with an empty slot takes.
#include <FS.h>
class SimSDClass : public FS {
public:
  bool begin(uint8_t = 0) { return false; }
  bool mediaPresent() { return false; }
};
extern SimSDClass SD;
#endif
