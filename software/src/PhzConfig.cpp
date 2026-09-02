/* Phazerville Config File
 *
 * Primarily stored on LittleFS in flash storage,
 * or SD card if available, or other any similar FS object.
 * Supercedes previous EEPROM mechanism
 */
#ifdef __IMXRT1062__
#include <string.h>
#include "PhzConfig.h"
#include "HSUtils.h"
#include "util/util_misc.h"
#include "usb_desc.h"

#ifdef MTP_INTERFACE
#include <MTP_Teensy.h>
#endif

namespace PhzConfig {

XenoFS myfs;
File dataFile;

uint32_t XenoFS::rootLogBytes() {
  if (!mounted) return 0;
  // lfs_dir_open fetches the directory's metadata pair; m.off is how far
  // into the block the valid log extends, i.e. what every open() re-walks.
  lfs_dir_t dir;
  if (lfs_dir_open(&lfs, &dir, "/") < 0) return 0;
  const uint32_t used = dir.m.off;
  lfs_dir_close(&lfs, &dir);
  return used;
}
ConfigMap cfg_store;
ConfigMap data_store;

// Change tracking, so a save that would rewrite a file with its own contents
// can be skipped. Every flash write is expensive here in a way that is easy
// to miss: LittleFS_Program's program and erase run with interrupts OFF
// (cores/teensy4/eeprom.c), and a new file costs a 64 KB block erase --
// ~250 ms on the bench, during which audio, USB and the preset bus all
// stall. Captain's FLUSH handler and Quadrants' preset autosave both write
// files whose contents have usually not changed; before this they paid the
// full price every time.
//
// The map is clean when it holds exactly what `map_source` on `map_source_fs`
// holds: right after a successful load_config from it or save_config to it,
// until a setValue/setData actually changes a value, a delete removes a key,
// or clear_config runs. Filenames are copied because callers build them in
// stack buffers (Quadrants' BANK_NNN.DAT).
static bool map_dirty = true;
static char map_source[16] = "";
static FS *map_source_fs = nullptr;

static void mark_clean(const char *filename, FS &fs) {
  strncpy(map_source, filename, sizeof(map_source) - 1);
  map_source[sizeof(map_source) - 1] = 0;
  map_source_fs = &fs;
  map_dirty = false;
}
static void mark_dirty() {
  map_dirty = true;
}
static bool is_clean_copy_of(const char *filename, FS &fs) {
  return !map_dirty && map_source_fs == &fs && map_source[0] &&
         strcmp(map_source, filename) == 0;
}

// Specify size to use of onboard Teensy Program Flash chip.
// the maximum flash available for LittleFS is 960 blocks of 1024 bytes
#if defined(ARDUINO_TEENSY41)
// 8MB program flash: give the FS real headroom (30 preset-bus slots at
// ~12KB each plus banks never fit 512KB once LittleFS block overhead bites)
static constexpr uint32_t diskSize = 1024 * 1024 * 4;
#else
static constexpr uint32_t diskSize = 1024 * 512;
#endif
// custom file format header
static constexpr uint32_t HEADER_SIZE = 12;

FLASHMEM
void Init()
{
#ifdef MTP_INTERFACE
  MTP.begin();
#endif
  if (SDcard_Ready) {
#ifdef MTP_INTERFACE
    MTP.addFilesystem(SD, "SD_Card");
#endif
    SERIAL_PRINTLN("SD card available for preset storage");
    //listFiles(SD);
  }

  // This mounts or creates a LittleFS drive in Teensy PCB Flash.
  if (!myfs.begin(diskSize)) {
    SERIAL_PRINTLN("LittleFS unavailable!! Settings WILL NOT BE SAVED!");
    return;
  }
  SERIAL_PRINTLN("LittleFS initialized.");

  // Bound the root directory's metadata log. littlefs appends one commit per
  // file create/close/rename and rewrites (compacts) the log only when it
  // reaches metadata_max, which the library leaves at 0 = the whole 64 KB
  // block. Every open() walks that log from the start, CRC-checking each
  // commit, so a log fattened by small commits taxes every file operation
  // after it: measured 4.5 ms per open() with the log at 60 KB, which put a
  // 5-file preset recall at 25-30 ms instead of 6. At 8 KB an open() costs
  // under a millisecond and compaction runs every ~60 commits. Compaction
  // erases the pair's other block (~280 ms, interrupts off), but it can only
  // fire on a commit, and every commit here already sits inside an operation
  // that erases a data block (a save, the current-slot persist); a recall
  // never commits. littlefs splits a directory into a second metadata pair
  // (two more of the 64 blocks the presets are budgeted against) when the
  // COMPACTED content exceeds metadata_max/2 (lfs_dir_compact). Compacted,
  // the root is one name tag + one CTZ tag per file, ~28 bytes; this
  // module's ~40 files come to ~1.2 KB against a 4 KB limit. Effective from
  // the next commit: littlefs reads the threshold through the config pointer
  // it kept at mount.
  myfs.setMetadataMax(8192);

#ifdef MTP_INTERFACE
  MTP.addFilesystem(myfs, "Internal_LFS");
#endif

  /*
  if (myfs.mediaPresent()) {
    listFiles(myfs);
    //load_config();
  }
  */
}

void clear_config() {
  cfg_store.clear();
  data_store.clear();
  mark_dirty();
}

void setValue(KEY key, VALUE value)
{
  auto it = cfg_store.find(key);
  if (it != cfg_store.end()) {
    if (it->second == value) return;   // no change, map stays clean
    it->second = value;
  } else {
    cfg_store.emplace(key, value);
  }
  mark_dirty();
}

bool getValue(KEY key, VALUE &value)
{
  auto thing = cfg_store.find(key);
  if (thing != cfg_store.end()) {
    value = thing->second;
    return true;
  }
  return false;
}

void deleteKey(KEY key) {
  if (cfg_store.erase(key)) mark_dirty();
}

void setData(KEY key, VALUE value) {
  auto it = data_store.find(key);
  if (it != data_store.end()) {
    if (it->second == value) return;
    it->second = value;
  } else {
    data_store.emplace(key, value);
  }
  mark_dirty();
}
bool getData(KEY key, VALUE &value) {
  auto thing = data_store.find(key);
  if (thing != data_store.end()) {
    value = thing->second;
    return true;
  }
  return false;
}
void deleteData(KEY key) {
  if (data_store.erase(key)) mark_dirty();
}

bool unsaved_changes() { return map_dirty; }

// Header + records for one chunk, written STRICTLY SEQUENTIALLY.
//
// This used to write a placeholder header, the records, then seek back to
// backfill the count and checksum. On littlefs a write behind the end of an
// open file is copy-on-write: the block is relocated -- a fresh 64 KB erase
// -- and everything after the write position is copied over byte by byte
// through the 128-byte cache (lfs_file_flush). Two chunks meant two extra
// erases per save on top of the one the file itself costs, which is how a
// 2 KB config took a second to write. The count and checksum are cheap to
// know up front: one pass over the map first.
static void chunk_header(uint8_t *h, const char *sig, ConfigMap &store) {
  uint16_t record_count = 0;
  uint64_t checksum = 0;
  for (auto &i : store) {
    checksum ^= i.second;
    record_count++;
  }
  h[0] = sig[0]; h[1] = sig[1];
  h[2] = record_count & 0xFF; h[3] = record_count >> 8;
  for (int i = 0; i < 8; ++i) h[4 + i] = (uint8_t)(checksum >> (8 * i));
}

size_t save_chunk(const size_t offset, const char* sig, ConfigMap &store) {
  uint8_t header_buf[HEADER_SIZE];
  chunk_header(header_buf, sig, store);
  if (dataFile.write(header_buf, HEADER_SIZE) != HEADER_SIZE) {
    SERIAL_PRINTLN("!! ERROR while writing file header !!\n");
    return 0;
  }

  size_t bytes_written = 0;
  for (auto &i : store)
  {
    int result = dataFile.write((const uint8_t*)&i.first, sizeof(i.first)) +
                dataFile.write((const uint8_t*)&i.second, sizeof(i.second));
    if (result != (sizeof(i.first) + sizeof(i.second))) {
      // something went wrong
      SERIAL_PRINTLN("!! ERROR while writing file !!\n   Result = %d\n", result);
      return 0;
    }
    bytes_written += result;
  }

  SERIAL_PRINTLN("Bytes written = %u\n", bytes_written);
  return offset + HEADER_SIZE + bytes_written;
}

// Same bytes save_chunk writes, into RAM. Returns the bytes used, or 0 when
// `cap` is too small (nothing partial is left behind for the caller to trust).
static size_t chunk_to_mem(uint8_t *buf, size_t cap, const char *sig,
                           ConfigMap &store) {
  const size_t need = HEADER_SIZE + store.size() * (sizeof(KEY) + sizeof(VALUE));
  if (need > cap) return 0;
  chunk_header(buf, sig, store);
  uint8_t *p = buf + HEADER_SIZE;
  for (auto &i : store) {
    memcpy(p, &i.first, sizeof(i.first));   p += sizeof(i.first);
    memcpy(p, &i.second, sizeof(i.second)); p += sizeof(i.second);
  }
  return need;
}

FLASHMEM size_t serialize(uint8_t *buf, size_t cap,
                          bool (*pred)(KEY), KEY (*remap)(KEY)) {
  ConfigMap *cfg = &cfg_store, *data = &data_store;
  ConfigMap cfg_out, data_out;
  if (pred || remap) {
    for (auto &kv : cfg_store) {
      if (pred && !pred(kv.first)) continue;
      cfg_out[remap ? remap(kv.first) : kv.first] = kv.second;
    }
    for (auto &kv : data_store) {
      if (pred && !pred(kv.first)) continue;
      data_out[remap ? remap(kv.first) : kv.first] = kv.second;
    }
    cfg = &cfg_out; data = &data_out;
  }
  const size_t a = chunk_to_mem(buf, cap, "PZ", *cfg);
  if (!a) return 0;
  const size_t b = chunk_to_mem(buf + a, cap - a, "PX", *data);
  if (!b) return 0;
  return a + b;
}

// One chunk out of a RAM image; advances *pos past a chunk it recognised.
// Validation matches load_chunk: signature, record count, xor-of-values
// checksum. "Not this chunk" and "this chunk, but damaged" are different
// answers: the first sends the caller on to the other signature, the second
// has to fail the whole image, or a container with a corrupt G section would
// recall the half of it that happened to verify.
enum ChunkResult : uint8_t { CHUNK_ABSENT, CHUNK_OK, CHUNK_CORRUPT };
static ChunkResult chunk_from_mem(const uint8_t *buf, size_t len, size_t *pos,
                                  const char *sig, ConfigMap &store) {
  if (*pos + HEADER_SIZE > len) return CHUNK_ABSENT;
  const uint8_t *h = buf + *pos;
  if (h[0] != sig[0] || h[1] != sig[1]) return CHUNK_ABSENT;
  const size_t count = (size_t)h[2] | (size_t)h[3] << 8;
  uint64_t expected = 0;
  for (int i = 0; i < 8; ++i) expected |= (uint64_t)h[4 + i] << (8 * i);
  const size_t body = count * (sizeof(KEY) + sizeof(VALUE));
  if (*pos + HEADER_SIZE + body > len) return CHUNK_CORRUPT;
  const uint8_t *p = h + HEADER_SIZE;
  uint64_t computed = 0;
  for (size_t i = 0; i < count; ++i) {
    KEY k; VALUE v;
    memcpy(&k, p, sizeof(k)); p += sizeof(k);
    memcpy(&v, p, sizeof(v)); p += sizeof(v);
    store.insert_or_assign(k, v);
    computed ^= v;
  }
  *pos += HEADER_SIZE + body;
  return computed == expected ? CHUNK_OK : CHUNK_CORRUPT;
}

FLASHMEM bool deserialize(const uint8_t *buf, size_t len) {
  cfg_store.clear();
  data_store.clear();
  mark_dirty();   // the map is nobody's file until it is saved somewhere
  size_t pos = 0;
  bool any = false;
  while (pos < len) {
    const size_t before = pos;
    const ChunkResult c = chunk_from_mem(buf, len, &pos, "PZ", cfg_store);
    const ChunkResult d = chunk_from_mem(buf, len, &pos, "PX", data_store);
    if (c == CHUNK_CORRUPT || d == CHUNK_CORRUPT ||
        (c == CHUNK_ABSENT && d == CHUNK_ABSENT)) {
      SERIAL_PRINTLN("PhzConfig: bad image at %u\n", (unsigned)before);
      (void)before;   // only the debug print reads it
      cfg_store.clear();
      data_store.clear();
      return false;
    }
    any = true;
  }
  return any;
}

bool save_config(const char* filename, FS &fs)
{
    SERIAL_PRINTLN("\nSaving Config: %s\n", filename);

    // Nothing changed since this exact file was loaded or written: the bytes
    // on flash already ARE the map. Skipping is what makes a preset-bus
    // save's Captain FLUSH, or a Quadrants autosave with no edits, free
    // instead of a second-long, interrupts-off block erase. The exists()
    // check keeps a deleted file honest.
    if (is_clean_copy_of(filename, fs) && fs.exists(filename)) {
      SERIAL_PRINTLN("PhzConfig: %s unchanged, not rewritten\n", filename);
      return true;
    }

    const char* const TEMPFILE = "PEWPEW.TMP";
    bool success = true;

    // opens a file or creates a file if not present,
    // FILE_WRITE will append data
    // FILE_WRITE_BEGIN will overwrite from 0
    // O_TRUNC to truncate file size to what was written
    fs.remove(TEMPFILE);
    dataFile = fs.open(TEMPFILE, FILE_WRITE_BEGIN);
    if (dataFile) {
      size_t sz  = save_chunk( 0, "PZ",  cfg_store);
      if (sz) sz = save_chunk(sz, "PX", data_store);
      // Close on EVERY path, and fail the save when a chunk short-wrote.
      // Previously a failed save_chunk left `success` true AND left the
      // temp file open: the rename below then moved a half-written file
      // over a good config, and the still-open lfs_file_t (which stays
      // registered in lfs->mlist, pointing at metadata the rename has
      // moved) was only closed later, when the next load_config/save_config
      // reassigned the global `dataFile` -- a delayed write into a stale
      // metadata pair, i.e. exactly the kind of thing that faults deep in
      // lfs_dir_commit during some LATER save. save_filtered() below
      // already got both of these right; this one was the outlier.
      dataFile.close();
      if (!sz) {
        HS::PokePopup(HS::MESSAGE_POPUP, "Write ERROR !!");
        success = false;
        fs.remove(TEMPFILE);   // never leave a half-written temp behind
      }
    } else {
      SERIAL_PRINTLN("PhzConfig: Error opening %s\n", filename);
      HS::PokePopup(HS::MESSAGE_POPUP, "File ERROR !!");
      success = false;
    }

    if (success) {
      // Rename FIRST. littlefs replaces an existing destination atomically,
      // so the config is never absent: it is either the old file or the new
      // one. The previous remove-then-rename left a window where a reset --
      // a power cut, or the 134-baud bootloader reboot the bench uses --
      // destroyed GLOBALS.CFG outright. That happened: a reset landed inside
      // this window, GLOBALS.CFG and GLOBALS.BAK were both gone on the next
      // boot, and the module came up at a factory-reset prompt that only a
      // physical button could answer.
      success = fs.rename(TEMPFILE, filename);
      if (!success) {
        fs.remove(filename);                       // SD: no replace-on-rename
        success = fs.rename(TEMPFILE, filename);
      }
      if (!success)
        HS::PokePopup(HS::MESSAGE_POPUP, "TempFile ERR !!");
    }

    if (success) mark_clean(filename, fs);
    return success;
}

FLASHMEM bool save_filtered(const char* filename, FS &fs,
                   bool (*pred)(KEY), KEY (*remap)(KEY))
{
    SERIAL_PRINTLN("\nSaving filtered config: %s\n", filename);

    // Build filtered/remapped copies; the live map stays untouched.
    ConfigMap cfg_out, data_out;
    for (auto &kv : cfg_store) {
      if (pred && !pred(kv.first)) continue;
      cfg_out[remap ? remap(kv.first) : kv.first] = kv.second;
    }
    for (auto &kv : data_store) {
      if (pred && !pred(kv.first)) continue;
      data_out[remap ? remap(kv.first) : kv.first] = kv.second;
    }

    const char* const TEMPFILE = "PEWPEW.TMP";
    bool success = true;

    fs.remove(TEMPFILE);
    dataFile = fs.open(TEMPFILE, FILE_WRITE_BEGIN);
    if (dataFile) {
      size_t sz  = save_chunk( 0, "PZ", cfg_out);
      if (sz) sz = save_chunk(sz, "PX", data_out);
      dataFile.close();
      if (!sz) success = false;
    } else {
      SERIAL_PRINTLN("PhzConfig: Error opening %s\n", filename);
      success = false;
    }

    if (success) {
      // Rename-first, as above: never leave the destination absent.
      success = fs.rename(TEMPFILE, filename);
      if (!success) {
        fs.remove(filename);
        success = fs.rename(TEMPFILE, filename);
      }
    }

    return success;
}

// The header is read-only here: the caller tries both signatures against the
// same twelve bytes, so the record bytes go through their own buffer. They
// used to go through `buf`, which left the last record's key where the next
// signature check looked -- a key of 0x5850 ('P','X') would have read the
// records that followed as a second data chunk with a garbage count.
bool load_chunk(const uint8_t *hdr, const char *sig, ConfigMap &store) {
  // quick signature check
  if (hdr[0] != sig[0] || hdr[1] != sig[1]) return false;

  size_t record_count = 0;
  size_t expected_record_count = uint16_t(hdr[2]) | uint16_t(hdr[3]) << 8;
  uint64_t expected_checksum =
          (uint64_t)hdr[4] |
          (uint64_t)hdr[5] << 8 |
          (uint64_t)hdr[6] << 16 |
          (uint64_t)hdr[7] << 24 |
          (uint64_t)hdr[8] << 32 |
          (uint64_t)hdr[9] << 40 |
          (uint64_t)hdr[10] << 48 |
          (uint64_t)hdr[11] << 56;
  uint64_t computed_checksum = 0;

  static_assert(sizeof(KEY) + sizeof(VALUE) == 10, "config data size mismatch");
  uint8_t buf[sizeof(KEY) + sizeof(VALUE)];
  size_t pos = 0;
  // Bounded by the header's count, not by end-of-file: a chunk with no
  // records (a filtered save that matched nothing) is followed by the next
  // chunk's header, which is not records.
  while (record_count < expected_record_count && dataFile.available()) {
    uint8_t n = dataFile.read();
    buf[pos++] = n;

    if (pos >= (sizeof(KEY) + sizeof(VALUE))) {
      store.insert_or_assign(
          (uint16_t)buf[0] |
          (uint16_t)buf[1] << 8,

          (uint64_t)buf[2] |
          (uint64_t)buf[3] << 8 |
          (uint64_t)buf[4] << 16 |
          (uint64_t)buf[5] << 24 |
          (uint64_t)buf[6] << 32 |
          (uint64_t)buf[7] << 40 |
          (uint64_t)buf[8] << 48 |
          (uint64_t)buf[9] << 56
          );

      computed_checksum ^=
          (uint64_t)buf[2] |
          (uint64_t)buf[3] << 8 |
          (uint64_t)buf[4] << 16 |
          (uint64_t)buf[5] << 24 |
          (uint64_t)buf[6] << 32 |
          (uint64_t)buf[7] << 40 |
          (uint64_t)buf[8] << 48 |
          (uint64_t)buf[9] << 56;

      ++record_count;
      pos = 0;
    }
  }
  // Multiple chunks can be packed in series in one file; a chunk that ends
  // early, or mid-record, is a truncated file, not a short chunk.
  if (record_count != expected_record_count || pos != 0) {
    SERIAL_PRINTLN("Loaded %u Records. (expected %u) -- truncated\n",
                   record_count, expected_record_count);
    HS::PokePopup(HS::MESSAGE_POPUP, "Corrupt File!!");
    return false;
  }
  SERIAL_PRINTLN("Loaded %u Records. (expected %u)\n", record_count, expected_record_count);
  SERIAL_PRINTLN("Checksum: %s (actual: %lx%lx)\n",
      (computed_checksum == expected_checksum)? "OK" : "ERROR",
      (uint32_t)computed_checksum, (uint32_t)(computed_checksum >> 32));
  SERIAL_PRINTLN("(File header checksum: %lx%lx)\n",
      (uint32_t)expected_checksum, (uint32_t)(expected_checksum >> 32));

  if (computed_checksum != expected_checksum)
    HS::PokePopup(HS::MESSAGE_POPUP, "Corrupt File!!");

  return (computed_checksum == expected_checksum);
}

bool load_config(const char* filename, FS &fs)
{
  cfg_store.clear();
  data_store.clear();
  mark_dirty();

  SERIAL_PRINTLN("\nLoading Config: %s\n", filename);
  dataFile = fs.open(filename);
  if (!dataFile) {
    SERIAL_PRINTLN("ERROR opening %s\n", filename);
    return false;
  }

  uint8_t buf[HEADER_SIZE];
  size_t pos = 0;

  do {
    // read in header
    pos = 0;
    while (dataFile.available() && pos < HEADER_SIZE) {
      uint8_t n = dataFile.read();
      buf[pos++] = n;
    }
    // A partial header is a truncated file. It used to be checked against
    // whatever the previous header left in buf, which is how a file with a
    // few stray bytes on the end could pass its signature check.
    if (pos != HEADER_SIZE) {
      SERIAL_PRINTLN("PhzConfig: short header (%u bytes)\n", (unsigned)pos);
      dataFile.close();
      return false;
    }

    // check for every chunk signature
    const bool has_config = load_chunk(buf, "PZ", cfg_store);
    const bool has_data = load_chunk(buf, "PX", data_store);

    if (!has_config && !has_data) {
      SERIAL_PRINTLN("PhzConfig: Bad signature... %x %x", buf[0], buf[1]);
      dataFile.close();
      return false; // no bueno
    }

  } while (dataFile.available());

  dataFile.close();
  mark_clean(filename, fs);
  return true; // everything was fine!
}

FLASHMEM
bool backup_config()
{
  File src = myfs.open(CONFIG_FILENAME, FILE_READ);
  if (!src) return false;
  myfs.remove(BACKUP_FILENAME);
  File dst = myfs.open(BACKUP_FILENAME, FILE_WRITE_BEGIN);
  if (!dst) { src.close(); return false; }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while ((n = src.read(buf, sizeof(buf))) > 0) {
    if (dst.write(buf, n) != (size_t)n) { ok = false; break; }
  }
  src.close();
  dst.close();
  if (!ok) myfs.remove(BACKUP_FILENAME);  // no partial backups
  return ok;
}

FLASHMEM
void listFiles(FS &fs)
{
  Serial.print("\n     Space Used = ");
  Serial.println(fs.usedSize());
  Serial.print("Filesystem Size = ");
  Serial.println(fs.totalSize());

  printDirectory(fs);
}

FLASHMEM
void eraseFiles(FS &fs)
{
  //myfs.quickFormat();
  fs.format();
  Serial.println("\nFilesystem formatted - All files erased !");
}

FLASHMEM
void printDirectory(FS &fs) {
  Serial.println("Directory\n---------");
  printDirectory(fs.open("/"), 0);
  Serial.println();
}

FLASHMEM
void printDirectory(File dir, int numSpaces) {
   while(true) {
     File entry = dir.openNextFile();
     if (! entry) {
       //Serial.println("** no more files **");
       break;
     }
     printSpaces(numSpaces);
     Serial.print(entry.name());
     if (entry.isDirectory()) {
       Serial.println("/");
       printDirectory(entry, numSpaces+2);
     } else {
       // files have sizes, directories do not
       printSpaces(36 - numSpaces - strlen(entry.name()));
       Serial.print("  ");
       Serial.println(entry.size(), DEC);
     }
     entry.close();
   }
}

FLASHMEM
void printSpaces(int num) {
  for (int i=0; i < num; i++) {
    Serial.print(" ");
  }
}

} // namespace PhzConfig
#endif
