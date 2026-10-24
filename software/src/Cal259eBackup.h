#ifndef CAL259EBACKUP_H_
#define CAL259EBACKUP_H_

#include <stdint.h>

#include "Bus200eMaster.h"  // Bus200eMasterState
#include "Cal259eFlow.h"    // BackupPort, BackupWhy

// ---------------------------------------------------------------------------
// The 259e flow's preset backup: the master BACKUP of one 259e into the card
// image (as the console 'm' does), stored in CAL259BK.BIN. Pure: everything the
// firmware does sits behind BackupMaster, so the rules are host-tested
// (test/test_cal259e_backup.cpp) against a fake master. CalRoutinePorts.cpp
// supplies the real one (OC::PresetBus, OC::PresetEngine).
//
// What it refuses, and why it matters:
//   * FwCardMode() true. While the firmware card is served (fw_mode stays true
//     until a reboot, even after the card was disarmed with 'M'), the card
//     image IS the served firmware; a BACKUP writes the 259e's 990 bytes over
//     bytes 0..989 of it, in RAM, while the 259e itself may be reading it.
//     Refused at Begin, and again when the job completes.
//   * a master job that is not this one (busy, abandoned, the console's, the
//     bridge's, a RESTORE, another address).
//   * anything but exactly 990 bytes transferred: a short or long bank is not a
//     net, and a restore of it would damage the module.
//   * a file that does not read back with the right length and CRC.
// ---------------------------------------------------------------------------

struct BackupMaster {
  virtual bool FwCardMode() = 0;                 // PresetBus::FwCardMode
  virtual Bus200eMasterState State() = 0;
  virtual void Reset() = 0;                      // retire a DONE/FAILED result
  virtual int Backup(uint8_t addr) = 0;          // MasterBackup: 0 accepted
  virtual uint8_t ModAddr() = 0;                 // Bus200eMasterModAddr
  virtual bool IsRestore() = 0;                  // Bus200eMasterIsRestore
  virtual uint32_t Bytes() = 0;                  // Bus200eMasterBytesTransferred
  virtual const uint8_t *Image() = 0;            // MasterCardImage (null: none)
  virtual uint32_t Crc32(const uint8_t *d, uint32_t n) = 0;
  virtual bool Save(uint8_t addr, const uint8_t *d, uint32_t n, uint32_t crc) = 0;  // CAL259BK.BIN
  virtual bool Verify(uint8_t addr, uint32_t n, uint32_t crc) = 0;  // read the file back
};

class Cal259eBackup : public BackupPort {
 public:
  explicit Cal259eBackup(BackupMaster &m) : m_(m) {}
  bool Begin(uint8_t addr) override;
  int Poll() override;
  BackupWhy Why() const override { return why_; }
  uint32_t Bytes() const override { return bytes_; }
  uint32_t Crc() const override { return crc_; }

 private:
  BackupMaster &m_;
  bool mine_ = false;
  uint8_t addr_ = 0;
  BackupWhy why_ = BK_NONE;
  uint32_t bytes_ = 0, crc_ = 0;
};

// The Failed screen's three rows (1..3) for a backup that did not happen: each at
// most 21 columns, the last always saying nothing was written.
const char *Cal259eBackupText(BackupWhy w, int line);

#endif  // CAL259EBACKUP_H_
