// Buchla 200e bus address -> model lookup. See the header for provenance,
// the three caveats, and why this table (not a protocol command) is the
// identification mechanism.
#include "Buchla200eModuleTable.h"

// On target this table must live in FLASH: Teensy 4 puts .rodata in DTCM by
// default, and ~670 bytes of model names is not worth a DTCM bank. PROGMEM
// is a no-op on host builds (see test/host_stubs/Arduino.h). Direct reads
// work on both -- flash is memory-mapped on this core, so no pgm_read_*
// accessor is needed.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define B200E_TABLE_DATA PROGMEM
#define B200E_TABLE_CODE FLASHMEM
#else
#define B200E_TABLE_DATA
#define B200E_TABLE_CODE
#endif

// Transcribed from Studio H 2Wireless.ino getDisplayMessage(), with the "e"
// the products actually carry (251e, 272e) added to every model number --
// their switch says "251 A", the panel says 251e, and the panel wins. 266h
// is spelled as Studio H spells it. Keep sorted by address: the lookup below
// relies on nothing, but a sorted table is far easier to diff against the
// source if it is ever revised.
static const Buchla200eModuleEntry kModules[] B200E_TABLE_DATA = {
  // LOCAL addition, not from the 2Wireless source: Oren's MARF (a 248, the
  // Multiple Arbitrary Function generator -- no "e", it is not an e-series
  // module) sits at 0x0E, in the 0x08-0x0F range the Buchla table leaves
  // unclaimed. It runs the MARF firmware this project's bus layer was
  // ported from, answers QUERY, and obeys broadcast RECALL/SAVE, so a bus
  // sweep should name it.
  {0x0E, "248"},
  {0x10, "257e A"},   {0x11, "257e B"},   {0x12, "267e A"},   {0x13, "267e B"},
  {0x20, "210e"},     {0x21, "225e"},     {0x23, "227e"},     {0x24, "249e A"},
  {0x25, "249e B"},   {0x28, "259e A"},   {0x29, "259e B"},   {0x2A, "259e C"},
  {0x2B, "259e D"},   {0x2C, "261e A"},   {0x2D, "261e B"},   {0x2E, "261e C"},
  {0x2F, "261e D"},   {0x30, "260e A"},   {0x31, "260e B"},   {0x32, "266e A"},
  {0x33, "266e B"},   {0x34, "256e A1"},  {0x35, "256e B1"},  {0x36, "256e A2"},
  {0x37, "256e B2"},  {0x38, "281e A1"},  {0x39, "281e A2"},  {0x3A, "281e B1"},
  {0x3B, "281e B2"},  {0x3C, "281e C1"},  {0x3D, "281e C2"},  {0x3E, "281e D1"},
  {0x3F, "281e D2"},  {0x40, "222e A"},   {0x41, "250e A"},   {0x42, "250e B"},
  {0x44, "291e A"},   {0x45, "291e B"},   {0x48, "292e A"},   {0x49, "292e B"},
  {0x4A, "292e C"},   {0x4B, "292e D"},   {0x4C, "285e FS A"},{0x4D, "285e FS B"},
  {0x4E, "285e BM A"},{0x4F, "285e BM B"},{0x5A, "223e M"},   {0x5B, "223e A"},
  {0x5C, "251e A"},   {0x5E, "272e"},     {0x5F, "252e A"},   {0x60, "206e"},
  {0x63, "207e"},     {0x64, "296e A"},   {0x65, "296e B"},   {0x66, "230e A"},
  {0x67, "230e B"},   {0x68, "230e C"},   {0x69, "230e D"},   {0x70, "218e"},
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
