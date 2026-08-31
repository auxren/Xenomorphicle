// Buchla 200e bus address -> model lookup. See the header for provenance,
// the three caveats, and why this table (not a protocol command) is the
// identification mechanism.
#include "Buchla200eModuleTable.h"

// On target this table must live in FLASH: Teensy 4 puts .rodata in DTCM by
// default, and ~670 bytes of model names is not worth a DTCM bank. PROGMEM
// is a no-op on host builds (see test/host_stubs/Arduino.h). Direct reads
// work on both -- flash is memory-mapped on this core, so no pgm_read_*
// accessor is needed.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define B200E_TABLE_DATA PROGMEM
#define B200E_TABLE_CODE FLASHMEM
#else
#define B200E_TABLE_DATA
#define B200E_TABLE_CODE
#endif

// Transcribed from Studio H 2Wireless.ino getDisplayMessage(). Keep sorted by
// address: the lookup below relies on nothing, but a sorted table is far
// easier to diff against the source if it is ever revised.
static const Buchla200eModuleEntry kModules[] B200E_TABLE_DATA = {
  {0x10, "257 A"},   {0x11, "257 B"},   {0x12, "267 A"},   {0x13, "267 B"},
  {0x20, "210"},     {0x21, "225"},     {0x23, "227"},     {0x24, "249 A"},
  {0x25, "249 B"},   {0x28, "259 A"},   {0x29, "259 B"},   {0x2A, "259 C"},
  {0x2B, "259 D"},   {0x2C, "261 A"},   {0x2D, "261 B"},   {0x2E, "261 C"},
  {0x2F, "261 D"},   {0x30, "260 A"},   {0x31, "260 B"},   {0x32, "266 A"},
  {0x33, "266 B"},   {0x34, "256 A1"},  {0x35, "256 B1"},  {0x36, "256 A2"},
  {0x37, "256 B2"},  {0x38, "281 A1"},  {0x39, "281 A2"},  {0x3A, "281 B1"},
  {0x3B, "281 B2"},  {0x3C, "281 C1"},  {0x3D, "281 C2"},  {0x3E, "281 D1"},
  {0x3F, "281 D2"},  {0x40, "222 A"},   {0x41, "250 A"},   {0x42, "250 B"},
  {0x44, "291 A"},   {0x45, "291 B"},   {0x48, "292 A"},   {0x49, "292 B"},
  {0x4A, "292 C"},   {0x4B, "292 D"},   {0x4C, "285 FS A"},{0x4D, "285 FS B"},
  {0x4E, "285 BM A"},{0x4F, "285 BM B"},{0x5A, "223 M"},   {0x5B, "223 A"},
  {0x5C, "251 A"},   {0x5E, "272"},     {0x5F, "252 A"},   {0x60, "206"},
  {0x63, "207"},     {0x64, "296 A"},   {0x65, "296 B"},   {0x66, "230 A"},
  {0x67, "230 B"},   {0x68, "230 C"},   {0x69, "230 D"},   {0x70, "218"},
  {0x72, "266h"},
};

static const int kModuleCount =
    (int)(sizeof(kModules) / sizeof(kModules[0]));

B200E_TABLE_CODE
const char *Buchla200eModelForAddress(uint8_t addr) {
  for (int i = 0; i < kModuleCount; ++i) {
    if (kModules[i].addr == addr) return kModules[i].name;
  }
  return nullptr;
}

B200E_TABLE_CODE
int Buchla200eModuleCount() {
  return kModuleCount;
}

B200E_TABLE_CODE
const Buchla200eModuleEntry *Buchla200eModuleAt(int index) {
  if (index < 0 || index >= kModuleCount) return nullptr;
  return &kModules[index];
}
