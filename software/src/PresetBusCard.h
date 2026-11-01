#ifndef PRESETBUSCARD_H_
#define PRESETBUSCARD_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// 200e storage-card slave emulator (address 0x50).
//
// Real Buchla storage cards are 24xx-series I2C EEPROMs; the 2WIRELESS WPM
// serves the same address from FRAM. Both speak the same wire protocol:
//   write txn:  [ptr_hi][ptr_lo][data...]   -- 2-byte big-endian pointer,
//               remaining bytes stored at ptr++ (linear, wraps at card size;
//               FRAM-style, no EEPROM page wrap -- matches the WPM)
//   read txn:   bytes streamed from ptr++ (wraps at card size); the pointer
//               survives transactions, so pointer-write then repeated-start
//               read is the normal random-access sequence.
// A 0/1-byte write moves (or half-moves: hi byte only, lo forced 0) the
// pointer and stores nothing -- the WPM presence probe (empty write) is
// exactly this and must stay side-effect-free on the image.
//
// BSP-free and host-testable, same pattern as PresetBus200e: the transport
// (LPI2C1 slave ISR on target, the test suite on host) drives these entry
// points. All of them are ISR-context safe: no allocation, no I/O, O(1).
//
// SERVING IS HARD-GATED AT THE TRANSPORT: this state machine never decides
// whether to ACK 0x50 -- the transport must not route events here (nor match
// the address in hardware) while a real preset manager owns 0x50.
// ---------------------------------------------------------------------------

// 64K = the ENTIRE address space a 2-byte card pointer can name, so the wrap
// below can never silently eat a transfer: whatever a master writes, it lands
// somewhere distinct. Sized up from 32768 (24LC256) after a real 251e BACKUP
// -- 30 slots x 2104 = 63120 bytes -- wrapped at 32K and overwrote its own
// first 30352 bytes with its tail. 24LC512-shaped; still one flat malloc.
#define BUSCARD_SIZE 65536u  // 24LC512 / full 16-bit pointer span

typedef struct {
  uint32_t txns_write;   // write transactions (incl. pointer-only)
  uint32_t txns_read;
  uint32_t bytes_written;
  uint32_t bytes_read;
} BusCardStats;

// Attach backing storage (caller owns it; NULL detaches). Resets pointer,
// transaction state, stats and coverage; leaves the image contents untouched.
// size must be a power of two >= 4 (pointer wrap is a mask); returns 0 on
// success, -1 on a bad size (card left detached).
int BusCardInit(uint8_t *image, uint32_t size);

// The USB Firmware Card for AVR32 modules (251e, 257e, 267e, 296e, 225h, 226h,
// 218e...) is the same card holding a "#$200EFU" (v2) image, read with a
// 3-byte big-endian pointer over a 0x38000-byte window; reads beyond the
// window return 0xFF (erased flash) and the pointer does not wrap into the
// image. addr_bytes selects 2 (24xx/FRAM, image must be a power of two) or 3
// (any size <= 16 MB). The transport picks the width from the image magic the
// way the card firmware does: "#!" -> 2, "#$" -> 3.
int BusCardInitEx(uint8_t *image, uint32_t size, uint8_t addr_bytes);
uint8_t BusCardAddrBytes(void);

// Address 0x50 matched. is_read = R/W bit of the address byte.
void BusCardStart(int is_read);

// Master wrote a byte to us (write transaction only).
void BusCardRxByte(uint8_t b);

// Master reads a byte from us; returns the byte to put on the wire.
// Reads past the image (no card attached) return 0xFF like a floating bus.
uint8_t BusCardTxByte(void);

// STOP or repeated-START: close the open transaction.
void BusCardStop(void);

// Transport prefetch correction: a hardware TX register is one byte deep,
// so the transport feeds byte N+1 while N shifts out; when the master NAKs,
// that prefetched byte was never sent. Rewinding once on read-leg close
// keeps the pointer identical to a real 24xx address counter, so chunked
// current-address reads (no pointer rewrite per chunk) stay aligned.
void BusCardTxRewind(void);

// ---- capture coverage (what a master BACKUP actually wrote) ----------------
// The lifetime counters above cannot say whether a capture is whole: a byte
// lost on receive lowers bytes_written, a burst the module resends raises it,
// and the image keeps whatever an earlier transfer left in the holes. The
// Studio H CSR showed all three (2026-09-19: twelve captures climbing +13 per
// attempt past its 3,180-byte bank, stale slots that agreed with each other).
//
// Coverage follows the card ADDRESS each byte lands at, since the BACKUP
// (mem_off 0) was reset with BusCardCoverageReset():
//   contig     bytes [0, contig) were each written, in order. This is the
//              size of the capture that can be trusted.
//   extent     highest offset written + 1.
//   data_bytes every data byte written, rewrites included (= the old count).
//   rewrites   data bytes written below contig: a resent burst, or a pointer
//              that jumped backwards.
//   beyond     data bytes written above contig: something before them is
//              missing (a hole). first_hole is where the first one opened.
//   bursts     write transactions that carried data.
// Every module seen so far writes its bank in ascending bursts from offset 0,
// each a [ptr_hi][ptr_lo][data...] transaction (24xx page-write style; the
// burst breaks at record and card-page boundaries). A bank written out of
// order would read as HOLED here even if every byte arrived -- conservative
// on purpose: a capture is only called whole when the addresses prove it.
// ISR-written, loop-read; the reset is a few word stores.
#define BUSCARD_NO_HOLE 0xFFFFFFFFu
typedef struct {
  uint32_t contig;
  uint32_t extent;
  uint32_t data_bytes;
  uint32_t rewrites;
  uint32_t beyond;
  uint32_t bursts;
  uint32_t first_hole;   // BUSCARD_NO_HOLE when none
} BusCardCoverage;

typedef enum : uint8_t {
  BUSCARD_CAPTURE_EMPTY = 0,  // nothing written since the reset
  BUSCARD_CAPTURE_CLEAN,      // [0, extent) written once each, in order
  BUSCARD_CAPTURE_REWRITTEN,  // whole, but some bytes were written twice
  BUSCARD_CAPTURE_HOLED,      // a gap below extent: do not trust past contig
} BusCardCaptureVerdict;

void BusCardCoverageReset(void);
const BusCardCoverage *BusCardGetCoverage(void);
BusCardCaptureVerdict BusCardCaptureJudge(const BusCardCoverage *c);
const char *BusCardCaptureVerdictText(BusCardCaptureVerdict v);

int  BusCardAttached(void);
int  BusCardDirty(void);        // image modified since last ClearDirty
void BusCardClearDirty(void);
uint32_t BusCardPointer(void);  // current memory pointer (debug/tests)
const BusCardStats *BusCardGetStats(void);

#endif  // PRESETBUSCARD_H_
