// The 259e calibration request/reply client: see Cal259eBus.h.
#include "Cal259eBus.h"

#include <string.h>

#include "CalRec.h"

constexpr uint32_t Cal259eBus::kBusyRetryMs, Cal259eBus::kBusyGiveUpMs,
    Cal259eBus::kReplyTimeoutMs, Cal259eBus::kCommitWaitMs, Cal259eBus::kCommitVerdictMs,
    Cal259eBus::kPollGapMs;
constexpr int Cal259eBus::kReadRetries;
constexpr uint8_t Cal259eBus::kStatusValid, Cal259eBus::kStatusOk, Cal259eBus::kStatusRefused,
    Cal259eBus::kStatusNoSlot;

// READs of the record: the last starts at 22, not 24. The module sends no data
// for arg > 22 (CONTRACT, 259e behaviour), so bytes 22..29 are fetched as the
// final 8 and only the 6 new ones (24..29) are kept.
static const uint8_t kRecordArgs[4] = {0, 8, 16, 22};

void Cal259eBus::Begin(BusPort &bus, CalRoutine::Clock &clock, uint8_t target) {
  bus_ = &bus;
  clock_ = &clock;
  target_ = target;
  op_ = OP_NONE;
  result_ = 0;
  has_record_ = false;
}

bool Cal259eBus::Start(Op op) {
  if (!bus_ || busy()) return false;
  op_ = op;
  result_ = 0;
  phase_ = PH_SEND;
  busy_open_ = false;
  retries_ = 0;
  sends_ = 0;
  chunk_ = 0;
  last_send_rc_ = 0;
  return true;
}

void Cal259eBus::BeginRead(uint8_t what, uint8_t arg) {
  const uint8_t payload[2] = {what, arg};
  frame_n_ = (uint8_t)CalRec::BuildFrame(CalRec::kCmdRead, target_, payload, 2, frame_);
  want_what_ = what;
  want_arg_ = arg;
  phase_ = PH_SEND;
  busy_open_ = false;
  retries_ = 0;
}

bool Cal259eBus::ReadStatus() {
  if (!Start(OP_STATUS)) return false;
  BeginRead(2, 0);
  return true;
}

bool Cal259eBus::ReadAdc(uint8_t ch) {
  if (ch > 1) return false;  // the module sends no data for what 1 with arg > 1
  if (!Start(OP_ADC)) return false;
  BeginRead(1, ch);
  return true;
}

bool Cal259eBus::ReadRecord() {
  if (!Start(OP_RECORD)) return false;
  has_record_ = false;
  BeginRead(0, kRecordArgs[0]);
  return true;
}

bool Cal259eBus::PutRecord(const uint8_t rec[30]) {
  if (!Start(OP_PUT)) return false;
  memcpy(put_rec_, rec, 30);
  // The seq of what is being staged: the COMMIT's OK is only believed when the
  // module's status names this seq (see TickCommit).
  CalRec::Record r;
  put_seq_valid_ = CalRec::Decode259e(rec, CalRec::kRecLen259e, &r);
  put_seq_ = put_seq_valid_ ? r.seq : 0;
  frame_n_ = 0;  // built per chunk in TickPut
  return true;
}

bool Cal259eBus::Commit(uint16_t crc, uint8_t flags) {
  // Build into a local buffer: a refused call must not touch the in-flight
  // operation's frame_.
  uint8_t payload[4] = {(uint8_t)CalRec::kRecLen259e, (uint8_t)(crc >> 8), (uint8_t)crc, flags};
  const uint8_t plen = flags ? 4 : 3;  // n = 8 means flags = 0
  uint8_t local[CalRec::kMaxFrame];
  const int n = CalRec::BuildFrame(CalRec::kCmdCommit, target_, payload, plen, local);
  if (n < 0) return false;
  if (!Start(OP_COMMIT)) return false;
  memcpy(frame_, local, (size_t)n);
  frame_n_ = (uint8_t)n;
  cstage_ = C_SEND;
  return true;
}

// 0 sent, 1 not yet (busy gap), < 0 error.
int Cal259eBus::SendCurrent() {
  const uint32_t now = clock_->Ms();
  if (busy_open_ && now - busy_wait_t_ < kBusyRetryMs) return 1;
  // Never put on the wire anything the allowlist would refuse.
  if (frame_n_ == 0 || !CalRec::FrameAllowed(frame_, frame_n_)) return E_INTERNAL;
  // A reply still queued now belongs to an earlier request: drop it.
  uint8_t junk[CalRec::kMaxFrame], jn;
  for (int i = 0; i < 8 && bus_->PopReply(junk, &jn); ++i) {}
  const int rc = bus_->Send(frame_, frame_n_);
  if (rc == 0) {
    busy_open_ = false;
    ++sends_;
    return 0;
  }
  if (rc == -1) {
    if (!busy_open_) { busy_open_ = true; busy_start_ = now; }
    busy_wait_t_ = now;
    if (now - busy_start_ >= kBusyGiveUpMs) { busy_open_ = false; return E_BUS_BUSY; }
    return 1;
  }
  busy_open_ = false;
  last_send_rc_ = rc;
  return E_BUS_ERROR;
}

// 0 not ours, 1 matched and stored in last_data_, < 0 error.
int Cal259eBus::HandleReply(const uint8_t *f, uint8_t n) {
  CalRec::Reply r;
  if (!CalRec::ParseReply(f, n, &r)) return 0;
  if (r.src != target_ || r.what != want_what_ || r.arg != want_arg_) return 0;
  bool ok;
  switch (r.what) {
    case 0: ok = (r.len == 8 || r.len == 0); break;
    case 1: ok = (r.len == 3); break;
    case 2: ok = (r.len == 4); break;
    default: ok = false; break;
  }
  if (!ok) return E_BAD_REPLY;
  memcpy(last_data_, r.data, r.len);
  last_len_ = r.len;
  return 1;
}

void Cal259eBus::StoreStatus() {
  status_.flags = last_data_[0];
  status_.fmt = last_data_[1];
  status_.seq = (uint16_t)((last_data_[2] << 8) | last_data_[3]);
}

// One step of the READ in frame_: 0 busy, 1 reply stored, < 0 error.
int Cal259eBus::ReadStep() {
  if (phase_ == PH_SEND) {
    const int rc = SendCurrent();
    if (rc == 1) return 0;
    if (rc < 0) return rc;
    t0_ = clock_->Ms();
    phase_ = PH_WAIT;
  }
  uint8_t f[CalRec::kMaxFrame], n;
  for (int i = 0; i < 8 && bus_->PopReply(f, &n); ++i) {
    const int h = HandleReply(f, n);
    if (h != 0) return h;
  }
  if (clock_->Ms() - t0_ >= kReplyTimeoutMs) {
    if (retries_ >= kReadRetries) return E_NO_REPLY;
    ++retries_;
    phase_ = PH_SEND;
  }
  return 0;
}

int Cal259eBus::TickRead() {
  const int r = ReadStep();
  if (r == 0) return 0;
  if (r < 0) { Finish(r); return r; }
  switch (op_) {
    case OP_STATUS:
      StoreStatus();
      break;
    case OP_ADC:
      adc_ = (uint16_t)((last_data_[0] << 8) | last_data_[1]);
      adc_n_ = last_data_[2];
      break;
    case OP_RECORD:
      if (last_len_ == 0) {
        if (chunk_ != 0) { Finish(E_BAD_REPLY); return E_BAD_REPLY; }
        has_record_ = false;  // no record: a result, not an error
        break;
      }
      if (chunk_ < 3) memcpy(record_ + kRecordArgs[chunk_], last_data_, 8);
      else memcpy(record_ + 24, last_data_ + 2, 6);  // bytes 22..29 -> keep 24..29
      if (chunk_ < 3) {
        ++chunk_;
        BeginRead(0, kRecordArgs[chunk_]);
        return 0;
      }
      has_record_ = true;
      break;
    default:
      break;
  }
  Finish(1);
  return 1;
}

int Cal259eBus::TickPut() {
  static const uint8_t kOff[4] = {0, 8, 16, 24};
  static const uint8_t kLen[4] = {8, 8, 8, 6};
  if (frame_n_ == 0) {
    uint8_t payload[9];
    payload[0] = kOff[chunk_];
    memcpy(payload + 1, put_rec_ + kOff[chunk_], kLen[chunk_]);
    const int n = CalRec::BuildFrame(CalRec::kCmdPut, target_, payload,
                                     (uint8_t)(1 + kLen[chunk_]), frame_);
    if (n < 0) { Finish(E_BUS_ERROR); return E_BUS_ERROR; }
    frame_n_ = (uint8_t)n;
  }
  const int rc = SendCurrent();
  if (rc == 1) return 0;
  if (rc < 0) { Finish(rc); return rc; }
  frame_n_ = 0;
  if (++chunk_ < 4) return 0;
  Finish(1);
  return 1;
}

int Cal259eBus::TickCommit() {
  const uint32_t now = clock_->Ms();
  if (cstage_ == C_SEND) {
    const int rc = SendCurrent();  // -1 here means nothing went out, so a retry is safe
    if (rc == 1) return 0;
    if (rc == E_BUS_ERROR && last_send_rc_ != -1) {
      // The Wire layer reported an error (NAK, arbitration lost, timeout) for
      // the COMMIT. A NAK on the last byte can come after the module latched the
      // frame, so "nothing went out" is NOT known. Count it as sent (the flow
      // waits for a verdict if cancelled) and ask the module for its status:
      // the one COMMIT is never resent.
      ++sends_;
      op_t0_ = now;
      cstage_ = C_WAIT;
      return 0;
    }
    if (rc < 0) { Finish(rc); return rc; }
    op_t0_ = now;
    cstage_ = C_WAIT;
    return 0;
  }
  if (cstage_ == C_WAIT) {
    if (now - op_t0_ < kCommitWaitMs) return 0;
    BeginRead(2, 0);
    cstage_ = C_POLL;
  }
  if (now - op_t0_ >= kCommitWaitMs + kCommitVerdictMs) {
    Finish(E_COMMIT_UNKNOWN);
    return E_COMMIT_UNKNOWN;
  }
  if (cstage_ == C_GAP) {
    if (now - t0_ < kPollGapMs) return 0;
    BeginRead(2, 0);
    cstage_ = C_POLL;
  }
  const int r = ReadStep();
  if (r == 0) return 0;
  if (r < 0) {
    // The COMMIT is on the wire. Whatever went wrong with the status read (no
    // reply, busy, a send error, a malformed reply), the only honest answers
    // left are a verdict or E_COMMIT_UNKNOWN: keep asking until the window ends.
    t0_ = now;
    cstage_ = C_GAP;
    return 0;
  }
  StoreStatus();
  const uint8_t fl = status_.flags;
  int res = 0;
  if (fl & kStatusRefused) res = (fl & kStatusNoSlot) ? E_NO_SLOT : E_COMMIT_REFUSED;
  else if ((fl & kStatusOk) && (fl & kStatusValid) && put_seq_valid_ && status_.seq == put_seq_)
    res = 1;
  // Bits 2 and 3 describe the last commit SINCE POWER-UP, not this one: a COMMIT
  // frame lost after an earlier OK commit leaves bit2 set. So "ok" counts only
  // when the module also holds a valid record (bit0) whose seq is the one we
  // put. Anything else keeps polling and ends E_COMMIT_UNKNOWN.
  if (res) { Finish(res); return res; }
  t0_ = now;
  cstage_ = C_GAP;
  return 0;
}

int Cal259eBus::Tick() {
  if (!bus_ || op_ == OP_NONE || result_ != 0) return result_;
  switch (op_) {
    case OP_STATUS:
    case OP_ADC:
    case OP_RECORD: return TickRead();
    case OP_PUT: return TickPut();
    case OP_COMMIT: return TickCommit();
    default: return 0;
  }
}
