// The Cal Routine's 251e target: see Cal251eFlow.h.
//
// Firmware: all of it runs from flash (FLASHMEM), none of it is ISR code.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define C251_CODE FLASHMEM
#else
#define C251_CODE
#endif

#include "Cal251eFlow.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace CM = CvMeterMath;
namespace R = CalRec251e;

// =============================================================================
// Line formats

namespace Cal251eLines {

static const char *const kHeaders[] = {
  "CALR#,version=2",
  "CALR#,251START,addr",
  "CALR#,251OLD,rec76hex_or_NONE_or_ERRn_or_ERRn+hex",
  "CALR#,251LOOP,in,mv,reading_mv",
  "CALR#,251PT,ch,kind,raw_code,mv",
  "CALR#,251LINE,ch,a_mv_per_code,b_mv,nonlin_mv,bow_mv,inl_mv",
  "CALR#,251STAT,ch,noise_rms_mv,lsb_mv,verify_mv,anchor_mv,warn_hex",
  "CALR#,251WARN,ch,kind,value_mv,limit_mv",
  "CALR#,251COMMIT,n,seq",
  "CALR#,251FIT,ch,gain_hex,offset,pred_worst_mv,anchor_mv",
  "CALR#,251REC,addr,rec76hex",
  "CALR#,251END,result,warn",
};

C251_CODE int HeaderCount() { return int(sizeof(kHeaders) / sizeof(kHeaders[0])); }
C251_CODE const char *Header(int i) { return i >= 0 && i < HeaderCount() ? kHeaders[i] : ""; }

C251_CODE void AppendFixed(char *buf, size_t n, size_t *pos, double v, int decimals) {
  char t[32];
  CM::FormatFixed(t, sizeof(t), CM::ScaleRound(v, decimals), decimals);
  CM::Append(buf, n, pos, ",");
  CM::Append(buf, n, pos, t);
}

C251_CODE int Start(char *buf, size_t n, uint8_t addr) {
  if (!n) return 0;
  char t[32];
  snprintf(t, sizeof(t), "CALR,251START,%02X", unsigned(addr));
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C251_CODE int Loop(char *buf, size_t n, int in, int32_t mv, double reading_mv) {
  if (!n) return 0;
  char t[48];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,251LOOP,%d,%ld", in, long(mv));
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, reading_mv, 3);
  return int(pos);
}

C251_CODE int Point(char *buf, size_t n, int ch, char kind, uint16_t raw, double mv) {
  if (!n) return 0;
  char t[48];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,251PT,%d,%c,%u", ch, kind, unsigned(raw));
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, mv, 3);
  return int(pos);
}

C251_CODE int Line(char *buf, size_t n, int ch, double a, double b, double nonlin_mv, double bow_mv,
                   double inl_mv) {
  if (!n) return 0;
  char t[32];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,251LINE,%d", ch);
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, a, 5);
  AppendFixed(buf, n, &pos, b, 3);
  AppendFixed(buf, n, &pos, nonlin_mv, 2);
  AppendFixed(buf, n, &pos, bow_mv, 2);
  AppendFixed(buf, n, &pos, inl_mv, 2);
  return int(pos);
}

C251_CODE int Stat(char *buf, size_t n, int ch, double noise_mv, double lsb_mv, double verify_mv,
                   double anchor_mv, unsigned warn) {
  if (!n) return 0;
  char t[32];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,251STAT,%d", ch);
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, noise_mv, 3);
  AppendFixed(buf, n, &pos, lsb_mv, 4);
  AppendFixed(buf, n, &pos, verify_mv, 2);
  AppendFixed(buf, n, &pos, anchor_mv, 2);
  snprintf(t, sizeof(t), ",%X", warn);
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C251_CODE int Warn(char *buf, size_t n, int ch, const char *kind, double value_mv, double limit_mv) {
  if (!n) return 0;
  char t[40];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,251WARN,%d,%s", ch, kind);
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, value_mv, 2);
  AppendFixed(buf, n, &pos, limit_mv, 2);
  return int(pos);
}

C251_CODE int Commit(char *buf, size_t n, int count, unsigned seq) {
  if (!n) return 0;
  char t[48];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,251COMMIT,%d,%u", count, seq);
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C251_CODE int Fit(char *buf, size_t n, int ch, const CalRec::Channel &c, double pred_worst_mv,
                  double anchor_mv) {
  if (!n) return 0;
  char t[48];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,251FIT,%d,%04X,%d", ch, unsigned(c.gain), int(c.offset));
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, pred_worst_mv, 2);
  AppendFixed(buf, n, &pos, anchor_mv, 2);
  return int(pos);
}

C251_CODE int Old(char *buf, size_t n, const uint8_t *rec, int err) {
  if (!n) return 0;
  // Longest: "CALR,251OLD," 12 + "ERR_2147483647," 15 + 76 hex + NUL = 104; t has room to spare.
  char t[2 * R::kRecLen + 40];
  int k = snprintf(t, sizeof(t), "CALR,251OLD,");
  if (rec) {
    // err != 0 with bytes: a record that was read but does not decode (ERR_<n>,<hex>).
    if (err != 0) k += snprintf(t + k, sizeof(t) - size_t(k), "ERR_%d,", -err);
    for (int i = 0; i < R::kRecLen; ++i) k += snprintf(t + k, sizeof(t) - size_t(k), "%02X", unsigned(rec[i]));
  } else if (err == 0) {
    snprintf(t + k, sizeof(t) - size_t(k), "NONE");
  } else {
    snprintf(t + k, sizeof(t) - size_t(k), "ERR_%d", -err);
  }
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C251_CODE int Rec(char *buf, size_t n, uint8_t addr, const uint8_t rec[R::kRecLen]) {
  if (!n) return 0;
  char t[2 * R::kRecLen + 24];
  int k = snprintf(t, sizeof(t), "CALR,251REC,%02X,", unsigned(addr));
  for (int i = 0; i < R::kRecLen; ++i) k += snprintf(t + k, sizeof(t) - size_t(k), "%02X", unsigned(rec[i]));
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C251_CODE int End(char *buf, size_t n, const char *result, unsigned warn) {
  if (!n) return 0;
  size_t pos = 0;
  char t[24];
  buf[0] = 0;
  CM::Append(buf, n, &pos, "CALR,251END,");
  CM::Append(buf, n, &pos, result ? result : "");
  snprintf(t, sizeof(t), ",warn=%X", warn);
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

}  // namespace Cal251eLines

namespace L = Cal251eLines;

#define C251_LOG(fn, ...) do { \
    char line_[L::kMaxLine]; \
    L::fn(line_, sizeof(line_), __VA_ARGS__); \
    Log(line_); \
  } while (0)

// =============================================================================
// Constants and names

constexpr int Cal251eFlow::kChannels, Cal251eFlow::kFirstLoopOut, Cal251eFlow::kFitPoints,
    Cal251eFlow::kVerifyPoints, Cal251eFlow::kLoopPoints, Cal251eFlow::kPoints, Cal251eFlow::kWindows;
constexpr int32_t Cal251eFlow::kIdLowMv, Cal251eFlow::kIdHighMv;
constexpr double Cal251eFlow::kIdMoveMv, Cal251eFlow::kIdOtherMv, Cal251eFlow::kLoopbackMv,
    Cal251eFlow::kPatchErrMv, Cal251eFlow::kRepeatMv, Cal251eFlow::kDriftMv,
    Cal251eFlow::kTopGuardMv, Cal251eFlow::kNonlinMv, Cal251eFlow::kVerifyMvLimit,
    Cal251eFlow::kNonlinGrossMv, Cal251eFlow::kVerifyGrossMv;
constexpr int Cal251eFlow::kWriteCap;
constexpr uint32_t Cal251eFlow::kResultsMaxAgeMs, Cal251eFlow::kBusTimeoutMs;
constexpr bool Cal251eFlow::kAllowWrite;

const int32_t Cal251eFlow::kFitMv[Cal251eFlow::kFitPoints] = {1200, 2400, 3600, 4800, 6000, 7200, 8400, 9600};
const int32_t Cal251eFlow::kVerifyMv[Cal251eFlow::kVerifyPoints] = {300, 3000, 5400, 7800, 9600};
const int32_t Cal251eFlow::kLoopMv[Cal251eFlow::kLoopPoints] = {300, 1200, 3600, 6000, 9600};

C251_CODE const char *Cal251eFlow::StageName(Stage s) {
  switch (s) {
    case Stage::Idle: return "IDLE";
    case Stage::NoSelfCal: return "NO_SELF_CAL";
    case Stage::Scan: return "SCAN";
    case Stage::Warmup: return "WARMUP";
    case Stage::PromptLoop: return "PROMPT_LOOP";
    case Stage::Loopback: return "LOOPBACK";
    case Stage::PromptPatch: return "PROMPT_PATCH";
    case Stage::Identify: return "IDENTIFY";
    case Stage::Measure: return "MEASURE";
    case Stage::Results: return "RESULTS";
    case Stage::Confirm: return "CONFIRM";
    case Stage::Write: return "WRITE";
    case Stage::Verify: return "VERIFY";
    case Stage::Done: return "DONE";
    case Stage::Failed: return "FAILED";
  }
  return "?";
}

C251_CODE const char *Cal251eFlow::FailName(FailCode f) {
  switch (f) {
    case F_NONE: return "NONE";
    case F_NO_SELF_CAL: return "NO_SELF_CAL";
    case F_INPUT_UNCAL: return "INPUT_UNCAL";
    case F_NO_251E: return "NO_251E";
    case F_PATCH: return "PATCH";
    case F_LOOPBACK: return "LOOPBACK";
    case F_IDENTITY: return "IDENTITY";
    case F_NO_OVERRIDE: return "NO_OVERRIDE";
    case F_NOISY: return "NOISY";
    case F_DRIFT: return "DRIFT";
    case F_INPUT_RANGE: return "INPUT_RANGE";
    case F_SCALE: return "SCALE";
    case F_FIT: return "FIT";
    case F_NONLIN: return "NONLIN";
    case F_RECORD: return "RECORD";
    case F_VERIFY: return "VERIFY";
    case F_BUS: return "BUS";
    case F_COMMIT_REFUSED: return "COMMIT_REFUSED";
    case F_COMMIT_UNKNOWN: return "COMMIT_UNKNOWN";
    case F_READBACK: return "READBACK";
    case F_CANCELLED: return "CANCELLED";
    case F_EXPIRED: return "EXPIRED";
    case F_INTERNAL: return "INTERNAL";
    case F_NO_INPUT: return "NO_INPUT";
    case F_WRITE_CAP: return "WRITE_CAP";
    case F_OLD_UNREADABLE: return "OLD_UNREADABLE";
  }
  return "?";
}

C251_CODE const char *Cal251eCancelledName(int rc) {
  switch (rc) {
    case 1: return "CANCELLED_OK";
    case E_NO_SLOT:
    case E_COMMIT_REFUSED: return "CANCELLED_COMMIT_REFUSED";
    case E_COMMIT_UNKNOWN: return "CANCELLED_COMMIT_UNKNOWN";
    default: return "CANCELLED_BUS";
  }
}

// =============================================================================
// The flow

C251_CODE void Cal251eFlow::Log(const char *line) {
  if (log_) log_(line);
}

// Lets go of the Xeno's outputs now and schedules the 251e's RELEASE (sent by PumpRelease).
C251_CODE void Cal251eFlow::ReleaseHolds() {
  if (out_) out_->ReleaseAll();
  if (ovr_live_) { rel_pending_ = true; rel_started_ = false; }
}

C251_CODE void Cal251eFlow::BusTarget(uint8_t addr) { bus_.Begin(*busport_, *clock_, addr); }

// One frame at a time on the shared bus client, and never over a stage that is using it.
C251_CODE void Cal251eFlow::PumpRelease() {
  if (!rel_pending_ || cancel_pending_) return;
  if (stage_ == Stage::Scan || stage_ == Stage::Identify || stage_ == Stage::Measure ||
      stage_ == Stage::Verify || (stage_ == Stage::Write && sub_ != S_NONE))
    return;
  if (!rel_started_) {
    BusTarget(CalRec251e::kBusAddr);
    if (!bus_.Release()) { rel_pending_ = false; return; }
    rel_started_ = true;
    return;
  }
  if (bus_.Tick() != 0) {  // sent, or failed: the module's 5 s timer lets go either way
    rel_pending_ = false;
    rel_started_ = false;
    ovr_live_ = false;
  }
}

C251_CODE void Cal251eFlow::Fail(FailCode f) {
  ReleaseHolds();
  win_.Stop();
  acq_ = A_NONE;
  BusTarget(R::kBusAddr);  // drops anything in flight; an un-ACKed COMMIT never goes out
  sub_ = S_NONE;
  fail_ = f;
  C251_LOG(End, FailName(f), WarnWord());
  SetStage(Stage::Failed);
}

C251_CODE void Cal251eFlow::Finish() {
  ReleaseHolds();
  sub_ = S_NONE;
  fail_ = F_NONE;
  C251_LOG(End, "OK", WarnWord());
  SetStage(Stage::Done);
}

C251_CODE bool Cal251eFlow::Expired() const {
  return results_ms_ && now_ - results_ms_ >= kResultsMaxAgeMs;
}

C251_CODE uint32_t Cal251eFlow::results_age_ms() const {
  return results_ms_ && clock_ ? clock_->Ms() - results_ms_ : 0;
}

C251_CODE bool Cal251eFlow::Begin(OutputPort &out, InputPort &in, MvPort &mv, Clock &clock,
                                  BusPort &bus, EntropyPort &entropy, bool self_cal_present,
                                  LogFn log) {
  if (cancel_pending_) return false;  // a cancelled COMMIT's verdict is never dropped
  out.ReleaseAll();                   // a Begin mid-run never leaves the previous run's outputs held
  out_ = &out;
  in_ = &in;
  mvp_ = &mv;
  clock_ = &clock;
  busport_ = &bus;
  entropy_ = &entropy;
  log_ = log;
  win_.Stop();
  now_ = clock.Ms();
  BusTarget(R::kBusAddr);
  for (int k = 0; k < kChannels; ++k)
    for (int n = 0; n < CalRoutineMath::kNodes; ++n) tables_[k][n] = out.TableCode(kFirstLoopOut + k, n);
  sub_ = S_NONE;
  fail_ = F_NONE;
  acq_ = A_NONE;
  rel_pending_ = rel_started_ = false;
  // (ovr_live_ is left as it was, and a RELEASE still pending from the previous run is dropped here:
  // the next ReleaseHolds sends one, and the module lets go by itself 5 s after the last override.
  // Commits since power-up are kept: the write cap is per power-up, not per run.)
  had_record_ = false;
  old_read_ok_ = true;
  old_seq_ = 0;
  loop_i_ = -1;
  loop_bad_ = 0.0;
  id_step_ = 0;
  id_ok_ = true;
  id_bad_ch_ = id_seen_in_ = -1;
  pt_ = -1;
  memset(noise_ss_, 0, sizeof(noise_ss_));
  noise_n_ = 0;
  commit_sent_ = false;
  commit_rc_ = 0;
  cancel_pending_ = false;
  cancelled_verdict_ = 0;
  results_ms_ = 0;
  memset(rec_, 0, sizeof(rec_));
  memset(&result_, 0, sizeof(result_));
  memset(fit_, 0, sizeof(fit_));
  memset(ver_, 0, sizeof(ver_));
  memset(an_, 0, sizeof(an_));
  for (int i = 0; i < L::HeaderCount(); ++i) Log(L::Header(i));
  if (!self_cal_present) {  // the output tables were not meter-set: refuse
    fail_ = F_NO_SELF_CAL;
    C251_LOG(End, FailName(fail_), WarnWord());
    SetStage(Stage::NoSelfCal);
    return true;
  }
  for (int k = 0; k < kChannels; ++k) {
    if (!mv.Calibrated(k)) {  // the inputs are the measuring instrument: they must be calibrated
      fail_ = F_INPUT_UNCAL;
      C251_LOG(End, FailName(fail_), WarnWord());
      SetStage(Stage::NoSelfCal);
      return true;
    }
  }
  SetStage(Stage::Scan);
  sub_ = S_STATUS;
  bus_.ReadStatus();
  return true;
}

C251_CODE void Cal251eFlow::Tick() {
  if (!clock_) return;
  now_ = clock_->Ms();
  PumpRelease();
  switch (stage_) {
    case Stage::Scan: {
      const int rc = bus_.Tick();
      if (rc == 0) break;
      if (sub_ == S_STATUS) {
        if (rc == E_NO_REPLY || rc == E_BAD_REPLY) { Fail(F_NO_251E); break; }
        if (rc < 0) { Fail(F_BUS); break; }
        if (bus_.status().fmt != 1) { Fail(F_NO_251E); break; }
        had_record_ = (bus_.status().flags & Cal251eBus::kStatusValid) != 0;
        old_seq_ = bus_.status().seq;
        C251_LOG(Start, R::kBusAddr);
        if (had_record_) {
          sub_ = S_OLDREC;
          bus_.ReadRecord();
          break;
        }
        C251_LOG(Old, nullptr, 0);
      } else {  // S_OLDREC: a failed read is logged, not fatal (a measuring-only run goes on)
        R::Record r;
        if (rc == 1 && bus_.has_record() && R::Decode(bus_.record(), R::kRecLen, &r)) {
          C251_LOG(Old, bus_.record(), 0);
        } else {
          old_read_ok_ = false;
          if (rc == 1 && bus_.has_record()) C251_LOG(Old, bus_.record(), -1000);  // read, but does not decode
          else C251_LOG(Old, nullptr, rc < 0 ? rc : -1000);
        }
      }
      sub_ = S_NONE;
      SetStage(Stage::Warmup);
      break;
    }
    case Stage::Loopback:
      TickLoop();
      break;
    case Stage::PromptPatch:
      if (Expired()) Fail(F_EXPIRED);
      break;
    case Stage::Identify:
      if (Expired()) Fail(F_EXPIRED);
      else TickIdentify();
      break;
    case Stage::Measure:
      if (Expired()) Fail(F_EXPIRED);
      else TickMeasure();
      break;
    case Stage::Results:
    case Stage::Confirm:
      // The age limit guards the write only; a read-only Results screen stays readable.
      if (kAllowWrite && Expired()) Fail(F_EXPIRED);
      break;
    case Stage::Write:
      TickWrite();
      break;
    case Stage::Verify:
      TickVerify();
      break;
    default:
      break;
  }
}

C251_CODE void Cal251eFlow::Continue() {
  if (!clock_) return;
  now_ = clock_->Ms();
  switch (stage_) {
    case Stage::Warmup:
      SetStage(Stage::PromptLoop);
      break;
    case Stage::PromptLoop:
      loop_i_ = 0;
      loop_bad_ = 0.0;
      results_ms_ = now_ ? now_ : 1;  // the readings age from the first loopback point
      StartLoopPoint();
      break;
    case Stage::PromptPatch:
      StartIdentify();
      break;
    case Stage::Results:
      if (kAllowWrite) SetStage(Stage::Confirm);  // else: read-only, discard at Results
      break;
    default:
      break;
  }
}

C251_CODE void Cal251eFlow::Cancel() {
  if (stage_ == Stage::Idle) return;
  if (clock_) now_ = clock_->Ms();
  ReleaseHolds();
  if (stage_ == Stage::Done || stage_ == Stage::Failed || stage_ == Stage::NoSelfCal) {
    SetStage(Stage::Idle);  // dismissed; nothing logged, nothing sent
    return;
  }
  // The COMMIT frame is on the wire: its verdict is still read and reported.
  if (stage_ == Stage::Write && sub_ == S_COMMIT && (commit_sent_ || bus_.sends() > 0)) {
    cancel_pending_ = true;
    return;
  }
  win_.Stop();
  acq_ = A_NONE;
  BusTarget(R::kBusAddr);
  sub_ = S_NONE;
  fail_ = F_CANCELLED;
  if (commit_rc_ != 0) {
    cancelled_verdict_ = commit_rc_;
    C251_LOG(End, Cal251eCancelledName(commit_rc_), WarnWord());
  } else {
    C251_LOG(End, FailName(fail_), WarnWord());
  }
  SetStage(Stage::Failed);
}

// ---- the acquisition -----------------------------------------------------------

C251_CODE void Cal251eFlow::AcqStart(const uint16_t codes[kChannels], int windows, bool via_bus) {
  acq_bus_ = via_bus;
  acq_windows_ = windows;
  acq_w_ = 0;
  acq_i_ = 0;
  if (codes) memcpy(acq_codes_, codes, sizeof(acq_codes_));
  sub_ = S_ACQ;
  if (via_bus) {
    BusTarget(R::kBusAddr);
    acq_ = A_SEND;
  } else {
    win_.Start(*in_, *clock_);
    acq_ = A_WIN;
  }
}

C251_CODE bool Cal251eFlow::AcqTick() {
  switch (acq_) {
    case A_SEND:
      if (!bus_.Override(uint8_t(acq_i_), acq_codes_[acq_i_])) { Fail(F_INTERNAL); return false; }
      ovr_live_ = true;  // from the first frame on: it may reach the module
      acq_ = A_SENDING;
      return false;
    case A_SENDING: {
      const int rc = bus_.Tick();
      if (rc == 0) return false;
      if (rc < 0) { Fail(F_BUS); return false; }
      if (++acq_i_ < kChannels) { acq_ = A_SEND; return false; }
      win_.Start(*in_, *clock_);
      acq_ = A_WIN;
      return false;
    }
    case A_WIN: {
      CM::PointStats s[8];
      if (!win_.Tick(s)) return false;
      for (int k = 0; k < kChannels; ++k) {
        if (s[k].samples == 0) { Fail(F_NO_INPUT); return false; }
        if (s[k].max_raw - s[k].min_raw > CM::kMaxNoisePpCounts) { Fail(F_NOISY); return false; }
        const double mv = double(mvp_->Millivolts(k, s[k].mean_raw));
        const double top = double(mvp_->Millivolts(k, 0.f));
        if (mv > top - kTopGuardMv) { Fail(F_INPUT_RANGE); return false; }
        wmv_[acq_w_][k] = mv;
      }
      if (++acq_w_ < acq_windows_) {
        if (acq_bus_) { acq_i_ = 0; acq_ = A_SEND; }  // refresh the overrides before every window
        else win_.Start(*in_, *clock_);
        return false;
      }
      for (int k = 0; k < kChannels; ++k) {
        double sum = 0.0;
        for (int w = 0; w < acq_windows_; ++w) sum += wmv_[w][k];
        mv_[k] = sum / double(acq_windows_);
        if (acq_windows_ > 1 && fabs(wmv_[0][k] - wmv_[1][k]) > kRepeatMv) { Fail(F_DRIFT); return false; }
        if (acq_bus_ && acq_windows_ > 1 && stage_ == Stage::Measure) {
          const double d = wmv_[0][k] - wmv_[1][k];
          noise_ss_[k] += 0.5 * d * d;
        }
      }
      if (acq_bus_ && acq_windows_ > 1 && stage_ == Stage::Measure) ++noise_n_;
      acq_ = A_NONE;
      return true;
    }
    default:
      return false;
  }
}

// ---- loopback: Xeno outputs E-H into inputs 1-4 -------------------------------

C251_CODE void Cal251eFlow::StartLoopPoint() {
  SetStage(Stage::Loopback);
  for (int k = 0; k < kChannels; ++k)
    out_->Hold(kFirstLoopOut + k,
               out_->CodeForMv(kFirstLoopOut + k, tables_[k], kLoopMv[loop_i_]));
  AcqStart(nullptr, kWindows, false);
}

C251_CODE void Cal251eFlow::TickLoop() {
  if (!AcqTick()) return;
  for (int k = 0; k < kChannels; ++k) {
    C251_LOG(Loop, k + 1, kLoopMv[loop_i_], mv_[k]);
    const double e = fabs(mv_[k] - double(kLoopMv[loop_i_]));
    if (e > kPatchErrMv) { Fail(F_PATCH); return; }
    if (e > loop_bad_) loop_bad_ = e;
  }
  if (++loop_i_ < kLoopPoints) {
    StartLoopPoint();
    return;
  }
  ReleaseHolds();
  sub_ = S_NONE;
  result_.loop_worst_mv = float(loop_bad_);
  if (loop_bad_ > kLoopbackMv) { Fail(F_LOOPBACK); return; }
  SetStage(Stage::PromptPatch);
}

// ---- identify: each 251e channel alone ----------------------------------------

C251_CODE void Cal251eFlow::StartIdentify() {
  SetStage(Stage::Identify);
  id_step_ = 0;
  id_bad_ch_ = id_seen_in_ = -1;
  StartIdStep();
}

C251_CODE void Cal251eFlow::StartIdStep() {
  uint16_t codes[kChannels];
  const uint16_t lo = (uint16_t)R::CodeOfMv(kIdLowMv), hi = (uint16_t)R::CodeOfMv(kIdHighMv);
  for (int k = 0; k < kChannels; ++k) codes[k] = (id_step_ >= 1 && k == id_step_ - 1) ? hi : lo;
  AcqStart(codes, 1, true);
}

C251_CODE void Cal251eFlow::TickIdentify() {
  if (sub_ == S_ID_STATUS) {
    const int rc = bus_.Tick();
    if (rc == 0) return;
    if (rc < 0) { Fail(F_BUS); return; }
    // The OVERRIDEs have no reply: the status bit is the only proof the module took them.
    if (!(bus_.status().flags & Cal251eBus::kStatusOverride)) { Fail(F_NO_OVERRIDE); return; }
    id_step_ = 1;
    StartIdStep();
    return;
  }
  if (!AcqTick()) return;
  if (id_step_ == 0) {
    for (int k = 0; k < kChannels; ++k) id_base_[k] = mv_[k];
    sub_ = S_ID_STATUS;
    BusTarget(R::kBusAddr);
    bus_.ReadStatus();
    return;
  }
  const int ch = id_step_ - 1;
  double best = kIdOtherMv;
  int seen = -1;
  bool ok = true;
  for (int k = 0; k < kChannels; ++k) {
    const double d = mv_[k] - id_base_[k];
    if (d > best) { best = d; seen = k; }
    if (k == ch ? d < kIdMoveMv : d > kIdOtherMv) ok = false;
  }
  if (!ok) {
    id_bad_ch_ = ch;
    id_seen_in_ = seen;
    Fail(F_IDENTITY);
    return;
  }
  if (++id_step_ <= kChannels) {
    StartIdStep();
    return;
  }
  pt_ = 0;
  StartPoint();
}

// ---- measure ------------------------------------------------------------------

C251_CODE void Cal251eFlow::StartPoint() {
  SetStage(Stage::Measure);
  uint16_t codes[kChannels];
  if (pt_ < kFitPoints || pt_ == kPoints - 1) {
    const uint16_t c = (uint16_t)R::CodeOfMv(kFitMv[pt_ < kFitPoints ? pt_ : 0]);
    for (int k = 0; k < kChannels; ++k) codes[k] = c;
  } else {
    // The corrected code the record would send for this stage code, held raw.
    const uint16_t sc = (uint16_t)R::CodeOfMv(kVerifyMv[pt_ - kFitPoints]);
    for (int k = 0; k < kChannels; ++k)
      codes[k] = R::Apply(sc, result_.ch[k].gain, result_.ch[k].offset);
  }
  AcqStart(codes, kWindows, true);
}

C251_CODE void Cal251eFlow::TickMeasure() {
  if (!AcqTick()) return;
  if (pt_ < kFitPoints) {
    for (int k = 0; k < kChannels; ++k) {
      fit_[k][pt_].code = acq_codes_[k];
      fit_[k][pt_].mv = mv_[k];
      C251_LOG(Point, k, 'F', acq_codes_[k], mv_[k]);
    }
    if (pt_ == kFitPoints - 1) {
      FitChannels();
      if (stage_ != Stage::Measure) return;
    }
  } else if (pt_ < kPoints - 1) {
    const int v = pt_ - kFitPoints;
    for (int k = 0; k < kChannels; ++k) {
      ver_[k][v].code = acq_codes_[k];
      ver_[k][v].mv = mv_[k];
      C251_LOG(Point, k, 'V', acq_codes_[k], mv_[k]);
    }
    if (v == kVerifyPoints - 1) {
      for (int k = 0; k < kChannels; ++k) {
        double worst = 0.0;
        for (int i = 0; i < kVerifyPoints; ++i) {
          const double e = ver_[k][i].mv - double(R::CodeOfMv(kVerifyMv[i])) * R::kMvPerCode;
          if (fabs(e) > fabs(worst)) worst = e;
        }
        result_.verify_mv[k] = float(worst);
      }
      for (int k = 0; k < kChannels; ++k) {
        result_.noise_mv[k] = noise_n_ ? float(sqrt(noise_ss_[k] / double(noise_n_))) : 0.f;
        unsigned warn = 0;
        if (result_.warn_nonlin & (1u << k)) warn |= 1u;
        if (result_.warn_anchor & (1u << k)) warn |= 4u;
        const double av = fabs(double(result_.verify_mv[k]));
        if (av > kVerifyMvLimit && av <= kVerifyGrossMv && !kAllowWrite) {
          result_.warn_verify = uint8_t(result_.warn_verify | (1u << k));
          warn |= 2u;
          C251_LOG(Warn, k, "VERIFY", av, kVerifyMvLimit);
        }
        C251_LOG(Stat, k, double(result_.noise_mv[k]), double(result_.a_mv[k]), double(result_.verify_mv[k]),
                 double(result_.anchor_mv[k]), warn);
      }
      for (int k = 0; k < kChannels; ++k) {
        const double av = fabs(double(result_.verify_mv[k]));
        // Over the gross limit always; over the write limit only in a build that can write.
        if (av > kVerifyGrossMv || (kAllowWrite && av > kVerifyMvLimit)) { Fail(F_VERIFY); return; }
      }
    }
  } else {
    for (int k = 0; k < kChannels; ++k) {
      C251_LOG(Point, k, 'D', acq_codes_[k], mv_[k]);
      if (fabs(mv_[k] - fit_[k][0].mv) > kDriftMv) { Fail(F_DRIFT); return; }
    }
  }
  if (++pt_ < kPoints) {
    StartPoint();
    return;
  }
  FinishMeasure();
}

// After the fit points: the line, the physical checks, then the record per channel. Every
// channel is evaluated and logged even after one has failed (the data of the others is what the
// owner needs); the run ends with the FIRST failure found (F_SCALE before F_FIT: a nonsense line is
// a scale problem).
C251_CODE void Cal251eFlow::FitChannels() {
  FailCode first = F_NONE;
  for (int k = 0; k < kChannels; ++k) {
    R::Analog an;
    if (!R::Line(fit_[k], kFitPoints, &an)) { if (first == F_NONE) first = F_FIT; continue; }
    an_[k] = an;
    const double nl = R::Nonlinearity(fit_[k], kFitPoints, an);
    double bow = 0.0, inl = 0.0;
    R::BowInl(fit_[k], kFitPoints, &bow, &inl);
    result_.bow_mv[k] = float(bow);
    result_.inl_mv[k] = float(inl);
    C251_LOG(Line, k, an.a, an.b, nl, bow, inl);
    result_.a_mv[k] = float(an.a);
    result_.b_mv[k] = float(an.b);
    result_.nonlin_mv[k] = float(nl);
    if (!R::ScaleOk(an)) { if (first == F_NONE) first = F_SCALE; continue; }
    CalRec::Channel c;
    R::Analog an2;
    double worst = 0.0;
    if (R::Fit(fit_[k], kFitPoints, fit_k_, &c, &an2, &worst) != R::FIT_OK) {
      if (first == F_NONE) first = F_FIT;
      continue;
    }
    // Over the gross limit always; over the write limit it fails only in a build that can write,
    // else it is reported (warn first: the limits are not yet set from data).
    if (nl > kNonlinGrossMv || (kAllowWrite && nl > kNonlinMv)) {
      if (first == F_NONE) first = F_NONLIN;
    } else if (nl > kNonlinMv) {
      result_.warn_nonlin = uint8_t(result_.warn_nonlin | (1u << k));
      C251_LOG(Warn, k, "NONLIN", nl, kNonlinMv);
    }
    double aw = 0.0;
    const bool anchors = R::AnchorsOk(an, c, &aw);
    result_.ch[k] = c;
    result_.anchor_mv[k] = float(aw);
    C251_LOG(Fit, k, c, worst, aw);
    if (aw > R::kAnchorGrossMv || (kAllowWrite && !anchors)) {
      if (first == F_NONE) first = F_RECORD;
    } else if (!anchors) {
      result_.warn_anchor = uint8_t(result_.warn_anchor | (1u << k));
      C251_LOG(Warn, k, "ANCHOR", aw, R::kAnchorTolMv);
    }
  }
  if (first != F_NONE) Fail(first);
}

C251_CODE void Cal251eFlow::FinishMeasure() {
  ReleaseHolds();  // the Xeno's outputs were not used; the 251e's overrides are released
  sub_ = S_NONE;
  pt_ = -1;
  result_.valid_mask = uint8_t((1u << kChannels) - 1);
  SetStage(Stage::Results);
}

// ---- write, verify -------------------------------------------------------------

C251_CODE void Cal251eFlow::ConfirmWrite() {
  if (!kAllowWrite || !clock_ || stage_ != Stage::Confirm) return;
  now_ = clock_->Ms();
  // Defence in depth: only a run whose channels all passed, whose readings are fresh and whose
  // records still pass the absolute anchors may write.
  if (result_.valid_mask != (1u << kChannels) - 1) { Fail(F_INTERNAL); return; }
  if (now_ - results_ms_ >= kResultsMaxAgeMs) { Fail(F_EXPIRED); return; }
  if (commits_power_up_ >= kWriteCap) { Fail(F_WRITE_CAP); return; }
  // A record exists that the Scan could not read: its unit ID and sequence number are unknown, so
  // nothing may overwrite it.
  if (had_record_ && !old_read_ok_) { Fail(F_OLD_UNREADABLE); return; }
  for (int k = 0; k < kChannels; ++k)
    if (!R::AnchorsOk(an_[k], result_.ch[k], nullptr)) { Fail(F_RECORD); return; }
  SetStage(Stage::Write);
  sub_ = S_NONE;  // TickWrite starts once any pending RELEASE is out
}

C251_CODE void Cal251eFlow::TickWrite() {
  if (sub_ == S_NONE) {
    if (rel_pending_) return;
    BusTarget(R::kBusAddr);
    sub_ = S_READREC;
    bus_.ReadRecord();
    return;
  }
  const int rc = bus_.Tick();
  if (sub_ == S_COMMIT && bus_.sends() > 0) commit_sent_ = true;
  if (rc == 0) return;
  if (sub_ == S_READREC) {
    if (rc < 0) { Fail(F_BUS); return; }
    R::Record r, old;
    const bool decoded = bus_.has_record() && R::Decode(bus_.record(), R::kRecLen, &old);
    if (had_record_ && !decoded) {
      // A record exists but cannot be read now: it would be overwritten with a new unit ID and
      // seq 1 and never logged. Refuse; nothing has been sent.
      if (bus_.has_record()) C251_LOG(Old, bus_.record(), -1000);
      else C251_LOG(Old, nullptr, -1000);
      Fail(F_OLD_UNREADABLE);
      return;
    }
    if (decoded) {
      C251_LOG(Old, bus_.record(), 0);
      memcpy(r.unit_id, old.unit_id, sizeof(r.unit_id));  // the unit keeps its ID
      r.seq = uint16_t(old.seq + 1);
      if (r.seq == 0) r.seq = 1;
    } else {
      entropy_->Fill(r.unit_id, int(sizeof(r.unit_id)));
      r.seq = 1;
    }
    r.date = entropy_->UnixTime();
    for (int k = 0; k < kChannels; ++k) r.ch[k] = result_.ch[k];
    R::Encode(r, rec_);
    crc_ = uint16_t((rec_[R::kRecLen - 2] << 8) | rec_[R::kRecLen - 1]);
    C251_LOG(Rec, R::kBusAddr, rec_);
    sub_ = S_PUT;
    bus_.PutRecord(rec_);
    return;
  }
  if (sub_ == S_PUT) {
    if (rc < 0) { Fail(F_BUS); return; }
    // The status just before the COMMIT is the baseline for believing a refused bit.
    sub_ = S_PRESTAT;
    BusTarget(R::kBusAddr);
    bus_.ReadStatus();
    return;
  }
  if (sub_ == S_PRESTAT) {
    if (rc < 0) { Fail(F_BUS); return; }
    sub_ = S_COMMIT;
    commit_sent_ = false;
    ++commits_power_up_;
    R::Record sent;
    R::Decode(rec_, R::kRecLen, &sent);
    C251_LOG(Commit, commits_power_up_, unsigned(sent.seq));
    if (!bus_.Commit(crc_)) Fail(F_BUS);
    return;
  }
  // S_COMMIT: its one verdict. Never sent again.
  commit_rc_ = rc;
  sub_ = S_NONE;
  if (cancel_pending_) {
    cancel_pending_ = false;
    ReleaseHolds();
    fail_ = F_CANCELLED;
    cancelled_verdict_ = rc;
    C251_LOG(End, Cal251eCancelledName(rc), WarnWord());
    SetStage(Stage::Failed);
    return;
  }
  if (rc == 1) {
    SetStage(Stage::Verify);
    bus_.ReadRecord();
    return;
  }
  if (rc == E_NO_SLOT || rc == E_COMMIT_REFUSED) { Fail(F_COMMIT_REFUSED); return; }
  // Only these two mean the frame never went on the wire. Every other outcome, a Wire error
  // included (a NAK on the last byte may follow a latched frame), is unknown.
  if (rc == E_BUS_BUSY || rc == E_INTERNAL) { Fail(F_BUS); return; }
  Fail(F_COMMIT_UNKNOWN);
}

C251_CODE void Cal251eFlow::TickVerify() {
  const int rc = bus_.Tick();
  if (rc == 0) return;
  if (rc == 1 && bus_.has_record() && memcmp(bus_.record(), rec_, R::kRecLen) == 0) {
    Finish();
    return;
  }
  Fail(F_READBACK);
}
