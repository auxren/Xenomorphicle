// 200e storage-card slave emulator. See PresetBusCard.h for the protocol.
#include "PresetBusCard.h"

#include <stddef.h>

static uint8_t *card = NULL;
static uint32_t card_size = 0;
static uint32_t ptr_mask = 0;    // 0xFFFF-style (2-byte, image-sized) or 0xFFFFFF (3-byte)
static uint8_t  addr_bytes = 2;  // pointer bytes the master sends
static uint32_t ptr = 0;         // memory pointer, survives transactions
static uint8_t dirty = 0;

// Open-transaction state. phase counts pointer bytes received this write
// transaction (< addr_bytes = still collecting the pointer, else data phase).
static uint8_t in_txn = 0;       // 0 = idle, 1 = write, 2 = read
static uint8_t phase = 0;
static uint32_t ptr_staged = 0;

static BusCardStats stats;

int BusCardInitEx(uint8_t *image, uint32_t size, uint8_t abytes) {
  card = NULL;
  card_size = 0;
  ptr_mask = 0;
  addr_bytes = 2;
  ptr = 0;
  dirty = 0;
  in_txn = 0;
  phase = 0;
  stats = BusCardStats();
  if (!image) return 0;  // detach is always fine
  if (abytes != 2 && abytes != 3) return -1;
  if (size < 4) return -1;
  // 2-byte mode wraps like a 24xx/FRAM: the pointer space must be the image.
  if (abytes == 2 && (size & (size - 1))) return -1;
  if (abytes == 3 && size > 0x1000000u) return -1;
  card = image;
  card_size = size;
  addr_bytes = abytes;
  ptr_mask = (abytes == 2) ? (size - 1) : 0xFFFFFFu;
  return 0;
}

int BusCardInit(uint8_t *image, uint32_t size) { return BusCardInitEx(image, size, 2); }

void BusCardStart(int is_read) {
  if (in_txn) BusCardStop();  // lost STOP: close the old transaction first
  in_txn = is_read ? 2 : 1;
  phase = 0;
  if (is_read) stats.txns_read++; else stats.txns_write++;
}

void BusCardRxByte(uint8_t b) {
  if (in_txn != 1) return;
  if (phase < addr_bytes) {            // pointer bytes, big-endian, high byte first
    const uint32_t shifted = (uint32_t)b << (8 * (addr_bytes - 1 - phase));
    ptr_staged = (phase == 0) ? shifted : (ptr_staged | shifted);
    if (++phase == addr_bytes) ptr = ptr_staged & ptr_mask;
  } else if (card) {                   // data
    if (ptr < card_size) {             // beyond the window: a real card's erased flash, read-only
      card[ptr] = b;
      dirty = 1;
      stats.bytes_written++;
    }
    ptr = (ptr + 1) & ptr_mask;
  }
}

uint8_t BusCardTxByte(void) {
  if (!card) return 0xFF;
  const uint8_t b = (ptr < card_size) ? card[ptr] : 0xFF;
  ptr = (ptr + 1) & ptr_mask;
  stats.bytes_read++;
  return b;
}

void BusCardTxRewind(void) {
  if (!card) return;
  ptr = (ptr - 1) & ptr_mask;
  if (stats.bytes_read) stats.bytes_read--;
}

void BusCardStop(void) {
  // A short write moved only the leading pointer byte(s): commit them with the
  // rest forced to 0, 24xx-style (the pointer write is what the master asked
  // for; losing it would corrupt a hi-only positioning sequence).
  if (in_txn == 1 && phase > 0 && phase < addr_bytes) ptr = ptr_staged & ptr_mask;
  in_txn = 0;
  phase = 0;
}

int BusCardAttached(void) { return card != NULL; }
int BusCardDirty(void) { return dirty; }
void BusCardClearDirty(void) { dirty = 0; }
uint32_t BusCardPointer(void) { return ptr; }
uint8_t BusCardAddrBytes(void) { return addr_bytes; }
const BusCardStats *BusCardGetStats(void) { return &stats; }
