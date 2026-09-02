#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __IMXRT1062__
#include <LittleFS.h>
#include <SD.h>
#include <unordered_map>

extern bool SDcard_Ready;

namespace PhzConfig {
  using KEY = uint16_t;
  using VALUE = uint64_t;
  using ConfigMap = std::unordered_map<KEY, VALUE>;

  const char * const CONFIG_FILENAME = "GLOBALS.CFG";

  // LittleFS_Program plus the two things the library keeps protected that
  // this module has to see: how full the root directory's metadata log is,
  // and the compaction threshold that bounds it. Every open() on littlefs
  // walks that log from the start, CRC-checking each commit, so a log that
  // has grown fat on small commits (a CrashReport append, a slot-number
  // write, a rename) taxes every file operation afterwards. The threshold
  // (config.metadata_max) is read by littlefs at commit time through the
  // pointer it kept at mount, so it can be set after begin().
  class XenoFS : public LittleFS_Program {
  public:
    uint32_t rootLogBytes();          // bytes of the root log in use; 0 = unmounted
    uint32_t metadataMax() const { return config.metadata_max; }  // 0 = block_size
    void setMetadataMax(uint32_t bytes) { config.metadata_max = bytes; }
  };

  extern XenoFS myfs;

  // Forward Decl
  void Init();
  void listFiles(FS &fs = myfs);
  bool load_config(const char* filename = CONFIG_FILENAME, FS &fs = myfs);
  bool save_config(const char* filename = CONFIG_FILENAME, FS &fs = myfs);
  void clear_config();

  void setValue(KEY key, VALUE value);
  bool getValue(KEY key, VALUE &value);
  void deleteKey(KEY key);

  // Save a filtered/remapped copy of the currently loaded map to a file,
  // leaving the live map untouched. Used by the preset engine to extract
  // one preset's keys out of a bank file. remap may be null (identity).
  bool save_filtered(const char* filename, FS &fs,
                     bool (*pred)(KEY), KEY (*remap)(KEY));

  // The same bytes save_config would write, into RAM (optionally filtered
  // and remapped like save_filtered), and back. The preset engine packs
  // config sections into its slot container straight from RAM so no scratch
  // file -- and no interrupts-off block erase -- stands between the map and
  // the container. serialize returns bytes used, 0 if cap is too small.
  size_t serialize(uint8_t *buf, size_t cap,
                   bool (*pred)(KEY) = nullptr, KEY (*remap)(KEY) = nullptr);
  bool deserialize(const uint8_t *buf, size_t len);
  // One config value out of a serialized image WITHOUT touching the map.
  // For readers that run while some app owns the map (a slot import peeking
  // at the container's manifest) and must leave it exactly as they found it.
  // Scans the "PZ" chunk only; false when the image is malformed or the key
  // is absent.
  bool peek(const uint8_t *buf, size_t len, KEY key, VALUE &value);

  // True when the map differs from the file it was last loaded from or saved
  // to. save_config skips the write when it is false and the target is that
  // same file.
  bool unsaved_changes();

  void setData(KEY key, VALUE value);
  bool getData(KEY key, VALUE &value);
  void deleteData(KEY key);

  // Copy GLOBALS.CFG -> GLOBALS.BAK (called once per boot after a good
  // load, so a corrupt primary can be recovered instead of blocking boot
  // at the ConfirmReset prompt). Returns false on any I/O failure.
  bool backup_config();
  const char * const BACKUP_FILENAME = "GLOBALS.BAK";

  void printDirectory(FS &fs = myfs);
  void printDirectory(File dir, int numSpaces);
  void printSpaces(int num);
  void eraseFiles(FS &fs = myfs);

}
#else
// Teensy 3.x: no LittleFS/SD. Inert stubs keep shared code compiling
// (HemisphereApplet GetData/SetData, Main.cpp setup); persistence on T3
// goes through the EEPROM PageStorage paths instead.
namespace PhzConfig {
  using KEY = uint16_t;
  using VALUE = uint64_t;

  inline void Init() {}
  inline bool load_config(const char* = nullptr) { return false; }
  inline bool save_config(const char* = nullptr) { return false; }
  inline void clear_config() {}
  inline bool backup_config() { return false; }

  inline size_t serialize(uint8_t *, size_t, bool (*)(KEY) = nullptr, KEY (*)(KEY) = nullptr) { return 0; }
  inline bool deserialize(const uint8_t *, size_t) { return false; }
  inline bool peek(const uint8_t *, size_t, KEY, VALUE &) { return false; }
  inline bool unsaved_changes() { return false; }

  inline void setValue(KEY, VALUE) {}
  inline bool getValue(KEY, VALUE &) { return false; }
  inline void deleteKey(KEY) {}

  inline void setData(KEY, VALUE) {}
  inline bool getData(KEY, VALUE &) { return false; }
  inline void deleteData(KEY) {}
}
#endif
