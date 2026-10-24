#ifndef CAL259EBUS_H_
#define CAL259EBUS_H_

#include <stdint.h>

#include "CalRoutineTypes.h"  // CalRoutine::Clock

// ---------------------------------------------------------------------------
// The 259e calibration request/reply client (Buchla_FW docs/cv-calibration/
// CONTRACT.md, "259e behaviour where the contract above was open (31.2C4)").
// Pure and non-blocking: the bus and the clock sit behind ports, so the whole
// client is host-tested (test/test_cal259e_bus.cpp) against Sim259e.
//
// One operation at a time. Start it (ReadStatus, ReadAdc, ReadRecord,
// PutRecord, Commit: each returns false while another is in flight), then call
// Tick() until it stops returning 0. Tick() sends at most one frame per call.
// The result stays readable until the next operation starts.
//
// What the module does and does not answer (so what is retried):
//   READ  is the only command with a reply. A READ is resent up to
//         kReadRetries times when no matching reply arrives in
//         kReplyTimeoutMs, then E_NO_REPLY.
//   PUT   has no reply: done when every frame was ACKed.
//   COMMIT has no reply and flushes the module's receive queue while it
//         rewrites flash. It is sent ONCE and never resent: the module is asked
//         for its status instead (READ what 2) after kCommitWaitMs.
// A send refused because the bus is not quiet (-1) never put anything on the
// wire, so it is retried every kBusyRetryMs for up to kBusyGiveUpMs
// (E_BUS_BUSY). Any other send failure is E_BUS_ERROR and is not retried.
//
// After an ACKed COMMIT, status-read failures (send errors, malformed replies,
// silence) are polled through until the window ends: E_COMMIT_UNKNOWN.
//
// A reply counts only if it came from the target and names the outstanding
// request's what and arg; anything else is dropped without comment. The reply
// length is checked per `what` (E_BAD_REPLY otherwise): what 0 carries 8 bytes
// or none ("no record" / out of range), what 1 carries 3 (hi, lo, n), what 2
// carries 4 (flags, fmt, seq_hi, seq_lo).
// ---------------------------------------------------------------------------

// The 200e bus as the client sees it. Send returns OC::PresetBus::SendRawFrame's
// codes (0 ACK, -1 bus not quiet, -2 bad args, else a Wire error); PopReply
// hands over the oldest queued 0x19 frame.
struct BusPort {
  virtual int Send(const uint8_t *f, uint8_t n) = 0;
  virtual bool PopReply(uint8_t *f, uint8_t *n) = 0;
};

enum Error {
  E_BUS_BUSY = -1,        // the bus stayed busy for kBusyGiveUpMs
  E_NO_REPLY = -2,        // a READ went unanswered after its retries
  E_BAD_REPLY = -3,       // a matching reply of the wrong shape
  E_COMMIT_REFUSED = -4,  // the module refused the record (status bit3)
  E_NO_SLOT = -5,         // all slots full and compaction not allowed (bits 3+4)
  E_COMMIT_UNKNOWN = -6,  // no verdict within the window; the commit may or may not have landed
  E_BUS_ERROR = -7,       // a send failed other than "bus busy" (NAK, bad args). Never returned for
                          // a Commit once its frame was ACKed: after that only ok, E_COMMIT_REFUSED,
                          // E_NO_SLOT or E_COMMIT_UNKNOWN end the operation
  E_INTERNAL = -8,        // a frame CalRec::FrameAllowed refuses was about to be sent; nothing was
};

struct Status { uint8_t flags, fmt; uint16_t seq; };

class Cal259eBus {
 public:
  static constexpr uint32_t kBusyRetryMs = 5;
  static constexpr uint32_t kBusyGiveUpMs = 1000;
  static constexpr uint32_t kReplyTimeoutMs = 60;
  static constexpr int kReadRetries = 3;
  static constexpr uint32_t kCommitWaitMs = 1500;    // before the first status read
  static constexpr uint32_t kCommitVerdictMs = 3000; // ... and this long after it
  static constexpr uint32_t kPollGapMs = 50;         // between status reads while polling
  static constexpr uint8_t kStatusOk = 0x04, kStatusRefused = 0x08, kStatusNoSlot = 0x10;

  void Begin(BusPort &bus, CalRoutine::Clock &clock, uint8_t target);

  bool ReadStatus();
  bool ReadAdc(uint8_t ch);
  bool ReadRecord();
  bool PutRecord(const uint8_t rec[30]);
  bool Commit(uint16_t crc, uint8_t flags);

  // 0 busy, 1 done, < 0 Error. Before any operation: 0.
  int Tick();
  bool busy() const { return op_ != OP_NONE && result_ == 0; }

  Status status() const { return status_; }
  uint16_t adc() const { return adc_; }
  uint8_t adc_n() const { return adc_n_; }
  // The 30 record bytes of the last ReadRecord; has_record() is false when the
  // module has none (a distinct outcome, not an error).
  const uint8_t *record() const { return record_; }
  bool has_record() const { return has_record_; }
  // How many frames of the current/last operation went on the wire (tests, logs).
  int sends() const { return sends_; }

 private:
  enum Op : uint8_t { OP_NONE, OP_STATUS, OP_ADC, OP_RECORD, OP_PUT, OP_COMMIT };
  enum Phase : uint8_t {
    PH_SEND,          // about to (re)send the current frame
    PH_WAIT,          // frame ACKed, waiting for the reply
  };
  enum CStage : uint8_t {
    C_SEND,           // the COMMIT frame itself, not yet ACKed
    C_WAIT,           // ACKed: waiting out the flash rewrite
    C_POLL,           // a status READ in flight
    C_GAP,            // between two status reads
  };

  bool Start(Op op);
  void Finish(int result) { result_ = result; }
  void BeginRead(uint8_t what, uint8_t arg);
  int SendCurrent();                 // 0 sent, 1 retry later, <0 error
  int HandleReply(const uint8_t *f, uint8_t n);  // 0 no match, 1 matched and stored, <0 error
  void StoreStatus();
  int ReadStep();                    // one SEND/WAIT step of a READ: 0 busy, 1 got it, <0 error
  int TickRead();
  int TickPut();
  int TickCommit();

  BusPort *bus_ = nullptr;
  CalRoutine::Clock *clock_ = nullptr;
  uint8_t target_ = 0;

  Op op_ = OP_NONE;
  Phase phase_ = PH_SEND;
  CStage cstage_ = C_SEND;
  int result_ = 0;

  uint8_t frame_[18] = {};
  uint8_t frame_n_ = 0;
  uint8_t want_what_ = 0, want_arg_ = 0;
  uint32_t t0_ = 0;          // when the current READ frame was ACKed (or the poll gap began)
  uint32_t op_t0_ = 0;       // COMMIT: when the frame was ACKed
  uint32_t busy_start_ = 0, busy_wait_t_ = 0;
  bool busy_open_ = false;   // a busy window is open (busy_start_)
  int retries_ = 0;
  int sends_ = 0;
  uint8_t chunk_ = 0;        // RECORD: 0..3; PUT: next frame 0..3

  uint8_t put_rec_[30] = {};

  Status status_ = {};
  uint16_t adc_ = 0;
  uint8_t adc_n_ = 0;
  uint8_t record_[32] = {};
  bool has_record_ = false;
  uint8_t last_data_[13] = {};
  uint8_t last_len_ = 0;
};

#endif  // CAL259EBUS_H_
