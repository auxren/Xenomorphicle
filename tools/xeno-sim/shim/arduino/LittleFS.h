#ifndef XENOSIM_LITTLEFS_H_
#define XENOSIM_LITTLEFS_H_
// RAM-backed stand-in for LittleFS-on-program-flash (see FS.h).
#include <FS.h>
#include <stdint.h>

// The protected surface PhzConfig's XenoFS subclass reaches into. The real
// LittleFS keeps lfs, config and mounted protected for exactly this use.
// Just enough littlefs API for XenoFS::rootLogBytes() to compile; a RAM
// volume has no metadata log, so lfs_dir_open fails and it reports 0.
struct lfs_t {};
struct lfs_mdir_t { uint32_t off = 0; };
struct lfs_dir_t { lfs_mdir_t m; };
// block_size is what XenoFS::blockBytes() reports; the preset engine's
// free-space guard divides by it. 4096 is the geometry the real XenoFS mounts.
struct lfs_config { uint32_t metadata_max = 0; uint32_t block_size = 4096; };
inline int lfs_dir_open(lfs_t *, lfs_dir_t *, const char *) { return -1; }
inline int lfs_dir_close(lfs_t *, lfs_dir_t *) { return 0; }

class LittleFS_Program : public FS {
public:
  bool begin(uint32_t size) { vol_.capacity = size; mounted = true; return true; }
  bool quickFormat() { vol_.files.clear(); return true; }
  bool format() { vol_.files.clear(); return true; }
protected:
  bool mounted = false;
  lfs_t lfs = {};
  lfs_config config = {};
};
class LittleFS_QSPIFlash : public LittleFS_Program {};
#endif
