// The 259e flow's preset backup: see Cal259eBackup.h.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define C259B_CODE FLASHMEM
#else
#define C259B_CODE
#endif

#include "Cal259eBackup.h"

C259B_CODE bool Cal259eBackup::Begin(uint8_t addr) {
  mine_ = false;
  why_ = BK_NONE;
  bytes_ = crc_ = 0;
  // The served firmware image must never be the target of a BACKUP.
  if (m_.FwCardMode()) { why_ = BK_FW_CARD; return false; }
  const Bus200eMasterState st = m_.State();
  if (st == BUS200E_MASTER_DONE || st == BUS200E_MASTER_FAILED) m_.Reset();
  else if (st != BUS200E_MASTER_IDLE) { why_ = BK_BUSY; return false; }  // a job runs
  if (m_.Backup(addr) != 0) { why_ = BK_START; return false; }
  mine_ = true;
  addr_ = addr;
  return true;
}

C259B_CODE int Cal259eBackup::Poll() {
  if (!mine_) return -1;
  const Bus200eMasterState st = m_.State();
  if (st == BUS200E_MASTER_FAILED) {
    mine_ = false;
    why_ = BK_MASTER_FAILED;
    m_.Reset();
    return -1;
  }
  if (st == BUS200E_MASTER_IDLE) { mine_ = false; why_ = BK_LOST; return -1; }  // reset under us
  if (st != BUS200E_MASTER_DONE) return 0;
  mine_ = false;
  // The DONE must be this job's: the console 'm' / 'x' or the bridge share the
  // master FSM and could have retired ours and run their own.
  if (m_.ModAddr() != addr_ || m_.IsRestore()) { why_ = BK_OTHER_JOB; return -1; }
  // fw_mode can only turn on at boot, but the check is cheap and the cost of
  // being wrong is a corrupted firmware image.
  if (m_.FwCardMode()) { why_ = BK_FW_CARD; return -1; }
  bytes_ = m_.Bytes();
  if (bytes_ != Cal259eFlow::kBackupBankBytes) { why_ = BK_SIZE; return -1; }
  const uint8_t *img = m_.Image();
  if (!img) { why_ = BK_NO_IMAGE; return -1; }
  crc_ = m_.Crc32(img, bytes_);
  if (!m_.Save(addr_, img, bytes_, crc_)) { why_ = BK_SAVE; return -1; }
  if (!m_.Verify(addr_, bytes_, crc_)) { why_ = BK_VERIFY; return -1; }
  return 1;
}

// Three rows of at most 21 columns (the Failed screen's rows 1..3).
C259B_CODE const char *Cal259eBackupText(BackupWhy w, int line) {
  static const char *const kNone = "Nothing was written.";
  static const char *const k[][3] = {
    /* BK_NONE */          {"Preset backup failed", "or was refused.", kNone},
    /* BK_FW_CARD */       {"Firmware card served.", "Reboot the Xeno with", "the card disarmed."},
    /* BK_BUSY */          {"Another bus job is", "running. Try again.", kNone},
    /* BK_START */         {"The bus refused the", "backup command.", kNone},
    /* BK_MASTER_FAILED */ {"The 259e did not", "answer the backup.", kNone},
    /* BK_LOST */          {"The backup job was", "reset by something.", kNone},
    /* BK_OTHER_JOB */     {"Another bus job took", "the backup's place.", kNone},
    /* BK_NO_IMAGE */      {"No card image to", "save the backup from.", kNone},
    /* BK_SIZE */          {"Backup is not exactly", "990 bytes.", kNone},
    /* BK_SAVE */          {"CAL259BK.BIN could", "not be saved.", kNone},
    /* BK_VERIFY */        {"CAL259BK.BIN did not", "read back right.", kNone},
    /* BK_TIMEOUT */       {"The backup took too", "long.", kNone},
  };
  const int i = int(w) < int(sizeof(k) / sizeof(k[0])) ? int(w) : 0;
  return line >= 0 && line < 3 ? k[i][line] : "";
}
