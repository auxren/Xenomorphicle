// The 251e calibration request/reply client: see Cal251eBus.h.
// Firmware: all of it runs from flash (FLASHMEM), none of it is ISR code.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define C251B_CODE FLASHMEM
#else
#define C251B_CODE
#endif

#include "Cal251eBus.h"

#include <string.h>

#include "CalRec.h"
#include "CalRec251e.h"

constexpr uint32_t Cal251eBus::kBusyRetryMs, Cal251eBus::kBusyGiveUpMs, Cal251eBus::kReplyTimeoutMs,
    Cal251eBus::kCommitWaitMs, Cal251eBus::kCommitVerdictMs, Cal251eBus::kPollGapMs;
constexpr int Cal251eBus::kReadRetries, Cal251eBus::kRecLen;
constexpr uint8_t Cal251eBus::kStatusValid, Cal251eBus::kStatusOverride, Cal251eBus::kStatusOk,
    Cal251eBus::kStatusRefused;

// READ what 0 args: the module sends no data for arg > 30, so the last chunk starts at 30 and
// repeats bytes 30 and 31 (a free integrity check, like the 259e client's overlap).
static const uint8_t kReadArgs[5] = {0, 8, 16, 24, 30};
static const uint8_t kPutOff[5] = {0, 8, 16, 24, 32};
static const uint8_t kPutLen[5] = {8, 8, 8, 8, 6};

C251B_CODE void Cal251eBus::Begin(BusPort &bus, CalRoutine::Clock &clock, uint8_t target) {
  bus_ = &bus;
  clock_ = &clock;
  target_ = target;
  op_ = OP_NONE;
  result_ = 0;
  has_record_ = false;
  status_valid_ = false;
}

C251B_CODE bool Cal251eBus::Start(Op op) {
  if (!bus_ || busy()) return false;
  op_ = op;
  result_ = 0;
  phase_ = PH_SEND;
  busy_open_ = false;
  retries_ = 0;
  sends_ = 0;
  chunk_ = 0;
  seen_ = 0;
  last_send_rc_ = 0;
  return true;
}

C251B_CODE void Cal251eBus::BeginRead(uint8_t what, uint8_t arg) {
  const uint8_t payload[2] = {what, arg};
  frame_n_ = (uint8_t)CalRec::BuildFrame(CalRec::kCmdRead, target_, payload, 2, frame_);
  want_what_ = what;
  want_arg_ = arg;
  phase_ = PH_SEND;
  busy_open_ = false;
  retries_ = 0;
}

C251B_CODE bool Cal251eBus::ReadStatus() {
  if (!Start(OP_STATUS)) return false;
  BeginRead(2, 0);
  return true;
}

C251B_CODE bool Cal251eBus::ReadRecord() {
  if (!Start(OP_RECORD)) return false;
  has_record_ = false;
  BeginRead(0, kReadArgs[0]);
  return true;
}

C251B_CODE bool Cal251eBus::PutRecord(const uint8_t rec[kRecLen]) {
  if (!Start(OP_PUT)) return false;
  memcpy(put_rec_, rec, kRecLen);
  CalRec251e::Record r;
  put_seq_valid_ = CalRec251e::Decode(rec, kRecLen, &r);
  put_seq_ = put_seq_valid_ ? r.seq : 0;
  frame_n_ = 0;
  return true;
}

C251B_CODE bool Cal251eBus::Commit(uint16_t crc) {
  const uint8_t payload[3] = {(uint8_t)kRecLen, (uint8_t)(crc >> 8), (uint8_t)crc};
  uint8_t local[CalRec::kMaxFrame];
  const int n = CalRec::BuildFrame(CalRec::kCmdCommit, target_, payload, 3, local);
  if (n < 0) return false;
  if (!Start(OP_COMMIT)) return false;
  base_refused_ = !status_valid_ || (status_.flags & kStatusRefused) != 0;
  memcpy(frame_, local, (size_t)n);
  frame_n_ = (uint8_t)n;
  cstage_ = C_SEND;
  return true;
}

C251B_CODE bool Cal251eBus::Override(uint8_t ch, uint16_t code) {
  if (ch > 3 || code > CalRec251e::kMaxCode) return false;
  const uint8_t payload[3] = {ch, (uint8_t)(code >> 8), (uint8_t)code};
  uint8_t local[CalRec::kMaxFrame];
  const int n = CalRec::BuildFrame(CalRec::kCmdOverride, target_, payload, 3, local);
  if (n < 0 || !Start(OP_ONE)) return false;
  memcpy(frame_, local, (size_t)n);
  frame_n_ = (uint8_t)n;
  return true;
}

C251B_CODE bool Cal251eBus::Release() {
  uint8_t local[CalRec::kMaxFrame];
  const int n = CalRec::BuildFrame(CalRec::kCmdRelease, target_, nullptr, 0, local);
  if (n < 0 || !Start(OP_ONE)) return false;
  memcpy(frame_, local, (size_t)n);
  frame_n_ = (uint8_t)n;
  return true;
}

// 0 sent, 1 not yet (busy gap), < 0 error.
C251B_CODE int Cal251eBus::SendCurrent() {
  const uint32_t now = clock_->Ms();
  if (busy_open_ && now - busy_wait_t_ < kBusyRetryMs) return 1;
  if (frame_n_ == 0 || !CalRec::FrameAllowed(frame_, frame_n_)) return E_INTERNAL;
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

C251B_CODE int Cal251eBus::HandleReply(const uint8_t *f, uint8_t n) {
  CalRec::Reply r;
  if (!CalRec::ParseReply(f, n, &r)) return 0;
  if (r.src != target_ || r.what != want_what_ || r.arg != want_arg_) return 0;
  bool ok;
  switch (r.what) {
    case 0: ok = (r.len == 8 || r.len == 0); break;
    case 2: ok = (r.len == 4); break;
    default: ok = false; break;
  }
  if (!ok) return E_BAD_REPLY;
  memcpy(last_data_, r.data, r.len);
  last_len_ = r.len;
  return 1;
}

C251B_CODE void Cal251eBus::StoreStatus() {
  status_.flags = last_data_[0];
  status_.fmt = last_data_[1];
  status_.seq = (uint16_t)((last_data_[2] << 8) | last_data_[3]);
  status_valid_ = true;
}

C251B_CODE int Cal251eBus::ReadStep() {
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

C251B_CODE int Cal251eBus::TickRead() {
  const int r = ReadStep();
  if (r == 0) return 0;
  if (r < 0) { Finish(r); return r; }
  if (op_ == OP_STATUS) {
    StoreStatus();
  } else {  // OP_RECORD
    if (last_len_ == 0) {
      if (chunk_ != 0) { Finish(E_BAD_REPLY); return E_BAD_REPLY; }
      has_record_ = false;  // no record: a result, not an error
      Finish(1);
      return 1;
    }
    for (int i = 0; i < 8; ++i) {
      const int off = kReadArgs[chunk_] + i;
      if (off >= kRecLen) continue;
      if ((seen_ >> off & 1u) && record_[off] != last_data_[i]) {
        has_record_ = false;
        Finish(E_INCONSISTENT);
        return E_INCONSISTENT;
      }
      record_[off] = last_data_[i];
      seen_ |= uint64_t(1) << off;
    }
    if (chunk_ < 4) {
      ++chunk_;
      BeginRead(0, kReadArgs[chunk_]);
      return 0;
    }
    has_record_ = true;
  }
  Finish(1);
  return 1;
}

C251B_CODE int Cal251eBus::TickPut() {
  if (frame_n_ == 0) {
    uint8_t payload[9];
    payload[0] = kPutOff[chunk_];
    memcpy(payload + 1, put_rec_ + kPutOff[chunk_], kPutLen[chunk_]);
    const int n = CalRec::BuildFrame(CalRec::kCmdPut, target_, payload,
                                     (uint8_t)(1 + kPutLen[chunk_]), frame_);
    if (n < 0) { Finish(E_BUS_ERROR); return E_BUS_ERROR; }
    frame_n_ = (uint8_t)n;
  }
  const int rc = SendCurrent();
  if (rc == 1) return 0;
  if (rc < 0) { Finish(rc); return rc; }
  frame_n_ = 0;
  if (++chunk_ < 5) return 0;
  Finish(1);
  return 1;
}

C251B_CODE int Cal251eBus::TickOne() {
  const int rc = SendCurrent();
  if (rc == 1) return 0;
  Finish(rc < 0 ? rc : 1);
  return result_;
}

C251B_CODE int Cal251eBus::TickCommit() {
  const uint32_t now = clock_->Ms();
  if (cstage_ == C_SEND) {
    const int rc = SendCurrent();  // -1 here means nothing went out, so a retry is safe
    if (rc == 1) return 0;
    if (rc == E_BUS_ERROR && last_send_rc_ != -1) {
      // A Wire error may follow a frame the module latched: count it as sent and ask for
      // the status; the one COMMIT is never resent.
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
    t0_ = now;
    cstage_ = C_GAP;
    return 0;
  }
  StoreStatus();
  const uint8_t fl = status_.flags;
  int res = 0;
  if ((fl & kStatusRefused) && !base_refused_) res = E_COMMIT_REFUSED;
  else if ((fl & kStatusOk) && (fl & kStatusValid) && put_seq_valid_ && status_.seq == put_seq_)
    res = 1;
  // Bits 2 and 3 describe the last commit since power-up, so "ok" counts only with a valid
  // record whose seq is the one put; anything else keeps polling and ends E_COMMIT_UNKNOWN.
  if (res) { Finish(res); return res; }
  t0_ = now;
  cstage_ = C_GAP;
  return 0;
}

C251B_CODE int Cal251eBus::Tick() {
  if (!bus_ || op_ == OP_NONE || result_ != 0) return result_;
  switch (op_) {
    case OP_STATUS:
    case OP_RECORD: return TickRead();
    case OP_PUT: return TickPut();
    case OP_COMMIT: return TickCommit();
    case OP_ONE: return TickOne();
    default: return 0;
  }
}
