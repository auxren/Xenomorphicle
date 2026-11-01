// Host tests for the 200e storage-card slave emulator (src/PresetBusCard.cpp):
// 24xx/FRAM wire protocol -- pointer writes, sequential/wrapping reads and
// writes, pointer persistence across transactions, the WPM empty-probe
// no-op, lost-STOP recovery, and detached-card behaviour.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buscard test_buscard.cpp ../src/PresetBusCard.cpp && ./build/test_buscard
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/PresetBusCard.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static uint8_t img[BUSCARD_SIZE];

// ---- master-side helpers (what the wire would carry) ------------------------

static void m_write(const uint8_t *bytes, uint32_t n) {  // full write txn
  BusCardStart(0);
  for (uint32_t i = 0; i < n; ++i) BusCardRxByte(bytes[i]);
  BusCardStop();
}

static void m_write_at(uint16_t addr, const uint8_t *d, uint32_t n) {
  BusCardStart(0);
  BusCardRxByte(addr >> 8);
  BusCardRxByte(addr & 0xFF);
  for (uint32_t i = 0; i < n; ++i) BusCardRxByte(d[i]);
  BusCardStop();
}

static void m_read(uint8_t *out, uint32_t n) {  // current-address read txn
  BusCardStart(1);
  for (uint32_t i = 0; i < n; ++i) out[i] = BusCardTxByte();
  BusCardStop();
}

static void m_read_at(uint16_t addr, uint8_t *out, uint32_t n) {
  // pointer write, repeated-start, read -- the standard random-access read
  BusCardStart(0);
  BusCardRxByte(addr >> 8);
  BusCardRxByte(addr & 0xFF);
  BusCardStart(1);  // repeated start closes the write txn
  for (uint32_t i = 0; i < n; ++i) out[i] = BusCardTxByte();
  BusCardStop();
}

// ---- tests -------------------------------------------------------------------

static void test_init_gating() {
  CHECK(BusCardInit(img, 100) == -1);       // not a power of two
  CHECK(!BusCardAttached());
  CHECK(BusCardInit(img, 2) == -1);         // too small
  CHECK(BusCardInit(NULL, 0) == 0);         // detach ok
  CHECK(!BusCardAttached());
  CHECK(BusCardInit(img, BUSCARD_SIZE) == 0);
  CHECK(BusCardAttached());
  CHECK(!BusCardDirty());
  CHECK(BusCardPointer() == 0);
}

static void test_write_read_roundtrip() {
  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0xEE, sizeof(img));

  const uint8_t d[4] = { 0x11, 0x22, 0x33, 0x44 };
  m_write_at(0x1234, d, 4);
  CHECK(BusCardDirty());
  CHECK(img[0x1234] == 0x11 && img[0x1237] == 0x44);
  CHECK(img[0x1233] == 0xEE && img[0x1238] == 0xEE);  // neighbours untouched
  CHECK(BusCardPointer() == 0x1238);

  uint8_t r[4] = {};
  m_read_at(0x1234, r, 4);
  CHECK(memcmp(r, d, 4) == 0);
  CHECK(BusCardGetStats()->bytes_written == 4);
  CHECK(BusCardGetStats()->bytes_read == 4);
}

static void test_pointer_persistence() {
  BusCardInit(img, BUSCARD_SIZE);
  for (uint32_t i = 0; i < 8; ++i) img[0x0100 + i] = (uint8_t)i;

  uint8_t r[4];
  m_read_at(0x0100, r, 4);          // leaves pointer at 0x0104
  CHECK(BusCardPointer() == 0x0104);
  m_read(r, 4);                     // current-address read continues
  CHECK(r[0] == 4 && r[3] == 7);
}

static void test_wrap() {
  BusCardInit(img, BUSCARD_SIZE);
  const uint8_t d[3] = { 0xA1, 0xA2, 0xA3 };
  m_write_at(BUSCARD_SIZE - 2, d, 3);   // crosses the end
  CHECK(img[BUSCARD_SIZE - 2] == 0xA1);
  CHECK(img[BUSCARD_SIZE - 1] == 0xA2);
  CHECK(img[0] == 0xA3);                // linear wrap, no page boundary
  CHECK(BusCardPointer() == 1);

  uint8_t r[3];
  m_read_at(BUSCARD_SIZE - 2, r, 3);
  CHECK(r[0] == 0xA1 && r[1] == 0xA2 && r[2] == 0xA3);
}

static void test_probe_is_side_effect_free() {
  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0x5A, sizeof(img));
  BusCardClearDirty();
  const uint32_t p0 = BusCardPointer();

  m_write(NULL, 0);                 // the WPM presence probe: empty write
  CHECK(!BusCardDirty());
  CHECK(BusCardPointer() == p0);    // pointer untouched by a 0-byte write
  CHECK(BusCardGetStats()->bytes_written == 0);
  for (uint32_t i = 0; i < BUSCARD_SIZE; ++i)
    if (img[i] != 0x5A) { CHECK(!"image modified by empty probe"); break; }
}

static void test_short_writes() {
  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0, sizeof(img));

  // 1-byte write = hi-only pointer positioning (lo forced 0), no data
  const uint8_t hi = 0x12;
  m_write(&hi, 1);
  CHECK(BusCardPointer() == 0x1200);
  CHECK(!BusCardDirty());

  // 2-byte write = full pointer positioning, no data
  const uint8_t p[2] = { 0x00, 0x40 };
  m_write(p, 2);
  CHECK(BusCardPointer() == 0x0040);
  CHECK(!BusCardDirty());
}

static void test_lost_stop_recovery() {
  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0, sizeof(img));

  // transport drops a STOP: a new START must close the stale transaction
  BusCardStart(0);
  BusCardRxByte(0x30);              // half a pointer, then the STOP is lost
  BusCardStart(0);                  // next transaction begins
  BusCardRxByte(0x00);
  BusCardRxByte(0x10);
  BusCardRxByte(0x77);
  BusCardStop();
  CHECK(img[0x0010] == 0x77);       // new pointer parsed from scratch
  CHECK(img[0x3000] == 0 && img[0x3010] == 0);
}

static void test_tx_prefetch_rewind() {
  // The transport's TDF feeder always has one unsent byte in the TX
  // register when the master NAKs; BusCardTxRewind() on read-leg close
  // must land the pointer exactly where a real 24xx counter would.
  BusCardInit(img, BUSCARD_SIZE);
  for (uint32_t i = 0; i < 12; ++i) img[0x0200 + i] = (uint8_t)(0xC0 + i);

  // master chunk-reads 4 bytes; transport fed 5 (one prefetched)
  BusCardStart(0);
  BusCardRxByte(0x02);
  BusCardRxByte(0x00);
  BusCardStart(1);
  uint8_t r[5];
  for (int i = 0; i < 5; ++i) r[i] = BusCardTxByte();  // 5th = prefetch
  BusCardTxRewind();   // transport: NAK'd byte never left the wire
  BusCardStop();
  CHECK(r[3] == 0xC3);
  CHECK(BusCardPointer() == 0x0204);  // == start + 4, 24xx-compatible

  // next chunk via current-address read continues seamlessly: master takes
  // 4 more bytes, transport again fed one extra before the NAK
  BusCardStart(1);
  for (int i = 0; i < 5; ++i) r[i % 5] = BusCardTxByte();
  BusCardTxRewind();
  BusCardStop();
  CHECK(BusCardPointer() == 0x0208);

  BusCardStart(1);
  CHECK(BusCardTxByte() == 0xC8);  // exactly where a 24xx would be
  BusCardStop();
}

// A real 251e BACKUP is 30 slots x 2104 = 63120 bytes in one pointer-write
// plus a long data burst. At the old 32768-byte size that wrapped at stream
// byte 32768 and silently ate its own first 30352 bytes; the image must now
// hold the whole bank with every byte distinct and in order.
static void test_full_251e_bank_does_not_wrap() {
  static const uint32_t kBank = 30u * 2104u;  // 63120
  CHECK(kBank <= BUSCARD_SIZE);               // the point of the resize
  CHECK(kBank > 32768u);                      // ...and it used to wrap

  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0xFF, sizeof(img));

  // one transaction, pointer 0, then the whole bank streamed in
  BusCardStart(0);
  BusCardRxByte(0);
  BusCardRxByte(0);
  for (uint32_t i = 0; i < kBank; ++i)
    BusCardRxByte((uint8_t)(i * 31u + (i >> 8)));  // position-dependent
  BusCardStop();

  CHECK(BusCardPointer() == kBank);             // never wrapped to 0
  CHECK(BusCardGetStats()->bytes_written == kBank);
  uint32_t bad = 0;
  for (uint32_t i = 0; i < kBank; ++i)
    if (img[i] != (uint8_t)(i * 31u + (i >> 8))) bad++;
  CHECK(bad == 0);
  // the tail beyond the bank is still erased -- nothing overran
  CHECK(img[kBank] == 0xFF && img[BUSCARD_SIZE - 1] == 0xFF);

  // and it reads back as one continuous run, which is what DumpCard prints
  uint8_t r[8];
  m_read_at((uint16_t)(kBank - 4), r, 4);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t p = kBank - 4 + i;
    CHECK(r[i] == (uint8_t)(p * 31u + (p >> 8)));
  }
}

// The card pointer is 2 bytes on the wire, so 0x0000..0xFFFF is the entire
// space a master can name. Sizing the image to exactly that means no address
// a master can utter aliases onto another -- the wrap is now unreachable
// except by running off the very top.
static void test_pointer_space_is_fully_addressable() {
  CHECK(BUSCARD_SIZE == 0x10000u);
  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0, sizeof(img));

  const uint8_t hi[1] = { 0x80 };
  m_write(hi, 1);
  CHECK(BusCardPointer() == 0x8000);   // above the old size, not folded to 0

  const uint8_t d[1] = { 0x5C };
  m_write_at(0xFFFF, d, 1);            // last addressable byte
  CHECK(img[0xFFFF] == 0x5C);
  CHECK(img[0x7FFF] == 0);             // did NOT alias into the old top
  CHECK(BusCardPointer() == 0);        // ...and only there does it wrap
}

static void test_detached() {
  BusCardInit(NULL, 0);
  BusCardStart(1);
  CHECK(BusCardTxByte() == 0xFF);   // floating-bus reads
  BusCardStop();
  const uint8_t d[3] = { 0x00, 0x00, 0x99 };
  m_write(d, 3);                    // data write with no card: dropped
  CHECK(!BusCardDirty());
  CHECK(BusCardGetStats()->bytes_written == 0);
  // transactions still counted while detached
  CHECK(BusCardGetStats()->txns_write == 1);
  CHECK(BusCardGetStats()->txns_read == 1);
}

// ---- v2 firmware card: 3-byte pointer, 0x38000 window, 0xFF beyond ---------
// The USB Firmware Card serves AVR32 modules (251e, 257e, ...) with a 3-byte
// big-endian pointer over a 224 KB window; the card firmware picks the width
// from the image magic ("#$200EFU"). Reads past the window return 0xFF.

#define V2_SIZE 0x38000u
static uint8_t img2[V2_SIZE];

static void m3_read_at(uint32_t addr, uint8_t *out, uint32_t n) {
  BusCardStart(0);
  BusCardRxByte((addr >> 16) & 0xFF);
  BusCardRxByte((addr >> 8) & 0xFF);
  BusCardRxByte(addr & 0xFF);
  BusCardStart(1);
  for (uint32_t i = 0; i < n; ++i) out[i] = BusCardTxByte();
  BusCardStop();
}

static void test_v2_init_and_width() {
  CHECK(BusCardInitEx(img2, V2_SIZE, 3) == 0);     // not a power of two: fine in 3-byte mode
  CHECK(BusCardAttached());
  CHECK(BusCardAddrBytes() == 3);
  CHECK(BusCardInitEx(img2, V2_SIZE, 4) == -1);    // only 2 or 3
  CHECK(!BusCardAttached());
  CHECK(BusCardInitEx(img, 100, 2) == -1);         // 2-byte mode keeps the power-of-two rule
  CHECK(BusCardInit(img, BUSCARD_SIZE) == 0);
  CHECK(BusCardAddrBytes() == 2);
}

static void test_v2_random_access_above_64k() {
  BusCardInitEx(img2, V2_SIZE, 3);
  for (uint32_t i = 0; i < V2_SIZE; ++i) img2[i] = (uint8_t)(i * 7 + (i >> 8));
  uint8_t r[4];
  m3_read_at(0x000000, r, 2);
  CHECK(r[0] == img2[0] && r[1] == img2[1]);
  m3_read_at(0x012345, r, 4);
  CHECK(r[0] == img2[0x12345] && r[3] == img2[0x12348]);
  CHECK(BusCardPointer() == 0x12349);
  m3_read_at(0x037FFE, r, 2);                       // last two bytes of the window
  CHECK(r[0] == img2[0x37FFE] && r[1] == img2[0x37FFF]);
}

static void test_v2_sequential_read_crosses_64k() {
  BusCardInitEx(img2, V2_SIZE, 3);
  uint8_t r[4];
  m3_read_at(0x00FFFE, r, 4);                       // 0xFFFE 0xFFFF 0x10000 0x10001
  CHECK(r[0] == img2[0xFFFE] && r[1] == img2[0xFFFF]);
  CHECK(r[2] == img2[0x10000] && r[3] == img2[0x10001]);
  CHECK(BusCardPointer() == 0x10002);
  // current-address read continues from there (the bootloader's chunked reads)
  m_read(r, 2);
  CHECK(r[0] == img2[0x10002] && r[1] == img2[0x10003]);
}

static void test_v2_beyond_window_reads_ff() {
  BusCardInitEx(img2, V2_SIZE, 3);
  uint8_t r[3];
  m3_read_at(V2_SIZE - 1, r, 3);
  CHECK(r[0] == img2[V2_SIZE - 1]);
  CHECK(r[1] == 0xFF && r[2] == 0xFF);              // erased flash beyond the window
  CHECK(BusCardPointer() == V2_SIZE + 2);           // pointer keeps counting, no wrap into the image
  m3_read_at(0x800000, r, 1);                       // 24-bit space, far outside
  CHECK(r[0] == 0xFF);
  // a write beyond the window changes nothing
  BusCardClearDirty();
  BusCardStart(0);
  BusCardRxByte(0x03); BusCardRxByte(0x80); BusCardRxByte(0x00);
  BusCardRxByte(0x55);
  BusCardStop();
  CHECK(!BusCardDirty());
  CHECK(BusCardGetStats()->bytes_written == 0);
}

static void test_v2_partial_pointer_writes() {
  BusCardInitEx(img2, V2_SIZE, 3);
  memset(img2, 0, sizeof(img2));
  const uint8_t one[1] = { 0x02 };
  m_write(one, 1);                                  // hi only -> 0x020000
  CHECK(BusCardPointer() == 0x020000);
  const uint8_t two[2] = { 0x01, 0x23 };
  m_write(two, 2);                                  // hi+mid -> 0x012300
  CHECK(BusCardPointer() == 0x012300);
  const uint8_t three[3] = { 0x00, 0x00, 0x2B };
  m_write(three, 3);                                // full pointer, no data
  CHECK(BusCardPointer() == 0x00002B);
  CHECK(!BusCardDirty());
  const uint8_t wr[5] = { 0x01, 0x00, 0x00, 0xDE, 0xAD };
  m_write(wr, 5);                                   // data lands at 0x10000
  CHECK(img2[0x10000] == 0xDE && img2[0x10001] == 0xAD);
  CHECK(BusCardDirty());
}

static void test_v2_mode_does_not_change_v1() {
  // after a v2 session, a v1 init behaves exactly as before (wrap at 64K)
  BusCardInitEx(img2, V2_SIZE, 3);
  BusCardInit(img, BUSCARD_SIZE);
  const uint8_t d[2] = { 0x11, 0x22 };
  m_write_at(BUSCARD_SIZE - 1, d, 2);
  CHECK(img[BUSCARD_SIZE - 1] == 0x11 && img[0] == 0x22);
}

// ---- capture coverage --------------------------------------------------------
// A module's BACKUP, as the card sees it: ascending [ptr_hi][ptr_lo][data...]
// bursts from offset 0, each ending at a record or a card-page boundary.
// Geometry from the bench: CSR 106-byte records / 128-byte pages (54 bursts),
// 259e 33 / 64, 251e 2104 / 64 (pages assumed; only the order matters here).
static uint8_t bank_byte(uint32_t off) { return (uint8_t)(off * 7u + 3u); }

// Emit the bank as bursts. drop_after: in burst number drop_burst, deliver only
// the first drop_after data bytes (what a receiver that closes the transaction
// mid-burst keeps). resend_burst: send that burst twice.
static uint32_t emit_bank(uint32_t rec, uint32_t slots, uint32_t page,
                          int drop_burst = -1, uint32_t drop_after = 0,
                          int resend_burst = -1) {
  uint32_t bursts = 0;
  for (uint32_t s = 0; s < slots; ++s) {
    uint32_t a = s * rec, left = rec;
    while (left) {
      const uint32_t room = page - (a % page);
      const uint32_t n = left < room ? left : room;
      uint8_t d[256];
      for (uint32_t i = 0; i < n; ++i) d[i] = bank_byte(a + i);
      const int reps = ((int)bursts == resend_burst) ? 2 : 1;
      for (int r = 0; r < reps; ++r) {
        if ((int)bursts == drop_burst) m_write_at((uint16_t)a, d, drop_after);
        else m_write_at((uint16_t)a, d, n);
      }
      a += n; left -= n; ++bursts;
    }
  }
  return bursts;
}

static bool bank_ok(uint32_t len) {
  for (uint32_t i = 0; i < len; ++i) if (img[i] != bank_byte(i)) return false;
  return true;
}

static void test_coverage_csr_clean() {
  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0xFF, sizeof(img));
  const uint32_t bursts = emit_bank(106, 30, 128);
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(bursts == 54);                          // the WPM capture's burst count
  CHECK(c->contig == 3180 && c->extent == 3180);
  CHECK(c->data_bytes == 3180 && c->bursts == 54);
  CHECK(c->rewrites == 0 && c->beyond == 0 && c->first_hole == BUSCARD_NO_HOLE);
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_CLEAN);
  CHECK(bank_ok(3180));
  CHECK(img[3180] == 0xFF);
}

static void test_coverage_csr_holed_burst() {
  // 2026-09-19 shape: one burst loses its tail on receive, the card pointer
  // still walks to the end of the bank, the count comes up short, and the
  // tail keeps what was there before.
  BusCardInit(img, BUSCARD_SIZE);
  memset(img, 0xA5, sizeof(img));              // "an earlier capture"
  emit_bank(106, 30, 128, /*drop_burst*/ 3, /*drop_after*/ 40);
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(BusCardPointer() == 3180);             // the old tell: ptr reached the end ...
  CHECK(c->data_bytes < 3180);                 // ... while the count did not
  CHECK(c->extent == 3180);
  CHECK(c->contig < 3180);                     // the trustworthy size stops at the hole
  CHECK(c->first_hole == c->contig);
  CHECK(c->beyond > 0);
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_HOLED);
  CHECK(img[c->contig] == 0xA5);               // the stale byte the hole leaves
}

static void test_coverage_resent_burst() {
  // A module that resends a burst (its EEPROM write NACK-retried) writes the
  // same addresses twice: the raw count runs past the bank, coverage does not.
  BusCardInit(img, BUSCARD_SIZE);
  emit_bank(106, 30, 128, -1, 0, /*resend_burst*/ 10);
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(c->data_bytes > 3180);
  CHECK(c->contig == 3180 && c->extent == 3180 && c->beyond == 0);
  CHECK(c->rewrites == c->data_bytes - 3180);
  CHECK(c->bursts == 55);
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_REWRITTEN);
  CHECK(bank_ok(3180));
}

static void test_coverage_partial_then_resend_heals() {
  // The tail of a burst is lost and the module resends the whole burst at
  // once: the hole is filled before anything past it lands, so the capture
  // is whole (with rewrites), not holed.
  BusCardInit(img, BUSCARD_SIZE);
  const uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  m_write_at(0, d, 3);
  m_write_at(0, d, 8);
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(c->contig == 8 && c->extent == 8 && c->rewrites == 3 && c->beyond == 0);
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_REWRITTEN);
}

static void test_coverage_other_modules_unchanged() {
  // Every module that already captured correctly reads exactly as before:
  // contig == the raw count == the bank, CLEAN.
  struct { uint32_t rec, slots, page; } g[] = {
    {33, 30, 64},     // 259e, 990
    {56, 30, 64},     // 257e, 1680
    {25, 30, 64},     // 272e, 750
    {14, 30, 64},     // 227e, 420
    {2104, 30, 64},   // 251e, 63120
  };
  for (auto &m : g) {
    BusCardInit(img, BUSCARD_SIZE);
    emit_bank(m.rec, m.slots, m.page);
    const BusCardCoverage *c = BusCardGetCoverage();
    const uint32_t bank = m.rec * m.slots;
    CHECK(c->contig == bank && c->data_bytes == bank && c->extent == bank);
    CHECK(c->data_bytes == BusCardGetStats()->bytes_written);
    CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_CLEAN);
    CHECK(bank_ok(bank));
  }
}

static void test_coverage_ignores_pointer_writes_and_reads() {
  BusCardInit(img, BUSCARD_SIZE);
  const uint8_t ptr_only[2] = {0x00, 0x10};
  m_write(ptr_only, 2);                         // random-read positioning
  m_write(ptr_only, 0);                         // WPM-style empty probe
  uint8_t r[16];
  m_read_at(0x0000, r, 16);                     // a module reading the card
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(c->data_bytes == 0 && c->bursts == 0 && c->contig == 0 && c->extent == 0);
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_EMPTY);
  CHECK(BusCardCaptureJudge(nullptr) == BUSCARD_CAPTURE_EMPTY);
}

static void test_coverage_reset() {
  BusCardInit(img, BUSCARD_SIZE);
  emit_bank(33, 30, 64);
  const uint32_t written = BusCardGetStats()->bytes_written;
  BusCardCoverageReset();
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(c->contig == 0 && c->extent == 0 && c->data_bytes == 0 && c->bursts == 0);
  CHECK(c->first_hole == BUSCARD_NO_HOLE);
  CHECK(BusCardGetStats()->bytes_written == written);  // lifetime stats untouched
  CHECK(bank_ok(990));                                   // and the image
  // a second capture after the reset counts from zero again
  emit_bank(33, 30, 64);
  CHECK(c->contig == 990 && c->rewrites == 0 && BusCardCaptureJudge(c) == BUSCARD_CAPTURE_CLEAN);
  // without the reset the same second capture would be all rewrites
  emit_bank(33, 30, 64);
  CHECK(c->rewrites == 990 && BusCardCaptureJudge(c) == BUSCARD_CAPTURE_REWRITTEN);
  // Init resets coverage too
  BusCardInit(img, BUSCARD_SIZE);
  CHECK(c->contig == 0 && c->rewrites == 0 && c->first_hole == BUSCARD_NO_HOLE);
}

static void test_coverage_pointer_jump_is_holed() {
  // Two data bytes taken as a pointer (a mis-framed transaction) send the
  // rest of the burst elsewhere. Forward: a hole. Backward: rewrites, and the
  // next burst then lands past contig: still a hole.
  BusCardInit(img, BUSCARD_SIZE);
  const uint8_t d[4] = {9, 9, 9, 9};
  m_write_at(0, d, 4);
  m_write_at(0x0400, d, 4);
  CHECK(BusCardCaptureJudge(BusCardGetCoverage()) == BUSCARD_CAPTURE_HOLED);
  CHECK(BusCardGetCoverage()->first_hole == 4);
  BusCardInit(img, BUSCARD_SIZE);
  m_write_at(0, d, 4);
  m_write_at(2, d, 2);   // jumped back
  m_write_at(8, d, 4);   // the module's next burst, where it should be
  CHECK(BusCardGetCoverage()->rewrites == 2);
  CHECK(BusCardCaptureJudge(BusCardGetCoverage()) == BUSCARD_CAPTURE_HOLED);
}

static void test_coverage_verdict_text() {
  CHECK(strcmp(BusCardCaptureVerdictText(BUSCARD_CAPTURE_CLEAN), "CLEAN") == 0);
  CHECK(strcmp(BusCardCaptureVerdictText(BUSCARD_CAPTURE_HOLED), "HOLED") == 0);
  CHECK(strcmp(BusCardCaptureVerdictText(BUSCARD_CAPTURE_REWRITTEN), "REWRITTEN") == 0);
  CHECK(strcmp(BusCardCaptureVerdictText(BUSCARD_CAPTURE_EMPTY), "EMPTY") == 0);
}

int main() {
  test_v2_init_and_width();
  test_v2_random_access_above_64k();
  test_v2_sequential_read_crosses_64k();
  test_v2_beyond_window_reads_ff();
  test_v2_partial_pointer_writes();
  test_v2_mode_does_not_change_v1();
  test_init_gating();
  test_write_read_roundtrip();
  test_pointer_persistence();
  test_wrap();
  test_probe_is_side_effect_free();
  test_short_writes();
  test_lost_stop_recovery();
  test_tx_prefetch_rewind();
  test_full_251e_bank_does_not_wrap();
  test_pointer_space_is_fully_addressable();
  test_detached();
  test_coverage_csr_clean();
  test_coverage_csr_holed_burst();
  test_coverage_resent_burst();
  test_coverage_partial_then_resend_heals();
  test_coverage_other_modules_unchanged();
  test_coverage_ignores_pointer_writes_and_reads();
  test_coverage_reset();
  test_coverage_pointer_jump_is_holed();
  test_coverage_verdict_text();
  printf("test_buscard: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
