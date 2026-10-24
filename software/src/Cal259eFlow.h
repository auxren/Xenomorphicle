#ifndef CAL259EFLOW_H_
#define CAL259EFLOW_H_

#include <stddef.h>
#include <stdint.h>

#include "Cal259eBus.h"
#include "CalRec.h"
#include "CalRoutineMeasure.h"
#include "CalRoutineTypes.h"

// ---------------------------------------------------------------------------
// The Cal Routine's "259e" target: calibrates a Buchla 259e's two pitch CV
// inputs (channel 0 = principal pitch, 1 = modulation pitch) against the
// Xenomorpher's own meter-calibrated output A and the USB DMM, then writes the
// record over the 200e bus. Buchla_FW docs/superpowers/specs/
// 2026-10-24-cv-calibration-xeno-coordinator.md A2 (the run) and
// docs/cv-calibration/CONTRACT.md (record, frames, 259e behaviour).
//
// The run:
//   Begin       Self cal absent: NoSelfCal (refused, nothing happens). No meter:
//               NoMeter until one is connected.
//   Scan        sets the meter up (kMeterSetup, as Self does), then READ status
//               at 0x28..0x2B; the ones that answer are the candidates.
//   Warmup      both modules on >= 30 min: owner-confirmed (Continue).
//   per channel 0 then 1:
//   PromptPatch Xeno output A -> that 259e input, meter on the same point with
//               COM at the 259e (Continue).
//   Identify    hold kIdLowMv then kIdHighMv; every candidate's ADC word for the
//               channel (kAdcSamples fresh samples, median) must rise by
//               >= half the nominal step on exactly one candidate.
//   Measure     the 8 fit points then the 4 verify points: hold the stored
//               table's code (OutputPort::CodeForMv), MeterMeasure (the Self
//               rule, kReadings), the meter median within kPatchTolMv of the
//               requested mV, then the ADC median. Fit (CalRec::Fit259e on the
//               meter mV), gain within +-10 % of 0x9041, verify residuals
//               within +-3 cents. Output A is released after each channel.
//   Results     Continue -> Confirm; ConfirmWrite -> Backup.
//   Backup      a fresh preset backup of the target (BackupPort); fails: nothing
//               is written.
//   Write       ReadRecord (unit ID kept, seq + 1; else EntropyPort's 8 bytes,
//               seq 1), PutRecord, Commit(crc, 0). E_NO_SLOT: ConfirmCompact;
//               ConfirmCompact() -> a second Backup -> PutRecord -> Commit(crc, 1).
//   Verify      ReadRecord, byte-exact with what was put: Done.
//
// COMMIT is sent at most once per run, except the one compaction COMMIT after
// E_NO_SLOT and the owner's second confirm. Cancel releases every hold and
// sends nothing more; a Cancel after the COMMIT frame went out waits for its
// status, logs it (CALR,259END,CANCELLED_<result>) and ends Failed/F_CANCELLED.
//
// Only Xeno output A (kSourceOutput) is ever held. The meter must stay
// connected until Results: a meter gone fails the run (F_METER_LOST).
//
// Pure and non-blocking: every port is an interface, Tick() samples the clock
// once and does bounded work. Host-tested in test/test_cal259e_flow.cpp.
// ---------------------------------------------------------------------------

// A preset backup of one 259e (the master BACKUP, console 'm'), stored by the
// glue. Begin false: could not start (the flow fails F_BACKUP).
struct BackupPort {
  virtual bool Begin(uint8_t addr) = 0;
  virtual int Poll() = 0; /* 0 busy, 1 saved, <0 failed */
};

// The Teensy TRNG and the RTC (UnixTime 0 when the RTC was never set).
struct EntropyPort {
  virtual void Fill(uint8_t *d, int n) = 0;
  virtual uint32_t UnixTime() = 0;
};

class Cal259eFlow {
 public:
  using MeterPort = CalRoutine::MeterPort;
  using OutputPort = CalRoutine::OutputPort;
  using Clock = CalRoutine::Clock;
  using LogFn = CalRoutine::LogFn;

  enum class Stage {
    Idle, NoMeter, NoSelfCal, Scan, Warmup, PromptPatch, Identify, Measure, Results, Confirm,
    Backup, ConfirmCompact, Write, Verify, Done, Failed
  };

  // Appended only (the app and the log name them by FailName).
  enum FailCode {
    F_NONE, F_NO_METER, F_NO_SELF_CAL, F_NO_259E, F_SEVERAL_MOVED, F_IDENTITY, F_PATCH, F_NOISY,
    F_SCALE, F_FIT, F_VERIFY, F_BACKUP, F_BUS, F_COMMIT_REFUSED, F_COMMIT_UNKNOWN, F_READBACK,
    F_CANCELLED, F_METER_LOST, F_METER_CONFIG, F_METER_TIMEOUT
  };

  struct Result {
    CalRec::Channel ch[2];
    float worst_verify_cents[2];  // the verify residual of largest magnitude (signed)
    float nonlin_cents[2];        // worst |fit-point residual|
    uint8_t valid_mask;           // bit c: channel c fitted and verified
  };

  // ---- the run's constants ----
  static constexpr int kSourceOutput = 0;  // Xeno output A (panel label)
  static constexpr uint8_t kFirstAddr = 0x28, kLastAddr = 0x2B;
  static constexpr int kMaxCandidates = 4;
  static constexpr int kFitPoints = 8, kVerifyPoints = 4, kPoints = kFitPoints + kVerifyPoints;
  static const int32_t kPointMv[kPoints];  // 1200 .. 9600, then 600, 3000, 5400, 7800
  static constexpr int32_t kIdLowMv = 1200, kIdHighMv = 6000;
  static constexpr uint16_t kNominalGain = 0x9041;
  // The identity threshold: half of (6000 - 1200) * 4096 / 1200 / (0x9041 / 65536)
  // raw counts (the stock correction's step, ~29076 counts).
  static double IdentityThreshold();
  static constexpr double kGainTolerance = 0.10;  // fitted gain vs 0x9041
  static constexpr double kPatchTolMv = 50.0;     // meter median vs requested mV
  static constexpr double kVerifyCents = 3.0;
  static constexpr int kAdcSamples = 8;           // fresh samples per ADC median
  static constexpr uint32_t kAdcTimeoutMs = 2000; // ... gathered within this, else F_BUS

  // Starts a run (any earlier one is dropped and output A released). The CALR#
  // headers are logged first.
  void Begin(MeterPort &meter, OutputPort &out, Clock &clock, BusPort &bus, BackupPort &backup,
             EntropyPort &entropy, bool self_cal_present, LogFn log);
  void Tick();
  // Warmup: confirmed. PromptPatch: patched. Results: on to Confirm.
  void Continue();
  // Any stage: see the header comment. Done / Failed / NoSelfCal: back to Idle.
  void Cancel();
  // Confirm only: back up, then write.
  void ConfirmWrite();
  // ConfirmCompact only: second backup, then COMMIT with flags 1.
  void ConfirmCompact();

  Stage stage() const { return stage_; }
  uint8_t target() const { return target_; }    // 0 until identified
  int channel() const { return ch_; }           // -1 when none
  const Result &result() const { return result_; }
  FailCode fail() const { return fail_; }
  // Progress for the screen: the Measure point (0..kPoints-1) and its mV.
  int point_index() const { return pt_; }
  int32_t point_mv() const { return pt_ >= 0 && pt_ < kPoints ? kPointMv[pt_] : 0; }
  uint8_t candidates_mask() const;              // bit i: 0x28 + i answered the scan
  // The COMMIT's Cal259eBus::Tick result (1 ok, < 0 Error), 0 while none came.
  int commit_result() const { return commit_rc_; }
  // A Cancel arrived after the COMMIT frame went out: waiting for its status.
  bool cancel_pending() const { return cancel_pending_; }
  const uint8_t *record() const { return rec_; }  // the record put (valid from Write on)

  static const char *StageName(Stage s);
  static const char *FailName(FailCode f);

 private:
  enum Sub : uint8_t {
    S_NONE,
    // Scan
    S_CONFIG, S_STATUS,
    // Identify
    S_ID_SETTLE, S_ID_ADC,
    // Measure
    S_METER, S_ADC,
    // Write
    S_READREC, S_PUT, S_COMMIT,
  };

  void Log(const char *line);
  void SetStage(Stage s) { stage_ = s; }
  void Fail(FailCode f);
  void Finish();
  void ReleaseHolds();
  bool MeterGone();
  void BusTarget(uint8_t addr);

  void StartScan();
  void TickScan();
  void StartIdentify();
  void TickIdentify();
  void StartPoint();
  void TickMeasure();
  void FinishChannel();
  void TickBackup();
  void StartWrite();
  void TickWrite();
  void StartPut();
  void TickVerify();

  // The fresh-sample ADC median on the current bus target.
  void AdcStart(uint8_t ch);
  int AdcStep();  // 0 busy, 1 done (adc_median_), < 0 failed (bus error or timeout)

  MeterPort *meter_ = nullptr;
  OutputPort *out_ = nullptr;
  Clock *clock_ = nullptr;
  BusPort *busport_ = nullptr;
  BackupPort *backup_ = nullptr;
  EntropyPort *entropy_ = nullptr;
  LogFn log_ = nullptr;

  CalRoutine::MeterQuery query_;
  CalRoutine::MeterMeasure meas_;
  Cal259eBus bus_;

  Stage stage_ = Stage::Idle;
  Sub sub_ = S_NONE;
  FailCode fail_ = F_NONE;
  uint32_t now_ = 0;
  uint32_t t0_ = 0;

  uint16_t table_[CalRoutineMath::kNodes] = {};  // output A's stored table, read at Begin

  // scan
  int cfg_i_ = 0;
  int scan_i_ = 0;
  uint8_t cand_[kMaxCandidates] = {};
  int n_cand_ = 0;
  uint8_t target_ = 0;

  // identify
  int ch_ = -1;
  int id_level_ = 0;   // 0 low, 1 high
  int id_i_ = 0;       // candidate being sampled
  uint16_t id_low_[kMaxCandidates] = {};
  uint8_t id_moved_ = 0;  // bit i: candidate i rose by the identity threshold

  // measure
  int pt_ = -1;
  CalRec::FitPoint fit_pts_[kFitPoints] = {};
  CalRec::FitPoint ver_pts_[kVerifyPoints] = {};
  double meter_mv_ = 0.0;

  // ADC sampling
  uint8_t adc_ch_ = 0;
  uint32_t adc_t0_ = 0;
  bool adc_base_ = false;
  uint8_t adc_last_n_ = 0;
  int adc_k_ = 0;
  uint16_t adc_s_[kAdcSamples] = {};
  uint16_t adc_median_ = 0;

  // write
  bool compact_ = false;     // the compaction path (second backup, flags 1)
  bool commit_sent_ = false; // a COMMIT frame was ACKed in this run
  int commit_rc_ = 0;
  bool cancel_pending_ = false;
  uint8_t rec_[CalRec::kRecLen259e] = {};
  uint16_t crc_ = 0;

  Result result_ = {};
};

// ---- line formats --------------------------------------------------------------
//
// CALR v2 lines of the 259e run (golden examples: test/fixtures/cal259e_lines.txt).
// Each writes one line without the line ending into buf (truncated to n) and
// returns its length. The flow logs the CALR# headers at Begin(); they include
// the meter rule's RETRY and REREAD, which MeterMeasure logs during the run.
namespace Cal259eLines {

constexpr size_t kMaxLine = 160;

int HeaderCount();
const char *Header(int i);  // 0 is "CALR#,version=2"

// addr: two upper-case hex digits ("28").
int Start(char *buf, size_t n, uint8_t addr);
int Point(char *buf, size_t n, int ch, int32_t mv, double meter_mv, uint16_t raw_median,
          int samples);
int Fit(char *buf, size_t n, int ch, const CalRec::Channel &c, double worst_cents,
        double nonlin_cents);
int Rec(char *buf, size_t n, uint8_t addr, const uint8_t rec[CalRec::kRecLen259e]);
// result: "OK", a FailName, or "CANCELLED_<commit result>".
int End(char *buf, size_t n, const char *result);

}  // namespace Cal259eLines

#endif  // CAL259EFLOW_H_
