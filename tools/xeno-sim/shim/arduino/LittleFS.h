#ifndef XENOSIM_LITTLEFS_H_
#define XENOSIM_LITTLEFS_H_
// RAM-backed stand-in for LittleFS-on-program-flash (see FS.h).
#include <FS.h>
class LittleFS_Program : public FS {
public:
  bool begin(uint32_t size) { vol_.capacity = size; return true; }
  bool quickFormat() { vol_.files.clear(); return true; }
  bool format() { vol_.files.clear(); return true; }
};
class LittleFS_QSPIFlash : public LittleFS_Program {};
#endif
