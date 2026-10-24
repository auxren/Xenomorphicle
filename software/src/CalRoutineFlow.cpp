// The calibration routine's step machine. See the header for the design and
// design/cal-routine-design.md 6.2-6.4 for the reasons.
//
// Firmware: all of it runs from flash (FLASHMEM). None of it is ISR code, and
// the T41 images have little ITCM to spare. Every function here has external
// linkage (members or named-namespace functions) so the section attribute is
// honoured.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define CALR_CODE FLASHMEM
#else
#define CALR_CODE
#endif

#include "CalRoutineFlow.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "InputCurveStore.h"
#include "UsbTmc.h"

namespace CM = CvMeterMath;
namespace CR = CalRoutineMath;

// =============================================================================
// Line formats

namespace CalRoutineLines {

// One line being built; truncates cleanly at n.
struct Builder {
  char *b;
  size_t n;
  size_t pos;
  void Init(char *buf, size_t cap);
  void Str(const char *s);
  void Field(const char *s);  // "," then s
  void Int(long v);
  void Uns(unsigned long v);
  void Fix(double v, int decimals);
  void Hex(unsigned v, int digits);
  int Done() const;
};

CALR_CODE void Builder::Init(char *buf, size_t cap) {
  b = buf;
  n = cap;
  pos = 0;
  if (n) b[0] = 0;
}
CALR_CODE void Builder::Str(const char *s) { if (n) CM::Append(b, n, &pos, s); }
CALR_CODE void Builder::Field(const char *s) { Str(","); Str(s); }
CALR_CODE void Builder::Int(long v) {
  char t[16];
  snprintf(t, sizeof(t), ",%ld", v);
  Str(t);
}
CALR_CODE void Builder::Uns(unsigned long v) {
  char t[16];
  snprintf(t, sizeof(t), ",%lu", v);
  Str(t);
}
CALR_CODE void Builder::Fix(double v, int decimals) {
  char t[32];
  CM::FormatFixed(t, sizeof(t), CM::ScaleRound(v, decimals), decimals);
  Str(",");
  Str(t);
}
CALR_CODE void Builder::Hex(unsigned v, int digits) {
  char t[16];
  snprintf(t, sizeof(t), ",%0*X", digits, v);
  Str(t);
}
CALR_CODE int Builder::Done() const { return int(pos); }

static const char *const kHeaders[] = {
  "CALR#,version=1",
  "CALR#,METER,identity",
  "CALR#,STATE,ms,stage,out,in",
  "CALR#,OUT,out",
  "CALR#,NODE,out,k,target_mv,code,measured_mv,iters,rail",
  "CALR#,REREAD,out,target_mv,code,spread_mv",
  "CALR#,SWEEP,out,target_mv,code,measured_mv,resid_mv,counted",
  "CALR#,ORES,out,result,reason,worst_mv,worst_at_mv,rail_mask",
  "CALR#,IN,in,src_out,lo_mv,hi_mv",
  "CALR#,IPT,in,mv,code,mean_raw,pp_raw,samples,used",
  "CALR#,IRES,in,result,verdict,gain_uv,offset_mv,holdout_mv,line_dev_mv,points",
  "CALR#,RETRY,scpi,attempt",
  "CALR#,LOST,out",
  "CALR#,BACK,out",
  "CALR#,RESULT,can_save,out_pass,in_pass",
  "CALR#,SAVE,result",
  "CALR#,DISCARD",
  "CALR#,CANCEL,stage",
  "CALR#,FAIL,reason",
};

CALR_CODE int HeaderCount() { return int(sizeof(kHeaders) / sizeof(kHeaders[0])); }
CALR_CODE const char *Header(int i) { return i >= 0 && i < HeaderCount() ? kHeaders[i] : ""; }

// Panel label 0..7 -> 1..8; anything else (none) -> 0.
CALR_CODE long Label(int x) { return x >= 0 && x < 8 ? long(x + 1) : 0L; }

CALR_CODE int Meter(char *buf, size_t n, const char *identity) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,METER,");
  // One field: commas become ';', control bytes go, trailing blanks too.
  char id[81];
  size_t k = 0;
  for (const char *p = identity ? identity : ""; *p && k < sizeof(id) - 1; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c >= 0x7F) continue;
    id[k++] = c == ',' ? ';' : char(c);
  }
  while (k && id[k - 1] == ' ') --k;
  id[k] = 0;
  l.Str(k ? id : "-");
  return l.Done();
}

CALR_CODE int State(char *buf, size_t n, uint32_t ms, const char *stage, int out, int in) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,STATE");
  l.Uns(ms);
  l.Field(stage);
  l.Int(Label(out));
  l.Int(Label(in));
  return l.Done();
}

CALR_CODE int Out(char *buf, size_t n, int out) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,OUT");
  l.Int(Label(out));
  return l.Done();
}

CALR_CODE int Node(char *buf, size_t n, int out, int k, int32_t target_mv, uint16_t code,
                   double measured_mv, int iters, bool at_rail) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,NODE");
  l.Int(Label(out));
  l.Int(k);
  l.Int(target_mv);
  l.Uns(code);
  l.Fix(measured_mv, 3);
  l.Int(iters);
  l.Int(at_rail ? 1 : 0);
  return l.Done();
}

CALR_CODE int Reread(char *buf, size_t n, int out, int32_t target_mv, uint16_t code,
                     double spread_mv) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,REREAD");
  l.Int(Label(out));
  l.Int(target_mv);
  l.Uns(code);
  l.Fix(spread_mv, 3);
  return l.Done();
}

CALR_CODE int Sweep(char *buf, size_t n, int out, int32_t target_mv, uint16_t code,
                    double measured_mv, bool counted) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,SWEEP");
  l.Int(Label(out));
  l.Int(target_mv);
  l.Uns(code);
  l.Fix(measured_mv, 3);
  l.Fix(measured_mv - double(target_mv), 3);
  l.Int(counted ? 1 : 0);
  return l.Done();
}

CALR_CODE int OutResult(char *buf, size_t n, int out, bool pass, const char *reason,
                        float worst_mv, int32_t worst_at_mv, uint16_t rail_mask) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,ORES");
  l.Int(Label(out));
  l.Field(pass ? "PASS" : "FAIL");
  l.Field(reason);
  l.Fix(worst_mv, 3);
  l.Int(worst_at_mv);
  l.Hex(rail_mask, 3);
  return l.Done();
}

CALR_CODE int In(char *buf, size_t n, int in, int src_out, int32_t lo_mv, int32_t hi_mv) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,IN");
  l.Int(Label(in));
  l.Int(Label(src_out));
  l.Int(lo_mv);
  l.Int(hi_mv);
  return l.Done();
}

CALR_CODE int InPoint(char *buf, size_t n, int in, int32_t mv, uint16_t code,
                      const CM::PointStats &s, bool used) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,IPT");
  l.Int(Label(in));
  l.Int(mv);
  l.Uns(code);
  l.Fix(s.mean_raw, 2);
  l.Fix(s.max_raw - s.min_raw, 2);
  l.Uns(s.samples);
  l.Int(used ? 1 : 0);
  return l.Done();
}

CALR_CODE int InResult(char *buf, size_t n, int in, const char *outcome, CM::Verdict v,
                       const CM::Fit &line, float holdout_mv, float line_dev_mv, int points) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,IRES");
  l.Int(Label(in));
  l.Field(outcome);
  l.Field(CM::VerdictName(v));
  l.Fix(line.gain * 1000.f, 1);
  l.Fix(line.offset_mv, 2);
  l.Fix(holdout_mv, 2);
  l.Fix(line_dev_mv, 2);
  l.Int(points);
  return l.Done();
}

CALR_CODE int Retry(char *buf, size_t n, const char *scpi, int attempt) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,RETRY");
  l.Field(scpi);
  l.Int(attempt);
  return l.Done();
}

CALR_CODE int Lost(char *buf, size_t n, int out) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,LOST");
  l.Int(Label(out));
  return l.Done();
}

CALR_CODE int Back(char *buf, size_t n, int out) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,BACK");
  l.Int(Label(out));
  return l.Done();
}

CALR_CODE int Result(char *buf, size_t n, bool can_save, uint8_t out_pass_mask,
                     uint8_t in_pass_mask) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,RESULT");
  l.Int(can_save ? 1 : 0);
  l.Hex(out_pass_mask, 2);
  l.Hex(in_pass_mask, 2);
  return l.Done();
}

CALR_CODE int Save(char *buf, size_t n, bool ok) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,SAVE");
  l.Field(ok ? "OK" : "FAIL");
  return l.Done();
}

CALR_CODE int Discard(char *buf, size_t n) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,DISCARD");
  return l.Done();
}

CALR_CODE int Cancel(char *buf, size_t n, const char *stage) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,CANCEL");
  l.Field(stage);
  return l.Done();
}

CALR_CODE int Fail(char *buf, size_t n, const char *reason) {
  Builder l;
  l.Init(buf, n);
  l.Str("CALR,FAIL");
  l.Field(reason);
  return l.Done();
}

// The console Q's lines, as CvMeter.cpp printed them before (docs/CV-Meter.md).
CALR_CODE int IncalPatch(char *buf, size_t n, int in, int out, const int32_t mv[3]) {
  if (!n) return 0;
  const int w = snprintf(buf, n, "INCAL,PATCH,in=%d,out=%d,points=%ld,%ld,%ld", in + 1, out + 1,
                         (long)mv[0], (long)mv[1], (long)mv[2]);
  return w < 0 ? 0 : (size_t(w) < n ? w : int(n - 1));
}

CALR_CODE int IncalPoint(char *buf, size_t n, int in, int k, int32_t mv, const CM::PointStats &s) {
  Builder l;
  l.Init(buf, n);
  char t[48];
  snprintf(t, sizeof(t), "INCAL,POINT,in=%d,%d,%ld", in + 1, k + 1, (long)mv);
  l.Str(t);
  l.Fix(s.mean_raw, 2);
  l.Fix(s.max_raw - s.min_raw, 2);
  l.Uns(s.samples);
  return l.Done();
}

CALR_CODE int IncalResult(char *buf, size_t n, int in, int out, const CM::Fit &fit,
                          float residual_mv, float noise_pp, CM::Verdict v) {
  Builder l;
  l.Init(buf, n);
  char t[48];
  snprintf(t, sizeof(t), "INCAL,RESULT,in=%d,out=%d,gain_uv=", in + 1, out + 1);
  l.Str(t);
  char f[32];
  CM::FormatFixed(f, sizeof(f), CM::ScaleRound(fit.gain * 1000.f, 1), 1);
  l.Str(f);
  l.Str(",offset_mv=");
  CM::FormatFixed(f, sizeof(f), CM::ScaleRound(fit.offset_mv, 2), 2);
  l.Str(f);
  l.Str(",resid_mv=");
  CM::FormatFixed(f, sizeof(f), CM::ScaleRound(residual_mv, 2), 2);
  l.Str(f);
  l.Str(",noise_pp=");
  CM::FormatFixed(f, sizeof(f), CM::ScaleRound(noise_pp, 2), 2);
  l.Str(f);
  l.Str(",verdict=");
  l.Str(CM::VerdictName(v));
  return l.Done();
}

}  // namespace CalRoutineLines

namespace L = CalRoutineLines;

// =============================================================================
// Input point (shared with the console Q)

namespace CalRoutine {

const char *const kMeterSetup[5] = {
  "*CLS", "CONF:VOLT:DC 20", "VOLT:DC:NPLC 10", "VOLT:DC:AZ ON", "TRIG:SOUR IMM",
};

CALR_CODE void InputPoint::Start(OutputPort &out, InputPort &in, Clock &clock, int out_label,
                                 uint16_t code, int in_label) {
  out_ = &out;
  in_ = &in;
  clock_ = &clock;
  in_label_ = in_label;
  out.Hold(out_label, code);
  t0_ = clock.Ms();
  phase_ = P_SETTLE;
}

CALR_CODE bool InputPoint::Tick(CM::PointStats *s) {
  if (phase_ == P_IDLE) return false;
  const uint32_t now = clock_->Ms();
  if (phase_ == P_SETTLE) {
    if (now - t0_ < kInSettleMs) return false;
    in_->StartWindow(in_label_);
    t0_ = now;
    phase_ = P_MEASURE;
    return false;
  }
  if (now - t0_ < kInMeasureMs) return false;
  if (in_->WindowDone(s)) {
    phase_ = P_IDLE;
    return true;
  }
  if (now - t0_ >= kInMeasureMs + kInWindowTimeoutMs) {
    s->mean_raw = s->min_raw = s->max_raw = 0.f;
    s->samples = 0;
    phase_ = P_IDLE;
    return true;
  }
  return false;
}

}  // namespace CalRoutine

using namespace CalRoutine;

// =============================================================================
// The flow

CALR_CODE bool CalRoutineFlow::Results::CanSave() const {
  for (int o = 0; o < 8; ++o)
    if (!out_pass[o]) return false;
  return true;
}

CALR_CODE const char *CalRoutineFlow::StageName(Stage s) {
  switch (s) {
    case Stage::Idle: return "IDLE";
    case Stage::NoMeter: return "NOMETER";
    case Stage::ConfigMeter: return "CONFIG";
    case Stage::PromptOutput: return "PROMPT_OUT";
    case Stage::CalOutput: return "CAL_OUT";
    case Stage::SweepOutput: return "SWEEP_OUT";
    case Stage::PromptInput: return "PROMPT_IN";
    case Stage::CalInput: return "CAL_IN";
    case Stage::Results: return "RESULTS";
    case Stage::Failed: return "FAILED";
  }
  return "?";
}

CALR_CODE const char *CalRoutineFlow::OutReasonName(uint8_t r) {
  switch (r) {
    case OUT_OK: return "OK";
    case OUT_NOT_RUN: return "NOT_RUN";
    case OUT_NOISY: return "NOISY";
    case OUT_NOT_MONOTONE: return "NOT_MONOTONE";
    case OUT_SLOPE: return "SLOPE";
    case OUT_RESIDUAL: return "RESIDUAL";
  }
  return "?";
}

CALR_CODE void CalRoutineFlow::Log(const char *line) {
  if (log_) log_(line);
}

#define CALR_LOG(fn, ...) do { \
    char line_[L::kMaxLine]; \
    L::fn(line_, sizeof(line_), __VA_ARGS__); \
    Log(line_); \
  } while (0)

CALR_CODE void CalRoutineFlow::SetStage(Stage s) {
  stage_ = s;
  CALR_LOG(State, now_, StageName(s), cur_out_, cur_in_);
}

CALR_CODE void CalRoutineFlow::Begin(MeterPort &meter, OutputPort &out, InputPort &in, Clock &clock) {
  out.ReleaseAll();  // a Begin mid-run never leaves the previous run's output held
  meter_ = &meter;
  out_ = &out;
  in_ = &in;
  clock_ = &clock;
  now_ = clock.Ms();
  memset(&results_, 0, sizeof(results_));
  for (int o = 0; o < 8; ++o) {
    for (int k = 0; k < CR::kNodes; ++k) results_.table[o][k] = out.TableCode(o, k);
    results_.out_reason[o] = OUT_NOT_RUN;
    results_.in_verdict[o] = CM::VERDICT_NO_SAMPLES;
  }
  q_phase_ = Q_NONE;
  m_phase_ = M_NONE;
  ipoint_.Stop();
  lost_ = false;
  resume_out_ = -1;
  cur_out_ = -1;
  cur_in_ = -1;
  for (int i = 0; i < L::HeaderCount(); ++i) Log(L::Header(i));
  if (meter.Connected()) StartConfig();
  else SetStage(Stage::NoMeter);
}

CALR_CODE void CalRoutineFlow::StartConfig() {
  CALR_LOG(Meter, meter_->Identity());
  cfg_i_ = 0;
  QStart(kMeterSetup[0], kConfigTimeoutMs);
  SetStage(Stage::ConfigMeter);
}

CALR_CODE void CalRoutineFlow::Tick() {
  if (!meter_) return;
  now_ = clock_->Ms();
  switch (stage_) {
    case Stage::NoMeter:
      TickNoMeter();
      break;
    case Stage::ConfigMeter:
    case Stage::CalOutput:
    case Stage::SweepOutput:
      if (!meter_->Connected()) {
        Lost();
        break;
      }
      if (stage_ == Stage::ConfigMeter) TickConfig();
      else if (stage_ == Stage::CalOutput) TickNode();
      else TickSweep();
      break;
    case Stage::CalInput:
      TickInput();  // the meter is not in this loop
      break;
    default:
      break;
  }
}

CALR_CODE void CalRoutineFlow::Continue() {
  if (!meter_) return;
  now_ = clock_->Ms();
  if (stage_ == Stage::PromptOutput) {
    if (!meter_->Connected()) Lost();
    else StartOutput(cur_out_);
  } else if (stage_ == Stage::PromptInput) {
    StartInput(cur_in_);
  }
}

CALR_CODE void CalRoutineFlow::Skip() {
  if (!meter_ || stage_ != Stage::PromptInput) return;
  now_ = clock_->Ms();
  const int i = cur_in_;
  results_.in_pass[i] = false;
  results_.in_verdict[i] = CM::VERDICT_NO_SAMPLES;
  results_.in_points[i] = 0;
  results_.in_holdout_mv[i] = 0.f;
  results_.line[i].gain = results_.line[i].offset_mv = 0.f;
  memset(results_.curve_mv_x2[i], 0, sizeof(results_.curve_mv_x2[i]));
  CALR_LOG(InResult, i, "SKIP", CM::VERDICT_NO_SAMPLES, results_.line[i], 0.f, 0.f, 0);
  NextAfterInput();
}

CALR_CODE void CalRoutineFlow::Cancel() {
  if (stage_ == Stage::Idle) return;
  if (clock_) now_ = clock_->Ms();
  if (out_) out_->ReleaseAll();
  ipoint_.Stop();
  q_phase_ = Q_NONE;
  m_phase_ = M_NONE;
  lost_ = false;
  for (int o = 0; o < 8; ++o) results_.out_pass[o] = results_.in_pass[o] = false;
  CALR_LOG(Cancel, StageName(stage_));
  cur_out_ = cur_in_ = -1;
  SetStage(Stage::Idle);
}

CALR_CODE void CalRoutineFlow::Fail(const char *reason) {
  out_->ReleaseAll();
  ipoint_.Stop();
  q_phase_ = Q_NONE;
  m_phase_ = M_NONE;
  lost_ = false;
  for (int o = 0; o < 8; ++o) results_.out_pass[o] = results_.in_pass[o] = false;
  CALR_LOG(Fail, reason);
  SetStage(Stage::Failed);
}

// The meter went away. Nothing measured on the current output is kept: a
// replug or a hub moves the readings by tenths of a mV, so the output starts
// again from node 0 once the meter is back and set up again.
CALR_CODE void CalRoutineFlow::Lost() {
  out_->ReleaseAll();
  q_phase_ = Q_NONE;
  m_phase_ = M_NONE;
  resume_out_ = cur_out_;
  lost_ = true;
  lost_ms_ = now_;
  CALR_LOG(Lost, cur_out_);
  SetStage(Stage::NoMeter);
}

CALR_CODE void CalRoutineFlow::TickNoMeter() {
  if (meter_->Connected()) {
    if (lost_) CALR_LOG(Back, resume_out_);
    StartConfig();
    return;
  }
  if (lost_ && now_ - lost_ms_ >= kReconnectMs) Fail("METER_LOST");
}

CALR_CODE void CalRoutineFlow::TickConfig() {
  const QResult r = QStep();
  if (r == QR_BUSY) return;
  if (r == QR_LOST) { Lost(); return; }
  if (r == QR_FAIL) { Fail("METER_CONFIG"); return; }
  if (++cfg_i_ < int(sizeof(kMeterSetup) / sizeof(kMeterSetup[0]))) {
    QStart(kMeterSetup[cfg_i_], kConfigTimeoutMs);
    return;
  }
  const bool resume = lost_ && resume_out_ >= 0;
  lost_ = false;
  if (resume) {
    StartOutput(resume_out_);
  } else {
    cur_out_ = 0;
    SetStage(Stage::PromptOutput);
  }
}

// ---- one meter query, retried ------------------------------------------------

CALR_CODE void CalRoutineFlow::QStart(const char *scpi, uint32_t timeout_ms) {
  q_scpi_ = scpi;
  q_timeout_ = timeout_ms;
  q_fails_ = 0;
  q_phase_ = Q_BEGIN;
  q_begin_ms_ = now_;
}

CALR_CODE CalRoutineFlow::QResult CalRoutineFlow::QFailure() {
  if (!meter_->Connected()) {
    q_phase_ = Q_NONE;
    return QR_LOST;
  }
  if (q_fails_ >= kQueryRetries) {
    q_phase_ = Q_NONE;
    return QR_FAIL;
  }
  ++q_fails_;
  CALR_LOG(Retry, q_scpi_, q_fails_);
  q_phase_ = Q_WAIT;
  q_t0_ = now_;
  return QR_BUSY;
}

CALR_CODE CalRoutineFlow::QResult CalRoutineFlow::QStep() {
  switch (q_phase_) {
    case Q_NONE:
      return QR_FAIL;
    case Q_WAIT:  // after a failed attempt
      if (now_ - q_t0_ < kRetryWaitMs) return QR_BUSY;
      q_begin_ms_ = now_;
      q_phase_ = Q_BEGIN;
      return QR_BUSY;
    case Q_BEGIN_WAIT:  // Begin() refused: the driver is busy or resynchronising
      if (now_ - q_t0_ < kRetryWaitMs) return QR_BUSY;
      q_phase_ = Q_BEGIN;
      return QR_BUSY;
    case Q_BEGIN:
      if (!meter_->Connected()) {
        q_phase_ = Q_NONE;
        return QR_LOST;
      }
      if (meter_->Begin(q_scpi_, q_timeout_)) {
        q_phase_ = Q_BUSY;
        q_t0_ = now_;
        return QR_BUSY;
      }
      if (now_ - q_begin_ms_ >= kBeginGiveUpMs) return QFailure();
      q_phase_ = Q_BEGIN_WAIT;
      q_t0_ = now_;
      return QR_BUSY;
    case Q_BUSY: {
      const char *reply = nullptr;
      const int r = meter_->Poll(&reply);
      if (r > 0) {
        q_phase_ = Q_NONE;
        q_reply_ = reply ? reply : "";
        return QR_DONE;
      }
      if (r == 0 && now_ - q_t0_ < q_timeout_ + kWatchdogMs) return QR_BUSY;
      return QFailure();
    }
  }
  return QR_FAIL;
}

// ---- one measurement: settle, kReadings READ?, median and spread ---------------

CALR_CODE void CalRoutineFlow::MStart(uint16_t code, int32_t target_mv) {
  out_->Hold(cur_out_, code);
  m_code_ = code;
  m_target_ = target_mv;
  m_t0_ = now_;
  m_phase_ = M_SETTLE;
  m_n_ = 0;
  m_reread_ = false;
}

CALR_CODE CalRoutineFlow::MResult CalRoutineFlow::MStep() {
  if (m_phase_ == M_SETTLE) {
    if (now_ - m_t0_ < kOutSettleMs) return MR_BUSY;
    m_phase_ = M_READ;
    QStart("READ?", kReadTimeoutMs);
    return MR_BUSY;
  }
  if (m_phase_ != M_READ) return MR_FAIL;
  QResult r = QStep();
  if (r == QR_DONE) {
    double volts = 0.0;
    if (!UsbTmc::ParseReading(q_reply_, &volts)) {
      r = QFailure();  // garbled: ask again, within the same retry budget
    } else {
      m_mv_[m_n_++] = volts * 1000.0;
      if (m_n_ < kReadings) {
        QStart("READ?", kReadTimeoutMs);
        return MR_BUSY;
      }
      m_reading_ = CR::Summarize(m_mv_, m_n_);
      if (CR::ReadingTrusted(m_reading_)) {
        m_phase_ = M_NONE;
        return MR_DONE;
      }
      if (!m_reread_) {  // one re-read; the untrusted readings are dropped
        m_reread_ = true;
        CALR_LOG(Reread, cur_out_, m_target_, m_code_, m_reading_.spread_mv);
        m_n_ = 0;
        QStart("READ?", kReadTimeoutMs);
        return MR_BUSY;
      }
      m_phase_ = M_NONE;
      return MR_NOISY;
    }
  }
  if (r == QR_BUSY) return MR_BUSY;
  m_phase_ = M_NONE;
  return r == QR_LOST ? MR_LOST : MR_FAIL;
}

// ---- outputs -----------------------------------------------------------------

CALR_CODE void CalRoutineFlow::StartOutput(int o) {
  out_->ReleaseAll();
  cur_out_ = o;
  cur_in_ = -1;
  for (int k = 0; k < CR::kNodes; ++k) {
    results_.table[o][k] = out_->TableCode(o, k);
    results_.at_rail[o][k] = false;
    node_mv_[k] = NAN;
  }
  results_.out_pass[o] = false;
  results_.out_reason[o] = OUT_NOT_RUN;
  results_.out_worst_mv[o] = 0.f;
  results_.out_worst_at_mv[o] = 0;
  CALR_LOG(Out, o);
  node_k_ = 0;
  SetStage(Stage::CalOutput);
  StartNode();
}

CALR_CODE void CalRoutineFlow::StartNode() {
  code_ = results_.table[cur_out_][node_k_];  // the stored code
  iters_ = 0;
  best_err_ = HUGE_VAL;
  best_code_ = code_;
  best_mv_ = 0.0;
  MStart(code_, node_k_ * CR::kNodeMv);
}

// Node k: start from the stored code, correct with the local slope until
// within kTargetMv, the correction no longer moves the code, or after
// kMaxIterations measurements; keep the best code measured. A clamped
// correction is measured like any other code, so a bad slope that
// overshoots past a rail is corrected on the next pass; only a measured
// shortfall at the rail flags the entry at_rail.
CALR_CODE void CalRoutineFlow::TickNode() {
  const MResult r = MStep();
  if (r == MR_BUSY) return;
  if (r == MR_LOST) { Lost(); return; }
  if (r == MR_FAIL) { Fail("METER_TIMEOUT"); return; }
  if (r == MR_NOISY) { FinishOutput(OUT_NOISY); return; }
  const int32_t target = node_k_ * CR::kNodeMv;
  const double mv = m_reading_.median_mv;
  uint16_t *t = results_.table[cur_out_];
  ++iters_;
  t[node_k_] = code_;
  node_mv_[node_k_] = mv;
  const double err = fabs(mv - double(target));
  if (err < best_err_) {
    best_err_ = err;
    best_code_ = code_;
    best_mv_ = mv;
  }
  if (err <= CR::kTargetMv || iters_ >= CR::kMaxIterations) {
    AcceptNode();
    return;
  }
  bool clamped = false;
  const uint16_t next = CR::NextCode(code_, mv, target, CR::LocalSlope(t, node_mv_, node_k_), &clamped);
  if (next == code_) {
    AcceptNode();
    return;
  }
  code_ = next;
  MStart(code_, target);
}

CALR_CODE void CalRoutineFlow::AcceptNode() {
  const int o = cur_out_, k = node_k_;
  const int32_t target = k * CR::kNodeMv;
  results_.table[o][k] = best_code_;
  node_mv_[k] = best_mv_;
  const bool rail = (best_code_ == 65535 && best_mv_ < double(target) - CR::kTargetMv) ||
                    (best_code_ == 0 && best_mv_ > double(target) + CR::kTargetMv);
  results_.at_rail[o][k] = rail;
  CALR_LOG(Node, o, k, target, best_code_, best_mv_, iters_, rail);
  if (++node_k_ < CR::kNodes) StartNode();
  else StartSweep();
}

CALR_CODE void CalRoutineFlow::StartSweep() {
  const CR::TableVerdict tv = CR::CheckTable(results_.table[cur_out_]);
  if (tv == CR::TABLE_NOT_MONOTONE) { FinishOutput(OUT_NOT_MONOTONE); return; }
  if (tv == CR::TABLE_SLOPE) { FinishOutput(OUT_SLOPE); return; }
  n_sweep_ = CR::SweepPoints(sweep_mv_, kSweepPoints);
  sweep_i_ = 0;
  SetStage(Stage::SweepOutput);
  sweep_code_ = out_->CodeForMv(cur_out_, results_.table[cur_out_], sweep_mv_[0]);
  MStart(sweep_code_, sweep_mv_[0]);
}

// Verify sweep: 40 points between the nodes through the normal pitch-to-code
// path on the new table. Points in a segment that ends at a rail entry are
// logged but not counted: the table cannot follow the DAC there (it
// interpolates towards a code that does not give its voltage), and a rail
// entry does not fail the output (design 6.2).
CALR_CODE void CalRoutineFlow::TickSweep() {
  const MResult r = MStep();
  if (r == MR_BUSY) return;
  if (r == MR_LOST) { Lost(); return; }
  if (r == MR_FAIL) { Fail("METER_TIMEOUT"); return; }
  if (r == MR_NOISY) { FinishOutput(OUT_NOISY); return; }
  const int o = cur_out_;
  const int32_t target = sweep_mv_[sweep_i_];
  const double mv = m_reading_.median_mv;
  int seg = target / CR::kNodeMv;
  if (seg > CR::kNodes - 2) seg = CR::kNodes - 2;
  const bool counted = !results_.at_rail[o][seg] && !results_.at_rail[o][seg + 1];
  const float resid = float(fabs(mv - double(target)));
  if (counted && resid > results_.out_worst_mv[o]) {
    results_.out_worst_mv[o] = resid;
    results_.out_worst_at_mv[o] = target;
  }
  CALR_LOG(Sweep, o, target, sweep_code_, mv, counted);
  if (++sweep_i_ < n_sweep_) {
    sweep_code_ = out_->CodeForMv(o, results_.table[o], sweep_mv_[sweep_i_]);
    MStart(sweep_code_, sweep_mv_[sweep_i_]);
    return;
  }
  FinishOutput(results_.out_worst_mv[o] <= CR::kPassMv ? OUT_OK : OUT_RESIDUAL);
}

CALR_CODE void CalRoutineFlow::FinishOutput(OutReason reason) {
  out_->ReleaseAll();
  m_phase_ = M_NONE;
  q_phase_ = Q_NONE;
  const int o = cur_out_;
  if (reason == OUT_NOISY)  // incomplete: nothing measured is kept
    for (int k = 0; k < CR::kNodes; ++k) {
      results_.table[o][k] = out_->TableCode(o, k);
      results_.at_rail[o][k] = false;
    }
  results_.out_pass[o] = reason == OUT_OK;
  results_.out_reason[o] = reason;
  uint16_t rails = 0;
  for (int k = 0; k < CR::kNodes; ++k)
    if (results_.at_rail[o][k]) rails = uint16_t(rails | (1u << k));
  CALR_LOG(OutResult, o, results_.out_pass[o], OutReasonName(reason), results_.out_worst_mv[o],
           results_.out_worst_at_mv[o], rails);
  NextAfterOutput();
}

CALR_CODE void CalRoutineFlow::NextAfterOutput() {
  if (cur_out_ < 7) {
    ++cur_out_;
    SetStage(Stage::PromptOutput);
    return;
  }
  cur_out_ = -1;
  cur_in_ = 0;
  SetStage(Stage::PromptInput);
}

// ---- inputs ------------------------------------------------------------------

// The source's span: only table segments whose both ends are real (not at
// the rail). Next to a rail entry, CodeForMv interpolates towards a code
// that does not give that entry's voltage, so a point there would be
// labelled with a voltage the output is not producing (a 12 V entry pinned
// at 65535 makes "11400 mV" about 11420 mV) and bend every input's curve.
CALR_CODE void CalRoutineFlow::StartInput(int i) {
  out_->ReleaseAll();
  cur_in_ = i;
  n_ipts_ = 0;
  const bool *rail = results_.at_rail[kSourceOutput];
  int k_lo = 0;
  while (k_lo < CR::kNodes - 1 && rail[k_lo]) ++k_lo;
  int k_hi = k_lo;
  while (k_hi + 1 < CR::kNodes && !rail[k_hi + 1]) ++k_hi;
  // From kInStepMv, or from the first real entry when entry 0 is at the rail.
  ip_mv_ = k_lo ? k_lo * CR::kNodeMv : kInStepMv;
  ip_top_mv_ = k_hi * CR::kNodeMv;
  CALR_LOG(In, i, kSourceOutput, ip_mv_, ip_top_mv_);
  SetStage(Stage::CalInput);
  if (ip_top_mv_ < ip_mv_ + 3 * kInStepMv) {  // under 4 points: cannot build a curve
    FinishInput();
    return;
  }
  StartInputPoint();
}

CALR_CODE void CalRoutineFlow::StartInputPoint() {
  ip_code_ = out_->CodeForMv(kSourceOutput, results_.table[kSourceOutput], ip_mv_);
  ipoint_.Start(*out_, *in_, *clock_, kSourceOutput, ip_code_, cur_in_);
}

// Every kInStepMv across the span StartInput chose on output A; a point near
// raw 0 (the input's top rail) ends the sweep and is not used. Points near
// raw 4095 are kept, so an unpatched input fails RAIL rather than vanishing.
CALR_CODE void CalRoutineFlow::TickInput() {
  CM::PointStats s;
  if (!ipoint_.Tick(&s)) return;
  const bool used = !s.samples || s.mean_raw >= kInTopGuardCounts;
  CALR_LOG(InPoint, cur_in_, ip_mv_, ip_code_, s, used);
  if (used) {
    ipts_[n_ipts_].mv = ip_mv_;
    ipts_[n_ipts_].stats = s;
    ++n_ipts_;
  }
  if (!used || !s.samples || n_ipts_ >= kMaxInputPoints || ip_mv_ + kInStepMv > ip_top_mv_) {
    FinishInput();
    return;
  }
  ip_mv_ += kInStepMv;
  StartInputPoint();
}

CALR_CODE void CalRoutineFlow::FinishInput() {
  out_->ReleaseAll();
  ipoint_.Stop();
  const int i = cur_in_;
  int16_t curve[CR::kCurvePoints] = {};
  CM::Fit line = {0.f, 0.f};
  float dev = 0.f, holdout = 0.f;
  const CM::Verdict v = CR::BuildInputCurve(ipts_, n_ipts_, curve, &line, &dev, &holdout);
  const bool pass = v == CM::VERDICT_OK;
  results_.in_verdict[i] = v;
  results_.in_pass[i] = pass;
  results_.line[i] = line;
  results_.in_holdout_mv[i] = holdout;
  results_.in_points[i] = uint8_t(n_ipts_);
  memcpy(results_.curve_mv_x2[i], curve, sizeof(curve));
  CALR_LOG(InResult, i, pass ? "PASS" : "FAIL", v, line, holdout, dev, n_ipts_);
  NextAfterInput();
}

CALR_CODE void CalRoutineFlow::NextAfterInput() {
  if (cur_in_ < 7) {
    ++cur_in_;
    SetStage(Stage::PromptInput);
    return;
  }
  FinishRun();
}

CALR_CODE void CalRoutineFlow::FinishRun() {
  out_->ReleaseAll();
  cur_in_ = -1;
  cur_out_ = -1;
  SetStage(Stage::Results);
  uint8_t om = 0, im = 0;
  for (int n = 0; n < 8; ++n) {
    if (results_.out_pass[n]) om = uint8_t(om | (1u << n));
    if (results_.in_pass[n]) im = uint8_t(im | (1u << n));
  }
  CALR_LOG(Result, CanSave(), om, im);
}

// ---- save / discard ----------------------------------------------------------

CALR_CODE bool CalRoutineFlow::Save(SavePort &port) {
  if (!CanSave()) return false;
  now_ = clock_->Ms();
  const bool ok = CalRoutineSave(results_, port);
  CALR_LOG(Save, ok);
  if (ok) SetStage(Stage::Idle);
  return ok;
}

CALR_CODE void CalRoutineFlow::Discard() {
  if (stage_ != Stage::Results && stage_ != Stage::Failed) return;
  now_ = clock_->Ms();
  out_->ReleaseAll();
  for (int o = 0; o < 8; ++o) results_.out_pass[o] = results_.in_pass[o] = false;
  Log("CALR,DISCARD");
  SetStage(Stage::Idle);
}

CALR_CODE bool CalRoutineSave(const CalRoutineFlow::Results &r, CalRoutine::SavePort &port) {
  if (!r.CanSave()) return false;
  uint8_t mask = 0;
  uint8_t source[8];
  for (int i = 0; i < 8; ++i) {
    if (r.in_pass[i]) mask = uint8_t(mask | (1u << i));
    source[i] = uint8_t(kSourceOutput + 1);
  }
  if (!port.SaveDacTable(r.table)) return false;
  if (!port.SaveCurves(r.curve_mv_x2, mask, source)) return false;
  if (!port.SaveLines(r.line, mask, source)) return false;
  port.WriteMarker();
  port.ResetAutotune();
  port.ReloadInputs();
  return true;
}

// =============================================================================
// Console Q

CALR_CODE void CalQFlow::Begin(OutputPort &out, InputPort &in, Clock &clock, int in_label,
                               int out_label, const int32_t mv[3], const uint16_t code[3],
                               uint16_t shared_scale, int32_t octave_mv) {
  out_ = &out;
  in_ = &in;
  clock_ = &clock;
  in_label_ = in_label;
  out_label_ = out_label;
  for (int i = 0; i < 3; ++i) {
    mv_[i] = mv[i];
    code_[i] = code[i];
    stats_[i].mean_raw = stats_[i].min_raw = stats_[i].max_raw = 0.f;
    stats_[i].samples = 0;
  }
  scale_ = shared_scale;
  octave_mv_ = octave_mv;
  verdict_ = CM::VERDICT_NO_SAMPLES;
  fit_.gain = fit_.offset_mv = 0.f;
  residual_mv_ = 0.f;
  k_ = 0;
  state_ = State::Measuring;
  point_.Start(out, in, clock, out_label, code_[0], in_label);
}

CALR_CODE CalQFlow::State CalQFlow::Tick() {
  if (state_ != State::Measuring) return state_;
  CM::PointStats s;
  if (!point_.Tick(&s)) return state_;
  stats_[k_] = s;
  char line[L::kMaxLine];
  L::IncalPoint(line, sizeof(line), in_label_, k_, mv_[k_], s);
  if (log_) log_(line);
  if (++k_ < 3) {
    point_.Start(*out_, *in_, *clock_, out_label_, code_[k_], in_label_);
    return state_;
  }
  out_->ReleaseAll();
  verdict_ = CM::CheckCalibration(stats_, mv_, scale_, octave_mv_, &fit_, &residual_mv_);
  float noise = 0.f;
  for (int i = 0; i < 3; ++i) {
    const float pp = stats_[i].max_raw - stats_[i].min_raw;
    if (pp > noise) noise = pp;
  }
  L::IncalResult(line, sizeof(line), in_label_, out_label_, fit_, residual_mv_, noise, verdict_);
  if (log_) log_(line);
  state_ = State::Done;
  return state_;
}

CALR_CODE void CalQFlow::Cancel() {
  if (state_ == State::Measuring && out_) out_->ReleaseAll();
  point_.Stop();
  state_ = State::Idle;
}

CALR_CODE bool CalRoutineDropCurve(InputCurveRecord &rec, int phys) {
  if (phys < 0 || phys >= kInputCurveInputs) return false;
  const uint8_t bit = uint8_t(1u << phys);
  if (!(rec.valid_mask & bit)) return false;
  rec.valid_mask = uint8_t(rec.valid_mask & ~bit);
  return true;
}
