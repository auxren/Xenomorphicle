// Host tests for the 259e flow's preset backup (src/Cal259eBackup.h) against a
// fake bus master:
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_cal259e_backup
//     test_cal259e_backup.cpp ../src/Cal259eBackup.cpp ../src/Cal259eFlow.cpp
//     ../src/Cal259eBus.cpp ../src/CalRec.cpp ../src/CalRoutineMeasure.cpp
//     ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../src/Cal259eBackup.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct FakeMaster : BackupMaster {
  bool fw = false;
  Bus200eMasterState st = BUS200E_MASTER_IDLE;
  uint8_t mod = 0;
  bool restore = false;
  uint32_t bytes = 0;
  int backup_rc = 0;
  bool save_ok = true, verify_ok = true, has_image = true;
  // The card image: while the firmware card is served this is the served
  // firmware (the thing a BACKUP must never overwrite).
  uint8_t image[1200];
  int backups = 0, resets = 0, saves = 0, verifies = 0;
  uint32_t saved_len = 0, saved_crc = 0;
  uint8_t saved_addr = 0;

  FakeMaster() { for (size_t i = 0; i < sizeof image; ++i) image[i] = uint8_t(0xF0 ^ i); }

  bool FwCardMode() override { return fw; }
  Bus200eMasterState State() override { return st; }
  void Reset() override { ++resets; st = BUS200E_MASTER_IDLE; }
  int Backup(uint8_t a) override {
    ++backups;
    if (backup_rc) return backup_rc;
    // What the real job does: the 259e's bytes land at the start of the image.
    memset(image, 0x5A, 990);
    mod = a; restore = false; st = BUS200E_MASTER_SENDING;
    return 0;
  }
  uint8_t ModAddr() override { return mod; }
  bool IsRestore() override { return restore; }
  uint32_t Bytes() override { return bytes; }
  const uint8_t *Image() override { return has_image ? image : nullptr; }
  uint32_t Crc32(const uint8_t *d, uint32_t n) override {
    uint32_t h = 2166136261u;
    if (n > sizeof image) n = uint32_t(sizeof image);  // a wrong size must fail an assert, not overrun
    for (uint32_t i = 0; i < n; ++i) { h ^= d[i]; h *= 16777619u; }
    return h;
  }
  bool Save(uint8_t a, const uint8_t *, uint32_t n, uint32_t crc) override {
    ++saves; saved_addr = a; saved_len = n; saved_crc = crc; return save_ok;
  }
  bool Verify(uint8_t a, uint32_t n, uint32_t crc) override {
    ++verifies;
    return verify_ok && a == saved_addr && n == saved_len && crc == saved_crc;
  }
  void Finish(uint32_t transferred) { bytes = transferred; st = BUS200E_MASTER_DONE; }
};

// S1: while the firmware card is served the BACKUP is refused, and the served image
// is left alone. (fw_mode stays true until a reboot, even with the card disarmed.)
static void fw_card_mode_refuses() {
  FakeMaster m;
  m.fw = true;
  uint8_t before[1200];
  memcpy(before, m.image, sizeof before);
  Cal259eBackup b(m);
  CHECK(!b.Begin(0x28));
  CHECK(b.Why() == BK_FW_CARD);
  CHECK(m.backups == 0 && m.resets == 0);
  CHECK(memcmp(before, m.image, sizeof before) == 0);
  CHECK(b.Poll() < 0);                          // and nothing to poll
  CHECK(strcmp(Cal259eBackupText(BK_FW_CARD, 1), "Reboot the Xeno with") == 0);
  CHECK(strcmp(BackupWhyName(BK_FW_CARD), "FW_CARD") == 0);
  // a stale DONE is not even retired
  m.st = BUS200E_MASTER_DONE;
  CHECK(!b.Begin(0x28) && m.resets == 0 && m.backups == 0);
}

static void fw_mode_turning_on_before_done_refuses() {
  FakeMaster m;
  Cal259eBackup b(m);
  CHECK(b.Begin(0x28));
  m.fw = true;
  m.Finish(990);
  CHECK(b.Poll() < 0 && b.Why() == BK_FW_CARD);
  CHECK(m.saves == 0);
}

static void good_backup() {
  FakeMaster m;
  Cal259eBackup b(m);
  CHECK(b.Begin(0x28));
  CHECK(m.backups == 1);
  CHECK(b.Poll() == 0);                         // SENDING: busy
  m.st = BUS200E_MASTER_TRANSFERRING;
  CHECK(b.Poll() == 0);
  m.Finish(990);
  CHECK(b.Poll() == 1);
  CHECK(b.Bytes() == 990 && b.Crc() != 0);
  CHECK(m.saves == 1 && m.saved_addr == 0x28 && m.saved_len == 990 && m.saved_crc == b.Crc());
  CHECK(m.verifies == 1);
  CHECK(b.Poll() < 0);                          // consumed
}

static void done_for_another_address() {
  FakeMaster m;
  Cal259eBackup b(m);
  CHECK(b.Begin(0x28));
  m.mod = 0x2A;                                 // the console retired ours and ran its own
  m.Finish(990);
  CHECK(b.Poll() < 0 && b.Why() == BK_OTHER_JOB);
  CHECK(m.saves == 0);
}

static void done_is_a_restore_job() {
  FakeMaster m;
  Cal259eBackup b(m);
  CHECK(b.Begin(0x28));
  m.mod = 0x28;
  m.restore = true;
  m.Finish(990);
  CHECK(b.Poll() < 0 && b.Why() == BK_OTHER_JOB);
  CHECK(m.saves == 0);
}

// Decision b: exactly 990 bytes, else F_BACKUP (989, 991, 0, a whole 64K card).
static void size_must_be_990() {
  for (uint32_t n : {0u, 957u, 989u, 991u, 1040u, 65536u}) {
    FakeMaster m;
    Cal259eBackup b(m);
    CHECK(b.Begin(0x28));
    m.Finish(n);
    CHECK(b.Poll() < 0);
    CHECK(b.Why() == BK_SIZE);
    CHECK(b.Bytes() == n);                      // logged as transferred
    CHECK(m.saves == 0);
  }
}

static void no_image() {
  FakeMaster m;
  m.has_image = false;
  Cal259eBackup b(m);
  b.Begin(0x28);
  m.Finish(990);
  CHECK(b.Poll() < 0 && b.Why() == BK_NO_IMAGE && m.saves == 0);
}

static void save_and_readback_failures() {
  {
    FakeMaster m;
    m.save_ok = false;
    Cal259eBackup b(m);
    b.Begin(0x28);
    m.Finish(990);
    CHECK(b.Poll() < 0 && b.Why() == BK_SAVE && m.verifies == 0);
  }
  {
    FakeMaster m;
    m.verify_ok = false;                        // the file does not read back: length or CRC
    Cal259eBackup b(m);
    b.Begin(0x28);
    m.Finish(990);
    CHECK(b.Poll() < 0 && b.Why() == BK_VERIFY && m.saves == 1 && m.verifies == 1);
  }
}

static void master_states() {
  {
    FakeMaster m;
    m.st = BUS200E_MASTER_TRANSFERRING;         // a job (ours, abandoned, the console's) runs
    Cal259eBackup b(m);
    CHECK(!b.Begin(0x28) && b.Why() == BK_BUSY && m.backups == 0);
  }
  {
    FakeMaster m;
    m.st = BUS200E_MASTER_DONE;                 // a leftover result is retired, then ours starts
    Cal259eBackup b(m);
    CHECK(b.Begin(0x28) && m.resets == 1 && m.backups == 1);
  }
  {
    FakeMaster m;
    m.backup_rc = -1;
    Cal259eBackup b(m);
    CHECK(!b.Begin(0x28) && b.Why() == BK_START);
  }
  {
    FakeMaster m;
    Cal259eBackup b(m);
    b.Begin(0x28);
    m.st = BUS200E_MASTER_FAILED;
    CHECK(b.Poll() < 0 && b.Why() == BK_MASTER_FAILED && m.resets == 1);
  }
  {
    FakeMaster m;
    Cal259eBackup b(m);
    b.Begin(0x28);
    m.st = BUS200E_MASTER_IDLE;                 // someone reset it under us
    CHECK(b.Poll() < 0 && b.Why() == BK_LOST);
  }
  {
    FakeMaster m;
    Cal259eBackup b(m);
    CHECK(b.Poll() < 0);                        // never begun
  }
}

// Every reason has three rows of at most 21 columns.
static void texts_fit() {
  for (int w = BK_NONE; w <= BK_TIMEOUT; ++w)
    for (int l = 0; l < 3; ++l) {
      const char *t = Cal259eBackupText(BackupWhy(w), l);
      CHECK(t && t[0] && strlen(t) <= 21);
    }
}

int main() {
  fw_card_mode_refuses();
  fw_mode_turning_on_before_done_refuses();
  good_backup();
  done_for_another_address();
  done_is_a_restore_job();
  size_must_be_990();
  no_image();
  save_and_readback_failures();
  master_states();
  texts_fit();
  printf("test_cal259e_backup: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
