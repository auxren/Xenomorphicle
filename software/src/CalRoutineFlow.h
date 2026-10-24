#ifndef CALROUTINEFLOW_H_
#define CALROUTINEFLOW_H_

#include <stddef.h>
#include <stdint.h>

#include "CalRoutineMath.h"
#include "CvMeterMath.h"

struct InputCurveRecord;

// ---------------------------------------------------------------------------
// The calibration routine's step machine (design/cal-routine-design.md
// 6.2-6.4): configure the USB DMM, closed-loop set all 11 DAC table points
// of each of the 8 outputs, verify each output with a 40-point sweep, then
// sweep each of the 8 inputs from calibrated output A and build its curve.
// It produces Results; the app (Cal Routine) shows them and saves them
// through CalRoutineSave().
//
// Pure: the meter, the outputs, the inputs and the clock sit behind the
// ports below, so the whole flow is host-tested (test/test_calroutine_flow.cpp)
// against simulated hardware. Compiled in every env, all of it in flash
// (none of it is ISR code). Tick() does bounded work: at most one meter
// Poll() and one port call of each kind per call.
//
// Indexing: outputs and inputs are PANEL labels 0..7 (A..H, 1..8)
// everywhere here, in the ports and in Results. The glue maps them to the
// physical channels (CvMeter.cpp InPhys/OutPhys); the SavePort glue maps
// Results' input arrays to the physical order of ICA1 and the curve file.
//
// Units: 1.2 V per octave, so 1 mV = 1 cent. DAC table entry k is k * 1200 mV.
// ---------------------------------------------------------------------------

namespace CalRoutine {

// The USB DMM. Implemented over USBTMCDevice (src/drivers/USBTMCDevice.h):
// Begin = BeginQuery, Poll maps Done -> 1 (reply = Reply()), Busy -> 0,
// Refused / Timeout / Error -> < 0, Recovering -> 0 while a query was
// started, Connected = connected(), Identity = the *IDN? reply the glue read.
struct MeterPort {
  // False when the query could not be started (no meter, busy, the driver
  // resynchronising after an error): the flow waits and tries again.
  virtual bool Begin(const char *scpi, uint32_t timeout_ms) = 0;
  // 0 busy, 1 done (*reply valid until the next Begin), < 0 error, timeout or refused.
  virtual int Poll(const char **reply) = 0;
  virtual bool Connected() const = 0;
  virtual const char *Identity() const = 0;
};

// The 8 CV outputs. Only the output under test is ever held; ReleaseAll
// lets go of everything this port holds (the glue may track its own holds
// so that it does not release outputs held by someone else).
struct OutputPort {
  virtual void Hold(int out, uint16_t code) = 0;
  virtual void ReleaseAll() = 0;
  // The stored calibration table's entry k (k * 1200 mV), the iteration's start.
  virtual uint16_t TableCode(int out, int k) const = 0;
  // The DAC code for mv through the normal pitch-to-code path, but on `table`
  // (the 11 entries being verified) instead of the stored one.
  virtual uint16_t CodeForMv(int out, const uint16_t table[CalRoutineMath::kNodes],
                             int32_t mv) const = 0;
};

// The 8 CV inputs. StartWindow begins averaging input `in`; WindowDone,
// asked once the flow's measure time has passed, fills *s with what was
// gathered since (false: not ready yet, ask again).
struct InputPort {
  virtual void StartWindow(int in) = 0;
  virtual bool WindowDone(CvMeterMath::PointStats *s) = 0;
};

struct Clock {
  virtual uint32_t Ms() const = 0;  // wraps; the flow only ever subtracts
};

// Where Results go. Arrays are panel-indexed (see above).
struct SavePort {
  virtual bool SaveDacTable(const uint16_t table[8][CalRoutineMath::kNodes]) = 0;
  virtual bool SaveCurves(const int16_t curves[8][CalRoutineMath::kCurvePoints],
                          uint8_t valid_mask, const uint8_t source[8]) = 0;
  // verify_mv[n]: input n's worst curve-vs-line deviation (ICA1's verify field).
  virtual bool SaveLines(const CvMeterMath::Fit lines[8], uint8_t valid_mask,
                         const uint8_t source[8], const float verify_mv[8]) = 0;
  virtual void WriteMarker() = 0;
  virtual void ResetAutotune() = 0;
  virtual void ReloadInputs() = 0;
};

typedef void (*LogFn)(const char *line);  // one line, no line ending

// ---- timings and limits ------------------------------------------------------

constexpr uint32_t kOutSettleMs = 150;     // after every DAC code change
constexpr int kReadings = 5;               // READ? per measurement -> Summarize
// One READ?: NPLC 1 with autozero off takes ~20 ms of integration plus the
// meter's trigger and USB turnaround (modelled at 60 ms); 3 s is only the
// give-up point.
constexpr uint32_t kReadTimeoutMs = 3000;
constexpr uint32_t kConfigTimeoutMs = 1000;
constexpr uint32_t kWatchdogMs = 500;      // past the timeout a busy Poll counts as failed
constexpr int kQueryRetries = 3;           // a failed query is retried this often
constexpr uint32_t kRetryWaitMs = 200;     // ... after this wait
constexpr uint32_t kBeginGiveUpMs = 5000;  // Begin() refused for this long: failed
constexpr uint32_t kReconnectMs = 20000;   // meter gone this long mid-run: Failed
constexpr uint32_t kInSettleMs = 300;      // the console Q's settle and measure
constexpr uint32_t kInMeasureMs = 600;
constexpr uint32_t kInWindowTimeoutMs = 1000;  // WindowDone still false after this: no samples
constexpr int32_t kInStepMv = 600;         // input sweep spacing and first point
constexpr int kMaxInputPoints = 20;
constexpr float kInTopGuardCounts = 32.f;  // a point this close to raw 0 ends the sweep
constexpr int kSourceOutput = 0;           // output A is every input's source
constexpr int kSweepPoints = 40;

// The meter setup, in order (all on UsbTmc::ScpiAllowed's list): DC volts on
// the fixed 20 V range, NPLC 1, one autozero now and none between readings,
// immediate trigger. Sent at Begin and again after every reconnect.
//
// Run time (Task 7): NPLC 10 with autozero ON measured ~20 min a run on the
// bench; NPLC 1 with the autozero refreshed once per output (kOutputZero)
// cuts the meter's share by about 7x. The 5 readings per point (median and
// spread) and the 150 ms settle stay: the median absorbs the extra noise of
// the shorter integration, and the spread check still catches a loose lead.
extern const char *const kMeterSetup[5];
// Sent at the start of every output, before its first reading: one fresh
// autozero, so the zero the output's readings share is never minutes old.
extern const char *const kOutputZero;

// Settles one CV output at a code, then averages one input: the measuring
// step of both the routine's input part and the console's Q.
class InputPoint {
 public:
  void Start(OutputPort &out, InputPort &in, Clock &clock, int out_label, uint16_t code,
             int in_label);
  // True once the point is measured (*s filled; samples == 0 when the input
  // port never delivered). The output stays held.
  bool Tick(CvMeterMath::PointStats *s);
  bool active() const { return phase_ != P_IDLE; }
  void Stop() { phase_ = P_IDLE; }

 private:
  enum Phase : uint8_t { P_IDLE, P_SETTLE, P_MEASURE };
  OutputPort *out_ = nullptr;
  InputPort *in_ = nullptr;
  Clock *clock_ = nullptr;
  int in_label_ = 0;
  uint32_t t0_ = 0;
  Phase phase_ = P_IDLE;
};

}  // namespace CalRoutine

class CalRoutineFlow {
 public:
  using MeterPort = CalRoutine::MeterPort;
  using OutputPort = CalRoutine::OutputPort;
  using InputPort = CalRoutine::InputPort;
  using Clock = CalRoutine::Clock;
  using SavePort = CalRoutine::SavePort;

  enum class Stage {
    Idle, NoMeter, ConfigMeter, PromptOutput, CalOutput, SweepOutput,
    PromptInput, CalInput, Results, Failed
  };

  // Why an output did not pass (Results::out_reason).
  enum OutReason : uint8_t {
    OUT_OK, OUT_NOT_RUN, OUT_NOISY, OUT_NOT_MONOTONE, OUT_SLOPE, OUT_RESIDUAL
  };

  struct Results {
    // The new tables. An output that failed NOISY (or was never run) keeps
    // its stored codes; one that failed a check keeps what it measured.
    uint16_t table[8][CalRoutineMath::kNodes];
    bool out_pass[8];
    bool at_rail[8][CalRoutineMath::kNodes];
    float out_worst_mv[8];      // worst |sweep residual| over the counted points
    int32_t out_worst_at_mv[8]; // ... and where
    int16_t curve_mv_x2[8][CalRoutineMath::kCurvePoints];
    CvMeterMath::Fit line[8];
    bool in_pass[8];
    CvMeterMath::Verdict in_verdict[8];
    float in_holdout_mv[8];
    uint8_t out_reason[8];      // OutReason
    uint8_t in_points[8];       // sweep points the input's curve was built from
    // The source voltages the input's curve was built from: CALR,IN's span,
    // narrowed to the points actually used (0, 0 when none were).
    int32_t in_lo_mv[8];
    int32_t in_hi_mv[8];
    float in_line_dev_mv[8];    // worst curve-vs-line deviation (ICA1's verify)

    bool CanSave() const;       // all 8 outputs passed
  };

  void SetLog(CalRoutine::LogFn fn) { log_ = fn; }

  // Starts a run: NoMeter until the meter is connected, then ConfigMeter.
  void Begin(MeterPort &meter, OutputPort &out, InputPort &in, Clock &clock);
  void Tick();
  // The owner confirmed the patch (PromptOutput / PromptInput).
  void Continue();
  // PromptInput only: leave this input uncalibrated and go on.
  void Skip();
  // Any stage: releases every hold, forgets the run's results, goes to Idle.
  void Cancel();
  // Results stage only: measure output o (panel label) again -- after a
  // NOISY lead or a failed check. Sets the meter up again, prompts for the
  // output (PromptOutput), runs it, and comes back to Results. Every other
  // output keeps what it measured. When o is the inputs' source (output A)
  // all 8 inputs are measured again afterwards, since their curves were
  // built on its table. False (nothing changes) outside Results or for an
  // o out of range.
  bool RetryOutput(int o);

  // Results stage: CalRoutineSave(), logged. True on success (then Idle).
  bool Save(SavePort &port);
  // Results (or Failed) stage: logged, then Idle. Nothing is written.
  void Discard();

  Stage stage() const { return stage_; }
  int current_output() const { return cur_out_; }  // -1 when none
  int current_input() const { return cur_in_; }    // -1 when none
  // Progress, for the screen. CalOutput: the table entry being set (0..10);
  // SweepOutput: the verify point and how many there are; CalInput: the
  // source voltage and the points taken so far.
  int node_index() const { return node_k_; }
  int sweep_index() const { return sweep_i_; }
  int sweep_points() const { return n_sweep_; }
  int32_t input_mv() const { return ip_mv_; }
  int input_points() const { return n_ipts_; }
  // NoMeter after the meter went away mid-run, and for how long (Failed
  // after kReconnectMs).
  bool meter_lost() const { return lost_; }
  uint32_t lost_for_ms() const { return lost_ ? now_ - lost_ms_ : 0; }
  // RetryOutput's output while it runs, else -1.
  int retry_output() const { return retry_out_; }
  // Failed stage: the reason logged with CALR,FAIL ("" otherwise).
  const char *fail_reason() const { return fail_reason_; }
  const Results &results() const { return results_; }
  // Results stage and every output passed. Out of line: an inline copy the
  // app does not inline would land in ITCM.
  bool CanSave() const;

  static const char *StageName(Stage s);
  static const char *OutReasonName(uint8_t r);

  // ---- internals: results of one meter query / one measurement ----
  enum QResult : uint8_t { QR_BUSY, QR_DONE, QR_FAIL, QR_LOST };
  enum MResult : uint8_t { MR_BUSY, MR_DONE, MR_NOISY, MR_FAIL, MR_LOST };

 private:
  enum QPhase : uint8_t { Q_NONE, Q_BEGIN, Q_BEGIN_WAIT, Q_WAIT, Q_BUSY };
  enum MPhase : uint8_t { M_NONE, M_SETTLE, M_READ };

  void Log(const char *line);
  void SetStage(Stage s);
  void Fail(const char *reason);
  void Lost();
  void StartConfig();
  void TickNoMeter();
  void TickConfig();
  void TickNode();
  void TickSweep();
  void TickInput();
  void StartOutput(int o);
  void StartNode();
  void AcceptNode();
  void StartSweep();
  void FinishOutput(OutReason r);
  void NextAfterOutput();
  void StartInput(int i);
  void StartInputPoint();
  void FinishInput();
  void NextAfterInput();
  void FinishRun();

  void QStart(const char *scpi, uint32_t timeout_ms);
  QResult QStep();
  QResult QFailure();
  void MStart(uint16_t code, int32_t target_mv);
  MResult MStep();

  MeterPort *meter_ = nullptr;
  OutputPort *out_ = nullptr;
  InputPort *in_ = nullptr;
  Clock *clock_ = nullptr;
  CalRoutine::LogFn log_ = nullptr;

  Stage stage_ = Stage::Idle;
  int cur_out_ = -1;
  int cur_in_ = -1;
  uint32_t now_ = 0;

  // meter query
  const char *q_scpi_ = nullptr;
  uint32_t q_timeout_ = 0;
  uint32_t q_t0_ = 0;        // phase start
  uint32_t q_begin_ms_ = 0;  // first Begin attempt of this try
  int q_fails_ = 0;
  QPhase q_phase_ = Q_NONE;
  const char *q_reply_ = nullptr;

  // one measurement: settle, kReadings READ?
  MPhase m_phase_ = M_NONE;
  uint32_t m_t0_ = 0;
  int m_n_ = 0;
  bool m_reread_ = false;
  uint16_t m_code_ = 0;
  int32_t m_target_ = 0;
  double m_mv_[CalRoutine::kReadings];
  CalRoutineMath::Reading m_reading_ = {0.0, 0.0, 0};

  // meter setup / loss
  int cfg_i_ = 0;
  bool lost_ = false;
  uint32_t lost_ms_ = 0;
  int resume_out_ = -1;
  int retry_out_ = -1;       // RetryOutput's output, -1 for a full run
  const char *fail_reason_ = "";

  // output in progress
  bool zeroing_ = false;     // kOutputZero is in flight, before node 0
  int node_k_ = 0;
  uint16_t code_ = 0;
  int iters_ = 0;
  uint16_t best_code_ = 0;
  double best_mv_ = 0.0;
  double best_err_ = 0.0;
  double node_mv_[CalRoutineMath::kNodes];
  int32_t sweep_mv_[CalRoutine::kSweepPoints];
  int n_sweep_ = 0;
  int sweep_i_ = 0;
  uint16_t sweep_code_ = 0;

  // input in progress
  CalRoutine::InputPoint ipoint_;
  CalRoutineMath::InputSweepPoint ipts_[CalRoutine::kMaxInputPoints];
  int n_ipts_ = 0;
  int32_t ip_mv_ = 0;
  uint16_t ip_code_ = 0;
  int32_t ip_top_mv_ = 0;     // the input sweep's span on output A, clear of its rail segments

  Results results_;
};

// Writes Results through the port, in this order: DAC table, curves, lines,
// marker, autotune reset, input reload. valid_mask bit n = in_pass[n];
// source[n] = 1 (output A, 1-based as ICA1 stores it) for every input.
// Refuses (false, nothing called) unless results.CanSave(); stops at the
// first failing step (false; the marker is then never written).
bool CalRoutineSave(const CalRoutineFlow::Results &results, CalRoutine::SavePort &port);

// Console Q: one input against one output at three DAC settings (two fit
// points and a verification point, CvMeterMath::CheckCalibration), measured
// with CalRoutine::InputPoint. Logs INCAL,POINT after each point and
// INCAL,RESULT at the end, byte for byte the console's established format.
class CalQFlow {
 public:
  enum class State : uint8_t { Idle, Measuring, Done };

  void SetLog(CalRoutine::LogFn fn) { log_ = fn; }
  // in, out: panel labels. Holds `out` at code[0] at once.
  void Begin(CalRoutine::OutputPort &out, CalRoutine::InputPort &in, CalRoutine::Clock &clock,
             int in_label, int out_label, const int32_t mv[3], const uint16_t code[3],
             uint16_t shared_scale, int32_t octave_mv);
  // Measuring until all three points are in; then the verdict is computed,
  // INCAL,RESULT logged, the output released, and the state is Done.
  State Tick();
  // Releases the output, Idle.
  void Cancel();

  State state() const { return state_; }
  CvMeterMath::Verdict verdict() const { return verdict_; }
  const CvMeterMath::Fit &fit() const { return fit_; }
  float residual_mv() const { return residual_mv_; }

 private:
  CalRoutine::OutputPort *out_ = nullptr;
  CalRoutine::InputPort *in_ = nullptr;
  CalRoutine::Clock *clock_ = nullptr;
  CalRoutine::LogFn log_ = nullptr;
  CalRoutine::InputPoint point_;
  State state_ = State::Idle;
  int in_label_ = 0;
  int out_label_ = 0;
  int k_ = 0;
  int32_t mv_[3] = {0, 0, 0};
  uint16_t code_[3] = {0, 0, 0};
  CvMeterMath::PointStats stats_[3];
  uint16_t scale_ = 0;
  int32_t octave_mv_ = 0;
  CvMeterMath::Verdict verdict_ = CvMeterMath::VERDICT_NO_SAMPLES;
  CvMeterMath::Fit fit_ = {0.f, 0.f};
  float residual_mv_ = 0.f;
};

// Console Q commit: drops input `phys`'s curve from the record (the new Q
// line is then what the apps use). True when its bit was set, i.e. the
// record changed and must be saved (then InputCal::Reload()).
bool CalRoutineDropCurve(InputCurveRecord &rec, int phys);

// ---- line formats --------------------------------------------------------------
//
// Every line the flow logs, as fixed formats (golden examples:
// test/fixtures/calr_lines.txt). Each writes one line without the line
// ending into buf (truncated to n) and returns its length. The CALR# header
// lines name the fields of each CALR,<SUB> line, in order; the flow logs
// them at Begin().
namespace CalRoutineLines {

constexpr size_t kMaxLine = 160;

int HeaderCount();
// i-th header line: 0 is "CALR#,version=1", then one per SUB.
const char *Header(int i);

int Meter(char *buf, size_t n, const char *identity);
int State(char *buf, size_t n, uint32_t ms, const char *stage, int out, int in);
int Out(char *buf, size_t n, int out);
int Node(char *buf, size_t n, int out, int k, int32_t target_mv, uint16_t code,
         double measured_mv, int iters, bool at_rail);
int Reread(char *buf, size_t n, int out, int32_t target_mv, uint16_t code, double spread_mv);
int Sweep(char *buf, size_t n, int out, int32_t target_mv, uint16_t code, double measured_mv,
          bool counted);
int OutResult(char *buf, size_t n, int out, bool pass, const char *reason, float worst_mv,
              int32_t worst_at_mv, uint16_t rail_mask);
int In(char *buf, size_t n, int in, int src_out, int32_t lo_mv, int32_t hi_mv);
int InPoint(char *buf, size_t n, int in, int32_t mv, uint16_t code,
            const CvMeterMath::PointStats &s, bool used);
int InResult(char *buf, size_t n, int in, const char *outcome, CvMeterMath::Verdict v,
             const CvMeterMath::Fit &line, float holdout_mv, float line_dev_mv, int points);
int Retry(char *buf, size_t n, const char *scpi, int attempt);
int Lost(char *buf, size_t n, int out);
int Back(char *buf, size_t n, int out);
int Redo(char *buf, size_t n, int out);
int Result(char *buf, size_t n, bool can_save, uint8_t out_pass_mask, uint8_t in_pass_mask);
int Save(char *buf, size_t n, bool ok);
int Discard(char *buf, size_t n);
int Cancel(char *buf, size_t n, const char *stage);
int Fail(char *buf, size_t n, const char *reason);

// The console Q's lines (docs/CV-Meter.md; golden: test/fixtures/cvmeter_lines.txt).
// out / in here are panel labels 0..7, printed 1-based.
int IncalPatch(char *buf, size_t n, int in, int out, const int32_t mv[3]);
int IncalPoint(char *buf, size_t n, int in, int k, int32_t mv, const CvMeterMath::PointStats &s);
int IncalResult(char *buf, size_t n, int in, int out, const CvMeterMath::Fit &fit,
                float residual_mv, float noise_pp, CvMeterMath::Verdict v);

}  // namespace CalRoutineLines

#endif  // CALROUTINEFLOW_H_
