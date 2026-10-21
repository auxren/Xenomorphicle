// Per-address cache of the firmware versions 200e modules have broadcast.
// See the header for why this is a cache of what was SHOWN and not the
// result of a query: the bus has no query for it.
#include "Buchla200eFwVersions.h"

#include <string.h>

// Same arrangement as Buchla200eModuleTable.cpp: on target the code lives in
// flash (it runs from the bus layer's loop-context hook and the app's draw,
// never from an ISR); on the host these are no-ops.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define B200E_FWVER_CODE FLASHMEM
#else
#define B200E_FWVER_CODE
#endif

namespace {

struct Entry {
  uint8_t  addr;
  uint8_t  used;                          // 0 = free slot
  uint8_t  ver[BUCHLA200E_FWVER_LEN];     // as sent, space-padded
  uint32_t seq;                           // when it was heard, in Note() calls
};

Entry    entries[BUCHLA200E_FWVER_SLOTS];
uint32_t generation;
uint32_t seq_counter;

int find(uint8_t addr) {
  for (int i = 0; i < BUCHLA200E_FWVER_SLOTS; ++i)
    if (entries[i].used && entries[i].addr == addr) return i;
  return -1;
}

// A free slot, else the one heard from longest ago.
int claim() {
  int oldest = 0;
  for (int i = 0; i < BUCHLA200E_FWVER_SLOTS; ++i) {
    if (!entries[i].used) return i;
    if (entries[i].seq < entries[oldest].seq) oldest = i;
  }
  return oldest;
}

void fill(uint8_t padded[BUCHLA200E_FWVER_LEN], const uint8_t *ver, uint8_t n) {
  for (int i = 0; i < BUCHLA200E_FWVER_LEN; ++i)
    padded[i] = (i < n) ? ver[i] : ' ';
}

}  // namespace

B200E_FWVER_CODE
void Buchla200eFwVersionNote(uint8_t addr, const uint8_t *ver, uint8_t n) {
  if (!ver || n == 0) return;
  addr &= 0x7F;
  uint8_t padded[BUCHLA200E_FWVER_LEN];
  fill(padded, ver, n);

  int i = find(addr);
  if (i >= 0 && memcmp(entries[i].ver, padded, BUCHLA200E_FWVER_LEN) == 0) {
    entries[i].seq = ++seq_counter;   // heard again, unchanged: just fresher
    return;
  }
  if (i < 0) i = claim();
  entries[i].addr = addr;
  entries[i].used = 1;
  memcpy(entries[i].ver, padded, BUCHLA200E_FWVER_LEN);
  entries[i].seq = ++seq_counter;
  ++generation;
}

B200E_FWVER_CODE
int Buchla200eFwVersionGet(uint8_t addr, char *out, int cap) {
  if (!out || cap <= 0) return 0;
  out[0] = 0;
  const int i = find(addr & 0x7F);
  if (i < 0) return 0;

  // Trailing padding is dropped, and padding means a space OR a NUL: the
  // 259e pads a 6-character string to 7 with a space ("31.2C1 "), the 272e
  // sends its C-string terminator instead ("31.0C1\0", bench 2026-10-20),
  // and neither byte is part of the version. A NUL anywhere else is still
  // shown as '.', like any other non-text byte.
  int len = BUCHLA200E_FWVER_LEN;
  while (len > 0 && (entries[i].ver[len - 1] == ' ' || entries[i].ver[len - 1] == 0))
    --len;
  if (len > cap - 1) len = cap - 1;
  for (int k = 0; k < len; ++k) {
    const uint8_t c = entries[i].ver[k];
    out[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
  }
  out[len] = 0;
  return len;
}

B200E_FWVER_CODE
bool Buchla200eFwVersionKnown(uint8_t addr) { return find(addr & 0x7F) >= 0; }

B200E_FWVER_CODE
int Buchla200eFwVersionCount(void) {
  int n = 0;
  for (int i = 0; i < BUCHLA200E_FWVER_SLOTS; ++i) n += entries[i].used ? 1 : 0;
  return n;
}

uint32_t Buchla200eFwVersionGeneration(void) { return generation; }

static const uint64_t kPackMarker = (uint64_t)0xA5 << 56;

B200E_FWVER_CODE
uint64_t Buchla200eFwVersionPack(uint8_t addr) {
  const int i = find(addr & 0x7F);
  if (i < 0) return 0;
  uint64_t v = kPackMarker;
  for (int k = 0; k < BUCHLA200E_FWVER_LEN; ++k)
    v |= (uint64_t)entries[i].ver[k] << (8 * k);
  return v;
}

B200E_FWVER_CODE
bool Buchla200eFwVersionRestore(uint8_t addr, uint64_t packed) {
  if ((packed & ((uint64_t)0xFF << 56)) != kPackMarker) return false;
  addr &= 0x7F;
  if (find(addr) >= 0) return false;   // the bus has spoken since power-up
  const int i = claim();
  entries[i].addr = addr;
  entries[i].used = 1;
  for (int k = 0; k < BUCHLA200E_FWVER_LEN; ++k)
    entries[i].ver[k] = (uint8_t)(packed >> (8 * k));
  entries[i].seq = ++seq_counter;
  return true;                         // generation deliberately untouched
}

B200E_FWVER_CODE
void Buchla200eFwVersionReset(void) {
  memset(entries, 0, sizeof(entries));
  generation = 0;
  seq_counter = 0;
}
