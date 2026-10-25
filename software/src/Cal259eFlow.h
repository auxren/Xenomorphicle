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
//   Measure     the kFitPoints (8) fit points then the kVerifyPoints (5) verify points: hold the stored
//               table's code (OutputPort::CodeForMv), MeterMeasure (the Self
//               rule, kReadings), the meter median within kPatchTolMv of the
//               requested mV, then the ADC median. Fit (CalRec::Fit259e on the
//               meter mV), gain within +-10 % of 0x9041, verify residuals
//               within +-3 cents. Output A is released after each channel.
//   Results     Continue -> Confirm; ConfirmWrite -> Backup (both only with
//               kAllowWrite, which is false in the firmware: Results is then the
//               end of a read-only run, discarded with Cancel). The results expire
//               kResultsMaxAgeMs after channel 0's FIRST measured point (the
//               oldest data), and so does a wait at channel 1's patch prompt
//               (any stage from then to Confirm ends Failed/F_EXPIRED: measure
//               again from the start). A channel passes
//               only if its worst verify residual is within kVerifyCents AND its
//               fit-point nonlinearity within kMaxNonlinearityCents.
//   Backup      a fresh preset backup of the target (BackupPort); fails: nothing
//               is written.
//   Write       ReadRecord (unit ID kept, seq + 1; else EntropyPort's 8 bytes,
//               seq 1), PutRecord, Commit(crc, 0). E_NO_SLOT: Failed/F_NO_SLOT
//               ("all 8 calibration slots are used; compaction is disabled").
//               With kAllowCompaction (false, see below): ConfirmCompact, then
//               ConfirmCompact() -> a second Backup -> PutRecord -> Commit(crc, 1).
//   Verify      ReadRecord, byte-exact with what was put: Done.
//
// COMMIT is sent at most once per run (the compaction COMMIT after E_NO_SLOT and
// the owner's second confirm exists only when kAllowCompaction). Cancel releases every hold and
// sends nothing more; a Cancel after the COMMIT frame went out waits for its
// status, logs it (CALR,259END,CANCELLED_<result>) and ends Failed/F_CANCELLED.
//
// Only Xeno output A (kSourceOutput) is ever held (there is no output B in this
// flow: the operator moves the one cable between the principal and the mod
// pitch input when prompted). The meter must stay
// connected until Results: a meter gone fails the run (F_METER_LOST).
//
// Pure and non-blocking: every port is an interface, Tick() samples the clock
// once and does bounded work. Host-tested in test/test_cal259e_flow.cpp.
// ---------------------------------------------------------------------------

// A preset backup of one 259e (the master BACKUP, console 'm'), stored by the
// glue. Begin false: could not start (the flow fails F_BACKUP). The port must
// tolerate being abandoned mid-dump: a Cancel, or the flow's give-up after
// kBackupTimeoutMs, just stops polling it (there is no abort call).
//
// Why a backup did not happen (BackupPort::Why, the Failed screen and the
// CALR,259BK line). BK_NONE: the port gave no reason.
enum BackupWhy : uint8_t {
  BK_NONE, BK_FW_CARD, BK_BUSY, BK_START, BK_MASTER_FAILED, BK_LOST, BK_OTHER_JOB,
  BK_NO_IMAGE, BK_SIZE, BK_SAVE, BK_VERIFY, BK_TIMEOUT
};
const char *BackupWhyName(BackupWhy w);  // "FAILED" for BK_NONE

struct BackupPort {
  virtual bool Begin(uint8_t addr) = 0;
  virtual int Poll() = 0; /* 0 busy, 1 saved, <0 failed */
  // After Begin false / Poll != 0: the reason, the bytes the transfer moved (a
  // good backup is exactly kBackupBankBytes) and the CRC-32 of the stored bank.
  virtual BackupWhy Why() const { return BK_NONE; }
  virtual uint32_t Bytes() const { return 0; }
  virtual uint32_t Crc() const { return 0; }
};

// The 8 unit-ID bytes (OCOTP serial, cycle counter, micros(), RTC and a call
// counter mixed; not the Teensy TRNG, whose registers are never brought up)
// and the RTC (UnixTime 0 when the RTC was never set).
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
    F_CANCELLED, F_METER_LOST, F_METER_CONFIG, F_METER_TIMEOUT,
    F_NO_SLOT,    // COMMIT refused with bits 3+4: all 8 slots used, compaction disabled
    F_NONLIN,     // a channel's fit-point nonlinearity is over kMaxNonlinearityCents
    F_EXPIRED,    // Results / Confirm older than kResultsMaxAgeMs: measure again
    F_INTERNAL    // a state the flow should never reach (nothing was sent)
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
  static constexpr int kFitPoints = 8, kVerifyPoints = 5, kPoints = kFitPoints + kVerifyPoints;
  static const int32_t kPointMv[kPoints];  // 1200 .. 9600, then 600, 3000, 5400, 7800, 9000
  static constexpr int32_t kIdLowMv = 1200, kIdHighMv = 6000;
  static constexpr uint16_t kNominalGain = 0x9041;
  // The identity threshold: half of (6000 - 1200) * 4096 / 1200 / (0x9041 / 65536)
  // raw counts (the stock correction's step, ~29076 counts).
  static double IdentityThreshold();
  static constexpr double kGainTolerance = 0.10;  // fitted gain vs 0x9041
  static constexpr double kPatchTolMv = 50.0;     // meter median vs requested mV
  static constexpr double kVerifyCents = 3.0;
  // PROVISIONAL, for the operator to confirm from real bench data: the largest fit-point
  // residual (UNCLAMPED, see CalRec::ResidualCentsUnclamped) a channel may show
  // after the straight-line fit. Not derived from measurements of a real 259e.
  static constexpr double kMaxNonlinearityCents = 5.0;
  // Results older than this are not written: the owner measured them with the
  // modules in some other state. Measure again.
  static constexpr uint32_t kResultsMaxAgeMs = 10u * 60u * 1000u;
  // THE WRITE IS DISABLED in the firmware. The 259e row does everything up to Results
  // (warm-up, scan, identify, measure, fit) read-only; Continue() at Results does not
  // reach Confirm, ConfirmWrite() does nothing, and no backup, PUT or COMMIT is reachable.
  // Why: (1) no real write to a 259e has been proven on hardware yet (and one is not
  // undoable: each 259e takes only 8 commits for its whole life); (2) the target scale
  // (4096 counts per octave) is not proven by the bench octave-ratio check (gate X3);
  // (3) the read-back of a stored record was wrong on the real module (31.2C4 corrupts
  // two bytes of every 8-byte reply: see Cal259eBus.cpp kRecordArgs). Re-enable only by
  // building with -DCAL259E_ALLOW_WRITE=1 after those are closed. The write code and its
  // tests are kept alive: the host suite and the simulator run with the constant on.
  // The macros count only when non-zero: -DCAL259E_ALLOW_WRITE=0 stays read-only.
#if defined(CAL259E_ALLOW_COMPACTION) && CAL259E_ALLOW_COMPACTION
#undef CAL259E_ALLOW_WRITE
#define CAL259E_ALLOW_WRITE 1   // compaction is a kind of write
#endif
#if defined(CAL259E_ALLOW_WRITE) && CAL259E_ALLOW_WRITE
  static constexpr bool kAllowWrite = true;
#else
  static constexpr bool kAllowWrite = false;
#endif
  // Compaction (ConfirmCompact: reset the 8 slots to the new record alone, COMMIT
  // flags 1) stays OFF until a tested restore path for CAL259BK.BIN exists: a
  // power cut during the 259e's rewrite re-inits its presets, and a backup that
  // cannot be restored is no net. The code is kept, tested with
  // -DCAL259E_ALLOW_COMPACTION=1, and unreachable in the firmware.
#if defined(CAL259E_ALLOW_COMPACTION) && CAL259E_ALLOW_COMPACTION
  static constexpr bool kAllowCompaction = true;
#else
  static constexpr bool kAllowCompaction = false;
#endif
  static constexpr int kAdcSamples = 8;           // fresh samples per ADC median
  static constexpr uint32_t kAdcTimeoutMs = 2000; // ... gathered within this, else F_BUS
  static constexpr uint32_t kBackupBankBytes = 990;  // a 259e's 30 x 33-byte preset bank
  static constexpr uint32_t kBackupTimeoutMs = 60000;  // backup still busy: F_BACKUP, nothing sent

  // Starts a run (any earlier one is dropped and output A released). The CALR#
  // headers are logged first.
  // Returns false, changing nothing, while a cancelled run's COMMIT verdict is
  // still pending (cancel_pending()): that verdict is never silently dropped.
  bool Begin(MeterPort &meter, OutputPort &out, Clock &clock, BusPort &bus, BackupPort &backup,
             EntropyPort &entropy, bool self_cal_present, LogFn log);
  void Tick();
  // Warmup: confirmed. PromptPatch: patched. Results: on to Confirm.
  void Continue();
  // Any stage: see the header comment. Done / Failed / NoSelfCal: back to Idle.
  // Once a COMMIT verdict came (commit_result() != 0: Verify, ConfirmCompact,
  // the compaction backup and PUT), the end line names it: CANCELLED_<result>.
  void Cancel();
  // Confirm only: back up, then write. Re-checks that both channels passed
  // (valid_mask == 3) and that the results are not expired.
  void ConfirmWrite();
  // ConfirmCompact only: second backup, then COMMIT with flags 1.
  void ConfirmCompact();

  Stage stage() const { return stage_; }
  uint8_t target() const { return target_; }    // 0 until identified
  int channel() const { return ch_; }           // -1 when none
  const Result &result() const { return result_; }
  FailCode fail() const { return fail_; }
  // With F_BACKUP: why the preset backup did not happen (BK_NONE otherwise).
  BackupWhy backup_why() const { return backup_why_; }
  // Progress for the screen: the Measure point (0..kPoints-1) and its mV.
  int point_index() const { return pt_; }
  int32_t point_mv() const { return pt_ >= 0 && pt_ < kPoints ? kPointMv[pt_] : 0; }
  uint8_t candidates_mask() const;              // bit i: 0x28 + i answered the scan
  // Channel ch's measured points (read-only, for the screen and tests): the
  // kFitPoints fit points then the kVerifyPoints verify points, in kPointMv
  // order; {raw ADC median, meter median mV}. Valid up to the point measured.
  const CalRec::FitPoint *points(int ch) const { return pts_[ch & 1]; }
  // The COMMIT's Cal259eBus::Tick result (1 ok, < 0 Error), 0 while none came.
  // A Cancel once it is non-zero logs CALR,259END,CANCELLED_<result>.
  int commit_result() const { return commit_rc_; }
  // A Cancel arrived after the COMMIT frame went out: waiting for its status.
  bool cancel_pending() const { return cancel_pending_; }
  // The COMMIT verdict (Cal259eBus::Tick's result: 1 ok, < 0 Error) of the last
  // run that was CANCELLED after its COMMIT went out; 0 when there was none.
  // Kept until the next Begin, so the menu can show it. Cancelled runs are the
  // ones whose Failed screen the owner may never see.
  int cancelled_verdict() const { return cancelled_verdict_; }
  const uint8_t *record() const { return rec_; }  // the record put (valid from Write on)
  // How long ago the last channel was fitted (Results, Confirm); 0 before.
  uint32_t results_age_ms() const;

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
  void BackupDone(bool ok);
  void FailBackup(BackupWhy why);
  bool Expired() const;  // older than kResultsMaxAgeMs since channel 0's first point
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

  uint32_t results_ms_ = 0;                      // when channel 0's first point began (0: none yet)
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
  CalRec::FitPoint pts_[2][kPoints] = {};  // per channel: fit points, then verify points
  double meter_mv_ = 0.0;

  // ADC sampling
  uint8_t adc_ch_ = 0;
  uint32_t adc_t0_ = 0;
  uint32_t backup_t0_ = 0;
  BackupWhy backup_why_ = BK_NONE;
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
  int cancelled_verdict_ = 0;
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
// CALR,259BK,<addr>,<bytes>,<crc32 hex>,<OK or a BackupWhyName>: one line per
// preset backup attempt, bytes as the master transferred them.
int Backup(char *buf, size_t n, uint8_t addr, uint32_t bytes, uint32_t crc, const char *result);

}  // namespace Cal259eLines

#endif  // CAL259EFLOW_H_
