#ifndef CAL251EFLOW_H_
#define CAL251EFLOW_H_

#include <stddef.h>
#include <stdint.h>

#include "Cal251eBus.h"
#include "Cal259eFlow.h"   // EntropyPort only
#include "CalRec251e.h"
#include "CalRoutineFlow.h"  // InputWindow, InputPort
#include "CalRoutineTypes.h"
#include "CvMeterMath.h"

// ---------------------------------------------------------------------------
// The Cal Routine's "251e" target (design: docs/specs/cal-251e.md): calibrates
// the four CV outputs of a Buchla 251e with NO bench meter. The Xenomorpher's
// own calibrated inputs 1-4 read the 251e's outputs, after a loopback has
// checked that chain (Xeno outputs E-H into inputs 1-4), and the record is
// written over the 200e bus.
//
// The run:
//   Begin       self cal absent or inputs 1-4 uncalibrated: NoSelfCal (nothing happens).
//   Scan        READ status at 0x5C (must answer, fmt 1); a record, if any, is read and logged
//               (CALR,251OLD). A record that cannot be read or decoded blocks the write.
//   Warmup      both on >= 30 min: owner-confirmed (Continue).
//   PromptLoop  Xeno out E,F,G,H -> in 1,2,3,4 (no 251e cable). Continue.
//   Loopback    5 voltages: outputs E-H held, inputs read (two windows); every reading must be
//               within kLoopbackMv of the commanded voltage.
//   PromptPatch the cable ends move to the 251e's CV1-CV4 (CV n -> in n). Continue.
//   Identify    each channel raised alone: only its own input may follow; status bit1 (override
//               active) must show.
//   Measure     8 fit points (raw code held, volts read), the fit and the physical checks per
//               channel, 5 verify points at the corrected code, one drift repeat.
//   Results     a read-only run ends here (discard with Cancel). With kAllowWrite: Continue ->
//               Confirm; ConfirmWrite -> Write (READ old record again and log it, PUT x5, COMMIT once) -> Verify
//               (READ back, byte-exact) -> Done.
//
// Only Xeno outputs E-H are held, and only in the loopback. Nothing here saves any Xeno
// calibration: the flow has no save port. The 251e's overrides are volatile (5 s timer) and
// are released on Fail, Cancel and the end of Measure (RELEASE, best effort, pumped by Tick()
// and by CalTask via background_work()).
//
// Pure and non-blocking: every port is an interface. Host-tested in test/test_cal251e_flow.cpp.
// ---------------------------------------------------------------------------

class Cal251eFlow {
 public:
  using OutputPort = CalRoutine::OutputPort;
  using InputPort = CalRoutine::InputPort;
  using Clock = CalRoutine::Clock;
  using LogFn = CalRoutine::LogFn;

  // The Xeno's calibrated inputs in millivolts (panel label 0..7). Calibrated(): the input has a
  // Cal Routine curve or line. Millivolts(): the averaged raw reading converted on that table (its
  // result at raw 0 is the top of the calibrated span).
  struct MvPort {
    virtual bool Calibrated(int label) const = 0;
    virtual float Millivolts(int label, float raw) const = 0;
  };

  enum class Stage {
    Idle, NoSelfCal, Scan, Warmup, PromptLoop, Loopback, PromptPatch, Identify, Measure,
    Results, Confirm, Write, Verify, Done, Failed
  };

  // Appended only (the log and the screen name them by FailName).
  enum FailCode {
    F_NONE, F_NO_SELF_CAL, F_INPUT_UNCAL, F_NO_251E, F_PATCH, F_LOOPBACK, F_IDENTITY,
    F_NO_OVERRIDE, F_NOISY, F_DRIFT, F_INPUT_RANGE, F_SCALE, F_FIT, F_NONLIN, F_RECORD, F_VERIFY,
    F_BUS, F_COMMIT_REFUSED, F_COMMIT_UNKNOWN, F_READBACK, F_CANCELLED, F_EXPIRED, F_INTERNAL,
    F_NO_INPUT,  // an input window delivered no samples
    F_WRITE_CAP,  // the cap on COMMITs per power-up was reached (kWriteCap)
    F_OLD_UNREADABLE  // a record exists but could not be read and decoded: never overwritten blind
  };

  struct Result {
    CalRec::Channel ch[CalRec251e::kNch];
    float a_mv[CalRec251e::kNch];        // measured mV per raw code
    float b_mv[CalRec251e::kNch];        // measured mV at raw code 0
    float nonlin_mv[CalRec251e::kNch];   // worst fit-point deviation from the line
    float verify_mv[CalRec251e::kNch];   // worst |measured - target| at the corrected codes (signed)
    float anchor_mv[CalRec251e::kNch];   // worst deviation of the record's prediction at the anchors
    float bow_mv[CalRec251e::kNch];      // peak of the quadratic component of the fit points
    float inl_mv[CalRec251e::kNch];      // worst residual of a quadratic fit of the fit points
    float noise_mv[CalRec251e::kNch];    // rms of the window-to-window difference / sqrt 2
    float loop_worst_mv;                 // worst |reading - commanded| in the loopback
    uint8_t valid_mask;                  // bit c: channel c fitted and verified
    // Channels over a WRITE limit that a measuring-only run reports instead of failing on
    // (see "warn first" below). Always 0 in a build with the write on: those runs fail instead.
    uint8_t warn_nonlin, warn_verify, warn_anchor;
  };

  // ---- the run's constants (limits PROVISIONAL until a measuring-only run on a 251e) ----
  static constexpr int kChannels = CalRec251e::kNch;
  static constexpr int kFirstLoopOut = 4;      // Xeno outputs E..H (panel 4..7) -> inputs 1..4
  static constexpr int kFitPoints = 8, kVerifyPoints = 5, kLoopPoints = 5;
  static constexpr int kPoints = kFitPoints + kVerifyPoints + 1;  // + the drift repeat
  static const int32_t kFitMv[kFitPoints];       // 1200 .. 9600
  static const int32_t kVerifyMv[kVerifyPoints];  // 300, 3000, 5400, 7800, 9600
  static const int32_t kLoopMv[kLoopPoints];      // 300, 1200, 3600, 6000, 9600
  static constexpr int32_t kIdLowMv = 1200, kIdHighMv = 6000;
  static constexpr double kIdMoveMv = 2400.0;   // own input must rise this much
  static constexpr double kIdOtherMv = 500.0;   // no other input may rise more than this
  static constexpr double kLoopbackMv = 2.5;    // loopback reading vs commanded
  static constexpr double kPatchErrMv = 500.0;  // a loopback error this big is a cable
  static constexpr double kRepeatMv = 1.5;      // the two windows of a point
  static constexpr double kDriftMv = 1.5;       // first fit point repeated at the end
  static constexpr double kTopGuardMv = 100.0;  // a reading this close to an input's top
  // Warn first. The limits below are PROVISIONAL, derived not measured. They are what the WRITE path
  // requires (a build with the write on fails the run on them). A measuring-only run (the shipped
  // firmware) only WARNs and reports (CALR,251WARN / 251STAT lines, Results footer) and stops on
  // the GROSS limits, which only a broken measurement or a wrong premise reaches. The measured
  // figures of the first runs are what should set the write limits.
  static constexpr double kNonlinMv = 3.0;
  static constexpr double kVerifyMvLimit = 4.0;
  static constexpr double kNonlinGrossMv = 12.0;
  static constexpr double kVerifyGrossMv = 16.0;
  static constexpr int kWriteCap = 20;          // COMMITs per power-up (one flash page erased by each)
  static constexpr uint32_t kResultsMaxAgeMs = 10u * 60u * 1000u;
  static constexpr int kWindows = 2;            // windows per point
  static constexpr uint32_t kBusTimeoutMs = 2000;

  // THE WRITE IS DISABLED in the firmware, exactly like the 259e's. Re-enable only by building
  // with -DCAL251E_ALLOW_WRITE=1 after the bench gates in docs/specs/cal-251e.md. The write code
  // and its tests are kept alive: the host suite and the simulator build with it on. The macro
  // counts only when non-zero: -DCAL251E_ALLOW_WRITE=0 stays read-only.
#if defined(CAL251E_ALLOW_WRITE) && CAL251E_ALLOW_WRITE
  static constexpr bool kAllowWrite = true;
#else
  static constexpr bool kAllowWrite = false;
#endif

  // Returns false, changing nothing, while a cancelled run's COMMIT verdict is pending.
  bool Begin(OutputPort &out, InputPort &in, MvPort &mv, Clock &clock, BusPort &bus,
             EntropyPort &entropy, bool self_cal_present, LogFn log);
  void Tick();
  void Continue();      // Warmup, PromptLoop, PromptPatch, Results (-> Confirm when kAllowWrite)
  void Cancel();
  void ConfirmWrite();  // Confirm only

  Stage stage() const { return stage_; }
  FailCode fail() const { return fail_; }
  const Result &result() const { return result_; }
  // Progress for the screen.
  int loop_index() const { return loop_i_; }
  int point_index() const { return pt_; }     // 0..kPoints-1 in the Measure stage, else -1
  // F_IDENTITY: the channel whose own input did not follow (or that moved another input), and the
  // input that did rise most for it (-1 none).
  int identity_channel() const { return id_bad_ch_; }
  int identity_input() const { return id_seen_in_; }
  // Warnings of a measuring-only run as one word: nonlin channels | verify << 4 | anchor << 8.
  unsigned warn_word() const { return WarnWord(); }
  int commits_since_power_up() const { return commits_power_up_; }  // since power-up, not reset by Begin
  bool had_record() const { return had_record_; }
  uint16_t record_seq() const { return old_seq_; }
  const uint8_t *record() const { return rec_; }  // the record put (valid from Write on)
  int commit_result() const { return commit_rc_; }
  bool cancel_pending() const { return cancel_pending_; }
  int cancelled_verdict() const { return cancelled_verdict_; }
  uint32_t results_age_ms() const;
  // Tick() still has work with the screen gone: a cancelled COMMIT verdict, or a RELEASE to send.
  bool background_work() const { return cancel_pending_ || rel_pending_; }

  // The measured points (read-only, for tests and the screen): fit points then verify points.
  const CalRec251e::FitPoint *fit_points(int ch) const { return fit_[ch & 3]; }
  const CalRec251e::FitPoint *verify_points(int ch) const { return ver_[ch & 3]; }

  static const char *StageName(Stage s);
  static const char *FailName(FailCode f);

 private:
  enum Sub : uint8_t { S_NONE, S_STATUS, S_OLDREC, S_ACQ, S_ID_STATUS, S_READREC, S_PUT, S_PRESTAT, S_COMMIT };
  enum Acq : uint8_t { A_NONE, A_SEND, A_SENDING, A_WIN };

  unsigned WarnWord() const {
    return unsigned(result_.warn_nonlin) | (unsigned(result_.warn_verify) << 4) | (unsigned(result_.warn_anchor) << 8);
  }
  void Log(const char *line);
  void SetStage(Stage s) { stage_ = s; }
  void Fail(FailCode f);
  void Finish();
  void ReleaseHolds();
  void PumpRelease();
  void BusTarget(uint8_t addr);
  bool Expired() const;

  void StartLoopPoint();
  void TickLoop();
  void StartIdentify();
  void TickIdentify();
  void StartIdStep();
  void StartPoint();
  void TickMeasure();
  void FitChannels();
  void FinishMeasure();
  void StartWrite();
  void TickWrite();
  void TickVerify();

  // The acquisition: hold codes (bus overrides, or the Xeno's outputs E-H in the loopback),
  // then kWindows (or one) input windows; mv_[] is the mean per channel.
  void AcqStart(const uint16_t codes[kChannels], int windows, bool via_bus);
  bool AcqTick();   // true when mv_[] is ready; false while busy or after a Fail

  OutputPort *out_ = nullptr;
  InputPort *in_ = nullptr;
  MvPort *mvp_ = nullptr;
  Clock *clock_ = nullptr;
  BusPort *busport_ = nullptr;
  EntropyPort *entropy_ = nullptr;
  LogFn log_ = nullptr;

  Cal251eBus bus_;
  CalRoutine::InputWindow win_;

  Stage stage_ = Stage::Idle;
  Sub sub_ = S_NONE;
  FailCode fail_ = F_NONE;
  uint32_t now_ = 0;
  uint32_t results_ms_ = 0;

  uint16_t tables_[kChannels][CalRoutineMath::kNodes] = {};  // Xeno outputs E-H, read at Begin
  bool had_record_ = false;
  bool old_read_ok_ = true;
  uint16_t old_seq_ = 0;

  // acquisition
  Acq acq_ = A_NONE;
  bool acq_bus_ = false;
  int acq_windows_ = 0, acq_w_ = 0, acq_i_ = 0;
  uint16_t acq_codes_[kChannels] = {};
  double wmv_[kWindows][kChannels] = {};
  double mv_[kChannels] = {};
  bool ovr_live_ = false;   // an OVERRIDE was sent and no RELEASE has been
  bool rel_pending_ = false, rel_started_ = false;

  int loop_i_ = -1;
  double loop_bad_ = 0.0;
  int id_step_ = 0;
  double id_base_[kChannels] = {};
  bool id_ok_ = true;
  int id_bad_ch_ = -1, id_seen_in_ = -1;

  int pt_ = -1;
  CalRec251e::FitPoint fit_[kChannels][kFitPoints] = {};
  CalRec251e::FitPoint ver_[kChannels][kVerifyPoints] = {};
  CalRec251e::Analog an_[kChannels] = {};
  double first_mv_[kChannels] = {};

  // The fit's target millivolts per stage code. Always the stock constant; a member only so that
  // the host test can replay a deliberately wrong-scale fit (the 259e's mistake) through the
  // whole flow and see it rejected. The physical checks never read it.
  double fit_k_ = CalRec251e::kMvPerCode;

  double noise_ss_[kChannels] = {};
  int noise_n_ = 0;
  int commits_power_up_ = 0;

  bool commit_sent_ = false;
  int commit_rc_ = 0;
  bool cancel_pending_ = false;
  int cancelled_verdict_ = 0;
  uint8_t rec_[CalRec251e::kRecLen] = {};
  uint16_t crc_ = 0;

  Result result_ = {};
};

// ---- line formats --------------------------------------------------------------
//
// CALR v2 lines of the 251e run (golden examples: test/fixtures/cal251e_lines.txt).
namespace Cal251eLines {

constexpr size_t kMaxLine = 160;

int HeaderCount();
const char *Header(int i);  // 0 is "CALR#,version=2"

int Start(char *buf, size_t n, uint8_t addr);
int Loop(char *buf, size_t n, int in, int32_t mv, double reading_mv);
// kind: 'F' fit, 'V' verify, 'D' drift.
int Point(char *buf, size_t n, int ch, char kind, uint16_t raw, double mv);
// The measured analog line of a channel (logged before any judgement of it).
int Line(char *buf, size_t n, int ch, double a, double b, double nonlin_mv, double bow_mv, double inl_mv);
// Per channel, once the verify points are in: noise_rms_mv, lsb_mv (the DAC step in mV), the worst
// verify error (signed), the worst anchor deviation, warn bits (1 NONLIN, 2 VERIFY, 4 ANCHOR, hex).
int Stat(char *buf, size_t n, int ch, double noise_mv, double lsb_mv, double verify_mv, double anchor_mv, unsigned warn);
// A limit the write path needs and this run is over; kind is NONLIN, VERIFY or ANCHOR.
int Warn(char *buf, size_t n, int ch, const char *kind, double value_mv, double limit_mv);
// One per COMMIT: n counts COMMITs since power-up, seq is the record's sequence number.
int Commit(char *buf, size_t n, int count, unsigned seq);
int Fit(char *buf, size_t n, int ch, const CalRec::Channel &c, double pred_worst_mv, double anchor_mv);
// rec is the record bytes, or null for none; err a Cal251eBus Error (nonzero with bytes: they were
// read but do not decode, logged ERR_<n>,<hex>).
int Old(char *buf, size_t n, const uint8_t *rec, int err);
int Rec(char *buf, size_t n, uint8_t addr, const uint8_t rec[CalRec251e::kRecLen]);
// warn: the run's warning word (Cal251eFlow::warn_word()), printed as warn=<hex>; 0 when clean.
int End(char *buf, size_t n, const char *result, unsigned warn);

}  // namespace Cal251eLines

#endif  // CAL251EFLOW_H_
