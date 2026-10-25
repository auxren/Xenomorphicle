// Host tests for the calibration routine's step machine (src/CalRoutineFlow.h)
// against simulated hardware: a bowed DAC per output, a USB meter that reads
// it (with noise, latency, transient errors, unplugging), and bent ADC inputs
// patched 1:1 to the outputs (output A -> input 1 ... H -> 8, any patch the
// test sets). Also the console Q on the same input stage, the save
// sequence and the CALR/INCAL line formats (fixtures/calr_lines.txt,
// fixtures/cvmeter_lines.txt). Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_calroutine_flow
//     test_calroutine_flow.cpp ../src/CalRoutineFlow.cpp ../src/CalRoutineMath.cpp
//     ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../src/CalRoutineFlow.h"
#include "../src/CalRoutineMarker.h"
#include "fake_meter.h"
#include "../src/InputCurveStore.h"
#include "../src/UsbTmc.h"

using namespace CalRoutine;
namespace CM = CvMeterMath;
namespace CR = CalRoutineMath;
typedef CalRoutineFlow::Stage Stage;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// ---- log capture -------------------------------------------------------------

static std::vector<std::string> g_log;
static void capture(const char *line) { g_log.push_back(line); }
static int count_prefix(const char *p) {
  int n = 0;
  for (const auto &l : g_log) n += l.compare(0, strlen(p), p) == 0;
  return n;
}

// ---- simulated hardware ------------------------------------------------------

// What a reading looks like after the meter's NR3 text and ParseReading.
static double via_text(double mv) {
  char b[48];
  snprintf(b, sizeof(b), "%+.9E", mv / 1000.0);
  return strtod(b, nullptr) * 1000.0;
}

// Input i reads the output patched into it (patch[i]; -1 = nothing, 0 V):
// raw = zero - v * counts_per_mv * (1 + gain) - offset + bend sin. The default
// model is the firmware's nominal one (0 V at raw 4095, 12 V at raw 0);
// Realistic() is the bench's (0 V near raw 3635-3685, ~2.75 mV per count,
// so the top of the window is near 10.65 V).
struct FakeInputs : InputPort {
  FakeOutputs *outs;
  FakeClock *clk;
  double gain[8] = {0.01, -0.012, 0.005, 0.0, -0.008, 0.015, -0.004, 0.009};
  double offset[8] = {2.0, -3.0, 1.0, 0.5, -1.5, 2.5, 0.0, -2.0};
  double bend[8] = {3.0, 2.0, 4.0, 1.0, 2.5, 3.5, 1.5, 2.0};
  double pp[8] = {2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
  double zero[8] = {4095, 4095, 4095, 4095, 4095, 4095, 4095, 4095};
  double counts_per_mv = 4096.0 / 12000.0;
  int patch[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  bool started = false;
  uint32_t start_ms = 0;
  int windows = 0;
  int dead_windows = 0;       // the next N windows deliver no samples
  bool short_window = false;  // WindowDone asked before 600 ms
  std::vector<uint8_t> window_mask;  // outputs held when each window started
  FakeInputs(FakeOutputs *o, FakeClock *c) : outs(o), clk(c) {}
  void Realistic() {
    const double z[8] = {3641, 3652, 3635, 3685, 3660, 3647, 3672, 3638};
    const double g[8] = {0.02, -0.03, 0.01, 0.035, -0.015, 0.0, 0.025, -0.02};
    const double b[8] = {3.0, -2.0, 4.5, 1.0, -3.5, 2.5, 5.0, -1.5};
    for (int i = 0; i < 8; ++i) { zero[i] = z[i]; gain[i] = g[i]; bend[i] = b[i]; offset[i] = 0; }
    counts_per_mv = 1.0 / 2.75;
  }
  double Unclamped(int i, double v) const {
    return zero[i] - v * counts_per_mv * (1.0 + gain[i]) - offset[i] + bend[i] * std::sin(kPi * v / 12000.0);
  }
  double Raw(int i, double v) const { return std::fmax(0.0, std::fmin(4095.0, Unclamped(i, v))); }
  void StartWindow() override {
    started = true;
    start_ms = clk->t;
    window_mask.push_back(outs->mask);
  }
  bool WindowDone(CM::PointStats s[8]) override {
    if (clk->t - start_ms < 600) short_window = true;
    for (int i = 0; i < 8; ++i) {
      const double m = Raw(i, outs->OutMv(patch[i]));
      s[i].mean_raw = float(m);
      s[i].min_raw = float(std::fmax(0.0, m - pp[i] / 2));
      s[i].max_raw = float(std::fmin(4095.0, m + pp[i] / 2));
      s[i].samples = dead_windows > 0 ? 0 : 3333;
    }
    if (dead_windows > 0) --dead_windows;
    ++windows;
    started = false;
    return true;
  }
};

typedef CalRoutine::Mode Mode;

struct Rig {
  SimDac dac;
  FakeClock clk;
  FakeOutputs outs{&dac};
  FakeMeter meter{&outs, &clk};
  FakeInputs ins{&outs, &clk};
  CalRoutineFlow flow;
  uint32_t t_begin = 0;
  // Outputs stages hold only the output under test; the input stages may
  // hold any; nothing is held anywhere else.
  bool hold_rule_broken = false;
  Rig() {
    g_log.clear();
    outs.clk = &clk;
    flow.SetLog(capture);
  }
  void Begin(Mode m = Mode::Self, bool marked = true) {
    t_begin = clk.t;
    flow.Begin(meter, outs, ins, clk, m, marked);
  }
  void Step(uint32_t ms = 5) {
    flow.Tick();
    const Stage s = flow.stage();
    if (outs.mask) {
      const bool out_stage = s == Stage::CalOutput || s == Stage::SweepOutput;
      const bool in_stage = s == Stage::CheckPatch || s == Stage::CalInput;
      if (out_stage && outs.held != flow.current_output()) hold_rule_broken = true;
      if (!out_stage && !in_stage) hold_rule_broken = true;
    }
    clk.t += ms;
  }
  // Runs, pressing Continue at every prompt, until `until` returns true or
  // the run ends (Results, Failed, Idle, or a failed patch check). Returns
  // false on the time limit.
  bool Run(std::function<bool()> until = nullptr, uint32_t limit_ms = 60u * 60u * 1000u) {
    const uint32_t t0 = clk.t;
    while (clk.t - t0 < limit_ms) {
      const Stage s = flow.stage();
      if (until && until()) return true;
      if (s == Stage::Results || s == Stage::Failed || s == Stage::Idle || s == Stage::PatchFailed)
        return true;
      if (s == Stage::PromptOutput || s == Stage::PromptPatch || s == Stage::WarnMarker)
        flow.Continue();
      Step();
    }
    return false;
  }
  double Minutes() const { return (clk.t - t_begin) / 60000.0; }
};

// The node iteration as the flow documents it, against a noise-free meter.
static void ref_table(const SimDac &dac, int o, const uint16_t stored[CR::kNodes], double meter_off,
                      uint16_t table[CR::kNodes], bool rail[CR::kNodes]) {
  double meas[CR::kNodes];
  for (int k = 0; k < CR::kNodes; ++k) { meas[k] = NAN; table[k] = stored[k]; }
  for (int k = 0; k < CR::kNodes; ++k) {
    const int32_t target = k * 1200;
    uint16_t code = stored[k], best = code;
    double best_mv = 0, best_err = 1e300;
    for (int it = 1;; ++it) {
      const double mv = via_text(dac.Mv(o, code) + meter_off);
      table[k] = code;
      meas[k] = mv;
      const double err = std::fabs(mv - target);
      if (err < best_err) { best_err = err; best = code; best_mv = mv; }
      if (err <= CR::kTargetMv || it >= CR::kMaxIterations) break;
      bool r = false;
      const uint16_t next = CR::NextCode(code, mv, target, CR::LocalSlope(table, meas, k), &r);
      if (next == code) break;
      code = next;
    }
    table[k] = best;
    meas[k] = best_mv;
    rail[k] = (best == 65535 && best_mv < target - CR::kTargetMv) ||
              (best == 0 && best_mv > target + CR::kTargetMv);
  }
}

// ---- tests -------------------------------------------------------------------

static void test_happy_path_all_pass() {
  Rig r;
  r.Begin();
  CHECK(r.flow.stage() == Stage::ConfigMeter);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  const auto &res = r.flow.results();
  for (int o = 0; o < 8; ++o) {
    CHECK(res.out_pass[o]);
    CHECK(res.out_worst_mv[o] <= CR::kPassMv);
    for (int k = 0; k < CR::kNodes; ++k) CHECK(!res.at_rail[o][k]);
  }
  for (int i = 0; i < 8; ++i) {
    CHECK(res.in_pass[i]);
    CHECK(res.in_verdict[i] == CM::VERDICT_OK);
    CHECK(res.in_holdout_mv[i] < 3.0f);
    CHECK(res.in_points[i] >= 18);
  }
  CHECK(r.flow.CanSave());
  // The meter setup, in order, before anything else, ending with the error
  // queue check.
  const size_t ns = sizeof(kMeterSetup) / sizeof(kMeterSetup[0]);
  CHECK(r.meter.sent.size() > ns);
  for (size_t i = 0; i < ns && r.meter.sent.size() > ns; ++i) CHECK(r.meter.sent[i] == kMeterSetup[i]);
  CHECK(r.meter.sent.size() > ns && r.meter.sent[ns - 1] == "SYST:ERR?");
  // Autozero is ON for every reading, so no per-output re-zero and no
  // per-output error query: the setup's SYST:ERR? is the only one, and the
  // first thing after it is a reading.
  CHECK(r.meter.sent.size() > ns && r.meter.sent[ns] == "READ?");
  int errs = 0, az = 0;
  for (const auto &l : r.meter.sent) {
    errs += l == "SYST:ERR?";
    az += l.compare(0, 11, "VOLT:DC:AZ ") == 0;
  }
  CHECK(errs == 1 && r.meter.err_queries == 1);
  CHECK(az == 1);  // the setup's AZ ON
  for (int i = 0; i < 8; ++i) {
    CHECK(res.in_lo_mv[i] == kInStepMv);
    CHECK(res.in_hi_mv[i] > res.in_lo_mv[i] + 9000);
    CHECK(res.in_line_dev_mv[i] > 0.f);
  }
  CHECK(!r.ins.short_window);
  CHECK(r.outs.held == -1);
  float worst = 0;
  for (int o = 0; o < 8; ++o) worst = std::fmax(worst, res.out_worst_mv[o]);
  printf("  simulated run: %.1f min for 8 outputs + 8 inputs (READ? %u ms), %d READ?, "
         "worst sweep residual %.3f mV\n",
         r.Minutes(), unsigned(r.meter.read_ms), r.meter.reads, double(worst));
}

static void test_results_match_math() {
  Rig r;
  r.meter.noise_mv = 0.0;
  r.outs.stored[3][7] = uint16_t(r.outs.stored[3][7] + 300);  // a stored table that is off
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  for (int o = 0; o < 8; ++o) {
    uint16_t t[CR::kNodes];
    bool rail[CR::kNodes];
    ref_table(r.dac, o, r.outs.stored[o], 0.0, t, rail);
    for (int k = 0; k < CR::kNodes; ++k) {
      CHECK(r.flow.results().table[o][k] == t[k]);
      CHECK(r.flow.results().at_rail[o][k] == rail[k]);
    }
  }
}

static void test_meter_timeout_releases_all() {
  Rig r;
  r.Begin();
  CHECK(r.Run([&] { return r.flow.stage() == Stage::CalOutput && r.outs.held >= 0; }));
  r.meter.never_answer = true;
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed);
  CHECK(r.outs.held == -1);
  CHECK(r.outs.releases > 0);
  CHECK(!r.flow.CanSave());
  CHECK(!r.flow.results().CanSave());
  CHECK(count_prefix("CALR,FAIL,") == 1);
  CHECK(count_prefix("CALR,RETRY,") == kQueryRetries);
}

static void test_query_retries_after_transient_errors() {
  Rig r;
  r.Begin();
  CHECK(r.Run([&] { return r.meter.reads >= 7; }));
  r.meter.fail_next = 2;  // the real driver's ~2 XFER_ERR_IN after a recovery
  CHECK(r.Run([&] { return r.flow.stage() == Stage::PromptOutput && r.flow.current_output() == 1; }));
  CHECK(r.flow.results().out_pass[0]);
  CHECK(count_prefix("CALR,RETRY,READ?,") == 2);
  // And a driver that refuses to start queries for a while (resyncing).
  r.meter.refuse_begin = 8;
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(r.flow.CanSave());
}

static void test_cancel_any_stage_releases_all() {
  const Stage stages[] = {Stage::NoMeter, Stage::ConfigMeter, Stage::PromptOutput,
                          Stage::CalOutput, Stage::SweepOutput, Stage::WarnMarker,
                          Stage::PromptPatch, Stage::CheckPatch, Stage::PatchFailed,
                          Stage::CalInput, Stage::Results, Stage::Failed};
  for (Stage want : stages) {
    Rig r;
    if (want == Stage::NoMeter) r.meter.connected = false;
    if (want == Stage::PatchFailed) r.ins.patch[2] = -1;
    if (want == Stage::WarnMarker) r.Begin(Mode::Inputs, false);
    else r.Begin();
    bool reached = false;
    const uint32_t t0 = r.clk.t;
    while (r.clk.t - t0 < 60u * 60u * 1000u) {
      const Stage s = r.flow.stage();
      if (s == want && (want != Stage::CalOutput || r.outs.held >= 0) &&
          (want != Stage::CheckPatch || r.outs.mask) &&
          (want != Stage::CalInput || r.outs.mask == 0xFF)) { reached = true; break; }
      if (s == Stage::Results && want != Stage::Results) break;
      if (want == Stage::Failed && s == Stage::CalOutput) r.meter.never_answer = true;
      if (want != Stage::PromptOutput && s == Stage::PromptOutput) r.flow.Continue();
      if (want != Stage::PromptPatch && s == Stage::PromptPatch) r.flow.Continue();
      if (want != Stage::WarnMarker && s == Stage::WarnMarker) r.flow.Continue();
      if (s == Stage::NoMeter && want == Stage::NoMeter) { reached = true; break; }
      r.Step();
    }
    CHECK(reached);
    r.flow.Cancel();
    CHECK(r.flow.stage() == Stage::Idle);
    CHECK(r.outs.held == -1 && r.outs.mask == 0);
    CHECK(!r.flow.CanSave());
    CHECK(!r.flow.results().CanSave());
    // Nothing moves after a cancel.
    const int holds = r.outs.holds;
    for (int i = 0; i < 2000; ++i) r.Step();
    CHECK(r.outs.holds == holds && r.flow.stage() == Stage::Idle);
    if (!reached) printf("  stage %s not reached\n", CalRoutineFlow::StageName(want));
  }
}

static void test_noisy_point_retries_not_stores() {
  {
    Rig r;
    r.meter.noise_mv = 0.0;
    // READ? 5..7 of output A (the second measurement's readings; 4 is its
    // throwaway): a loose lead, 1 mV p-p.
    r.meter.extra = [](int n) { return (n >= 5 && n < 8) ? ((n & 1) ? 0.5 : -0.5) : 0.0; };
    r.Begin();
    CHECK(r.Run([&] { return r.flow.stage() == Stage::PromptOutput && r.flow.current_output() == 1; }));
    CHECK(count_prefix("CALR,REREAD,1,") == 1);
    CHECK(r.flow.results().out_pass[0]);
    uint16_t t[CR::kNodes];
    bool rail[CR::kNodes];
    ref_table(r.dac, 0, r.outs.stored[0], 0.0, t, rail);
    for (int k = 0; k < CR::kNodes; ++k) CHECK(r.flow.results().table[0][k] == t[k]);
  }
  {
    // Output B stays noisy: it fails NOISY, keeps its stored table, and the
    // run goes on to C.
    Rig r;
    r.Begin();
    r.meter.extra = [&r](int n) { return r.outs.held == 1 ? ((n & 1) ? 1.0 : -1.0) : 0.0; };
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Results);
    const auto &res = r.flow.results();
    CHECK(!res.out_pass[1]);
    CHECK(res.out_reason[1] == CalRoutineFlow::OUT_NOISY);
    for (int k = 0; k < CR::kNodes; ++k) CHECK(res.table[1][k] == r.outs.stored[1][k]);
    CHECK(res.out_pass[2]);
    CHECK(!r.flow.CanSave());
    CHECK(count_prefix("CALR,ORES,2,FAIL,NOISY,") == 1);
  }
}

static void test_no_meter_stays_nometer() {
  Rig r;
  r.meter.connected = false;
  r.Begin();
  for (int i = 0; i < 20000; ++i) {
    if (i == 100) r.flow.Continue();
    r.Step();
  }
  CHECK(r.flow.stage() == Stage::NoMeter);
  CHECK(r.outs.holds == 0);
  CHECK(r.meter.sent.empty());
  // Plugging it in starts the run.
  r.meter.connected = true;
  r.Step();
  CHECK(r.flow.stage() == Stage::ConfigMeter);
}

static void test_holds_only_current_output() {
  Rig r;
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(!r.hold_rule_broken);
}

static void test_failed_input_is_uncalibrated() {
  Rig r;
  r.ins.pp[3] = 40.0;  // input 4 (index 3): noisy patch
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  const auto &res = r.flow.results();
  CHECK(!res.in_pass[3]);
  CHECK(res.in_verdict[3] == CM::VERDICT_NOISY);
  for (int i = 0; i < 8; ++i)
    if (i != 3) CHECK(res.in_pass[i]);
  CHECK(r.flow.CanSave());
  CHECK(count_prefix("CALR,IRES,4,FAIL,NOISY,") == 1);
  // Skip (an unpatched input at the patch check) leaves it uncalibrated too.
  Rig s;
  s.ins.patch[5] = -1;
  s.Begin();
  CHECK(s.Run());
  CHECK(s.flow.stage() == Stage::PatchFailed);
  s.flow.Skip();
  CHECK(s.Run());
  CHECK(s.flow.stage() == Stage::Results);
  CHECK(!s.flow.results().in_pass[5]);
  CHECK(s.flow.results().in_pass[6]);
  CHECK(s.flow.CanSave());
  CHECK(count_prefix("CALR,IRES,6,SKIP,") == 1);
}

static void test_rail_does_not_block_save() {
  Rig r;
  r.dac.clip[5] = 11900.0;  // output F cannot reach 12 V
  r.Begin();
  CHECK(r.Run());
  const auto &res = r.flow.results();
  CHECK(res.at_rail[5][10]);
  CHECK(res.table[5][10] == 65535);
  for (int k = 0; k < 10; ++k) CHECK(!res.at_rail[5][k]);
  CHECK(res.out_pass[5]);
  CHECK(r.flow.CanSave());
  CHECK(count_prefix("CALR,NODE,6,10,12000,65535,") == 1);
}

static void test_rail_overshoot_remeasured() {
  // A kink near the top: the stored code is far off and the first slope
  // estimate overshoots past 65535, though 12 V is reachable (~65000).
  Rig r;
  r.dac.custom = [](int o, uint16_t c) {
    if (o != 2) return c / 65535.0 * 12000.0;
    const double lin = c * 12000.0 / 66000.0;
    return c <= 64000 ? lin : 64000 * 12000.0 / 66000.0 + (c - 64000) * 2.0 * 12000.0 / 66000.0;
  };
  r.outs.stored[2][10] = 62000;
  r.Begin();
  CHECK(r.Run([&] { return r.flow.stage() == Stage::SweepOutput && r.flow.current_output() == 2; }));
  const auto &res = r.flow.results();
  CHECK(!res.at_rail[2][10]);
  CHECK(res.table[2][10] < 65535);
  CHECK(res.table[2][10] > 64500);
  // The clamped code was measured before at_rail was ruled out.
  bool measured_rail = false;
  for (const auto &rc : r.meter.read_at) measured_rail |= rc.first == 2 && rc.second == 65535;
  CHECK(measured_rail);
  CHECK(count_prefix("CALR,NODE,3,10,12000,65535,") == 0);
}

static void test_reconnect_restarts_current_output() {
  Rig r;
  r.meter.noise_mv = 0.0;
  r.Begin();
  // Unplug in the middle of output D, node 4.
  CHECK(r.Run([&] { return count_prefix("CALR,NODE,4,4,") == 1; }));
  r.meter.connected = false;
  for (int i = 0; i < 400; ++i) r.Step();  // 2 s gone
  CHECK(r.flow.stage() == Stage::NoMeter);
  CHECK(r.outs.held == -1);
  r.meter.connected = true;
  r.meter.busy = false;
  r.meter.offset_mv = 0.2;  // the replug moved the readings
  const size_t sent_before = r.meter.sent.size();
  CHECK(r.Run([&] { return r.flow.stage() == Stage::PromptOutput && r.flow.current_output() == 4; }));
  CHECK(count_prefix("CALR,LOST,4") == 1);
  CHECK(count_prefix("CALR,BACK,4") == 1);
  CHECK(count_prefix("CALR,OUT,4") == 2);
  CHECK(count_prefix("CALR,NODE,4,0,") == 2);
  // The meter was set up again, first thing after the replug.
  const size_t ns = sizeof(kMeterSetup) / sizeof(kMeterSetup[0]);
  CHECK(r.meter.sent.size() > sent_before + ns);
  for (size_t i = 0; i < ns && r.meter.sent.size() > sent_before + ns; ++i)
    CHECK(r.meter.sent[sent_before + i] == kMeterSetup[i]);
  // Output D's table is entirely from after the replug.
  uint16_t t[CR::kNodes];
  bool rail[CR::kNodes];
  ref_table(r.dac, 3, r.outs.stored[3], 0.2, t, rail);
  for (int k = 0; k < CR::kNodes; ++k) CHECK(r.flow.results().table[3][k] == t[k]);
  CHECK(r.flow.results().out_pass[3]);

  // Gone for good: Failed after the reconnect timeout, nothing held.
  Rig g;
  g.Begin();
  CHECK(g.Run([&] { return g.flow.stage() == Stage::SweepOutput; }));
  g.meter.connected = false;
  CHECK(g.Run(nullptr, kReconnectMs + 5000));
  CHECK(g.flow.stage() == Stage::Failed);
  CHECK(g.outs.held == -1);
  CHECK(count_prefix("CALR,FAIL,METER_LOST") == 1);
}

// True mV that input i's model reads as `raw` (FakeInputs::Raw inverted).
static double true_mv_at(const FakeInputs &ins, int i, double raw) {
  double lo = -2000, hi = 14000;
  for (int it = 0; it < 80; ++it) {
    const double mid = 0.5 * (lo + hi);
    // Raw() clamps; use the unclamped model so the inversion is exact.
    const double r = ins.Unclamped(i, mid);
    if (r > raw) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}

static void test_input_span_capped_below_source_rail() {
  // Output A cannot reach 12 V: its entry 10 sits at the rail, so codes
  // interpolated towards it are too high. The input sweep must stay below
  // that segment, and the curves must be right where they are measured.
  for (int bottom = 0; bottom < 2; ++bottom) {
    Rig r;
    r.dac.clip[0] = 11900.0;
    if (bottom) r.dac.offset[0] = 3.0;  // entry 0 at the rail too (0 V unreachable)
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Results);
    const auto &res = r.flow.results();
    CHECK(res.at_rail[0][10]);
    CHECK(res.at_rail[0][0] == (bottom == 1));
    // Only input 1 hangs on output A; the others keep their own outputs' spans.
    const int32_t lo = bottom ? 1200 : 600, hi = 10800;
    char want[64];
    snprintf(want, sizeof(want), "CALR,IN,1,1,%ld,%ld", (long)lo, (long)hi);
    CHECK(count_prefix(want) == 1);
    for (int i = 2; i <= 8; ++i) {
      snprintf(want, sizeof(want), "CALR,IN,%d,%d,600,12000", i, i);
      CHECK(count_prefix(want) == 1);
    }
    for (const auto &l : g_log) {
      if (l.compare(0, 9, "CALR,IPT,") != 0) continue;
      int in, mv;
      if (sscanf(l.c_str(), "CALR,IPT,%d,%d,", &in, &mv) == 2 && in == 1) CHECK(mv >= lo && mv <= hi);
    }
    CHECK(res.in_lo_mv[0] == lo && res.in_hi_mv[0] == hi);
    for (int i = 0; i < 8; ++i) {
      CHECK(res.in_pass[i]);
      CHECK(res.in_holdout_mv[i] <= 1.0f);
      // Every breakpoint inside the measured span is within 1 mV of the truth.
      double worst = 0;
      for (int b = 0; b < CR::kCurvePoints; ++b) {
        const double t = true_mv_at(r.ins, i, double(b << CR::kCurveShift));
        if (t < res.in_lo_mv[i] || t > res.in_hi_mv[i]) continue;
        worst = std::fmax(worst, std::fabs(res.curve_mv_x2[i][b] / 2.0 - t));
      }
      CHECK(worst <= 1.0);
      if (worst > 1.0) printf("  input %d: worst curve error %.2f mV in %ld..%ld mV\n", i + 1,
                              worst, (long)res.in_lo_mv[i], (long)res.in_hi_mv[i]);
    }
    CHECK(r.flow.CanSave());
  }
}

static void test_rebegin_releases() {
  Rig r;
  r.Begin();
  CHECK(r.Run([&] { return r.flow.stage() == Stage::CalOutput && r.outs.held >= 0; }));
  r.meter.connected = false;
  r.flow.Begin(r.meter, r.outs, r.ins, r.clk);
  CHECK(r.outs.held == -1);
  CHECK(r.flow.stage() == Stage::NoMeter);
  r.meter.connected = true;
  CHECK(r.Run([&] { return r.flow.stage() == Stage::CalInput && r.outs.mask == 0xFF; }));
  r.flow.Begin(r.meter, r.outs, r.ins, r.clk);
  CHECK(r.outs.mask == 0);
  CHECK(r.flow.stage() == Stage::ConfigMeter);
}

// Every line the flow sends passes the driver's allowlist, and the setup is
// the one the bench showed gives real conversions: NPLC 10, autozero ON.
static void test_meter_lines_allowed() {
  for (const char *l : kMeterSetup) {
    CHECK(UsbTmc::ScpiAllowed(l));
    if (!UsbTmc::ScpiAllowed(l)) printf("  refused: %s\n", l);
  }
  CHECK(UsbTmc::ScpiAllowed("READ?"));
  CHECK(UsbTmc::ScpiAllowed("*IDN?"));  // the app's identity query
  bool nplc10 = false, az_on = false, other_nplc = false, az_other = false;
  for (const char *l : kMeterSetup) {
    const std::string x = l;
    nplc10 |= x == "VOLT:DC:NPLC 10";
    az_on |= x == "VOLT:DC:AZ ON";
    other_nplc |= x.compare(0, 14, "VOLT:DC:NPLC ") == 0 && x != "VOLT:DC:NPLC 10";
    az_other |= x.compare(0, 11, "VOLT:DC:AZ ") == 0 && x != "VOLT:DC:AZ ON";
  }
  CHECK(nplc10 && az_on && !other_nplc && !az_other);
  CHECK(std::string(kMeterSetup[sizeof(kMeterSetup) / sizeof(kMeterSetup[0]) - 1]) == kErrorQuery);
}

// A meter that rejected a setup line says so
// only in its error queue: the flow asks SYST:ERR? and stops on anything but
// an empty queue, with nothing held.
static void test_meter_config_error_fails() {
  {
    Rig r;
    r.meter.err_reply = [](int) { return "-113,\"Undefined header\""; };
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed);
    CHECK(std::string(r.flow.fail_reason()) == "METER_CONFIG");
    CHECK(count_prefix("CALR,FAIL,METER_CONFIG") == 1);
    CHECK(count_prefix("CALR,MERR,SETUP,-113;\"Undefined header\"") == 1);
    CHECK(r.meter.reads == 0);  // nothing measured on a meter in an unknown state
    CHECK(r.outs.held == -1);
  }
  {
    // "+0,..." is the empty queue too.
    Rig r;
    r.meter.err_reply = [](int) { return "+0,\"No error\""; };
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Results && r.flow.CanSave());
    CHECK(count_prefix("CALR,MERR,") == 0);
  }
}

// The bench rule at NPLC 10: the reading in progress at a code change spans
// the step (about the midpoint). Every code change is followed by a settle and
// one throwaway READ?; only the readings after it reach a median.
static void test_throwaway_after_code_change() {
  Rig r;
  r.meter.noise_mv = 0.0;
  r.outs.stored[3][7] = uint16_t(r.outs.stored[3][7] + 300);  // big first steps
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  // The model engaged: some readings did span a code change ...
  int straddled = 0, bad = 0;
  for (size_t i = 0; i < r.meter.read_mv.size(); ++i) {
    if (!r.meter.read_straddled[i]) continue;
    ++straddled;
    // ... and each of them is the first READ? after its code change.
    if (i > 0 && r.meter.read_hold[i - 1] == r.meter.read_hold[i]) ++bad;
  }
  CHECK(straddled > 50);
  CHECK(bad == 0);
  // Reads per code change: 1 throwaway + 3 (a sweep point), or + 10 (a node's
  // final). Never a bare 3 or 10, and no reading set is mixed.
  std::map<int, int> per_hold;
  for (int h : r.meter.read_hold) ++per_hold[h];
  for (const auto &kv : per_hold) CHECK(kv.second == 4 || kv.second == 11);
  // A midpoint never reached a median: every table is the noise-free one.
  for (int o = 0; o < 8; ++o) {
    uint16_t t[CR::kNodes];
    bool rail[CR::kNodes];
    ref_table(r.dac, o, r.outs.stored[o], 0.0, t, rail);
    for (int k = 0; k < CR::kNodes; ++k) CHECK(r.flow.results().table[o][k] == t[k]);
  }
  // No settle and no throwaway would have: the same sweep step read as the
  // very first READ? is the midpoint of the step.
  CHECK(r.meter.read_straddled[0] || r.meter.read_straddled[1]);
}

// A node's reported and stored measured value is the median of 10 readings
// taken after the code is final (and a throwaway), not the iteration's 3.
static void test_node_final_uses_10_readings() {
  Rig r;
  r.meter.noise_mv = 0.0;
  // Alternating +-0.2 mV on every reading so the median is not just the mean.
  r.meter.extra = [](int n) { return (n % 2) ? 0.2 : -0.2; };
  r.Begin();
  CHECK(r.Run([&] { return r.flow.stage() == Stage::PromptOutput && r.flow.current_output() == 1; }));
  // Output A's reads grouped by code change (hold).
  std::vector<std::vector<size_t>> groups;
  int last = -1;
  for (size_t i = 0; i < r.meter.read_hold.size(); ++i) {
    if (r.meter.read_at[i].first != 0) break;
    if (r.meter.read_hold[i] != last) { groups.push_back({}); last = r.meter.read_hold[i]; }
    groups.back().push_back(i);
  }
  int finals = 0, sweeps = 0;
  std::vector<std::vector<size_t>> fin;
  for (const auto &g : groups) {
    if (g.size() == 11) { ++finals; fin.push_back(g); }
    if (g.size() == 4) ++sweeps;
  }
  CHECK(finals == CR::kNodes);
  CHECK(sweeps >= kSweepPoints);
  // Node 5's logged measured value is the median of the 10 after the throwaway.
  CHECK(fin.size() > 5);
  if (fin.size() <= 5) return;
  const auto &g = fin[5];
  std::vector<double> v;
  for (size_t j = 1; j < g.size(); ++j) v.push_back(r.meter.read_mv[g[j]]);
  std::sort(v.begin(), v.end());
  const double med = 0.5 * (v[4] + v[5]);
  char want[64];
  snprintf(want, sizeof(want), "CALR,NODE,1,5,6000,");
  bool found = false;
  for (const auto &l : g_log) {
    if (l.compare(0, strlen(want), want) != 0) continue;
    found = true;
    // CALR,NODE,out,k,target,code,measured,iters,rail
    int c = 0;
    size_t pos = 0;
    while (c < 6) { pos = l.find(',', pos) + 1; ++c; }
    const double logged = atof(l.c_str() + pos);
    CHECK(std::fabs(logged - med) < 0.0006);
    CHECK(kFinalReadings == 10 && kReadings == 3);
  }
  CHECK(found);
}

// The modelled run time: what one Self run costs at a few READ? latencies.
static void test_run_time_model() {
  const uint32_t lat[] = {450, 530, 650};
  for (uint32_t ms : lat) {
    Rig r;
    r.meter.read_ms = ms;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Results && r.flow.CanSave());
    uint32_t t_in = 0;  // when the input part began
    for (const auto &l : g_log) {
      if (l.compare(0, 10, "CALR,STATE") != 0 || l.find(",PROMPT_PATCH,") == std::string::npos) continue;
      unsigned long t = 0;
      sscanf(l.c_str(), "CALR,STATE,%lu,", &t);
      t_in = uint32_t(t);
      break;
    }
    const double outs_min = (t_in - r.t_begin) / 60000.0;
    printf("  model, READ? %3u ms: %.1f min (outputs %.1f, inputs %.1f), %d READ?\n",
           unsigned(ms), r.Minutes(), outs_min, r.Minutes() - outs_min, r.meter.reads);
    if (ms == 530) CHECK(r.Minutes() > 15.0 && r.Minutes() < 24.0);
  }
  // Each routine alone, at 530 ms per READ?.
  {
    Rig o;
    o.Begin(Mode::Outputs);
    CHECK(o.Run() && o.flow.stage() == Stage::Results);
    Rig i;
    i.ins.Realistic();
    i.Begin(Mode::Inputs);
    CHECK(i.Run() && i.flow.stage() == Stage::Results);
    printf("  model, READ? 530 ms: Outputs %.1f min, Inputs %.1f min (patch check + sweep, no meter)\n",
           o.Minutes(), i.Minutes());
    CHECK(i.Minutes() < 1.5);
  }
}

// The 4-byte Cal Routine marker (EEPROM [4140,4144)).
static void test_marker() {
  uint8_t b[4];
  CalRoutineMarkerEncode(kCalRoutineMarkerStored, b);
  CHECK(b[0] == 'C' && b[1] == 'R' && b[2] == kCalRoutineMarkerVersion && b[3] == 1);
  CHECK(CalRoutineMarkerStored(b));
  CalRoutineMarkerEncode(0, b);
  CHECK(!CalRoutineMarkerStored(b));
  const uint8_t erased[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t zero[4] = {0, 0, 0, 0};
  const uint8_t newer[4] = {'C', 'R', 2, 1};
  const uint8_t other[4] = {'A', 'M', 1, 1};
  CHECK(!CalRoutineMarkerStored(erased));
  CHECK(!CalRoutineMarkerStored(zero));
  CHECK(!CalRoutineMarkerStored(newer));
  CHECK(!CalRoutineMarkerStored(other));
  CHECK(CalRoutineMarkerStart == 4140 && CalRoutineMarkerSize == 4);
}

// ---- save ----------------------------------------------------------------------

struct FakeSave : SavePort {
  std::vector<std::string> calls;
  std::string fail_at;
  uint8_t curve_mask = 0, line_mask = 0;
  uint8_t src[8] = {};
  uint16_t table00 = 0;
  bool Step(const char *name) { calls.push_back(name); return fail_at != name; }
  bool SaveDacTable(const uint16_t t[8][CR::kNodes]) override {
    table00 = t[0][1];
    return Step("dac");
  }
  bool SaveCurves(const int16_t[8][CR::kCurvePoints], uint8_t m, const uint8_t s[8]) override {
    curve_mask = m;
    memcpy(src, s, 8);
    return Step("curves");
  }
  float verify[8] = {};
  bool SaveLines(const CM::Fit[8], uint8_t m, const uint8_t[8], const float v[8]) override {
    line_mask = m;
    memcpy(verify, v, sizeof(verify));
    return Step("lines");
  }
  void WriteMarker() override { Step("marker"); }
  void ResetAutotune() override { Step("autotune"); }
  void ReloadInputs() override { Step("reload"); }
};

static CalRoutineFlow::Results passing_results() {
  CalRoutineFlow::Results r;
  memset(&r, 0, sizeof(r));
  r.mode = uint8_t(Mode::Self);
  for (int i = 0; i < 8; ++i) { r.out_pass[i] = true; r.in_pass[i] = true; }
  r.in_pass[3] = false;
  r.table[0][1] = 6553;
  return r;
}

static void test_save_sequence_order() {
  FakeSave s;
  const auto r = passing_results();
  CHECK(CalRoutineSave(r, s));
  const std::vector<std::string> want = {"dac", "curves", "lines", "marker", "autotune", "reload"};
  CHECK(s.calls == want);
  CHECK(s.curve_mask == 0xF7 && s.line_mask == 0xF7);
  for (int i = 0; i < 8; ++i) CHECK(s.src[i] == i + 1);  // input n's source is output n
  CHECK(s.table00 == 6553);
  // Through the flow, after a real run.
  Rig rig;
  rig.Begin();
  CHECK(rig.Run());
  FakeSave fs;
  CHECK(rig.flow.Save(fs));
  CHECK(fs.calls == want);
  CHECK(fs.curve_mask == 0xFF);
  for (int i = 0; i < 8; ++i) CHECK(fs.verify[i] == rig.flow.results().in_line_dev_mv[i]);
  CHECK(rig.flow.stage() == Stage::Idle);
  CHECK(count_prefix("CALR,SAVE,OK") == 1);
}

static void test_save_refused_unless_can_save() {
  FakeSave s;
  auto r = passing_results();
  r.out_pass[6] = false;
  CHECK(!CalRoutineSave(r, s));
  CHECK(s.calls.empty());
  // The flow refuses outside Results, and after a cancel.
  Rig rig;
  rig.Begin();
  CHECK(rig.Run([&] { return rig.flow.stage() == Stage::PromptPatch; }));
  FakeSave fs;
  CHECK(!rig.flow.Save(fs));
  CHECK(fs.calls.empty());
  rig.flow.Cancel();
  CHECK(!CalRoutineSave(rig.flow.results(), fs));
  CHECK(fs.calls.empty());
  // Discard writes nothing.
  Rig d;
  d.Begin();
  CHECK(d.Run());
  d.flow.Discard();
  CHECK(d.flow.stage() == Stage::Idle);
  CHECK(count_prefix("CALR,DISCARD") == 1);
}

static void test_save_stops_on_failure() {
  const char *steps[] = {"dac", "curves", "lines"};
  for (int i = 0; i < 3; ++i) {
    FakeSave s;
    s.fail_at = steps[i];
    CHECK(!CalRoutineSave(passing_results(), s));
    CHECK(int(s.calls.size()) == i + 1);
    CHECK(!s.calls.empty() && s.calls.back() == steps[i]);
    for (const auto &c : s.calls) CHECK(c != "marker");
  }
  Rig rig;
  rig.Begin();
  CHECK(rig.Run());
  FakeSave fs;
  fs.fail_at = "curves";
  CHECK(!rig.flow.Save(fs));
  CHECK(rig.flow.stage() == Stage::Results);  // still there, to try again or discard
  CHECK(count_prefix("CALR,SAVE,FAIL") == 1);
}

// Results: a failed output is measured again, alone, and the run comes back
// to Results; output A again also redoes the inputs (their curves hang on it).
static void test_retry_output() {
  Rig r;
  bool noisy = true;
  r.meter.extra = [&](int n) { return noisy && r.outs.held == 2 ? ((n & 1) ? 1.0 : -1.0) : 0.0; };
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(!r.flow.results().out_pass[2]);
  CHECK(r.flow.results().out_reason[2] == CalRoutineFlow::OUT_NOISY);
  CHECK(!r.flow.CanSave());
  CalRoutineFlow::Results before = r.flow.results();
  CHECK(!r.flow.RetryOutput(8) && !r.flow.RetryOutput(-1));
  CHECK(r.flow.stage() == Stage::Results);

  noisy = false;  // the lead was reseated
  const size_t sent0 = r.meter.sent.size();
  CHECK(r.flow.RetryOutput(2));
  CHECK(r.flow.retry_output() == 2);
  CHECK(r.flow.stage() == Stage::ConfigMeter);  // set up again first
  CHECK(count_prefix("CALR,REDO,3") == 1);
  CHECK(r.Run([&] { return r.flow.stage() == Stage::PromptOutput; }));
  CHECK(r.flow.current_output() == 2);
  const size_t ns = sizeof(kMeterSetup) / sizeof(kMeterSetup[0]);
  for (size_t i = 0; i < ns && r.meter.sent.size() > sent0 + ns; ++i)
    CHECK(r.meter.sent[sent0 + i] == kMeterSetup[i]);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(r.flow.retry_output() == -1);
  CHECK(!r.hold_rule_broken && r.outs.held == -1);
  const auto &res = r.flow.results();
  CHECK(res.out_pass[2]);
  CHECK(r.flow.CanSave());
  CHECK(count_prefix("CALR,OUT,3") == 2);
  // Every input hangs on its own output, so after a retry in Self the patch
  // is checked and all 8 inputs are measured again (input 3 on C's new table).
  CHECK(count_prefix("CALR,IN,") == 16);
  CHECK(count_prefix("CALR,PRES,PASS") == 2);
  for (int o = 0; o < 8; ++o) {
    if (o == 2) continue;
    CHECK(res.out_pass[o] == before.out_pass[o]);
    for (int k = 0; k < CR::kNodes; ++k) CHECK(res.table[o][k] == before.table[o][k]);
  }
  for (int i = 0; i < 8; ++i) CHECK(res.in_pass[i]);
  CHECK(count_prefix("CALR,RESULT,1,FF,") == 1);

  // Output A again: the same.
  CHECK(r.flow.RetryOutput(0));
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(count_prefix("CALR,OUT,1") == 2);
  CHECK(count_prefix("CALR,IN,") == 24);
  CHECK(r.flow.CanSave());
  FakeSave fs;
  CHECK(r.flow.Save(fs));

  // Refused outside Results; a cancel mid-retry releases everything.
  Rig c;
  c.Begin();
  CHECK(!c.flow.RetryOutput(0));  // ConfigMeter
  CHECK(c.Run([&] { return c.flow.stage() == Stage::PromptPatch; }));
  CHECK(!c.flow.RetryOutput(1));
  CHECK(c.Run());
  CHECK(c.flow.RetryOutput(5));
  CHECK(c.Run([&] { return c.flow.stage() == Stage::SweepOutput && c.outs.held == 5; }));
  c.flow.Cancel();
  CHECK(c.flow.stage() == Stage::Idle && c.outs.held == -1 && !c.flow.CanSave());
  CHECK(c.flow.retry_output() == -1);

  // The meter gone when the retry is asked for: NoMeter, then on with it.
  Rig g;
  g.Begin();
  CHECK(g.Run());
  g.meter.connected = false;
  CHECK(g.flow.RetryOutput(4));
  CHECK(g.flow.stage() == Stage::NoMeter);
  for (int i = 0; i < 100; ++i) g.Step();
  CHECK(g.flow.stage() == Stage::NoMeter);
  g.meter.connected = true;
  CHECK(g.Run());
  CHECK(g.flow.stage() == Stage::Results && g.flow.CanSave());
  CHECK(count_prefix("CALR,OUT,5") == 2);
}

// ---- Q -------------------------------------------------------------------------

static std::vector<std::string> fixture_lines(const char *file, const char *prefix) {
  std::vector<std::string> out;
  std::ifstream f(file);
  std::string line;
  while (std::getline(f, line))
    if (line.compare(0, strlen(prefix), prefix) == 0) out.push_back(line);
  return out;
}

// The console's own printf formats before the move, kept here verbatim
// (CvMeter.cpp FlowPrepare / FlowTask / FlowFinish) to prove the new
// formatters print the same bytes.
static std::string legacy_point(int in, int k, int32_t mv, const CM::PointStats &s) {
  char a[64], b[64], out[256];
  CM::FormatFixed(a, sizeof(a), CM::ScaleRound(s.mean_raw, 2), 2);
  CM::FormatFixed(b, sizeof(b), CM::ScaleRound(s.max_raw - s.min_raw, 2), 2);
  snprintf(out, sizeof(out), "INCAL,POINT,in=%d,%d,%ld,%s,%s,%lu", in + 1, k + 1, (long)mv, a, b,
           (unsigned long)s.samples);
  return out;
}
static std::string legacy_result(int in, int out, const CM::Fit &fit, float resid, float noise,
                                 CM::Verdict v) {
  char g[64], o[64], r[64], n[64], line[384];  // 4 x 63-char fields + the fixed text is ~285
  CM::FormatFixed(g, sizeof(g), CM::ScaleRound(fit.gain * 1000.f, 1), 1);
  CM::FormatFixed(o, sizeof(o), CM::ScaleRound(fit.offset_mv, 2), 2);
  CM::FormatFixed(r, sizeof(r), CM::ScaleRound(resid, 2), 2);
  CM::FormatFixed(n, sizeof(n), CM::ScaleRound(noise, 2), 2);
  snprintf(line, sizeof(line), "INCAL,RESULT,in=%d,out=%d,gain_uv=%s,offset_mv=%s,resid_mv=%s,"
           "noise_pp=%s,verdict=%s", in + 1, out + 1, g, o, r, n, CM::VerdictName(v));
  return line;
}

// The Q's input (2) gets the scripted point; the other channels of the window
// get something else, which the Q must not read.
struct ScriptedInputs : InputPort {
  CM::PointStats pts[3];
  int i = 0;
  void StartWindow() override {}
  bool WindowDone(CM::PointStats s[8]) override {
    for (int c = 0; c < 8; ++c) s[c] = {1.f, 0.f, 2000.f, 7};
    s[2] = pts[i++ % 3];
    return true;
  }
};

static void test_q_lines_unchanged() {
  const char *fx = "fixtures/cvmeter_lines.txt";
  const auto patch = fixture_lines(fx, "INCAL,PATCH,");
  const auto point = fixture_lines(fx, "INCAL,POINT,");
  const auto result = fixture_lines(fx, "INCAL,RESULT,");
  CHECK(patch.size() == 1 && point.size() == 3 && result.size() == 1);
  if (patch.size() != 1 || point.size() != 3 || result.size() != 1) return;

  const int32_t mv[3] = {1200, 10800, 6000};
  const uint16_t code[3] = {6553, 58982, 32768};
  char b[CalRoutineLines::kMaxLine];
  CalRoutineLines::IncalPatch(b, sizeof(b), 2, 0, mv);
  CHECK(patch[0] == b);

  // The Q flow on the shared input stage, scripted readings.
  SimDac dac;
  FakeClock clk;
  FakeOutputs outs(&dac);
  ScriptedInputs ins;
  ins.pts[0] = {3685.5f, 3684.5f, 3686.5f, 3333};
  ins.pts[1] = {409.75f, 408.5f, 410.5f, 3334};
  ins.pts[2] = {2047.25f, 2046.25f, 2048.0f, 3333};
  g_log.clear();
  CalQFlow q;
  q.SetLog(capture);
  q.Begin(outs, ins, clk, 2, 0, mv, code, 15360, 1200);
  CHECK(outs.held == 0 && outs.held_code == code[0]);
  int guard = 0;
  uint32_t t0 = clk.t;
  while (q.Tick() != CalQFlow::State::Done && ++guard < 100000) {
    if (outs.held != -1 && outs.held != 0) CHECK(false);
    clk.t += 5;
  }
  CHECK(q.state() == CalQFlow::State::Done);
  CHECK(q.verdict() == CM::VERDICT_OK);
  CHECK(outs.held == -1);
  const uint32_t took = clk.t - t0;
  CHECK(took >= 3 * (kInSettleMs + kInMeasureMs) && took < 3 * (kInSettleMs + kInMeasureMs) + 100);
  CHECK(g_log.size() == 4);
  if (g_log.size() == 4) {
    for (int i = 0; i < 3; ++i) {
      CHECK(g_log[i] == point[i]);
      if (g_log[i] != point[i]) printf("  got:  %s\n  want: %s\n", g_log[i].c_str(), point[i].c_str());
    }
    CHECK(g_log[3] == result[0]);
    if (g_log[3] != result[0]) printf("  got:  %s\n  want: %s\n", g_log[3].c_str(), result[0].c_str());
  }
  // The new formatters print exactly what the console printed before.
  uint32_t lcg = 7;
  for (int t = 0; t < 2000; ++t) {
    lcg = lcg * 1103515245u + 12345u;
    const float m = float(lcg % 409600u) / 100.f;
    const CM::PointStats s = {m, m - float(lcg % 7) * 0.37f, m + float(lcg % 5) * 0.41f, lcg % 5000u};
    CalRoutineLines::IncalPoint(b, sizeof(b), int(lcg % 8), int(lcg % 3), int32_t(lcg % 24000) - 12000, s);
    CHECK(legacy_point(int(lcg % 8), int(lcg % 3), int32_t(lcg % 24000) - 12000, s) == b);
    const CM::Fit f = {-2.9f - float(lcg % 1000) * 1e-4f, float(lcg % 2000000) / 100.f - 10000.f};
    const float res = float(int(lcg % 4000) - 2000) / 97.f;
    const CM::Verdict v = CM::Verdict(lcg % 9);
    CalRoutineLines::IncalResult(b, sizeof(b), int(lcg % 8), int((lcg >> 4) % 8), f, res, m / 1000.f, v);
    CHECK(legacy_result(int(lcg % 8), int((lcg >> 4) % 8), f, res, m / 1000.f, v) == b);
  }
  // Cancel mid-point releases the output.
  CalQFlow c;
  c.Begin(outs, ins, clk, 0, 1, mv, code, 15360, 1200);
  CHECK(outs.held == 1);
  c.Tick();
  c.Cancel();
  CHECK(outs.held == -1 && c.state() == CalQFlow::State::Idle);
}

static void test_q_commit_drops_curve() {
  InputCurveRecord rec;
  memset(&rec, 0, sizeof(rec));
  rec.valid_mask = 0x29;  // inputs 0, 3, 5
  CHECK(CalRoutineDropCurve(rec, 3));
  CHECK(rec.valid_mask == 0x21);
  CHECK(!CalRoutineDropCurve(rec, 3));  // nothing to drop: no save needed
  CHECK(!CalRoutineDropCurve(rec, 1));
  CHECK(!CalRoutineDropCurve(rec, 8) && !CalRoutineDropCurve(rec, -1));
  CHECK(rec.valid_mask == 0x21);
}

// ---- CALR line formats ---------------------------------------------------------

static size_t fields(const std::string &l) {
  size_t n = 1;
  for (char c : l) n += c == ',';
  return n;
}

static void test_calr_lines_golden() {
  const char *fx = "fixtures/calr_lines.txt";
  const auto heads = fixture_lines(fx, "CALR#,");
  CHECK(int(heads.size()) == CalRoutineLines::HeaderCount());
  for (int i = 0; i < CalRoutineLines::HeaderCount() && i < int(heads.size()); ++i)
    CHECK(heads[i] == CalRoutineLines::Header(i));

  std::map<std::string, std::string> want;  // SUB -> the fixture's example line
  for (const auto &l : fixture_lines(fx, "CALR,")) {
    const size_t a = l.find(',') + 1, b = l.find(',', a);
    want[l.substr(a, b == std::string::npos ? std::string::npos : b - a)] = l;
  }
  char b[CalRoutineLines::kMaxLine];
  auto is = [&](const char *sub) {
    const bool ok = want.count(sub) && want[sub] == b;
    CHECK(ok);
    if (!ok) printf("  %s got: %s\n  want: %s\n", sub, b, want.count(sub) ? want[sub].c_str() : "(none)");
  };
  using namespace CalRoutineLines;
  Meter(b, sizeof(b), "Siglent Technologies,SDM3065X,SDM36GBX000000,1.01.01.25\r\n"); is("METER");
  State(b, sizeof(b), 123456, "CAL_OUT", 3, -1); is("STATE");
  Out(b, sizeof(b), 2); is("OUT");
  Node(b, sizeof(b), 2, 7, 8400, 45873, 8400.0937, 3, false); is("NODE");
  Reread(b, sizeof(b), 0, 3600, 19661, 0.734); is("REREAD");
  Sweep(b, sizeof(b), 4, 7440, 40630, 7440.4719, true); is("SWEEP");
  OutResult(b, sizeof(b), 5, true, "OK", 0.4126f, 10560, 0x400); is("ORES");
  In(b, sizeof(b), 0, 0, 600, 10800); is("IN");
  const CM::PointStats s = {3889.37f, 3888.25f, 3890.5f, 3333};
  InPoint(b, sizeof(b), 6, 600, 3277, s, true); is("IPT");
  const CM::Fit f = {-2.93049f, 12000.41f};
  InResult(b, sizeof(b), 3, "PASS", CM::VERDICT_OK, f, 1.234f, 4.567f, 19); is("IRES");
  Retry(b, sizeof(b), "READ?", 2); is("RETRY");
  MeterError(b, sizeof(b), "SETUP", "-113,\"Undefined header\"\n"); is("MERR");
  Lost(b, sizeof(b), 3); is("LOST");
  Back(b, sizeof(b), 3); is("BACK");
  Redo(b, sizeof(b), 2); is("REDO");
  Result(b, sizeof(b), true, 0xFF, 0xF7); is("RESULT");
  Save(b, sizeof(b), true); is("SAVE");
  Discard(b, sizeof(b)); is("DISCARD");
  Cancel(b, sizeof(b), "SWEEP_OUT"); is("CANCEL");
  Fail(b, sizeof(b), "METER_TIMEOUT"); is("FAIL");
  RunMode(b, sizeof(b), "SELF"); is("MODE");
  Warn(b, sizeof(b), "NO_MARKER"); is("WARN");
  Patch(b, sizeof(b), 2, 0x10, 3.25f); is("PATCH");
  PatchResult(b, sizeof(b), "FAIL", 0x14, 0x08); is("PRES");
  // Every SUB in the fixture has a header and the example has its field count.
  std::map<std::string, size_t> nf;
  for (int i = 1; i < HeaderCount(); ++i) {
    const std::string h = Header(i);
    const size_t a = h.find(',') + 1, e = h.find(',', a);
    nf[h.substr(a, e == std::string::npos ? std::string::npos : e - a)] = fields(h);
  }
  for (const auto &kv : want) {
    CHECK(nf.count(kv.first));
    CHECK(nf[kv.first] == fields(kv.second));
  }
  CHECK(want.size() == nf.size());

  // A full run, with a retry, a reread, a skipped input and a save: every
  // line is a header line or a CALR line of a known SUB with its field count.
  Rig r;
  r.meter.extra = [](int n) { return (n >= 5 && n < 8) ? ((n & 1) ? 0.5 : -0.5) : 0.0; };
  r.Begin();
  r.ins.patch[2] = -1;  // input 3 unpatched: the patch check fails, A skips it
  CHECK(r.Run([&] { return r.meter.reads >= 40; }));
  r.meter.fail_next = 1;
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::PatchFailed);
  r.flow.Skip();
  CHECK(r.Run());
  FakeSave fs;
  CHECK(r.flow.Save(fs));
  int heads_seen = 0;
  for (const auto &l : g_log) {
    if (l.compare(0, 6, "CALR#,") == 0) {
      CHECK(heads_seen < HeaderCount() && l == Header(heads_seen));
      ++heads_seen;
      continue;
    }
    CHECK(l.compare(0, 5, "CALR,") == 0);
    const size_t a = l.find(',') + 1, e = l.find(',', a);
    const std::string sub = l.substr(a, e == std::string::npos ? std::string::npos : e - a);
    const bool known = nf.count(sub) && nf[sub] == fields(l);
    CHECK(known);
    if (!known) printf("  irregular: %s\n", l.c_str());
    CHECK(l.size() < kMaxLine);
  }
  CHECK(heads_seen == HeaderCount());
  for (const char *sub : {"CALR,METER,", "CALR,STATE,", "CALR,OUT,", "CALR,NODE,", "CALR,REREAD,",
                          "CALR,SWEEP,", "CALR,ORES,", "CALR,IN,", "CALR,IPT,", "CALR,IRES,",
                          "CALR,RETRY,", "CALR,RESULT,", "CALR,SAVE,", "CALR,MODE,",
                          "CALR,PATCH,", "CALR,PRES,"})
    CHECK(count_prefix(sub) > 0);
}

// ---- separate routines; all-8 inputs; patch check -------------------------------

static std::vector<std::string> stages_seen() {
  std::vector<std::string> v;
  for (const auto &l : g_log) {
    if (l.compare(0, 11, "CALR,STATE,") != 0) continue;
    const size_t a = l.find(',', 11) + 1, b = l.find(',', a);
    v.push_back(l.substr(a, b - a));
  }
  return v;
}
static bool saw(const std::vector<std::string> &v, const char *stage) {
  return std::find(v.begin(), v.end(), std::string(stage)) != v.end();
}

static void test_mode_outputs_saves_only_tables() {
  Rig r;
  r.Begin(Mode::Outputs);
  CHECK(count_prefix("CALR,MODE,OUTPUTS") == 1);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  const auto st = stages_seen();
  CHECK(saw(st, "CAL_OUT") && saw(st, "SWEEP_OUT"));
  CHECK(!saw(st, "PROMPT_PATCH") && !saw(st, "CHECK_PATCH") && !saw(st, "CAL_IN"));
  CHECK(r.ins.windows == 0);
  CHECK(r.flow.results().HasOutputs() && !r.flow.results().HasInputs());
  CHECK(r.flow.CanSave());  // no input passed, and none is needed
  FakeSave fs;
  CHECK(r.flow.Save(fs));
  const std::vector<std::string> want = {"dac", "marker", "autotune"};
  CHECK(fs.calls == want);
  // A failed output still blocks it.
  auto res = passing_results();
  res.mode = uint8_t(Mode::Outputs);
  for (int i = 0; i < 8; ++i) res.in_pass[i] = false;
  FakeSave a;
  CHECK(CalRoutineSave(res, a) && a.calls == want);
  res.out_pass[4] = false;
  FakeSave b;
  CHECK(!CalRoutineSave(res, b) && b.calls.empty());
}

static void test_mode_inputs_saves_only_curves() {
  Rig r;
  r.meter.connected = false;  // no meter is needed
  r.Begin(Mode::Inputs);
  CHECK(count_prefix("CALR,MODE,INPUTS") == 1);
  CHECK(r.flow.stage() == Stage::PromptPatch);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(r.meter.sent.empty());
  const auto st = stages_seen();
  CHECK(!saw(st, "CAL_OUT") && !saw(st, "NOMETER") && !saw(st, "CONFIG"));
  const auto &res = r.flow.results();
  CHECK(!res.HasOutputs() && res.HasInputs());
  for (int o = 0; o < 8; ++o)
    for (int k = 0; k < CR::kNodes; ++k) CHECK(res.table[o][k] == r.outs.stored[o][k]);
  for (int i = 0; i < 8; ++i) CHECK(res.in_pass[i]);
  CHECK(r.flow.CanSave());
  FakeSave fs;
  CHECK(r.flow.Save(fs));
  const std::vector<std::string> want = {"curves", "lines", "reload"};
  CHECK(fs.calls == want);
  CHECK(fs.curve_mask == 0xFF && fs.line_mask == 0xFF);
  for (int i = 0; i < 8; ++i) CHECK(fs.src[i] == i + 1);
  // One passed input is enough; none is not. Outputs play no part.
  auto x = passing_results();
  x.mode = uint8_t(Mode::Inputs);
  for (int i = 0; i < 8; ++i) { x.in_pass[i] = i == 6; x.out_pass[i] = false; }
  FakeSave a;
  CHECK(CalRoutineSave(x, a) && a.calls == want && a.curve_mask == 0x40);
  x.in_pass[6] = false;
  FakeSave b;
  CHECK(!CalRoutineSave(x, b) && b.calls.empty());
}

// The bench's inputs (0 V near raw 3640, ~2.75 mV per count, top near 10.65 V),
// each bent differently, patched A->1 ... H->8: one sweep holds all 8 outputs
// at each point (each through its own table) and reads all 8 inputs in one
// window.
// The stored tables of a unit whose outputs were calibrated before (what
// Inputs alone relies on).
static void calibrate_stored(Rig &r) {
  for (int o = 0; o < 8; ++o) {
    uint16_t t[CR::kNodes];
    bool rail[CR::kNodes];
    ref_table(r.dac, o, r.outs.stored[o], 0.0, t, rail);
    memcpy(r.outs.stored[o], t, sizeof(t));
  }
}

static void test_inputs_all8_one_sweep() {
  Rig r;
  r.ins.Realistic();
  calibrate_stored(r);
  r.Begin(Mode::Inputs);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(!r.hold_rule_broken && r.outs.mask == 0);
  CHECK(!r.ins.short_window);
  const auto &res = r.flow.results();
  // Windows: the patch check's 9 (one low, then each output raised), then one
  // per sweep point with all 8 outputs held.
  CHECK(r.ins.windows >= 9 + 15);
  int sweep_windows = 0;
  for (size_t w = 9; w < r.ins.window_mask.size(); ++w) {
    CHECK(r.ins.window_mask[w] == 0xFF);
    ++sweep_windows;
  }
  CHECK(sweep_windows <= kMaxInputPoints);
  // Every input is logged at every point until its own top ends its sweep:
  // all 8 at the first point, and each input's used points plus at most the
  // one that ended it.
  int ipts = 0, first = 0, used_sum = 0;
  for (const auto &l : g_log) {
    if (l.compare(0, 9, "CALR,IPT,") != 0) continue;
    ++ipts;
    int in, mv;
    if (sscanf(l.c_str(), "CALR,IPT,%d,%d,", &in, &mv) == 2 && mv == kInStepMv) ++first;
  }
  for (int i = 0; i < 8; ++i) used_sum += r.flow.results().in_points[i];
  CHECK(first == 8);
  CHECK(ipts >= used_sum && ipts <= used_sum + 8 && ipts <= 8 * sweep_windows);
  for (int i = 0; i < 8; ++i) {
    CHECK(res.in_pass[i]);
    CHECK(res.in_verdict[i] == CM::VERDICT_OK);
    CHECK(res.in_points[i] >= 15);
    CHECK(res.in_lo_mv[i] == kInStepMv);
    CHECK(res.in_hi_mv[i] >= 9600 && res.in_hi_mv[i] <= 10800);  // the input's own top
    CHECK(res.in_holdout_mv[i] <= 1.0f);
    double worst = 0;
    for (int b = 0; b < CR::kCurvePoints; ++b) {
      const double t = true_mv_at(r.ins, i, double(b << CR::kCurveShift));
      if (t < res.in_lo_mv[i] || t > res.in_hi_mv[i]) continue;
      worst = std::fmax(worst, std::fabs(res.curve_mv_x2[i][b] / 2.0 - t));
    }
    CHECK(worst <= 1.0);
    if (worst > 1.0) printf("  input %d: worst curve error %.2f mV\n", i + 1, worst);
  }
  // Each point's code for input n is output n's own table at that voltage.
  for (const auto &l : g_log) {
    int in, mv, code;
    if (sscanf(l.c_str(), "CALR,IPT,%d,%d,%d,", &in, &mv, &code) != 3) continue;
    CHECK(code == r.outs.CodeForMv(in - 1, r.outs.stored[in - 1], mv));
  }
  CHECK(r.Minutes() < 1.5);
}

static void test_patch_check_catches_swap() {
  Rig r;
  r.ins.patch[2] = 4;  // input 3 has output E
  r.ins.patch[4] = 2;  // input 5 has output C
  r.Begin(Mode::Inputs);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::PatchFailed);
  CHECK(r.outs.mask == 0);
  CHECK(r.flow.patch_crossed() == 0x14);   // outputs C and E
  CHECK(r.flow.patch_missing() == 0x14);   // ... and neither moved its own input
  CHECK(r.flow.patch_moved(2) == 0x10);    // C moves input 5
  CHECK(r.flow.patch_moved(4) == 0x04);    // E moves input 3
  for (int o = 0; o < 8; ++o)
    if (o != 2 && o != 4) CHECK(r.flow.patch_moved(o) == (1u << o));
  CHECK(count_prefix("CALR,PATCH,3,10,") == 1);
  CHECK(count_prefix("CALR,PATCH,5,04,") == 1);
  CHECK(count_prefix("CALR,PRES,FAIL,14,14") == 1);
  CHECK(count_prefix("CALR,IPT,") == 0);  // nothing measured on a wrong patch
  r.flow.Skip();  // a crossing cannot be skipped
  CHECK(r.flow.stage() == Stage::PatchFailed);
  // Fixed: Continue checks again and runs.
  r.ins.patch[2] = 2;
  r.ins.patch[4] = 4;
  r.flow.Continue();
  CHECK(r.flow.stage() == Stage::CheckPatch);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(count_prefix("CALR,PRES,PASS,00,00") == 1);
  for (int i = 0; i < 8; ++i) CHECK(r.flow.results().in_pass[i]);
}

static void test_patch_check_catches_missing() {
  Rig r;
  r.ins.patch[3] = -1;  // nothing in input 4
  r.Begin(Mode::Inputs);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::PatchFailed);
  CHECK(r.flow.patch_crossed() == 0);
  CHECK(r.flow.patch_missing() == 0x08);
  CHECK(r.flow.patch_moved(3) == 0);
  CHECK(count_prefix("CALR,PATCH,4,00,") == 1);
  CHECK(count_prefix("CALR,PRES,FAIL,00,08") == 1);
  // A goes on without it.
  r.flow.Skip();
  CHECK(r.flow.stage() == Stage::CalInput);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  const auto &res = r.flow.results();
  CHECK(res.in_skipped == 0x08);
  CHECK(!res.in_pass[3]);
  for (int i = 0; i < 8; ++i)
    if (i != 3) CHECK(res.in_pass[i]);
  CHECK(count_prefix("CALR,IRES,4,SKIP,") == 1);
  CHECK(count_prefix("CALR,IN,4,") == 0);
  CHECK(r.flow.CanSave());
  // A stacked cable (output B into inputs 2 and 3, C nowhere) is a crossing.
  Rig m;
  m.ins.patch[2] = 1;
  m.Begin(Mode::Inputs);
  CHECK(m.Run());
  CHECK(m.flow.stage() == Stage::PatchFailed);
  CHECK(m.flow.patch_moved(1) == 0x06 && m.flow.patch_crossed() == 0x02 &&
        m.flow.patch_missing() == 0x04);
}

static void test_inputs_warn_without_marker() {
  {
    Rig r;
    r.Begin(Mode::Inputs, false);
    CHECK(r.flow.stage() == Stage::WarnMarker);
    CHECK(count_prefix("CALR,WARN,NO_MARKER") == 1);
    for (int i = 0; i < 100; ++i) r.Step();
    CHECK(r.flow.stage() == Stage::WarnMarker && r.outs.holds == 0);
    r.flow.Continue();  // go on anyway
    CHECK(r.flow.stage() == Stage::PromptPatch);
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Results && r.flow.CanSave());
  }
  {
    Rig r;
    r.Begin(Mode::Inputs, false);
    r.flow.Cancel();  // or not
    CHECK(r.flow.stage() == Stage::Idle && r.outs.holds == 0);
  }
  {
    Rig r;
    r.Begin(Mode::Inputs, true);
    CHECK(r.flow.stage() == Stage::PromptPatch);
    CHECK(count_prefix("CALR,WARN,") == 0);
    // The marker only matters to Inputs: Self calibrates the outputs first.
    Rig s;
    s.Begin(Mode::Self, false);
    CHECK(s.flow.stage() == Stage::ConfigMeter && count_prefix("CALR,WARN,") == 0);
  }
}

static void test_self_runs_both() {
  Rig r;
  r.outs.stored[3][5] = uint16_t(r.outs.stored[3][5] + 400);  // D's stored table is off
  r.Begin(Mode::Self);
  CHECK(count_prefix("CALR,MODE,SELF") == 1);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Results);
  const auto st = stages_seen();
  // Outputs first, then the patch, its check and the inputs.
  const auto first = [&](const char *s) {
    return int(std::find(st.begin(), st.end(), std::string(s)) - st.begin());
  };
  CHECK(first("CAL_OUT") < first("PROMPT_PATCH"));
  CHECK(first("PROMPT_PATCH") < first("CHECK_PATCH"));
  CHECK(first("CHECK_PATCH") < first("CAL_IN"));
  CHECK(first("CAL_IN") < first("RESULTS"));
  CHECK(!saw(st, "WARN_MARKER"));
  const auto &res = r.flow.results();
  CHECK(res.HasOutputs() && res.HasInputs());
  for (int i = 0; i < 8; ++i) CHECK(res.out_pass[i] && res.in_pass[i]);
  // The inputs ran on the new tables, not the stored ones.
  CHECK(res.table[3][5] != r.outs.stored[3][5]);
  for (const auto &l : g_log) {
    int in, mv, code;
    if (sscanf(l.c_str(), "CALR,IPT,%d,%d,%d,", &in, &mv, &code) != 3) continue;
    CHECK(code == r.outs.CodeForMv(in - 1, res.table[in - 1], mv));
  }
  FakeSave fs;
  CHECK(r.flow.Save(fs));
  const std::vector<std::string> want = {"dac", "curves", "lines", "marker", "autotune", "reload"};
  CHECK(fs.calls == want);
}

// A window with no samples (the ADC path stalled) is no reading, not raw 0:
// the check fails as a measurement failure, never as crossings.
static void test_patch_check_no_reading() {
  for (int dead_at = 0; dead_at < 2; ++dead_at) {  // the all-low window, then output A's
    Rig r;
    r.Begin(Mode::Inputs);
    r.flow.Continue();
    CHECK(r.Run([&] { return r.ins.windows >= dead_at; }));
    r.ins.dead_windows = 1;
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::PatchFailed);
    CHECK(r.flow.patch_no_reading());
    CHECK(r.flow.patch_crossed() == 0 && r.flow.patch_missing() == 0);
    CHECK(count_prefix("CALR,PRES,NO_READING,00,00") == 1);
    CHECK(r.outs.mask == 0);
    r.flow.Skip();  // nothing to skip
    CHECK(r.flow.stage() == Stage::PatchFailed);
    r.flow.Continue();  // checks again, and now passes
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Results && r.flow.CanSave());
  }
}

// Self with a patch that cannot be fixed now: the passed outputs are kept and
// saved as an Outputs run.
static void test_self_crossed_patch_can_save_outputs() {
  Rig r;
  r.ins.patch[2] = 4;
  r.ins.patch[4] = 2;
  r.Begin(Mode::Self);
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::PatchFailed);
  CHECK(!r.flow.CanSave());
  CHECK(r.flow.KeepOutputsOnly());
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(r.outs.mask == 0);
  const auto &res = r.flow.results();
  CHECK(res.HasOutputs() && !res.HasInputs());
  CHECK(r.flow.mode() == Mode::Outputs);
  CHECK(count_prefix("CALR,MODE,OUTPUTS") == 1);
  CHECK(count_prefix("CALR,RESULT,1,FF,00") == 1);
  CHECK(r.flow.CanSave());
  FakeSave fs;
  CHECK(r.flow.Save(fs));
  const std::vector<std::string> want = {"dac", "marker", "autotune"};
  CHECK(fs.calls == want);
  // Only from a failed patch check in Self.
  Rig i;
  i.ins.patch[3] = -1;
  i.Begin(Mode::Inputs);
  CHECK(i.Run() && i.flow.stage() == Stage::PatchFailed);
  CHECK(!i.flow.KeepOutputsOnly() && i.flow.stage() == Stage::PatchFailed);
  Rig o;
  o.Begin(Mode::Self);
  CHECK(o.Run([&] { return o.flow.stage() == Stage::PromptPatch; }));
  CHECK(!o.flow.KeepOutputsOnly() && o.flow.stage() == Stage::PromptPatch);
}

int main() {
  test_happy_path_all_pass();
  test_results_match_math();
  test_meter_timeout_releases_all();
  test_query_retries_after_transient_errors();
  test_cancel_any_stage_releases_all();
  test_noisy_point_retries_not_stores();
  test_no_meter_stays_nometer();
  test_holds_only_current_output();
  test_failed_input_is_uncalibrated();
  test_rail_does_not_block_save();
  test_rail_overshoot_remeasured();
  test_reconnect_restarts_current_output();
  test_input_span_capped_below_source_rail();
  test_rebegin_releases();
  test_meter_lines_allowed();
  test_meter_config_error_fails();
  test_throwaway_after_code_change();
  test_node_final_uses_10_readings();
  test_run_time_model();
  test_retry_output();
  test_marker();
  test_save_sequence_order();
  test_save_refused_unless_can_save();
  test_save_stops_on_failure();
  test_q_lines_unchanged();
  test_q_commit_drops_curve();
  test_calr_lines_golden();
  test_mode_outputs_saves_only_tables();
  test_mode_inputs_saves_only_curves();
  test_inputs_all8_one_sweep();
  test_patch_check_catches_swap();
  test_patch_check_catches_missing();
  test_inputs_warn_without_marker();
  test_self_runs_both();
  test_patch_check_no_reading();
  test_self_crossed_patch_can_save_outputs();
  printf("test_calroutine_flow: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
