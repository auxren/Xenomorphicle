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
static BusCardCoverage cov;
// Image provenance for BusCardRestoreCheck().
enum { IMG_NONE, IMG_FILE, IMG_CAPTURE_OPEN, IMG_CAPTURE_ENDED };
static uint8_t img_origin = IMG_NONE;
static uint8_t cap_done = 0;
static BusCardCaptureVerdict cap_verdict = BUSCARD_CAPTURE_EMPTY;
static uint32_t cap_writes = 0;  // stats.bytes_written when the capture ended
static uint32_t txn_data = 0;    // data bytes in the open write transaction

static void coverage_clear(void) {
  cov = BusCardCoverage();
  cov.first_hole = BUSCARD_NO_HOLE;
  txn_data = 0;
}

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
  coverage_clear();
  img_origin = IMG_NONE;
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
  txn_data = 0;
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
      if (txn_data++ == 0) cov.bursts++;
      cov.data_bytes++;
      if (ptr == cov.contig) {
        cov.contig++;
      } else if (ptr < cov.contig) {
        cov.rewrites++;
      } else {
        if (cov.first_hole == BUSCARD_NO_HOLE) cov.first_hole = cov.contig;
        cov.beyond++;
      }
      if (ptr + 1 > cov.extent) cov.extent = ptr + 1;
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

void BusCardCoverageReset(void) { coverage_clear(); }
const BusCardCoverage *BusCardGetCoverage(void) { return &cov; }

BusCardCaptureVerdict BusCardCaptureJudge(const BusCardCoverage *c) {
  if (!c || c->data_bytes == 0) return BUSCARD_CAPTURE_EMPTY;
  if (c->beyond || c->contig < c->extent) return BUSCARD_CAPTURE_HOLED;
  return c->rewrites ? BUSCARD_CAPTURE_REWRITTEN : BUSCARD_CAPTURE_CLEAN;
}

const char *BusCardCaptureVerdictText(BusCardCaptureVerdict v) {
  switch (v) {
    case BUSCARD_CAPTURE_EMPTY:     return "EMPTY";
    case BUSCARD_CAPTURE_CLEAN:     return "CLEAN";
    case BUSCARD_CAPTURE_REWRITTEN: return "REWRITTEN";
    case BUSCARD_CAPTURE_HOLED:     return "HOLED";
  }
  return "?";
}

void BusCardImageFromFile(int whole) { img_origin = whole ? IMG_FILE : IMG_NONE; }
void BusCardImageForget(void) { img_origin = IMG_NONE; }
void BusCardImageCaptureBegin(void) { img_origin = IMG_CAPTURE_OPEN; }

void BusCardImageCaptureEnd(int done) {
  if (img_origin != IMG_CAPTURE_OPEN) return;   // no capture began: nothing to judge
  img_origin = IMG_CAPTURE_ENDED;
  cap_done = done ? 1 : 0;
  cap_verdict = BusCardCaptureJudge(&cov);
  cap_writes = stats.bytes_written;
}

BusCardRestoreBlock BusCardRestoreCheck(void) {
  switch (img_origin) {
    case IMG_FILE:         return card ? BUSCARD_RESTORE_OK : BUSCARD_RESTORE_NO_IMAGE;
    case IMG_CAPTURE_OPEN: return BUSCARD_RESTORE_CAPTURE_OPEN;
    case IMG_CAPTURE_ENDED:
      if (!card) return BUSCARD_RESTORE_NO_IMAGE;
      if (!cap_done) return BUSCARD_RESTORE_CAPTURE_FAILED;
      if (cap_verdict != BUSCARD_CAPTURE_CLEAN && cap_verdict != BUSCARD_CAPTURE_REWRITTEN)
        return BUSCARD_RESTORE_CAPTURE_BAD;
      if (stats.bytes_written != cap_writes) return BUSCARD_RESTORE_WRITTEN_SINCE;
      return BUSCARD_RESTORE_OK;
    default:               return BUSCARD_RESTORE_NO_IMAGE;
  }
}

const char *BusCardRestoreBlockText(BusCardRestoreBlock b) {
  switch (b) {
    case BUSCARD_RESTORE_OK:             return "ok";
    case BUSCARD_RESTORE_NO_IMAGE:       return "no known-good image (no whole PBCARD.BIN, and no clean capture)";
    case BUSCARD_RESTORE_CAPTURE_OPEN:   return "a BACKUP is still running";
    case BUSCARD_RESTORE_CAPTURE_FAILED: return "the last BACKUP failed; the image was blanked for it";
    case BUSCARD_RESTORE_CAPTURE_BAD:    return "the last BACKUP was EMPTY or HOLED; the image is not the bank";
    case BUSCARD_RESTORE_WRITTEN_SINCE:  return "a module wrote into the card after the capture";
  }
  return "?";
}

int BusCardSelfBackupSettled(int dirty, int ours, uint32_t arm_ms, uint32_t now_ms) {
  if (!dirty || ours) return 1;
  if (!arm_ms) return 0;                         // bytes landed, not even seen by the loop yet
  return now_ms - arm_ms >= BUSCARD_SETTLE_MS;
}

int BusCardAttached(void) { return card != NULL; }
int BusCardDirty(void) { return dirty; }
void BusCardClearDirty(void) { dirty = 0; }
uint32_t BusCardPointer(void) { return ptr; }
uint8_t BusCardAddrBytes(void) { return addr_bytes; }
const BusCardStats *BusCardGetStats(void) { return &stats; }
