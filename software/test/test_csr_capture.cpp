// End-to-end host test of a master BACKUP of the Studio H CSR (0x20) into the
// card emulator: the real Bus200eMaster FSM driving the real PresetBusCard,
// wired the way PresetBus.cpp wires them on target (begin_capture blanks the
// image and resets coverage; capture_bytes is the contiguous coverage), fed a
// CSR-shaped write stream: 30 x 106-byte records, bursts split at record and
// 128-byte card-page boundaries, 54 bursts, the module's 0x0A at the end.
//
// What it pins down, each from a failure seen on the bench (2026-09-18/19):
//   * a lost burst tail is reported (HOLED, bytes = up to the hole), and the
//     hole holds 0xFF, never a byte from an earlier capture;
//   * a resent burst does not inflate the byte count past the bank;
//   * a clean capture is exactly 3,180 bytes, byte-exact;
//   * a RESTORE leaves the image alone.
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_csr_capture test_csr_capture.cpp ../src/Bus200eMaster.cpp
//      ../src/PresetBus200e.cpp ../src/PresetBusCard.cpp && ./build/test_csr_capture
#include <cstdio>
#include <cstring>

#include "../src/Bus200eMaster.h"
#include "../src/PresetBusCard.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static const uint32_t kRec = 106, kSlots = 30, kPage = 128, kBank = kRec * kSlots;
static uint8_t img[BUSCARD_SIZE];
static uint8_t bank[kBank];

// ---- the target's ops, against the real card emulator ----------------------
static uint32_t now_ms;
static uint32_t t_now(void) { return now_ms; }
static int t_gate(void) { return 1; }
static int t_probe(uint8_t) { return 0; }
static int t_send(const uint8_t *, uint8_t) { return 0; }
static void t_echo(const uint8_t *, uint8_t) {}
static uint32_t t_activity(void) {
  return Bus200eMasterIsRestore() ? BusCardGetStats()->bytes_read : BusCardGetStats()->bytes_written;
}
static void t_begin(void) {           // PresetBus.cpp master_begin_capture()
  memset(img, 0xFF, sizeof(img));
  BusCardCoverageReset();
  BusCardImageCaptureBegin();
}
static uint32_t t_capture(void) { return BusCardGetCoverage()->contig; }
static void t_end(int done) { BusCardImageCaptureEnd(done); }

static const Bus200eMasterOps ops = {
  t_now, t_gate, t_probe, t_send, t_echo, t_activity, t_begin, t_capture, t_end,
};

// ---- the CSR, as the card sees it -----------------------------------------
static void csr_backup(int drop_burst, uint32_t drop_after, int resend_burst) {
  uint32_t burst = 0;
  for (uint32_t s = 0; s < kSlots; ++s) {
    uint32_t a = s * kRec, left = kRec;
    while (left) {
      const uint32_t room = kPage - (a % kPage);
      const uint32_t n = left < room ? left : room;
      const int reps = ((int)burst == resend_burst) ? 2 : 1;
      for (int r = 0; r < reps; ++r) {
        const uint32_t send = ((int)burst == drop_burst) ? drop_after : n;
        BusCardStart(0);
        BusCardRxByte((uint8_t)(a >> 8));
        BusCardRxByte((uint8_t)a);
        for (uint32_t i = 0; i < send; ++i) BusCardRxByte(bank[a + i]);
        BusCardStop();
      }
      now_ms += 4;                    // ~4 ms between bursts (healthy bus)
      Bus200eMasterTask();
      a += n; left -= n; ++burst;
    }
  }
  Bus200eMasterXferDone(0x20);        // 04 22 20 0A 20
}

static void finish(void) {
  for (int i = 0; i < 100 && Bus200eMasterGetState() == BUS200E_MASTER_TRANSFERRING; ++i) {
    now_ms += 50;
    Bus200eMasterTask();
  }
}

static void start_backup(void) {
  Bus200eMasterReset();
  CHECK(Bus200eMasterBackup(0x20, 0x00) == 0);
  Bus200eMasterTask();                // clears, sends
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_WAIT_ACTIVITY);
}

static void test_holed_capture_shows_no_stale_bytes(void) {
  printf("test_holed_capture_shows_no_stale_bytes\n");
  memset(img, 0xA5, sizeof(img));     // what an earlier capture left behind
  start_backup();
  csr_backup(/*drop_burst*/ 25, /*drop_after*/ 30, -1);   // slot 14, an edited one
  finish();
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_DONE);
  CHECK(Bus200eMasterAcked() == 1);
  CHECK(BusCardPointer() == kBank);                   // the pointer still reached the end
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_HOLED);
  CHECK(Bus200eMasterBytesTransferred() == c->first_hole);
  CHECK(Bus200eMasterBytesTransferred() < kBank);
  CHECK(Bus200eMasterActivityBytes() < kBank);
  CHECK(c->first_hole == 1484 + 30);                 // slot 14, 30 bytes into its first burst
  uint32_t stale = 0;
  for (uint32_t i = 0; i < sizeof(img); ++i) stale += (img[i] == 0xA5);
  CHECK(stale == 0);                                  // nothing from the earlier capture survives
  for (uint32_t i = 1484 + 30; i < 1536; ++i) CHECK(img[i] == 0xFF);   // the hole reads as erased
  CHECK(memcmp(img, bank, 1484 + 30) == 0);          // everything before it is the bank
  CHECK(memcmp(img + 1536, bank + 1536, kBank - 1536) == 0);  // and after it
}

static void test_clean_capture_is_exact(void) {
  printf("test_clean_capture_is_exact\n");
  start_backup();
  csr_backup(-1, 0, -1);
  finish();
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_DONE);
  CHECK(Bus200eMasterBytesTransferred() == kBank);
  CHECK(Bus200eMasterActivityBytes() == kBank);
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_CLEAN);
  CHECK(c->bursts == 54);
  CHECK(memcmp(img, bank, kBank) == 0);
  CHECK(img[kBank] == 0xFF);
}

static void test_resent_burst_does_not_inflate(void) {
  printf("test_resent_burst_does_not_inflate\n");
  start_backup();
  csr_backup(-1, 0, /*resend_burst*/ 7);
  finish();
  const BusCardCoverage *c = BusCardGetCoverage();
  CHECK(Bus200eMasterBytesTransferred() == kBank);
  CHECK(Bus200eMasterActivityBytes() > kBank);       // the old number runs past the bank
  CHECK(BusCardCaptureJudge(c) == BUSCARD_CAPTURE_REWRITTEN);
  CHECK(memcmp(img, bank, kBank) == 0);
}

static void test_restore_keeps_image(void) {
  printf("test_restore_keeps_image\n");
  start_backup();
  csr_backup(-1, 0, -1);
  finish();
  Bus200eMasterReset();
  CHECK(Bus200eMasterRestore(0x20, 0x00) == 0);
  Bus200eMasterTask();
  CHECK(memcmp(img, bank, kBank) == 0);               // not blanked
  // the module reads the bank back out
  uint8_t r[kBank];
  BusCardStart(0); BusCardRxByte(0); BusCardRxByte(0);
  BusCardStart(1);
  for (uint32_t i = 0; i < kBank; ++i) r[i] = BusCardTxByte();
  BusCardStop();
  now_ms += 10; Bus200eMasterTask();
  finish();
  CHECK(memcmp(r, bank, kBank) == 0);
  CHECK(Bus200eMasterBytesTransferred() == kBank);
}

// The console's x after each kind of m: refused after a failed or holed one
// (the image is blanked), allowed after a clean one, with w patches, twice.
static void test_restore_refused_after_failed_backup(void) {
  printf("test_restore_refused_after_failed_backup\n");
  // a whole PBCARD.BIN, no BACKUP yet: x allowed
  BusCardImageFromFile(1);
  CHECK(BusCardRestoreCheck() == BUSCARD_RESTORE_OK);

  // m20 with the CSR silent (2026-10-31): NO_RESPONSE, image blanked
  memcpy(img, bank, kBank);
  start_backup();
  CHECK(BusCardRestoreCheck() == BUSCARD_RESTORE_CAPTURE_OPEN);
  now_ms += BUS200E_MASTER_ACTIVITY_TIMEOUT_MS + 10; Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_FAILED);
  CHECK(img[0] == 0xFF && img[kBank - 1] == 0xFF);    // what x would have pushed
  CHECK(BusCardRestoreCheck() == BUSCARD_RESTORE_CAPTURE_FAILED);

  // a holed m: refused
  start_backup();
  csr_backup(25, 30, -1);
  finish();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_DONE);
  CHECK(BusCardRestoreCheck() == BUSCARD_RESTORE_CAPTURE_BAD);

  // the bench workflow: m CLEAN -> w patch -> c -> x x
  start_backup();
  csr_backup(-1, 0, -1);
  finish();
  CHECK(BusCardRestoreCheck() == BUSCARD_RESTORE_OK);
  img[0x100] ^= 0x01;                                   // w0100xx
  CHECK(BusCardRestoreCheck() == BUSCARD_RESTORE_OK);
  Bus200eMasterReset();
  CHECK(Bus200eMasterRestore(0x20, 0x00) == 0);         // x20 x20
  Bus200eMasterTask();
  BusCardStart(0); BusCardRxByte(0); BusCardRxByte(0); BusCardStart(1);
  for (uint32_t i = 0; i < kBank; ++i) (void)BusCardTxByte();
  BusCardStop();
  now_ms += 10; Bus200eMasterTask(); finish();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_DONE);
  CHECK(BusCardRestoreCheck() == BUSCARD_RESTORE_OK);   // a RESTORE reads; still allowed
  img[0x100] ^= 0x01;
}

int main() {
  for (uint32_t i = 0; i < kBank; ++i) {
    // CSR-like content: 0xFF fill, a 0x00 trailer per record, a few edited slots
    const uint32_t s = i / kRec, b = i % kRec;
    bank[i] = (b == kRec - 1) ? 0x00 : (s % 7 == 0) ? (uint8_t)(0xD0 + (b % 3) * 0x10 + s) : 0xFF;
  }
  BusCardInit(img, BUSCARD_SIZE);
  Bus200eMasterInit(&ops);
  test_holed_capture_shows_no_stale_bytes();
  test_clean_capture_is_exact();
  test_resent_burst_does_not_inflate();
  test_restore_keeps_image();
  test_restore_refused_after_failed_backup();
  printf("\ntest_csr_capture: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
