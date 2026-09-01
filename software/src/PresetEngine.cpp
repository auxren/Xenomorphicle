// Whole-module preset engine for the 200e preset bus. See PresetEngine.h.
//
// ROUND 4 -- the Store crash is CLOSED, and it was never in this file. The
// fault is in the USB audio input path; the full write-up lives at the top of
// src/Audio/USB_F32.cpp. Short version: a Store blocks loop() for seconds
// across LittleFS program-flash writes, the USB audio transport's stream-stall
// recovery (USBAudioInInterface::resetBuffer) then computes its ring write
// index from a smoothed DWT timestamp with no modulo and no bound, and the
// receive ISR dereferences whatever .bss word that out-of-range index lands
// on. Nothing below needs to change; the guards are in USB_F32.cpp, where the
// buffers actually live. The round-1..3 history is kept below because the
// ruling-out is still valid and worth not repeating.
#if defined(ARDUINO_TEENSY41)

#include <Arduino.h>
#include "src/extern/dspinst.h"  // before <Audio.h>: same include guard,
                                 // and Audio's copy lacks some intrinsics
#include <Audio.h>
#include <SD.h>

#include "PresetEngine.h"
#include "OC_apps.h"
#include "OC_app_switcher.h"
#include "OC_storage.h"
#include "OC_core.h"
#include "OC_digital_inputs.h"
#include "OC_gpio.h"
#include "OC_ui.h"
#include "PhzConfig.h"
#include "PresetBus.h"   // GetStats(): bus-slave ISR count, for the save report
#include "HSUtils.h"
#include "src/drivers/FreqMeasure/OC_FreqMeasure.h"

extern uint_fast8_t MENU_REDRAW;  // Main.cpp
// ---------------------------------------------------------------------------
// The Store-crash investigation, round 3. Read this before theorising again.
//
// SYMPTOM: an ordinary Store resets the module, reliably, at the same point:
// right after Captain's CAPTAIN.DAT save prints its last chunk checksum --
// i.e. inside save_config()'s close/remove/rename tail, or step 4's copy_file
// chain immediately after it. Both are LittleFS-on-program-flash write paths.
//
// WHAT IS RULED OUT, with evidence:
//
// * WDOG timeout. SRC_SRSR bit 4 was never set. Still true. The watchdog_feed()
//   calls below stay as cheap insurance, but they were never the fix.
//
// * "SRSR 0x02 proves a CPU lockup." It does NOT. RT1060 RM 21.6.3 names the
//   bit lockup_sysresetreq: set by a CPU lockup OR by any software write of
//   SYSRESETREQ to SCB_AIRCR -- which is exactly how the Teensy core's own
//   default fault handler reboots (startup.c, after parking ~8s). The core
//   itself disambiguates with SRC_GPR5 == 0x0BAD00F1 (CrashReport.cpp). We
//   were never reading GPR5; Main.cpp now captures it at boot and 't' prints
//   [FAULT-REBOOT] vs [LOCKUP or SW-RESET].
//
// * "CrashReport has nothing for this one." It may well have had plenty:
//   CrashReportClass::printTo() calls clear() on its way OUT, so Main.cpp's
//   print-to-Serial-then-print-to-buffer sequence threw the report away and
//   wrote "No Crash Data To Report" into CRASH.LOG every single time. Fixed
//   in Main.cpp (capture first, echo the buffer).
//
// * Round 2's fix: CORE::app_isr_enabled left true across
//   DispatchAppEvent(FLUSH)/(RESUME). Flashed and tested live -- SAME CRASH,
//   same point. The freeze is kept below (it matches RecallSlot and is
//   defensible on its own), but it is NOT the cause, and the premise was
//   unsupported: nothing CORE_timer_ISR reaches touches PhzConfig at all.
//
// * ISR-vs-main-thread data races generally. Every interrupt live on a T4.1
//   build was audited: CORE timer, UI timer (ui.Poll -> GPIO + event ring
//   only, never gated by anything), LPI2C1 general-call slave, ADC DMA,
//   display LPSPI, audio I2S DMA + software_isr, USB device/host, Serial8.
//   NONE of them touch PhzConfig::cfg_store, PresetEngine state, `capture`,
//   LittleFS, or the heap. In particular PresetBus's lpi2c1_slave_isr only
//   pushes into its own SPSC ring (and, while card-serving, into the RAM-only
//   BusCard image) -- it shares nothing with SaveSlot's call chain.
//
// * "The race is inside the flash window." There is no window: LittleFS's
//   program-flash driver (eepromemu_flash_write/erase_sector in the core's
//   eeprom.c) runs from ITCM with __disable_irq() held across the whole
//   erase/program, so every interrupt above is masked while flash is busy.
//
// * Stack exhaustion. Measured, not estimated: the crashing boot's SelfTest
//   reported 6200 bytes of DTCM stack never touched, nowhere near the guard.
//
// WHAT IT ACTUALLY WAS (round 4, from the first real CrashReport):
//
//   reset_cause SRC_SRSR=00000002 SRC_GPR5=0BAD00F1  (a CAUGHT fault, not a
//   lockup -- GPR5 is the core fault handler's own marker)
//   Code was executing from 0x8E8, CFSR 0x82 = DACCVIOL + MMARVALID,
//   accessed 0x80000C on one run and 0x1B4 on another.
//
//   addr2line against the exact ELF: AudioInputUSB_F32::copy_to_buffers, and
//   0x8E8 is the `vstr s15, [lr, #4]` that writes rxBuffer[bIdx][j]->data[].
//   Not our call chain at all -- an ISR that keeps running while a Store
//   blocks loop(), on a wild block pointer. See src/Audio/USB_F32.cpp.
//
// So the save path is a TRIGGER, not the bug: it is simply the longest thing
// in the firmware that stops loop() and repeatedly masks interrupts, which is
// exactly the condition the transport's stall recovery gets wrong. Guards are
// in USB_F32.cpp; the 't' selftest prints their trip counts. If a Store ever
// crashes again, check that line first -- non-zero counts with no crash means
// the guards are doing their job, zero counts means look somewhere new.
// ---------------------------------------------------------------------------
extern void watchdog_feed();      // Main.cpp
extern uint32_t stack_low_water();  // Main.cpp: unused DTCM stack, in bytes

namespace OC {

// from OC_apps.cpp
extern void BuildAppData(AppData &data);
extern void ApplyAppData(const AppData &data);
extern void BuildGlobalSettingsValues();
extern void RestoreGlobalSettingsFromConfig(uint8_t scala_loaded_mask);
extern size_t ResolveAppIndexByID(uint16_t app_id);

namespace PresetEngine {

static constexpr uint16_t kQuadrantsAppId = TWOCCS("QS");
static constexpr uint8_t kScratchBank = 255;    // reserved: recall staging

// slot manifest keys, in the PRESETBUS namespace of PB_NN_G.CFG
static constexpr uint16_t kManifestKey = 8 << 8;    // == PRESETBUS_KEY
static constexpr uint16_t kSchemaKey  = kManifestKey | 0;
static constexpr uint16_t kFlagsKey   = kManifestKey | 1;
static constexpr uint64_t kSchemaVersion = 1;
// current bus slot, persisted (debounced) into GLOBALS.CFG so boot can
// restore the preset the case was on -- 200e power-up semantics
static constexpr uint16_t kCurSlotKey = kManifestKey | 0x13;

// Quadrants writes its live preset id under this bare (bank-globals) key
// when handling APP_EVENT_FLUSH, so the extractor knows which preset block
// to pull out of the bank map. 253 is unused in the bank key map.
static constexpr uint16_t kQuadLivePresetKey = 253;

enum ContentFlags : uint8_t {
  CONTENT_BANK    = 1 << 0,   // bank section present (Quadrants was active)
  CONTENT_SCENERY = 1 << 1,
  CONTENT_CAPTAIN = 1 << 2,
};

// LittleFS_Program's erase-sector size, which is also its allocation unit:
// SECTOR_SIZE in the core's LittleFS.cpp, keyed off the board exactly as it
// is there. A 4 MB partition on T4.1 is therefore only 64 blocks in total,
// and every file consumes one whether it holds 588 bytes or 64 KB.
#if defined(ARDUINO_TEENSY41)
static constexpr uint32_t kLfsBlockBytes = 65536;
#else
static constexpr uint32_t kLfsBlockBytes = 32768;
#endif
// container + the scratch file it renames through + metadata slack
static constexpr uint32_t kSaveBlocksNeeded = 3;

// ---- state -----------------------------------------------------------------
static volatile int8_t pending_save = -1;    // last-wins
static volatile int8_t pending_recall = -1;
static int8_t last_slot = -1;
static bool last_was_save = false;
static uint32_t cur_slot_dirty_ms = 0;   // 0 = clean
static uint32_t op_count = 0;            // completed save/recall operations
static bool last_save_ok = false;
// Why the most recent recall refused, or nullptr if it succeeded. A recall
// that fails validation used to exit WITHOUT bumping op_count, so the
// overlay's completion watch waited out its whole 4-second timeout and then
// showed a generic RECALL FAILED -- for what was actually an empty slot.
// That misdirection cost a night: the owner read it as a bus fault when the
// truth was that the 15s Teensy restores had wiped every PB_* file (a full
// restore erases ALL flash including LittleFS; a reflash does not).
static const char *last_recall_err = nullptr;
static bool busy = false;
static int quad_recall_hint = -1;
static bool skip_captain_restore = false;  // boot recall only

static DMAMEM AppData capture;               // RAM capture buffer (~4KB)

// active Quadrants preset during bank extraction (predicate/remap context)
static uint8_t extract_preset;

// ---- helpers ---------------------------------------------------------------

static FS &slot_fs() { return SDcard_Ready ? (FS &)SD : (FS &)PhzConfig::myfs; }
static void load_names();  // defined with the name store below

FLASHMEM static void slot_name(char *buf, uint8_t slot, char kind, const char *ext) {
  // PB_NN_K.EXT
  buf[0] = 'P'; buf[1] = 'B'; buf[2] = '_';
  buf[3] = '0' + slot / 10; buf[4] = '0' + slot % 10;
  buf[5] = '_'; buf[6] = kind; buf[7] = '.';
  buf[8] = ext[0]; buf[9] = ext[1]; buf[10] = ext[2]; buf[11] = 0;
}

// Source and destination filesystems are separate on purpose. Slot files live
// on slot_fs() (SD when there is a card), but the live names they restore to
// belong to whichever FS the owning app actually reads -- and Captain and
// Scenery only ever read myfs. Copying within one FS silently put their state
// on the card, where neither app looks.
FLASHMEM static bool copy_file(FS &sfs, const char *from, FS &dfs, const char *to) {
  File src = sfs.open(from);
  if (!src) return false;
  dfs.remove(to);
  File dst = dfs.open(to, FILE_WRITE_BEGIN);
  if (!dst) { src.close(); return false; }
  uint8_t buf[512];
  bool ok = true;
  int n;
  while ((n = src.read(buf, sizeof(buf))) > 0) {
    if (dst.write(buf, n) != (size_t)n) { ok = false; break; }
  }
  src.close();
  dst.close();
  if (!ok) dfs.remove(to);
  return ok;
}

// PB_NN_A.BIN framing
struct AppDataHeader {
  uint32_t fourcc;    // 'OCPB'
  uint16_t version;
  uint16_t used;
  uint16_t checksum;  // 16-bit sum over payload
  uint16_t reserved;
};
static constexpr uint32_t kAppDataFourcc = 0x4243504FUL;  // "OPCB" LE = 'O','P','C','B'... spelled: bytes O C P B
static uint16_t sum16(const uint8_t *p, size_t n) {
  uint16_t s = 0;
  while (n--) s += *p++;
  return s;
}

// Parse an app-data section from wherever the file cursor currently sits, so
// the legacy PB_NN_A.BIN reader and the container's 'A' section share one
// validator instead of drifting apart.
FLASHMEM static bool read_appdata_stream(File &f) {
  AppDataHeader h;
  bool ok = f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) &&
            h.fourcc == kAppDataFourcc && h.version == 1 &&
            h.used <= AppData::kAppDataSize;
  if (ok) {
    ok = f.read(capture.data, h.used) == h.used &&
         sum16(capture.data, h.used) == h.checksum;
    capture.used = h.used;
  }
  return ok;
}

FLASHMEM static bool read_appdata_file(uint8_t slot) {
  char name[12];
  slot_name(name, slot, 'A', "BIN");
  File f = slot_fs().open(name);
  if (!f) return false;
  const bool ok = read_appdata_stream(f);
  f.close();
  return ok;
}

// ---- slot container --------------------------------------------------------
//
// WHY ONE FILE PER SLOT. LittleFS_Program allocates whole erase sectors, and
// on Teensy 4.1 that sector is 65536 bytes (SECTOR_SIZE in the core's
// LittleFS.cpp; 32768 on a T4.0). Every file costs at least one whole block
// no matter how little it holds -- the 588-byte PB_04_A.BIN measured on the
// bench occupied 64 KB. With a 4 MB partition that is only 64 blocks TOTAL,
// so the old layout's 3-5 files per slot could not survive its own 30 slots:
// 30 x 3 = 90 blocks against a 64-block filesystem, i.e. the filesystem ran
// out somewhere around slot 20 while reporting well over a megabyte free.
//
// Packing every section into one file makes a slot cost one block instead of
// three-to-five, which is what actually makes 30 slots fit. Layout:
//
//   0x00  ContainerHeader                     16 bytes
//   0x10  SectionEntry[kMaxSections]          6 x 12 = 72 bytes
//   0x58  section payloads, in write order
//
// The directory is fixed-size and written LAST (seek back to 0), because
// section lengths are not known until their bytes are on disk -- the same
// backfill trick PhzConfig::save_chunk already uses for its record count.
//
// Sections are opaque byte ranges, so this deliberately needs no changes to
// PhzConfig: a PhzConfig-format section is simply the bytes that
// save_config() would have written, appended here and extracted to a scratch
// file on the way back out.
// ---------------------------------------------------------------------------

static constexpr uint32_t kContainerFourcc = 0x53425058UL;  // 'X','P','B','S'
static constexpr uint16_t kContainerVersion = 1;
static constexpr int kMaxSections = 6;

struct ContainerHeader {
  uint32_t fourcc;
  uint16_t version;
  uint16_t count;
  uint32_t reserved0;
  uint32_t reserved1;
};
struct SectionEntry {
  uint8_t  kind;      // 'G','A','B','S','C'; 0 = unused
  uint8_t  pad;
  uint16_t checksum;  // sum16 over the payload
  uint32_t offset;    // absolute, from file start
  uint32_t length;
};
static_assert(sizeof(ContainerHeader) == 16, "container header must be 16 bytes");
static_assert(sizeof(SectionEntry) == 12, "section entry must be 12 bytes");

static constexpr uint32_t kPayloadStart =
    sizeof(ContainerHeader) + kMaxSections * sizeof(SectionEntry);  // 88

// "PB_NN.PBS"
FLASHMEM static void container_name(char *buf, uint8_t slot) {
  buf[0] = 'P'; buf[1] = 'B'; buf[2] = '_';
  buf[3] = '0' + slot / 10; buf[4] = '0' + slot % 10;
  buf[5] = '.'; buf[6] = 'P'; buf[7] = 'B'; buf[8] = 'S'; buf[9] = 0;
}

struct ContainerWriter {
  File f;
  SectionEntry sec[kMaxSections];
  int n;
  uint32_t pos;
  bool ok;
};

FLASHMEM static bool cw_begin(ContainerWriter &w, const char *tmp) {
  w.n = 0;
  w.pos = kPayloadStart;
  w.ok = false;
  slot_fs().remove(tmp);
  w.f = slot_fs().open(tmp, FILE_WRITE_BEGIN);
  if (!w.f) return false;
  // Reserve the directory by WRITING placeholder bytes, not by seeking past
  // them: seeking beyond EOF on a freshly created file is not portable (the
  // host FS refuses it outright), and a hole would leave the directory
  // undefined if the commit never ran. cw_commit seeks back over these.
  const uint8_t zero[kPayloadStart] = { 0 };
  if (w.f.write(zero, sizeof(zero)) != sizeof(zero)) {
    w.f.close();
    slot_fs().remove(tmp);
    return false;
  }
  w.ok = true;
  return true;
}

// Append a whole file as one section. An absent or empty source is NOT a
// failure -- it just means the slot has no such content (no Scenery file
// yet, Quadrants not active) and no entry is recorded for it.
FLASHMEM static bool cw_add_file(ContainerWriter &w, char kind,
                                 FS &fs, const char *src) {
  if (!w.ok || w.n >= kMaxSections) return false;
  File s = fs.open(src, FILE_READ);
  if (!s) return false;
  const uint32_t want = (uint32_t)s.size();
  uint8_t buf[256];
  uint16_t sum = 0;
  uint32_t len = 0;
  int r;
  while ((r = s.read(buf, sizeof(buf))) > 0) {
    if (w.f.write(buf, r) != (size_t)r) { s.close(); w.ok = false; return false; }
    for (int i = 0; i < r; ++i) sum += buf[i];
    len += (uint32_t)r;
    watchdog_feed();
  }
  s.close();
  if (!len) return false;
  // A mid-stream read error used to end the loop quietly and record a
  // TRUNCATED section whose checksum matched the bytes that did arrive -- so
  // the container validated clean and the loss only surfaced as missing state
  // at recall. sum16 cannot see this; only the length can. Fail the save.
  if (len != want) {
    serial_printf("PresetEngine: section '%c' short: %lu of %lu bytes\n",
                  kind, (unsigned long)len, (unsigned long)want);
    w.ok = false;
    return false;
  }
  w.sec[w.n++] = { (uint8_t)kind, 0, sum, w.pos, len };
  w.pos += len;
  return true;
}

// The app-data section carries the same AppDataHeader framing the standalone
// PB_NN_A.BIN used, so read_appdata_stream() validates either source.
FLASHMEM static bool cw_add_appdata(ContainerWriter &w) {
  if (!w.ok || w.n >= kMaxSections) return false;
  const AppDataHeader h = { kAppDataFourcc, 1, (uint16_t)capture.used,
                            sum16(capture.data, capture.used), 0 };
  if (w.f.write((const uint8_t *)&h, sizeof(h)) != sizeof(h)) { w.ok = false; return false; }
  if (w.f.write(capture.data, capture.used) != capture.used) { w.ok = false; return false; }
  const uint32_t len = sizeof(h) + capture.used;
  const uint16_t sum = (uint16_t)(sum16((const uint8_t *)&h, sizeof(h)) +
                                  sum16(capture.data, capture.used));
  w.sec[w.n++] = { (uint8_t)'A', 0, sum, w.pos, len };
  w.pos += len;
  return true;
}

// Backfill the directory, then publish via rename so a torn write can never
// replace a good slot (same discipline as PhzConfig::save_config).
FLASHMEM static bool cw_commit(ContainerWriter &w, const char *tmp, const char *final_name) {
  bool ok = w.ok && w.f.seek(0);
  if (ok) {
    const ContainerHeader h = { kContainerFourcc, kContainerVersion,
                                (uint16_t)w.n, 0, 0 };
    ok = w.f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
  }
  for (int i = 0; i < kMaxSections && ok; ++i) {
    const SectionEntry e = (i < w.n) ? w.sec[i] : SectionEntry{ 0, 0, 0, 0, 0 };
    ok = w.f.write((const uint8_t *)&e, sizeof(e)) == sizeof(e);
  }
  if (w.f) w.f.close();
  if (!ok) { slot_fs().remove(tmp); return false; }
  // verify the bytes actually landed: LittleFS has produced 0-byte files
  // while reporting success on a degraded FS
  File v = slot_fs().open(tmp, FILE_READ);
  ok = v && v.size() >= kPayloadStart;
  if (v) v.close();
  if (!ok) { slot_fs().remove(tmp); return false; }
  // Rename FIRST. littlefs's rename atomically replaces an existing
  // destination, so on internal flash the old slot is never absent: it is
  // either the previous container or the new one. The previous
  // remove-then-rename opened a window where a power cut left the slot with
  // no container at all -- and for a migrated slot, no legacy files either,
  // since those were retired on the earlier commit. The only survivor was
  // the temp file, which the next save of ANY slot deletes in cw_begin.
  // The fallback remove is for SD, where replace-on-rename is not promised.
  ok = slot_fs().rename(tmp, final_name);
  if (!ok) {
    slot_fs().remove(final_name);
    ok = slot_fs().rename(tmp, final_name);
    // If THAT failed we have already removed the old container, so the tmp
    // file is now the only copy of this slot in existence. Leave it alone:
    // cw_begin reclaims the name on the next save, which is a block we can
    // spare, and until then a human has something to recover. Removing it
    // here -- as the first version of this did -- deleted the old container
    // and the freshly verified new one in the same breath, which is exactly
    // the slot loss the rename-first ordering exists to prevent, just moved
    // to a rarer path. Only the first-rename failure is safe to clean up
    // after, because there the old container is still in place.
    if (!ok)
      serial_printf("PresetEngine: %s left in place -- it is the only copy "
                    "of this slot\n", tmp);
    return ok;
  }
  return ok;
}

// Open a slot container and read its directory. Returns false when the file
// is absent or not a container -- the caller then falls back to the legacy
// multi-file layout.
FLASHMEM static bool container_open(uint8_t slot, File &f, SectionEntry *sec, int &n) {
  char name[12];
  container_name(name, slot);
  f = slot_fs().open(name, FILE_READ);
  if (!f) return false;
  ContainerHeader h;
  if (f.read((uint8_t *)&h, sizeof(h)) != sizeof(h) ||
      h.fourcc != kContainerFourcc || h.version != kContainerVersion ||
      h.count > kMaxSections) {
    f.close();
    return false;
  }
  n = h.count;
  for (int i = 0; i < n; ++i) {
    if (f.read((uint8_t *)&sec[i], sizeof(sec[i])) != sizeof(sec[i])) {
      f.close();
      return false;
    }
    // a section must lie inside the file
    if (sec[i].offset < kPayloadStart ||
        (uint64_t)sec[i].offset + sec[i].length > (uint64_t)f.size()) {
      f.close();
      return false;
    }
  }
  return true;
}

// Retire the pre-container files for a slot once its container is safely on
// disk. This is what actually reclaims the blocks: each of these cost a whole
// 64 KB sector, so a converted slot hands back two to four of them.
FLASHMEM static void remove_legacy_slot(uint8_t slot) {
  static const char kinds[] = { 'G', 'A', 'B', 'S', 'C' };
  static const char *const exts[] = { "CFG", "BIN", "DAT", "DAT", "DAT" };
  char name[12];
  for (unsigned i = 0; i < sizeof(kinds); ++i) {
    slot_name(name, slot, kinds[i], exts[i]);
    slot_fs().remove(name);
  }
}

FLASHMEM static const SectionEntry *find_section(const SectionEntry *sec, int n, char kind) {
  for (int i = 0; i < n; ++i)
    if (sec[i].kind == (uint8_t)kind) return &sec[i];
  return nullptr;
}

// Extract one section back out to a standalone file, verifying its checksum
// before the destination is touched is not possible in one pass without a
// buffer, so the copy is written to a scratch name and only then renamed.
FLASHMEM static bool section_to_file(File &f, const SectionEntry &e,
                                     const char *dest, FS &fs) {
  if (!f.seek(e.offset)) return false;
  static const char *const kScratch = "PB_XTR.TMP";
  fs.remove(kScratch);
  File d = fs.open(kScratch, FILE_WRITE_BEGIN);
  if (!d) return false;
  uint8_t buf[256];
  uint32_t left = e.length;
  uint16_t sum = 0;
  bool ok = true;
  while (left) {
    const uint32_t want = left < sizeof(buf) ? left : sizeof(buf);
    const int r = f.read(buf, want);
    if (r != (int)want) { ok = false; break; }
    if (d.write(buf, r) != (size_t)r) { ok = false; break; }
    for (int i = 0; i < r; ++i) sum += buf[i];
    left -= want;
    watchdog_feed();
  }
  d.close();
  if (ok) ok = (sum == e.checksum);
  if (!ok) { fs.remove(kScratch); return false; }
  fs.remove(dest);
  ok = fs.rename(kScratch, dest);
  if (!ok) fs.remove(kScratch);
  return ok;
}

// bank-key extraction: keep the active preset's block + the bank globals
// (bare keys 100-255) + VERSION_KEY; remap the preset block to preset 0.
FLASHMEM static bool bank_pred(PhzConfig::KEY k) {
  if (k == 0xFFFF) return true;                     // VERSION_KEY
  const uint16_t block = k >> 11;
  const uint16_t low = k & 0x7FF;
  if (block == 0 && low >= 100 && low < 256) return true;  // bank globals
  if (block == extract_preset && (low < 100 || low >= 256)) return true;
  return false;
}
FLASHMEM static PhzConfig::KEY bank_remap(PhzConfig::KEY k) {
  if (k == 0xFFFF) return k;
  const uint16_t low = k & 0x7FF;
  if ((k >> 11) == extract_preset && (low < 100 || low >= 256))
    return low;  // move to preset block 0
  return k;
}

// ---- save ------------------------------------------------------------------

FLASHMEM bool SaveSlot(uint8_t slot) {
  if (slot >= kNumSlots) return false;
  busy = true;
  serial_printf("PresetEngine: save slot %d\n", slot);

  // Free-space guard (LittleFS only; SD is effectively unbounded).
  //
  // This MUST be denominated in blocks, not bytes. LittleFS_Program hands out
  // whole erase sectors -- kLfsBlockBytes below -- so a slot that needs one
  // block cannot be placed when one block is not free, however many spare
  // BYTES the filesystem reports. The previous 24 KB threshold was smaller
  // than a single T4.1 sector, so it cheerfully green-lit saves the allocator
  // had no room for; the failure then surfaced as a torn write far downstream
  // instead of an honest refusal here.
  //
  // A save needs the container plus the scratch files it renames through, so
  // require kSaveBlocksNeeded free rather than exactly one.
  //
  // usedSize() has been observed reporting == totalSize() on a healthy FS,
  // so treat that state as "unknown" and rely on post-write verification.
  if (!SDcard_Ready) {
    const uint64_t total = PhzConfig::myfs.totalSize();
    const uint64_t used = PhzConfig::myfs.usedSize();
    if (used < total &&
        (total - used) / kLfsBlockBytes < kSaveBlocksNeeded) {
      HS::PokePopup(HS::MESSAGE_POPUP, "Disk full !!");
      busy = false;
      serial_printf("PresetEngine: save refused, %lu KB free < %lu blocks\n",
                    (unsigned long)((total - used) >> 10),
                    (unsigned long)kSaveBlocksNeeded);
      return false;
    }
  }

  const uint16_t app_id = app_switcher.current_app()->id();
  uint8_t flags = 0;
  // Section staging and container staging. Both are transient: they are
  // removed or renamed away before this function returns, so neither costs a
  // block in the steady state.
  static const char *const kSecTmp = "PB_SEC.TMP";
  static const char *const kCtrTmp = "PB_CTR.TMP";
  char final_name[12];
  container_name(final_name, slot);

  // Instrumentation for the Store-crash hunt (see the note at the top of
  // this file): how long the save blocked loop(), how much interrupt traffic
  // the bus slave took while it did, and -- the number that matters -- how
  // much DTCM stack was still unused afterwards. A save that survives with
  // a low_water in the low hundreds of bytes means the next one may not,
  // and that a fault taken during exception stacking is the mechanism.
  const uint32_t t_start = millis();
  const uint32_t isr_start = PresetBus::GetStats().isr_count;

  // 1. capture the app-data chunk stream to RAM (fast; ISR-bracketed).
  // app_isr stays OFF clear through step 7's APP_EVENT_RESUME below, so the
  // app's Process() cannot run on top of its own FLUSH/RESUME handler --
  // the same bracket RecallSlot() already puts around its equivalent region.
  // DEFENSIVE ONLY: this was round 2's candidate fix for the Store crash and
  // it did NOT fix it (see the note at the top of this file). Kept for
  // symmetry with RecallSlot, not as a diagnosis.
  CORE::app_isr_enabled = false;
  delay(1);
  BuildAppData(capture);

  // 2. ask the active app to flush its file-backed state (Captain, e.g.,
  // does its own load_config+save_config("CAPTAIN.DAT") right here)
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_FLUSH);
  watchdog_feed();

  // Steps 3-6 append the slot's sections to one container. Section ORDER is
  // constrained: the content flags must be settled before the 'G' section is
  // written, because the manifest that records them lives inside that map.
  ContainerWriter w;
  cw_begin(w, kCtrTmp);   // failure leaves w.ok false; every add below no-ops

  // 3. Quadrants active: extract its live preset + bank globals from the map
  if (app_id == kQuadrantsAppId) {
    uint64_t p = 0;
    if (PhzConfig::getValue(kQuadLivePresetKey, p) && p < 32) {
      extract_preset = (uint8_t)p;
      if (PhzConfig::save_filtered(kSecTmp, slot_fs(), bank_pred, bank_remap) &&
          cw_add_file(w, 'B', slot_fs(), kSecTmp))
        flags |= CONTENT_BANK;
      slot_fs().remove(kSecTmp);
    }
    watchdog_feed();
  }

  // 4. the file-backed app stores go straight in -- they are already files,
  // so unlike the config sections they need no staging round-trip.
  //
  // These come from myfs EXPLICITLY, not slot_fs(). Scenery and Captain call
  // PhzConfig::load_config/save_config without an FS argument, which defaults
  // to myfs -- so those two files only ever exist on internal flash, whatever
  // slot_fs() happens to be. Sourcing them from slot_fs() meant that with an
  // SD card inserted the open simply failed and both apps' state was silently
  // absent from every preset. (The old !SDcard_Ready fallback below could not
  // catch it: when !SDcard_Ready, slot_fs() ALREADY is myfs, so it re-tried
  // the call that had just failed, and when SD was present -- the only case
  // that needed a fallback -- the condition blocked it.)
  //
  // BANK_255.DAT is different and stays on slot_fs(): Quadrants genuinely
  // prefers SD (Quadrants.h:99-103, 2104-2107).
  if (cw_add_file(w, 'S', PhzConfig::myfs, "SCENERY.DAT")) flags |= CONTENT_SCENERY;
  watchdog_feed();
  if (cw_add_file(w, 'C', PhzConfig::myfs, "CAPTAIN.DAT")) flags |= CONTENT_CAPTAIN;
  watchdog_feed();

  // 5. globals + manifest, staged through kSecTmp because PhzConfig writes
  // whole files by name
  PhzConfig::load_config();  // base map = GLOBALS.CFG
  BuildGlobalSettingsValues();
  PhzConfig::setValue(kSchemaKey, kSchemaVersion);
  PhzConfig::setValue(kFlagsKey, flags);
  bool ok = PhzConfig::save_config(kSecTmp, slot_fs()) &&
            cw_add_file(w, 'G', slot_fs(), kSecTmp);
  slot_fs().remove(kSecTmp);
  watchdog_feed();

  // 6. app-data chunk stream, straight from the RAM capture
  bool ok2 = cw_add_appdata(w);
  watchdog_feed();

  // Publish the container, then retire the pre-container files this replaces.
  // cw_commit verifies the bytes landed and renames into place, so a torn
  // write leaves the previous slot intact rather than half-overwritten.
  const bool committed = ok && ok2 && cw_commit(w, kCtrTmp, final_name);
  if (!committed) {
    // Nothing renamed the staging file away, so remove it here: it is holding
    // a whole 64 KB block and cw_begin would not clear it until the next save.
    if (w.f) w.f.close();
    slot_fs().remove(kCtrTmp);
  }
  ok = committed;
  ok2 = committed;

  // Retiring the legacy files DELETES the only other copy of this preset, so
  // the bar for doing it is not "the container is the right size" -- 88 bytes
  // of anything passes that. Re-open and parse it: container_open validates
  // the fourcc, the version, the section count and every section's bounds, at
  // the cost of one directory read. The degraded filesystem that motivated
  // cw_commit's size check can just as easily return a right-sized file whose
  // directory bytes never landed, and that slot would then read as empty with
  // no legacy files left to fall back to.
  if (committed) {
    File v;
    SectionEntry vsec[kMaxSections];
    int vn = 0;
    if (container_open(slot, v, vsec, vn)) {
      v.close();
      remove_legacy_slot(slot);
    } else {
      serial_printf("PresetEngine: slot %d container did not re-open; "
                    "legacy files kept\n", slot);
    }
  }
  watchdog_feed();

  // 7. hand the config map back to the active app (Resume reloads its file).
  // Same hazard as step 2 -- Captain's Resume() also touches PhzConfig's
  // cfg_store and setups[] -- so app_isr stays off until this returns too.
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
  CORE::app_isr_enabled = true;
  watchdog_feed();

  last_slot = slot;
  last_was_save = true;
  last_save_ok = ok && ok2;
  cur_slot_dirty_ms = millis() | 1;
  op_count++;
  busy = false;
  HS::PokePopup(HS::MESSAGE_POPUP, (ok && ok2) ? "Bus save OK" : "Bus save ERR");
  serial_printf("PresetEngine: save slot %d %s (flags %02x)\n",
                slot, (ok && ok2) ? "ok" : "FAILED", flags);
  serial_printf("PresetEngine: save took %lums, bus ISRs %lu, "
                "stack unused %lu bytes\n",
                (unsigned long)(millis() - t_start),
                (unsigned long)(PresetBus::GetStats().isr_count - isr_start),
                (unsigned long)stack_low_water());
  return ok && ok2;
}

// ---- recall ----------------------------------------------------------------

// Scratch name the 'G' section is extracted to on the way back out, because
// PhzConfig::load_config takes a filename rather than an open file.
static const char *const kGlbTmp = "PB_GLB.TMP";

enum RecallStage : uint8_t { STAGE_OK = 0, STAGE_EMPTY = 1, STAGE_BAD = 2 };

// The validate-first half of a recall: app-data into `capture`, the slot's
// globals + manifest into the config map, WITHOUT touching live state.
// Reads a container when there is one and the pre-container files otherwise,
// so slots written before the layout change still recall.
FLASHMEM static RecallStage recall_stage_head(uint8_t slot, bool &from_container) {
  File f;
  SectionEntry sec[kMaxSections];
  int n = 0;
  from_container = container_open(slot, f, sec, n);

  if (from_container) {
    const SectionEntry *a = find_section(sec, n, 'A');
    const SectionEntry *g = find_section(sec, n, 'G');
    bool ok = a && g && f.seek(a->offset) && read_appdata_stream(f);
    if (ok) ok = section_to_file(f, *g, kGlbTmp, slot_fs());
    f.close();
    // The container exists, so a failure here is corruption, not emptiness.
    if (!ok) { slot_fs().remove(kGlbTmp); return STAGE_BAD; }
    const bool loaded = PhzConfig::load_config(kGlbTmp, slot_fs());
    // The scratch file has served its purpose the moment the map is loaded.
    // Left behind it pinned a whole 64 KB block -- 1 of the 64 the internal
    // filesystem has -- permanently, from the first container recall onwards.
    slot_fs().remove(kGlbTmp);
    return loaded ? STAGE_OK : STAGE_BAD;
  }

  // legacy multi-file layout
  if (!read_appdata_file(slot)) return STAGE_EMPTY;
  char name[12];
  slot_name(name, slot, 'G', "CFG");
  return PhzConfig::load_config(name, slot_fs()) ? STAGE_OK : STAGE_BAD;
}

// Stage the file-backed stores into their live names. Runs with the app world
// already frozen, so it only moves bytes around.
FLASHMEM static void recall_stage_files(uint8_t slot, bool from_container,
                                        uint64_t flags) {
  if (from_container) {
    File f;
    SectionEntry sec[kMaxSections];
    int n = 0;
    if (!container_open(slot, f, sec, n)) return;
    // Destination FS per file, mirroring the save side: the bank goes where
    // Quadrants looks (slot_fs()), Scenery and Captain go where THEY look
    // (myfs, always). Restoring these to the SD card put them somewhere
    // neither app ever reads.
    const SectionEntry *e;
    if ((flags & CONTENT_BANK) && (e = find_section(sec, n, 'B')) != nullptr) {
      if (section_to_file(f, *e, "BANK_255.DAT", slot_fs()))
        quad_recall_hint = kScratchBank;
      watchdog_feed();
    }
    if ((flags & CONTENT_SCENERY) && (e = find_section(sec, n, 'S')) != nullptr) {
      section_to_file(f, *e, "SCENERY.DAT", PhzConfig::myfs);
      watchdog_feed();
    }
    // Boot recall deliberately keeps the LIVE Captain config -- see the note
    // on the legacy branch below.
    if ((flags & CONTENT_CAPTAIN) && !skip_captain_restore &&
        (e = find_section(sec, n, 'C')) != nullptr) {
      section_to_file(f, *e, "CAPTAIN.DAT", PhzConfig::myfs);
      watchdog_feed();
    }
    f.close();
    return;
  }

  char name[12];
  if (flags & CONTENT_BANK) {
    slot_name(name, slot, 'B', "DAT");
    copy_file(slot_fs(), name, slot_fs(), "BANK_255.DAT");
    quad_recall_hint = kScratchBank;
    watchdog_feed();
  }
  if (flags & CONTENT_SCENERY) {
    slot_name(name, slot, 'S', "DAT");
    copy_file(slot_fs(), name, PhzConfig::myfs, "SCENERY.DAT");
    watchdog_feed();
  }
  // Boot recall deliberately keeps the LIVE Captain config: it's the
  // module's MIDI-interface setup (autosaved continuously), not scene
  // state - restoring the slot's snapshot at power-up silently rewound
  // the owner's mapping edits. Explicit recalls still restore it.
  if ((flags & CONTENT_CAPTAIN) && !skip_captain_restore) {
    slot_name(name, slot, 'C', "DAT");
    copy_file(slot_fs(), name, PhzConfig::myfs, "CAPTAIN.DAT");
    watchdog_feed();
  }
}

// A refused recall is still a FINISHED op: op_count bumps so the overlay's
// completion watch reports the reason at once instead of timing out into a
// generic failure, and the serial log gets a closing line instead of a
// dangling "recall slot N". last_slot is deliberately untouched -- nothing
// was recalled, so "the current preset" has not changed.
FLASHMEM static bool recall_refused(uint8_t slot, const char *why) {
  last_recall_err = why;
  last_was_save = false;
  op_count++;
  busy = false;
  serial_printf("PresetEngine: recall slot %d refused (%s)\n", slot, why);
  return false;
}

FLASHMEM bool RecallSlot(uint8_t slot) {
  if (slot >= kNumSlots) return false;
  busy = true;
  serial_printf("PresetEngine: recall slot %d\n", slot);

  // 1. validate everything before touching live state
  bool from_container = false;
  const RecallStage stage = recall_stage_head(slot, from_container);
  if (stage == STAGE_EMPTY) {
    HS::PokePopup(HS::MESSAGE_POPUP, "Empty preset");
    // put the map back (staging doesn't touch it on this path, but stay safe)
    return recall_refused(slot, "EMPTY SLOT");
  }
  if (stage == STAGE_BAD) {
    // map may now be cleared/partial: restore base-map ownership, then let
    // the app reload its own file so no later writer inherits slot content
    PhzConfig::load_config();
    app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
    HS::PokePopup(HS::MESSAGE_POPUP, "Bad preset");
    return recall_refused(slot, "BAD PRESET");
  }
  uint64_t schema = 0, flags = 0, meta = 0;
  PhzConfig::getValue(kSchemaKey, schema);
  PhzConfig::getValue(kFlagsKey, flags);
  PhzConfig::getValue((uint16_t)(1 << 8) /* METADATA_KEY */, meta);
  const uint16_t slot_app_id = meta & 0xFFFF;
  if (schema != kSchemaVersion) {
    PhzConfig::load_config();  // drop slot content, re-own the base map
    app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
    HS::PokePopup(HS::MESSAGE_POPUP, "Bad preset ver");
    return recall_refused(slot, "OLD PRESET");
  }

  // 2. freeze the app world
  CORE::app_isr_enabled = false;
  CORE::app_loop_enabled = false;
  delay(1);

  // 3. stage the file-backed stores (same multi-write LittleFS chain as
  // SaveSlot's step 4 -- see the watchdog note at the top of this file)
  recall_stage_files(slot, from_container, flags);

  // 4. apply global settings (map still holds PB_NN_G.CFG) + app chunks
  RestoreGlobalSettingsFromConfig(0);
  Scales::Validate();
  Chords::Validate();
  for (int i = 0; i < HS::TURING_MACHINE_COUNT; ++i)
    HS::user_turing_machines[i].Validate();
  ApplyAppData(capture);
  watchdog_feed();

  // 5. switch to the slot's app (missing app: stay put, partial recall)
  const size_t idx = ResolveAppIndexByID(slot_app_id);

  FreqMeasure.end();
  DigitalInputs::reInit();

  AudioNoInterrupts();
  app_switcher.set_current_app(idx);
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
  AudioInterrupts();
  watchdog_feed();

  // 6. run
  CORE::app_isr_enabled = true;
  CORE::app_loop_enabled = true;
  ::MENU_REDRAW = 1;

  last_slot = slot;
  last_was_save = false;
  last_recall_err = nullptr;
  cur_slot_dirty_ms = millis() | 1;
  op_count++;
  busy = false;
  HS::PokePopup(HS::MESSAGE_POPUP, "Bus recall OK");
  serial_printf("PresetEngine: recall slot %d done (app %04x)\n", slot, slot_app_id);
  return true;
}

// ---- service ---------------------------------------------------------------

FLASHMEM void Init() {
  pending_save = pending_recall = -1;
  quad_recall_hint = -1;
  load_names();
}

void RequestSave(uint8_t slot) {
  if (slot < kNumSlots) pending_save = slot;
}
void RequestRecall(uint8_t slot) {
  if (slot < kNumSlots) pending_recall = slot;
}

// persist the current slot ~3s after preset activity settles, so
// trigger-driven preset cycling never write-hammers GLOBALS.CFG
FLASHMEM static void persist_cur_slot() {
  cur_slot_dirty_ms = 0;
  if (last_slot < 0) return;
  PhzConfig::load_config();
  uint64_t v = 0;
  PhzConfig::getValue(kCurSlotKey, v);
  const bool changed = (int64_t)v != last_slot;
  if (changed) {
    PhzConfig::setValue(kCurSlotKey, (uint64_t)last_slot);
    PhzConfig::save_config();
  }
  // CRITICAL: hand the shared map back to the active app. Quadrants
  // assumes bank-map residency; leaving GLOBALS loaded here would make
  // its next preset save overwrite the bank file with the wrong map.
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
}

FLASHMEM void BootRecall() {
  // GLOBALS.CFG is the loaded map right after boot restore
  uint64_t v = 0;
  PhzConfig::load_config();
  if (!PhzConfig::getValue(kCurSlotKey, v) || v >= kNumSlots) return;
  if (!SlotUsed((uint8_t)v)) return;
  serial_printf("PresetEngine: boot recall slot %d\n", (int)v);
  skip_captain_restore = true;   // cleared after the first Process() pass
  RequestRecall((uint8_t)v);     // local only: no bus broadcast at power-up
}

FLASHMEM void Process() {
  // Read-then-clear needs no interrupt lock, and it is worth saying why
  // rather than adding one defensively.
  //
  // Every writer of these two is in LOOP context, not an ISR. The bus slave
  // ISR (PresetBus.cpp lpi2c1_slave_isr) only pushes bytes into its own SPSC
  // ring; the parser that turns those bytes into a SAVE or RECALL runs from
  // PresetBus::Task(), and its cb_save/cb_recall callbacks call RequestSave/
  // RequestRecall from there. The console commands and BootRecall() are loop
  // context too. So this function and every producer are the same thread and
  // cannot interleave.
  //
  // An earlier version of this comment claimed the slave ISR wrote these and
  // bracketed the clear in noInterrupts(). The lock was harmless but the
  // reason was invented, and a wrong comment about concurrency in this file
  // is worse than no comment -- the round-3 audit block above is the thing
  // people trust when they are debugging a preset-bus fault at 2am.
  const int8_t s = pending_save;
  if (s >= 0) {
    pending_save = -1;
    SaveSlot(s);
  }
  const int8_t r = pending_recall;
  if (r >= 0) {
    pending_recall = -1;
    RecallSlot(r);
    skip_captain_restore = false;   // only ever true for the boot recall
  }
  if (cur_slot_dirty_ms && millis() - cur_slot_dirty_ms > 3000)
    persist_cur_slot();
}

FLASHMEM int ConsumeQuadrantsRecallHint() {
  const int h = quad_recall_hint;
  quad_recall_hint = -1;
  return h;
}

int8_t LastSlot() { return last_slot; }
uint32_t OpCount() { return op_count; }
bool LastSaveOk() { return last_save_ok; }
const char *LastRecallError() { return last_recall_err; }

// ---- slot names --------------------------------------------------------
// One flat 30x16 byte file, whole thing cached in RAM. Deliberately not a
// PhzConfig file: reads/writes must never disturb the shared config map.
static char name_cache[kNumSlots][kNameLen + 1];  // +1: always NUL-safe

FLASHMEM static void load_names() {
  memset(name_cache, 0, sizeof(name_cache));
  File f = slot_fs().open("PBNAMES.BIN", FILE_READ);
  if (!f) return;
  for (int i = 0; i < kNumSlots; ++i) {
    if (f.read((uint8_t *)name_cache[i], kNameLen) != kNameLen) break;
    name_cache[i][kNameLen] = 0;
  }
  f.close();
}

const char *SlotName(uint8_t slot) {
  return (slot < kNumSlots) ? name_cache[slot] : "";
}

FLASHMEM void SetSlotName(uint8_t slot, const char *name) {
  if (slot >= kNumSlots) return;
  memset(name_cache[slot], 0, sizeof(name_cache[slot]));
  strncpy(name_cache[slot], name, kNameLen);
  // trim trailing spaces so "unnamed" stays honest
  for (int i = (int)strlen(name_cache[slot]) - 1;
       i >= 0 && name_cache[slot][i] == ' '; --i)
    name_cache[slot][i] = 0;
  File f = slot_fs().open("PBNAMES.BIN", FILE_WRITE_BEGIN);
  if (!f) return;
  for (int i = 0; i < kNumSlots; ++i)
    f.write((const uint8_t *)name_cache[i], kNameLen);
  f.close();
}

FLASHMEM bool SlotUsed(uint8_t slot) {
  if (slot >= kNumSlots) return false;
  char name[12];
  container_name(name, slot);
  {
    File c = slot_fs().open(name, FILE_READ);
    const bool have = c && c.size() >= (int)kPayloadStart;
    if (c) c.close();
    if (have) return true;
  }
  // pre-container layout
  slot_name(name, slot, 'G', "CFG");
  File f = slot_fs().open(name, FILE_READ);
  const bool used = f && f.size() > 16;
  if (f) f.close();
  return used;
}
bool LastWasSave() { return last_was_save; }
bool Busy() { return busy; }

}  // namespace PresetEngine
}  // namespace OC

#endif  // ARDUINO_TEENSY41
