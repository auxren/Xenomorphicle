#ifndef CALROUTINEFLOW_H_
#define CALROUTINEFLOW_H_

#include <stddef.h>
#include <stdint.h>

#include "CalRoutineMath.h"
#include "CalRoutineMeasure.h"
#include "CalRoutineTypes.h"
#include "CvMeterMath.h"

struct InputCurveRecord;

// ---------------------------------------------------------------------------
// The calibration routine's step machine (design/cal-routine-design.md
// 6.2-6.4, 9.2-9.3), in three modes (Mode below):
//   Outputs  configure the USB DMM, closed-loop set all 11 DAC table points
//            of each of the 8 outputs, verify each with a 20-point sweep.
//   Inputs   no meter: the owner patches output A -> input 1 ... H -> 8; a
//            patch check proves it (each output raised alone moves only its
//            own input), then one sweep holds all 8 outputs at each point
//            (each through its own table) and reads all 8 inputs in one
//            window; each input gets its own curve.
//   Self     Outputs, then Inputs on the new tables.
// It produces Results; the app (Cal Routine) shows them and saves them
// through CalRoutineSave(), which writes only the parts the mode measured.
//
// Pure: the meter, the outputs, the inputs and the clock sit behind the
// ports below, so the whole flow is host-tested (test/test_calroutine_flow.cpp)
// against simulated hardware. Compiled in every env, all of it in flash
// (none of it is ISR code). Tick() does bounded work: at most one meter
// Poll() and one port call of each kind per call, except that setting the
// outputs for an input window is 8 Holds and an input point logs 8 lines.
//
// Indexing: outputs and inputs are PANEL labels 0..7 (A..H, 1..8)
// everywhere here, in the ports and in Results. The glue maps them to the
// physical channels (CvMeter.cpp InPhys/OutPhys); the SavePort glue maps
// Results' input arrays to the physical order of ICA1 and the curve file.
//
// Units: 1.2 V per octave, so 1 mV = 1 cent. DAC table entry k is k * 1200 mV.
// ---------------------------------------------------------------------------

namespace CalRoutine {

// Which routine runs. Self is 0, so zeroed Results are a Self run's.
enum class Mode : uint8_t { Self = 0, Outputs = 1, Inputs = 2 };

// The 8 CV inputs, all in one window (the ADC samples them together).
// StartWindow begins averaging every input; WindowDone, asked once the flow's
// measure time has passed, fills s[0..7] (panel order) with what was gathered
// since (false: not ready yet, ask again).
struct InputPort {
  virtual void StartWindow() = 0;
  virtual bool WindowDone(CvMeterMath::PointStats s[8]) = 0;
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

// MeterPort, OutputPort, Clock, LogFn, the meter rule's timings (kOutSettleMs,
// kReadings, kFinalReadings, kReadTimeoutMs, the retry limits) and the meter
// setup (kMeterSetup, kErrorQuery, kConfigTimeoutMs) live in CalRoutineTypes.h.

// ---- timings and limits ------------------------------------------------------

constexpr uint32_t kReconnectMs = 20000;   // meter gone this long mid-run: Failed
constexpr uint32_t kInSettleMs = 300;      // the console Q's settle and measure
constexpr uint32_t kInMeasureMs = 600;
constexpr uint32_t kInWindowTimeoutMs = 1000;  // WindowDone still false after this: no samples
constexpr int32_t kInStepMv = 600;         // input sweep spacing and first point
constexpr int kMaxInputPoints = 20;
constexpr float kInTopGuardCounts = 32.f;  // a point this close to raw 0 ends that input's sweep
constexpr int kSweepPoints = 20;           // output verify points (CalRoutineMath::SweepPoints)
// Input n's source is output n (panel A -> 1 ... H -> 8).
// The patch check: all 8 outputs at kPatchLowMv, then each in turn at
// kPatchHighMv alone. An input "moves" when its mean shifts by more than
// kPatchMoveCounts from the all-low window. A correct patch moves its input
// by 5.4 V / ~2.75 mV per count = ~1960 counts (~1840 at 2.93, the nominal
// scale); an unpatched or other input moves by the noise and crosstalk, a few
// counts at most. 1000 counts (~2.75 V) is half way: about 900 counts of margin
// on each side, so neither a gain trim at its end (+-15 %, GAIN's window) nor
// a noisy input (NOISY is 24 counts p-p) can flip a verdict.
constexpr int32_t kPatchLowMv = 600;
constexpr int32_t kPatchHighMv = 6000;
constexpr float kPatchMoveCounts = 1000.f;

// Run time: each READ? is ~0.53 s, so the readings dominate. Per output:
// 11 nodes x (iteration measurements x 4 READ? + 11 READ? final) plus 20
// sweep points x 4 READ?. The inputs use no meter: 9 patch-check windows and
// about 18 sweep windows of 0.9 s each. test_run_time_model prints the
// modelled figures for each routine.

// Waits the input settle time, then averages all 8 inputs in one window:
// the measuring step of the routine's input part (whatever the caller set
// the outputs to) and, through InputPoint, of the console's Q.
class InputWindow {
 public:
  void Start(InputPort &in, Clock &clock);
  // True once measured (s[0..7] filled; samples == 0 when the input port
  // never delivered).
  bool Tick(CvMeterMath::PointStats s[8]);
  bool active() const { return phase_ != P_IDLE; }
  void Stop() { phase_ = P_IDLE; }

 private:
  enum Phase : uint8_t { P_IDLE, P_SETTLE, P_MEASURE };
  InputPort *in_ = nullptr;
  Clock *clock_ = nullptr;
  uint32_t t0_ = 0;
  Phase phase_ = P_IDLE;
};

// Settles one CV output at a code, then averages one input (one channel of
// the all-8 window): the console Q's measuring step.
class InputPoint {
 public:
  void Start(OutputPort &out, InputPort &in, Clock &clock, int out_label, uint16_t code,
             int in_label);
  // True once the point is measured (*s filled; samples == 0 when the input
  // port never delivered). The output stays held.
  bool Tick(CvMeterMath::PointStats *s);
  bool active() const { return win_.active(); }
  void Stop() { win_.Stop(); }

 private:
  InputWindow win_;
  int in_label_ = 0;
};

}  // namespace CalRoutine

class CalRoutineFlow {
 public:
  using MeterPort = CalRoutine::MeterPort;
  using OutputPort = CalRoutine::OutputPort;
  using InputPort = CalRoutine::InputPort;
  using Clock = CalRoutine::Clock;
  using SavePort = CalRoutine::SavePort;
  using Mode = CalRoutine::Mode;

  // WarnMarker: Inputs only, the outputs were not calibrated by Cal Routine
  // (Continue goes on, Cancel stops). PromptPatch: patch A->1 ... H->8.
  // CheckPatch: the patch check runs. PatchFailed: it found a crossed or an
  // empty jack (Continue checks again; Skip goes on without the empty ones).
  enum class Stage {
    Idle, NoMeter, ConfigMeter, PromptOutput, CalOutput, SweepOutput,
    WarnMarker, PromptPatch, CheckPatch, PatchFailed, CalInput, Results, Failed
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
    uint8_t in_skipped;         // inputs left out at the patch check (panel mask)
    uint8_t mode;               // Mode: which parts were measured

    bool HasOutputs() const;    // Outputs or Self: the tables are new
    bool HasInputs() const;     // Inputs or Self: the curves are new
    // Outputs, Self: all 8 outputs passed. Inputs: at least one input passed.
    bool CanSave() const;
  };

  void SetLog(CalRoutine::LogFn fn) { log_ = fn; query_.SetLog(fn); meas_.SetLog(fn); }

  // Starts a run. Outputs, Self: NoMeter until the meter is connected, then
  // ConfigMeter. Inputs: PromptPatch, or WarnMarker first when
  // !outputs_marked (the Cal Routine marker is absent: the tables the inputs
  // are measured against were not set by Cal Routine). The meter is not used.
  void Begin(MeterPort &meter, OutputPort &out, InputPort &in, Clock &clock,
             Mode mode = Mode::Self, bool outputs_marked = true);
  void Tick();
  // PromptOutput, PromptPatch: the owner confirmed the patch. WarnMarker: go
  // on anyway. PatchFailed: the patch was fixed, check it again.
  void Continue();
  // PatchFailed with no crossing (only empty jacks): leave the inputs whose
  // outputs moved nothing uncalibrated and measure the others.
  void Skip();
  // PatchFailed in Self: give up the inputs and keep the measured outputs.
  // The run becomes an Outputs run (Results with the outputs part only,
  // CanSave and Save as Outputs; CALR,MODE,OUTPUTS is logged). False and
  // nothing changes anywhere else.
  bool KeepOutputsOnly();
  // Any stage: releases every hold, forgets the run's results, goes to Idle.
  void Cancel();
  // Results stage only: measure output o (panel label) again -- after a
  // NOISY lead or a failed check. Sets the meter up again, prompts for the
  // output (PromptOutput), runs it, and comes back to Results. Every other
  // output keeps what it measured. In Self the inputs are then patched,
  // checked and measured again (input o's curve hangs on output o's table,
  // and the patch was disturbed to reach the meter). False (nothing changes)
  // outside Results, in Inputs mode, or for an o out of range.
  bool RetryOutput(int o);

  // Results stage: CalRoutineSave(), logged. True on success (then Idle).
  bool Save(SavePort &port);
  // Results (or Failed) stage: logged, then Idle. Nothing is written.
  void Discard();

  Stage stage() const { return stage_; }
  Mode mode() const { return mode_; }
  int current_output() const { return cur_out_; }  // -1 when none
  int current_input() const { return cur_in_; }    // -1 when none
  // Progress, for the screen. CalOutput: the table entry being set (0..10);
  // SweepOutput: the verify point and how many there are; CalInput: the
  // sweep voltage and the points taken so far; CheckPatch: the output being
  // raised (patch_index, -1 during the all-low window).
  int node_index() const { return node_k_; }
  int sweep_index() const { return sweep_i_; }
  int sweep_points() const { return n_sweep_; }
  int32_t input_mv() const { return ip_mv_; }
  int input_points() const { return n_sweep_in_; }
  int patch_index() const { return p_i_; }
  // After the patch check: the inputs output o moved (panel mask), outputs
  // that moved another input, and outputs that did not move their own.
  uint8_t patch_moved(int o) const { return o >= 0 && o < 8 ? p_moved_[o] : 0; }
  uint8_t patch_crossed() const { return p_crossed_; }
  uint8_t patch_missing() const { return p_missing_; }
  // The check stopped because a window delivered no samples (no reading:
  // neither crossed nor missing is known).
  bool patch_no_reading() const { return p_no_read_; }
  // NoMeter after the meter went away mid-run, and for how long (Failed
  // after kReconnectMs).
  bool meter_lost() const { return lost_; }
  uint32_t lost_for_ms() const { return lost_ ? now_ - lost_ms_ : 0; }
  // RetryOutput's output while it runs, else -1.
  int retry_output() const { return retry_out_; }
  // Failed stage: the reason logged with CALR,FAIL ("" otherwise).
  const char *fail_reason() const { return fail_reason_; }
  const Results &results() const { return results_; }
  // Results stage and Results::CanSave() (every output passed, or for an
  // Inputs run at least one input). Out of line: an inline copy the app does
  // not inline would land in ITCM.
  bool CanSave() const;

  static const char *StageName(Stage s);
  static const char *ModeName(Mode m);
  static const char *OutReasonName(uint8_t r);

  // ---- internals: results of one meter query / one measurement ----
  // The meter rule's results are CalRoutine::MeterQuery/MeterMeasure's.
  typedef CalRoutine::MeterQuery::Result QResult;
  typedef CalRoutine::MeterMeasure::Result MResult;

 private:

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
  void TickPatch();
  void StartOutput(int o);
  void StartNode();
  void StartFinal();
  void AcceptNode();
  void StartSweep();
  void FinishOutput(OutReason r);
  void NextAfterOutput();
  void ToInputs();
  void StartPatchCheck();
  void HoldPatch();
  void FinishPatchCheck();
  void StartInputs();
  void HoldInputPoint();
  void FinishInputs();
  void FinishRun();

  bool MeterErrorReply(const char *step);
  void MStart(uint16_t code, int32_t target_mv, int readings = CalRoutine::kReadings);

  MeterPort *meter_ = nullptr;
  OutputPort *out_ = nullptr;
  InputPort *in_ = nullptr;
  Clock *clock_ = nullptr;
  CalRoutine::LogFn log_ = nullptr;

  Stage stage_ = Stage::Idle;
  Mode mode_ = Mode::Self;
  int cur_out_ = -1;
  int cur_in_ = -1;
  uint32_t now_ = 0;

  // meter setup queries, and one measurement (settle, throwaway READ?, readings)
  CalRoutine::MeterQuery query_;
  CalRoutine::MeterMeasure meas_;
  bool node_final_ = false;   // the node's final measurement (kFinalReadings) is running

  // meter setup / loss
  int cfg_i_ = 0;
  bool lost_ = false;
  uint32_t lost_ms_ = 0;
  int resume_out_ = -1;
  int retry_out_ = -1;       // RetryOutput's output, -1 for a full run
  const char *fail_reason_ = "";

  // output in progress
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

  // inputs in progress (all 8 at once)
  CalRoutine::InputWindow iwin_;
  CvMeterMath::PointStats istats_[8];
  CalRoutineMath::InputSweepPoint ipts_[8][CalRoutine::kMaxInputPoints];
  int n_ipts_[8] = {};
  int32_t ip_lo_mv_[8] = {};  // input n's span on output n, clear of its rail segments
  int32_t ip_top_mv_[8] = {};
  uint16_t ip_code_[8] = {};
  uint8_t ip_open_ = 0;       // inputs still sweeping (panel mask)
  int32_t ip_mv_ = 0;         // the sweep voltage
  int n_sweep_in_ = 0;        // sweep points taken
  // patch check
  int p_i_ = -1;              // output raised; -1 the all-low window
  float p_low_[8] = {};       // each input's mean with every output low
  uint8_t p_moved_[8] = {};
  uint8_t p_crossed_ = 0;
  uint8_t p_missing_ = 0;
  bool p_no_read_ = false;

  Results results_;
};

// Writes the parts Results hold through the port, in this order: DAC table,
// curves, lines, marker, autotune reset, input reload. Outputs: DAC table,
// marker, autotune. Inputs: curves, lines, reload. Self: all six (the marker
// is only ever written with the tables). valid_mask bit n = in_pass[n];
// source[n] = n + 1 (output n, 1-based as ICA1 stores it). Refuses (false,
// nothing called) unless results.CanSave(); stops at the first failing step
// (false; the marker is then never written).
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

// kMaxLine, Retry and Reread are declared in CalRoutineMeasure.h (the meter
// rule logs them; they are defined in CalRoutineMeasure.cpp).

int HeaderCount();
// i-th header line: 0 is "CALR#,version=1", then one per SUB.
const char *Header(int i);

int Meter(char *buf, size_t n, const char *identity);
int State(char *buf, size_t n, uint32_t ms, const char *stage, int out, int in);
int Out(char *buf, size_t n, int out);
int Node(char *buf, size_t n, int out, int k, int32_t target_mv, uint16_t code,
         double measured_mv, int iters, bool at_rail);
int Sweep(char *buf, size_t n, int out, int32_t target_mv, uint16_t code, double measured_mv,
          bool counted);
int OutResult(char *buf, size_t n, int out, bool pass, const char *reason, float worst_mv,
              int32_t worst_at_mv, uint16_t rail_mask);
int In(char *buf, size_t n, int in, int src_out, int32_t lo_mv, int32_t hi_mv);
int InPoint(char *buf, size_t n, int in, int32_t mv, uint16_t code,
            const CvMeterMath::PointStats &s, bool used);
int InResult(char *buf, size_t n, int in, const char *outcome, CvMeterMath::Verdict v,
             const CvMeterMath::Fit &line, float holdout_mv, float line_dev_mv, int points);
// step: "SETUP" (after kMeterSetup).
int MeterError(char *buf, size_t n, const char *step, const char *reply);
int Lost(char *buf, size_t n, int out);
int Back(char *buf, size_t n, int out);
int Redo(char *buf, size_t n, int out);
int Result(char *buf, size_t n, bool can_save, uint8_t out_pass_mask, uint8_t in_pass_mask);
int Save(char *buf, size_t n, bool ok);
int Discard(char *buf, size_t n);
int Cancel(char *buf, size_t n, const char *stage);
int Fail(char *buf, size_t n, const char *reason);
// mode: "OUTPUTS" / "INPUTS" / "SELF" (CalRoutineFlow::ModeName), at Begin.
int RunMode(char *buf, size_t n, const char *mode);
// reason: "NO_MARKER" (Inputs without a Cal Routine output calibration).
int Warn(char *buf, size_t n, const char *reason);
// The patch check, one line per output raised: the inputs it moved (panel
// mask) and its own input's shift in raw counts (positive = up in volts).
int Patch(char *buf, size_t n, int out, uint8_t moved, float own_delta_raw);
// result: "PASS", "FAIL" (crossed / missing set) or "NO_READING" (a window
// delivered no samples; both masks 0). crossed: outputs that moved another
// input; missing: outputs that did not move their own (output masks).
int PatchResult(char *buf, size_t n, const char *result, uint8_t crossed, uint8_t missing);

// The console Q's lines (docs/CV-Meter.md; golden: test/fixtures/cvmeter_lines.txt).
// out / in here are panel labels 0..7, printed 1-based.
int IncalPatch(char *buf, size_t n, int in, int out, const int32_t mv[3]);
int IncalPoint(char *buf, size_t n, int in, int k, int32_t mv, const CvMeterMath::PointStats &s);
int IncalResult(char *buf, size_t n, int in, int out, const CvMeterMath::Fit &fit,
                float residual_mv, float noise_pp, CvMeterMath::Verdict v);

}  // namespace CalRoutineLines

#endif  // CALROUTINEFLOW_H_
