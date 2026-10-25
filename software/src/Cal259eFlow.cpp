// The Cal Routine's 259e target: see Cal259eFlow.h.
//
// Firmware: all of it runs from flash (FLASHMEM), none of it is ISR code.
// Every function has external linkage so the section attribute is honoured.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define C259_CODE FLASHMEM
#else
#define C259_CODE
#endif

#include "Cal259eFlow.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "CvMeterMath.h"
#include "UsbTmc.h"

namespace CM = CvMeterMath;

// =============================================================================
// Line formats

namespace Cal259eLines {

static const char *const kHeaders[] = {
  "CALR#,version=2",
  "CALR#,259START,addr",
  "CALR#,259PT,ch,mv,meter_mv,raw_median,n",
  "CALR#,259FIT,ch,gain_hex,offset,worst_cents,nonlin_cents",
  "CALR#,259REC,addr,rec60hex",
  "CALR#,259END,result",
  "CALR#,259BK,addr,bytes,crc_hex,result",
  // The meter rule's own lines (MeterMeasure), as in a Self run.
  "CALR#,RETRY,scpi,attempt",
  "CALR#,REREAD,out,target_mv,code,spread_mv",
};

C259_CODE int HeaderCount() { return int(sizeof(kHeaders) / sizeof(kHeaders[0])); }
C259_CODE const char *Header(int i) { return i >= 0 && i < HeaderCount() ? kHeaders[i] : ""; }

// "," then a fixed-decimal value, appended at *pos.
C259_CODE void AppendFixed(char *buf, size_t n, size_t *pos, double v, int decimals) {
  char t[32];
  CM::FormatFixed(t, sizeof(t), CM::ScaleRound(v, decimals), decimals);
  CM::Append(buf, n, pos, ",");
  CM::Append(buf, n, pos, t);
}

C259_CODE int Start(char *buf, size_t n, uint8_t addr) {
  if (!n) return 0;
  char t[32];
  snprintf(t, sizeof(t), "CALR,259START,%02X", unsigned(addr));
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C259_CODE int Point(char *buf, size_t n, int ch, int32_t mv, double meter_mv, uint16_t raw_median,
                    int samples) {
  if (!n) return 0;
  char t[48];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,259PT,%d,%ld", ch, long(mv));
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, meter_mv, 3);
  snprintf(t, sizeof(t), ",%u,%d", unsigned(raw_median), samples);
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C259_CODE int Fit(char *buf, size_t n, int ch, const CalRec::Channel &c, double worst_cents,
                  double nonlin_cents) {
  if (!n) return 0;
  char t[48];
  size_t pos = 0;
  buf[0] = 0;
  snprintf(t, sizeof(t), "CALR,259FIT,%d,%04X,%d", ch, unsigned(c.gain), int(c.offset));
  CM::Append(buf, n, &pos, t);
  AppendFixed(buf, n, &pos, worst_cents, 2);
  AppendFixed(buf, n, &pos, nonlin_cents, 2);
  return int(pos);
}

C259_CODE int Rec(char *buf, size_t n, uint8_t addr, const uint8_t rec[CalRec::kRecLen259e]) {
  if (!n) return 0;
  char t[2 * CalRec::kRecLen259e + 24];
  int k = snprintf(t, sizeof(t), "CALR,259REC,%02X,", unsigned(addr));
  for (int i = 0; i < CalRec::kRecLen259e; ++i)
    k += snprintf(t + k, sizeof(t) - size_t(k), "%02X", unsigned(rec[i]));
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, t);
  return int(pos);
}

C259_CODE int End(char *buf, size_t n, const char *result) {
  if (!n) return 0;
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, "CALR,259END,");
  CM::Append(buf, n, &pos, result ? result : "");
  return int(pos);
}

C259_CODE int Backup(char *buf, size_t n, uint8_t addr, uint32_t bytes, uint32_t crc,
                     const char *result) {
  if (!n) return 0;
  char t[64];
  snprintf(t, sizeof(t), "CALR,259BK,%02X,%lu,%08lX,", unsigned(addr), (unsigned long)bytes,
           (unsigned long)crc);
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, t);
  CM::Append(buf, n, &pos, result ? result : "");
  return int(pos);
}

}  // namespace Cal259eLines

namespace L = Cal259eLines;

#define C259_LOG(fn, ...) do { \
    char line_[L::kMaxLine]; \
    L::fn(line_, sizeof(line_), __VA_ARGS__); \
    Log(line_); \
  } while (0)

// =============================================================================
// Constants and names

constexpr int Cal259eFlow::kSourceOutput, Cal259eFlow::kMaxCandidates, Cal259eFlow::kFitPoints,
    Cal259eFlow::kVerifyPoints, Cal259eFlow::kPoints, Cal259eFlow::kAdcSamples;
constexpr uint8_t Cal259eFlow::kFirstAddr, Cal259eFlow::kLastAddr;
constexpr int32_t Cal259eFlow::kIdLowMv, Cal259eFlow::kIdHighMv;
constexpr uint16_t Cal259eFlow::kNominalGain;
constexpr double Cal259eFlow::kPatchTolMv, Cal259eFlow::kVerifyCents,
    Cal259eFlow::kMaxNonlinearityCents;
constexpr uint32_t Cal259eFlow::kResultsMaxAgeMs;
constexpr bool Cal259eFlow::kAllowCompaction;
constexpr bool Cal259eFlow::kAllowWrite;
constexpr uint32_t Cal259eFlow::kAdcTimeoutMs, Cal259eFlow::kBackupTimeoutMs,
    Cal259eFlow::kBackupBankBytes;

const int32_t Cal259eFlow::kPointMv[Cal259eFlow::kPoints] = {
  1200, 2400, 3600, 4800, 6000, 7200, 8400, 9600,  // fit
  // Verify (not fit points). 600 mV lies below the lowest fit point (1200 mV): it
  // checks the line's extrapolation toward 0 V, where a real input stage is least
  // linear. 9000 mV is the top check: the 9600 mV fit point's target (32768)
  // is above the 259e's 0x7FFF clamp, so its own residual cannot show a positive
  // error, and 9000 mV (30720) is in range.
  600, 3000, 5400, 7800, 9000,
};

C259_CODE double Cal259eFlow::IdentityThreshold(double counts_per_oct) {
  return 0.5 * double(kIdHighMv - kIdLowMv) * counts_per_oct / 1200.0 / (double(kNominalGain) / 65536.0);
}

C259_CODE const char *Cal259eFlow::StageName(Stage s) {
  switch (s) {
    case Stage::Idle: return "IDLE";
    case Stage::NoMeter: return "NOMETER";
    case Stage::NoSelfCal: return "NO_SELF_CAL";
    case Stage::Scan: return "SCAN";
    case Stage::Warmup: return "WARMUP";
    case Stage::PromptPatch: return "PROMPT_PATCH";
    case Stage::Identify: return "IDENTIFY";
    case Stage::Measure: return "MEASURE";
    case Stage::Results: return "RESULTS";
    case Stage::Confirm: return "CONFIRM";
    case Stage::Backup: return "BACKUP";
    case Stage::ConfirmCompact: return "CONFIRM_COMPACT";
    case Stage::Write: return "WRITE";
    case Stage::Verify: return "VERIFY";
    case Stage::Done: return "DONE";
    case Stage::Failed: return "FAILED";
  }
  return "?";
}

C259_CODE const char *BackupWhyName(BackupWhy w) {
  switch (w) {
    case BK_NONE: return "FAILED";
    case BK_FW_CARD: return "FW_CARD";
    case BK_BUSY: return "BUSY";
    case BK_START: return "START";
    case BK_MASTER_FAILED: return "MASTER_FAILED";
    case BK_LOST: return "LOST";
    case BK_OTHER_JOB: return "OTHER_JOB";
    case BK_NO_IMAGE: return "NO_IMAGE";
    case BK_SIZE: return "SIZE";
    case BK_SAVE: return "SAVE";
    case BK_VERIFY: return "VERIFY";
    case BK_TIMEOUT: return "TIMEOUT";
  }
  return "FAILED";
}

C259_CODE const char *Cal259eFlow::FailName(FailCode f) {
  switch (f) {
    case F_NONE: return "NONE";
    case F_NO_METER: return "NO_METER";
    case F_NO_SELF_CAL: return "NO_SELF_CAL";
    case F_NO_259E: return "NO_259E";
    case F_SEVERAL_MOVED: return "SEVERAL_MOVED";
    case F_IDENTITY: return "IDENTITY";
    case F_PATCH: return "PATCH";
    case F_NOISY: return "NOISY";
    case F_SCALE: return "SCALE";
    case F_FIT: return "FIT";
    case F_VERIFY: return "VERIFY";
    case F_BACKUP: return "BACKUP";
    case F_BUS: return "BUS";
    case F_COMMIT_REFUSED: return "COMMIT_REFUSED";
    case F_COMMIT_UNKNOWN: return "COMMIT_UNKNOWN";
    case F_READBACK: return "READBACK";
    case F_CANCELLED: return "CANCELLED";
    case F_METER_LOST: return "METER_LOST";
    case F_METER_CONFIG: return "METER_CONFIG";
    case F_METER_TIMEOUT: return "METER_TIMEOUT";
    case F_NO_SLOT: return "NO_SLOT";
    case F_NONLIN: return "NONLIN";
    case F_EXPIRED: return "EXPIRED";
    case F_INTERNAL: return "INTERNAL";
  }
  return "?";
}

// The COMMIT's outcome after a cancel, for CALR,259END.
C259_CODE const char *Cal259eCancelledName(int rc) {
  switch (rc) {
    case 1: return "CANCELLED_OK";
    case E_NO_SLOT: return "CANCELLED_NO_SLOT";
    case E_COMMIT_REFUSED: return "CANCELLED_COMMIT_REFUSED";
    case E_COMMIT_UNKNOWN: return "CANCELLED_COMMIT_UNKNOWN";
    default: return "CANCELLED_BUS";
  }
}

C259_CODE uint8_t Cal259eFlow::candidates_mask() const {
  uint8_t m = 0;
  for (int i = 0; i < n_cand_; ++i) m = uint8_t(m | (1u << (cand_[i] - kFirstAddr)));
  return m;
}

// =============================================================================
// The flow

C259_CODE void Cal259eFlow::Log(const char *line) {
  if (log_) log_(line);
}

C259_CODE void Cal259eFlow::ReleaseHolds() {
  if (out_) out_->ReleaseAll();
}

// Points the bus client at addr. Begin also drops whatever operation it had in
// flight: an unfinished READ or PUT is harmless, a COMMIT not yet ACKed never
// goes out (the Commit-after-ACK case never comes here: see Cancel).
C259_CODE void Cal259eFlow::BusTarget(uint8_t addr) {
  bus_.Begin(*busport_, *clock_, addr);
}

C259_CODE void Cal259eFlow::Fail(FailCode f) {
  ReleaseHolds();
  meas_.Reset();
  query_.Reset();
  BusTarget(target_ ? target_ : kFirstAddr);
  sub_ = S_NONE;
  fail_ = f;
  C259_LOG(End, FailName(f));
  SetStage(Stage::Failed);
}

C259_CODE void Cal259eFlow::Finish() {
  ReleaseHolds();
  sub_ = S_NONE;
  fail_ = F_NONE;
  C259_LOG(End, "OK");
  SetStage(Stage::Done);
}

// The meter must stay connected until the measurements are done.
C259_CODE bool Cal259eFlow::MeterGone() {
  if (meter_->Connected()) return false;
  Fail(F_METER_LOST);
  return true;
}

C259_CODE bool Cal259eFlow::Begin(MeterPort &meter, OutputPort &out, Clock &clock, BusPort &bus,
                                  BackupPort &backup, EntropyPort &entropy, bool self_cal_present,
                                  LogFn log) {
  // A cancelled run still waiting for its COMMIT verdict: starting over would
  // drop that verdict (and re-point the bus client). Refuse; nothing changes.
  if (cancel_pending_) return false;
  out.ReleaseAll();  // a Begin mid-run never leaves the previous run's output held
  meter_ = &meter;
  out_ = &out;
  clock_ = &clock;
  busport_ = &bus;
  backup_ = &backup;
  entropy_ = &entropy;
  log_ = log;
  query_.Bind(meter, log);
  meas_.Bind(meter, out, log);
  query_.Reset();
  meas_.Reset();
  now_ = clock.Ms();
  target_ = 0;
  BusTarget(kFirstAddr);
  for (int k = 0; k < CalRoutineMath::kNodes; ++k) table_[k] = out.TableCode(kSourceOutput, k);
  sub_ = S_NONE;
  fail_ = F_NONE;
  n_cand_ = 0;
  ch_ = -1;
  pt_ = -1;
  id_moved_ = 0;
  compact_ = false;
  commit_sent_ = false;
  commit_rc_ = 0;
  cancel_pending_ = false;
  cancelled_verdict_ = 0;
  backup_why_ = BK_NONE;
  results_ms_ = 0;
  memset(rec_, 0, sizeof(rec_));
  memset(&result_, 0, sizeof(result_));
  memset(pts_, 0, sizeof(pts_));
  for (int i = 0; i < L::HeaderCount(); ++i) Log(L::Header(i));
  if (!self_cal_present) {  // the output table was not meter-set: refuse
    fail_ = F_NO_SELF_CAL;
    C259_LOG(End, FailName(fail_));
    SetStage(Stage::NoSelfCal);
    return true;
  }
  if (!meter.Connected()) {
    fail_ = F_NO_METER;  // waits for one
    SetStage(Stage::NoMeter);
    return true;
  }
  StartScan();
  return true;
}

C259_CODE void Cal259eFlow::Tick() {
  if (!meter_) return;
  now_ = clock_->Ms();
  switch (stage_) {
    case Stage::NoMeter:
      if (meter_->Connected()) {
        fail_ = F_NONE;
        StartScan();
      }
      break;
    case Stage::Scan:
      if (!MeterGone()) TickScan();
      break;
    case Stage::Warmup:
    case Stage::PromptPatch:
      // Channel 1's patch prompt may be left open for hours: the age runs from
      // channel 0's first point (zero until then).
      if (!MeterGone() && Expired()) Fail(F_EXPIRED);
      break;
    case Stage::Identify:
      if (!MeterGone()) {
        if (Expired()) Fail(F_EXPIRED);
        else TickIdentify();
      }
      break;
    case Stage::Measure:
      if (!MeterGone()) {
        if (Expired()) Fail(F_EXPIRED);
        else TickMeasure();
      }
      break;
    case Stage::Results:
    case Stage::Confirm:
      // The age limit guards the write (modules may have drifted or been re-patched
      // since they were measured). A read-only build never writes, so a Results
      // screen left open stays readable until it is discarded.
      if (kAllowWrite && Expired()) Fail(F_EXPIRED);
      break;
    case Stage::Backup:
      TickBackup();
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

C259_CODE void Cal259eFlow::Continue() {
  if (!meter_) return;
  now_ = clock_->Ms();
  switch (stage_) {
    case Stage::Warmup:
      ch_ = 0;
      SetStage(Stage::PromptPatch);
      break;
    case Stage::PromptPatch:
      if (!MeterGone()) StartIdentify();
      break;
    case Stage::Results:
      if (kAllowWrite) SetStage(Stage::Confirm);  // else: read-only, discard at Results
      break;
    default:
      break;
  }
}

C259_CODE bool Cal259eFlow::Expired() const {
  return results_ms_ && now_ - results_ms_ >= kResultsMaxAgeMs;
}

C259_CODE uint32_t Cal259eFlow::results_age_ms() const {
  return results_ms_ && clock_ ? clock_->Ms() - results_ms_ : 0;
}

C259_CODE void Cal259eFlow::ConfirmWrite() {
  if (!kAllowWrite || !meter_ || stage_ != Stage::Confirm) return;
  now_ = clock_->Ms();
  // Defence in depth: only a run whose two channels both passed, and are still
  // fresh, may write. (The stage machine already guarantees the first.)
  if (result_.valid_mask != 3) { Fail(F_INTERNAL); return; }
  if (now_ - results_ms_ >= kResultsMaxAgeMs) { Fail(F_EXPIRED); return; }
  compact_ = false;
  SetStage(Stage::Backup);
  backup_t0_ = now_;
  if (!backup_->Begin(target_)) FailBackup(backup_->Why());
}

C259_CODE void Cal259eFlow::ConfirmCompact() {
  if (!kAllowWrite || !kAllowCompaction || !meter_ || stage_ != Stage::ConfirmCompact) return;
  now_ = clock_->Ms();
  compact_ = true;  // a fresh backup first, then the record again and COMMIT flags 1
  SetStage(Stage::Backup);
  backup_t0_ = now_;
  if (!backup_->Begin(target_)) FailBackup(backup_->Why());
}

C259_CODE void Cal259eFlow::Cancel() {
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
  meas_.Reset();
  query_.Reset();
  BusTarget(target_ ? target_ : kFirstAddr);  // drops a READ, PUT or un-ACKed COMMIT
  sub_ = S_NONE;
  fail_ = F_CANCELLED;
  // After a COMMIT verdict (Verify, ConfirmCompact, the compaction backup or
  // PUT) the module's flash may hold the record: say which verdict it was.
  // record() and result() keep the record put.
  if (commit_rc_ != 0) {
    cancelled_verdict_ = commit_rc_;
    C259_LOG(End, Cal259eCancelledName(commit_rc_));
  } else C259_LOG(End, FailName(fail_));
  SetStage(Stage::Failed);
}

// ---- scan: the meter set up, then who answers READ status ----------------------

C259_CODE void Cal259eFlow::StartScan() {
  SetStage(Stage::Scan);
  sub_ = S_CONFIG;
  cfg_i_ = 0;
  query_.Start(CalRoutine::kMeterSetup[0], CalRoutine::kConfigTimeoutMs, now_);
}

C259_CODE void Cal259eFlow::TickScan() {
  if (sub_ == S_CONFIG) {
    const CalRoutine::MeterQuery::Result r = query_.Step(now_);
    if (r == CalRoutine::MeterQuery::BUSY) return;
    if (r == CalRoutine::MeterQuery::LOST) { Fail(F_METER_LOST); return; }
    if (r == CalRoutine::MeterQuery::FAIL) { Fail(F_METER_CONFIG); return; }
    if (query_.scpi() && strcmp(query_.scpi(), CalRoutine::kErrorQuery) == 0 &&
        !UsbTmc::NoScpiError(query_.reply())) {
      Fail(F_METER_CONFIG);
      return;
    }
    const int n_setup = int(sizeof(CalRoutine::kMeterSetup) / sizeof(CalRoutine::kMeterSetup[0]));
    if (++cfg_i_ < n_setup) {
      query_.Start(CalRoutine::kMeterSetup[cfg_i_], CalRoutine::kConfigTimeoutMs, now_);
      return;
    }
    sub_ = S_STATUS;
    scan_i_ = 0;
    BusTarget(kFirstAddr);
    bus_.ReadStatus();
    return;
  }
  const int rc = bus_.Tick();
  if (rc == 0) return;
  if (rc == 1) {
    if (n_cand_ < kMaxCandidates) cand_[n_cand_++] = uint8_t(kFirstAddr + scan_i_);
  } else if (rc != E_NO_REPLY && rc != E_BAD_REPLY) {
    Fail(F_BUS);  // the bus itself is unusable (busy, NAK): nothing can be learned
    return;
  }
  if (kFirstAddr + ++scan_i_ <= kLastAddr) {
    BusTarget(uint8_t(kFirstAddr + scan_i_));
    bus_.ReadStatus();
    return;
  }
  if (n_cand_ == 0) {
    Fail(F_NO_259E);
    return;
  }
  sub_ = S_NONE;
  SetStage(Stage::Warmup);
}

// ---- the 259e's ADC: the median of kAdcSamples fresh samples ------------------
//
// The first reply only sets the baseline counter; a reply counts when its n
// differs from the last counted (or baseline) n, so every sample in the median
// was taken after the first ask. The median of 8 is the rounded mean of the
// middle two.

C259_CODE void Cal259eFlow::AdcStart(uint8_t ch) {
  adc_ch_ = ch;
  adc_t0_ = now_;
  adc_base_ = false;
  adc_k_ = 0;
  bus_.ReadAdc(ch);
}

C259_CODE int Cal259eFlow::AdcStep() {
  if (now_ - adc_t0_ >= kAdcTimeoutMs) return -1;
  const int rc = bus_.Tick();
  if (rc == 0) return 0;
  if (rc < 0) return -1;
  const uint8_t n = bus_.adc_n();
  if (!adc_base_) {
    adc_base_ = true;
    adc_last_n_ = n;
  } else if (n != adc_last_n_) {
    adc_last_n_ = n;
    adc_s_[adc_k_++] = bus_.adc();
  }
  if (adc_k_ >= kAdcSamples) {
    uint16_t s[kAdcSamples];
    memcpy(s, adc_s_, sizeof(s));
    for (int i = 1; i < kAdcSamples; ++i)  // insertion sort, 8 values
      for (int j = i; j > 0 && s[j - 1] > s[j]; --j) {
        const uint16_t t = s[j];
        s[j] = s[j - 1];
        s[j - 1] = t;
      }
    adc_median_ = uint16_t((uint32_t(s[kAdcSamples / 2 - 1]) + s[kAdcSamples / 2] + 1) / 2);
    return 1;
  }
  return bus_.ReadAdc(adc_ch_) ? 0 : -1;
}

// ---- identify: output A low then high, exactly one candidate moves ------------

C259_CODE void Cal259eFlow::StartIdentify() {
  SetStage(Stage::Identify);
  id_level_ = 0;
  id_moved_ = 0;
  out_->Hold(kSourceOutput, out_->CodeForMv(kSourceOutput, table_, kIdLowMv));
  t0_ = now_;
  sub_ = S_ID_SETTLE;
}

C259_CODE void Cal259eFlow::TickIdentify() {
  if (sub_ == S_ID_SETTLE) {
    if (now_ - t0_ < CalRoutine::kOutSettleMs) return;
    id_i_ = 0;
    BusTarget(cand_[0]);
    AdcStart(uint8_t(ch_));
    sub_ = S_ID_ADC;
    return;
  }
  const int r = AdcStep();
  if (r == 0) return;
  if (r < 0) { Fail(F_BUS); return; }
  if (id_level_ == 0) {
    id_low_[id_i_] = adc_median_;
  } else if (double(adc_median_) - double(id_low_[id_i_]) >= IdentityThreshold(fit_t())) {
    id_moved_ = uint8_t(id_moved_ | (1u << id_i_));
  }
  if (++id_i_ < n_cand_) {
    BusTarget(cand_[id_i_]);
    AdcStart(uint8_t(ch_));
    return;
  }
  if (id_level_ == 0) {
    id_level_ = 1;
    out_->Hold(kSourceOutput, out_->CodeForMv(kSourceOutput, table_, kIdHighMv));
    t0_ = now_;
    sub_ = S_ID_SETTLE;
    return;
  }
  int moved = 0, which = -1;
  for (int i = 0; i < n_cand_; ++i)
    if (id_moved_ & (1u << i)) { ++moved; which = i; }
  if (moved > 1) { Fail(F_SEVERAL_MOVED); return; }
  if (moved == 0) { Fail(F_IDENTITY); return; }
  if (ch_ == 0) {
    target_ = cand_[which];
    C259_LOG(Start, target_);
  } else if (cand_[which] != target_) {
    Fail(F_IDENTITY);  // another module moved: the patch is not on the target
    return;
  }
  BusTarget(target_);
  pt_ = 0;
  StartPoint();
}

// ---- measure: meter, then ADC, at every point --------------------------------

C259_CODE void Cal259eFlow::StartPoint() {
  SetStage(Stage::Measure);
  const int32_t mv = kPointMv[pt_];
  // The measurements age from the very first one: the oldest point is what the
  // limit protects (the 259e / patch / meter drift), whatever the stage.
  if (ch_ == 0 && pt_ == 0) results_ms_ = now_ ? now_ : 1;
  meas_.Start(kSourceOutput, out_->CodeForMv(kSourceOutput, table_, mv), mv, now_);
  sub_ = S_METER;
}

C259_CODE void Cal259eFlow::TickMeasure() {
  const int32_t mv = kPointMv[pt_];
  if (sub_ == S_METER) {
    const CalRoutine::MeterMeasure::Result r = meas_.Step(now_);
    if (r == CalRoutine::MeterMeasure::BUSY) return;
    if (r == CalRoutine::MeterMeasure::NOISY) { Fail(F_NOISY); return; }
    if (r == CalRoutine::MeterMeasure::LOST) { Fail(F_METER_LOST); return; }
    if (r != CalRoutine::MeterMeasure::DONE) { Fail(F_METER_TIMEOUT); return; }
    meter_mv_ = meas_.reading().median_mv;
    if (fabs(meter_mv_ - double(mv)) > kPatchTolMv) { Fail(F_PATCH); return; }
    AdcStart(uint8_t(ch_));
    sub_ = S_ADC;
    return;
  }
  const int r = AdcStep();
  if (r == 0) return;
  if (r < 0) { Fail(F_BUS); return; }
  const CalRec::FitPoint p = {adc_median_, meter_mv_};
  pts_[ch_][pt_] = p;
  C259_LOG(Point, ch_, mv, meter_mv_, adc_median_, kAdcSamples);
  if (++pt_ < kPoints) {
    StartPoint();
    return;
  }
  FinishChannel();
}

C259_CODE void Cal259eFlow::FinishChannel() {
  ReleaseHolds();
  sub_ = S_NONE;
  CalRec::Channel c;
  const CalRec::FitPoint *fit_pts = pts_[ch_], *ver_pts = pts_[ch_] + kFitPoints;
  if (CalRec::Fit259e(fit_pts, kFitPoints, fit_t(), &c) != CalRec::FIT_OK) {
    Fail(F_FIT);
    return;
  }
  double nonlin = 0.0, worst = 0.0;
  for (int i = 0; i < kFitPoints; ++i) {
    // Unclamped: the 9600 mV target is above the 259e's 0x7FFF clamp.
    const double r = CalRec::ResidualCentsUnclamped(fit_pts[i].raw, fit_pts[i].mv, c, fit_t());
    if (fabs(r) > nonlin) nonlin = fabs(r);
  }
  for (int i = 0; i < kVerifyPoints; ++i) {
    const double r = CalRec::ResidualCents(ver_pts[i].raw, ver_pts[i].mv, c, fit_t());
    if (fabs(r) > fabs(worst)) worst = r;
  }
  result_.ch[ch_] = c;
  result_.worst_verify_cents[ch_] = float(worst);
  result_.nonlin_cents[ch_] = float(nonlin);
  // Logged from result(), so the line and what the app shows are the same numbers.
  C259_LOG(Fit, ch_, c, double(result_.worst_verify_cents[ch_]), double(result_.nonlin_cents[ch_]));
  // Sanity (replaces the old +-10 % window around 0x9041, which could not fail on a wrong
  // T: the fit's gain is ~8.3333 * T whatever the meter reads). The second channel is
  // also checked against the first.
  sanity_ = (ch_ == 1 && (result_.valid_mask & 1u))
                ? CalRec::Sanity259e(result_.ch[0], c, sanity_t())
                : CalRec::Sanity259e(c, sanity_t());
  if (sanity_ != CalRec::SANITY_OK) {
    Fail(F_SCALE);
    return;
  }
  if (fabs(worst) > kVerifyCents) {
    Fail(F_VERIFY);
    return;
  }
  if (double(result_.nonlin_cents[ch_]) > kMaxNonlinearityCents) {
    Fail(F_NONLIN);
    return;
  }
  result_.valid_mask = uint8_t(result_.valid_mask | (1u << ch_));
  pt_ = -1;
  if (ch_ == 0) {
    ch_ = 1;
    SetStage(Stage::PromptPatch);
  } else {
    SetStage(Stage::Results);
  }
}

// ---- backup, write, verify ----------------------------------------------------

// The backup attempt's one CALR line (bytes as transferred, the stored bank's CRC).
C259_CODE void Cal259eFlow::BackupDone(bool ok) {
  C259_LOG(Backup, target_, backup_->Bytes(), backup_->Crc(),
           ok ? "OK" : BackupWhyName(backup_why_));
}

C259_CODE void Cal259eFlow::FailBackup(BackupWhy why) {
  backup_why_ = why;
  BackupDone(false);
  Fail(F_BACKUP);  // nothing was sent to the module
}

C259_CODE void Cal259eFlow::TickBackup() {
  const int rc = backup_->Poll();
  if (rc == 0) {
    if (now_ - backup_t0_ >= kBackupTimeoutMs) FailBackup(BK_TIMEOUT);  // abandoned, nothing sent
    return;
  }
  if (rc < 0) { FailBackup(backup_->Why()); return; }
  // The flow's own check, whatever the port is: a backup of anything but a full
  // 990-byte bank is not a net, and nothing is written without one.
  if (backup_->Bytes() != kBackupBankBytes) { FailBackup(BK_SIZE); return; }
  backup_why_ = BK_NONE;
  BackupDone(true);
  if (compact_) StartPut();
  else StartWrite();
}

C259_CODE void Cal259eFlow::StartWrite() {
  SetStage(Stage::Write);
  BusTarget(target_);
  sub_ = S_READREC;
  bus_.ReadRecord();
}

// The record again before every COMMIT (also before the compaction one: the
// staging buffer is the module's RAM, not to be relied on across the refusal).
C259_CODE void Cal259eFlow::StartPut() {
  SetStage(Stage::Write);
  BusTarget(target_);
  sub_ = S_PUT;
  bus_.PutRecord(rec_);
}

C259_CODE void Cal259eFlow::TickWrite() {
  const int rc = bus_.Tick();
  if (sub_ == S_COMMIT && bus_.sends() > 0) commit_sent_ = true;
  if (rc == 0) return;
  if (sub_ == S_READREC) {
    if (rc < 0) { Fail(F_BUS); return; }
    CalRec::Record r, old;
    if (bus_.has_record() && CalRec::Decode259e(bus_.record(), CalRec::kRecLen259e, &old)) {
      memcpy(r.unit_id, old.unit_id, sizeof(r.unit_id));  // the unit keeps its ID
      r.seq = uint16_t(old.seq + 1);
      if (r.seq == 0) r.seq = 1;
    } else {
      entropy_->Fill(r.unit_id, int(sizeof(r.unit_id)));
      r.seq = 1;
    }
    r.date = entropy_->UnixTime();
    r.ch[0] = result_.ch[0];
    r.ch[1] = result_.ch[1];
    CalRec::Encode259e(r, rec_);
    crc_ = uint16_t((rec_[CalRec::kRecLen259e - 2] << 8) | rec_[CalRec::kRecLen259e - 1]);
    C259_LOG(Rec, target_, rec_);
    StartPut();
    return;
  }
  if (sub_ == S_PUT) {
    if (rc < 0) { Fail(F_BUS); return; }
    sub_ = S_COMMIT;
    commit_sent_ = false;
    if (!bus_.Commit(crc_, compact_ ? 1 : 0)) Fail(F_BUS);
    return;
  }
  // S_COMMIT: its one verdict. Never sent again, except E_NO_SLOT -> compaction.
  commit_rc_ = rc;
  sub_ = S_NONE;
  if (cancel_pending_) {
    cancel_pending_ = false;
    ReleaseHolds();
    fail_ = F_CANCELLED;
    cancelled_verdict_ = rc;
    C259_LOG(End, Cal259eCancelledName(rc));
    SetStage(Stage::Failed);
    return;
  }
  if (rc == 1) {
    SetStage(Stage::Verify);
    bus_.ReadRecord();
    return;
  }
  if (rc == E_NO_SLOT && !compact_) {
    if (kAllowCompaction) SetStage(Stage::ConfirmCompact);
    else Fail(F_NO_SLOT);  // "all 8 calibration slots are used; compaction is disabled"
    return;
  }
  if (rc == E_NO_SLOT || rc == E_COMMIT_REFUSED) { Fail(F_COMMIT_REFUSED); return; }
  // Only these two mean the frame never went on the wire (the tx gate stayed
  // closed, or the allowlist refused it). Every other COMMIT outcome, a Wire
  // error included (a NAK on the last byte may follow a latched frame), is
  // unknown, not "nothing went out".
  if (rc == E_BUS_BUSY || rc == E_INTERNAL) { Fail(F_BUS); return; }
  Fail(F_COMMIT_UNKNOWN);
}

C259_CODE void Cal259eFlow::TickVerify() {
  const int rc = bus_.Tick();
  if (rc == 0) return;
  if (rc == 1 && bus_.has_record() && memcmp(bus_.record(), rec_, CalRec::kRecLen259e) == 0) {
    Finish();
    return;
  }
  Fail(F_READBACK);
}
