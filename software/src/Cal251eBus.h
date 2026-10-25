#ifndef CAL251EBUS_H_
#define CAL251EBUS_H_

#include <stdint.h>

#include "Cal259eBus.h"   // BusPort, Error, Status: the 259e client's bus layer, unchanged
#include "CalRec.h"
#include "CalRoutineTypes.h"

// ---------------------------------------------------------------------------
// The 251e calibration request/reply client (docs/specs/cal-251e.md; the
// module side is Buchla_FW docs/cv-calibration/CONTRACT.md "251e behaviour").
// Same shape and rules as Cal259eBus (one operation at a time, Tick() sends at
// most one frame, non-blocking, bus and clock behind ports), with these
// differences, all from the 251e's side of the contract:
//   * the record is 38 bytes: READ in five 8-byte chunks at args 0, 8, 16, 24 and
//     30 (a READ with arg > 30 returns nothing), bytes 30 and 31 read twice and
//     compared; no byte is distrusted (the 259e C4's reply-buffer quirk is a
//     property of that 8051 image; whether the 251e has one is unknown, so the
//     whole record must pass its CRC and the overlap compare);
//   * PUT is five frames (offsets 0, 8, 16, 24, 32; the last carries 6 bytes);
//   * COMMIT has no flags byte (n = 8); one page is rewritten, so there is no
//     slot-full outcome (E_NO_SLOT is never returned);
//   * OVERRIDE (0x10) and RELEASE (0x11) are one frame each and get no reply:
//     done when the frame is ACKed. The module drops every override 5 s after
//     the latest one, so the owner of the run refreshes them.
// COMMIT is sent once, never resent: after an ACK or a Wire error (which may
// follow a latched frame) the module is asked for its status after
// kCommitWaitMs and polled for kCommitVerdictMs; the verdict needs bit2 (ok),
// bit0 (record valid) and a seq equal to the record put, else E_COMMIT_UNKNOWN.
//
// Supported firmware: 30.7C4 and 30.7C5 (C5 preferred). On C4 the commit-result bits stay at their
// last value until the commit finishes, so a stale ok or refused bit is possible; on C5 an accepted
// COMMIT clears both at once and they are set again when it ends, so a status of 0 (flags without
// bits 2 and 3) means "in progress". The client is correct for both: it polls through "no verdict"
// until the window ends (E_COMMIT_UNKNOWN), believes ok only with the sequence number of the record
// put, and believes "refused" only if the status read just before the COMMIT (see Commit) did not
// already show it; otherwise a refused bit proves nothing and the poll goes on.
//
// ASSUMPTION (timings): the 251e erases and rewrites one page from its main
// loop in "a few ms" (CONTRACT); kCommitWaitMs is 300 ms, a margin over that
// and far under the 259e's 1.5 s. Not measured.
// ---------------------------------------------------------------------------

class Cal251eBus {
 public:
  static constexpr uint32_t kBusyRetryMs = 5;
  static constexpr uint32_t kBusyGiveUpMs = 1000;
  static constexpr uint32_t kReplyTimeoutMs = 60;
  static constexpr int kReadRetries = 3;
  static constexpr uint32_t kCommitWaitMs = 300;      // before the first status read
  static constexpr uint32_t kCommitVerdictMs = 3000;  // ... and this long after it
  static constexpr uint32_t kPollGapMs = 50;
  static constexpr uint8_t kStatusValid = 0x01, kStatusOverride = 0x02, kStatusOk = 0x04,
                           kStatusRefused = 0x08;
  static constexpr int kRecLen = 38;

  void Begin(BusPort &bus, CalRoutine::Clock &clock, uint8_t target);

  bool ReadStatus();
  bool ReadRecord();
  bool PutRecord(const uint8_t rec[kRecLen]);
  // The status last read (ReadStatus, with no Begin since) is the baseline for the refused bit; with
  // none, a refused bit is never believed.
  bool Commit(uint16_t crc);
  bool Override(uint8_t ch, uint16_t code);   // ch 0..3, code 0..4095
  bool Release();

  // 0 busy, 1 done, < 0 Error. Before any operation: 0.
  int Tick();
  bool busy() const { return op_ != OP_NONE && result_ == 0; }

  Status status() const { return status_; }
  const uint8_t *record() const { return record_; }
  bool has_record() const { return has_record_; }
  int sends() const { return sends_; }
  int last_send_error() const { return last_send_rc_; }

 private:
  enum Op : uint8_t { OP_NONE, OP_STATUS, OP_RECORD, OP_PUT, OP_COMMIT, OP_ONE };
  enum Phase : uint8_t { PH_SEND, PH_WAIT };
  enum CStage : uint8_t { C_SEND, C_WAIT, C_POLL, C_GAP };

  bool Start(Op op);
  void Finish(int result) { result_ = result; }
  void BeginRead(uint8_t what, uint8_t arg);
  int SendCurrent();
  int HandleReply(const uint8_t *f, uint8_t n);
  void StoreStatus();
  int ReadStep();
  int TickRead();
  int TickPut();
  int TickCommit();
  int TickOne();

  BusPort *bus_ = nullptr;
  CalRoutine::Clock *clock_ = nullptr;
  uint8_t target_ = 0;

  Op op_ = OP_NONE;
  Phase phase_ = PH_SEND;
  CStage cstage_ = C_SEND;
  int result_ = 0;

  uint8_t frame_[CalRec::kMaxFrame] = {};
  uint8_t frame_n_ = 0;
  uint8_t want_what_ = 0, want_arg_ = 0;
  uint32_t t0_ = 0, op_t0_ = 0;
  uint32_t busy_start_ = 0, busy_wait_t_ = 0;
  bool busy_open_ = false;
  int retries_ = 0;
  int sends_ = 0;
  int last_send_rc_ = 0;
  uint8_t chunk_ = 0;
  uint64_t seen_ = 0;       // RECORD: bit n set once record byte n has been read

  uint8_t put_rec_[kRecLen] = {};
  uint16_t put_seq_ = 0;
  bool put_seq_valid_ = false;

  Status status_ = {};
  bool status_valid_ = false;     // status_ was read since Begin()
  bool base_refused_ = true;      // COMMIT: the refused bit may be stale (baseline unknown or set)
  uint8_t record_[40] = {};
  bool has_record_ = false;
  uint8_t last_data_[13] = {};
  uint8_t last_len_ = 0;
};

#endif  // CAL251EBUS_H_
