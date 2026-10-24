// Host tests for the calibration routine's step machine (src/CalRoutineFlow.h)
// against simulated hardware: a bowed DAC per output, a USB meter that reads
// it (with noise, latency, transient errors, unplugging), and bent ADC inputs
// that read output A. Also the console Q on the same input stage, the save
// sequence and the CALR/INCAL line formats (fixtures/calr_lines.txt,
// fixtures/cvmeter_lines.txt). Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_calroutine_flow
//     test_calroutine_flow.cpp ../src/CalRoutineFlow.cpp ../src/CalRoutineMath.cpp
//     ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
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
#include "../src/InputCurveStore.h"

using namespace CalRoutine;
namespace CM = CvMeterMath;
namespace CR = CalRoutineMath;
typedef CalRoutineFlow::Stage Stage;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static const double kPi = 3.14159265358979323846;

// ---- log capture -------------------------------------------------------------

static std::vector<std::string> g_log;
static void capture(const char *line) { g_log.push_back(line); }
static int count_prefix(const char *p) {
  int n = 0;
  for (const auto &l : g_log) n += l.compare(0, strlen(p), p) == 0;
  return n;
}

// ---- simulated hardware ------------------------------------------------------

struct FakeClock : Clock {
  uint32_t t = 0xFFFF0000u;  // wraps during every run
  uint32_t Ms() const override { return t; }
};

// Output o: mV = nominal * (1 + gain) + offset + bow * sin(pi x), clipped at clip.
struct SimDac {
  // Full scale a little above 12 V on every output, as on the hardware.
  double gain[8] = {0.004, 0.006, 0.002, 0.009, 0.003, 0.011, 0.005, 0.007};
  double offset[8] = {-2.0, -1.5, -1.0, -0.5, -0.8, -1.2, -0.3, -2.5};
  double bow[8] = {4.0, 3.0, 2.0, 4.0, 1.0, 3.5, 2.5, 3.0};
  double clip[8] = {1e9, 1e9, 1e9, 1e9, 1e9, 1e9, 1e9, 1e9};
  std::function<double(int, uint16_t)> custom;  // replaces the model when set
  double Mv(int o, uint16_t code) const {
    if (custom) return custom(o, code);
    const double x = code / 65535.0;
    const double v = x * 12000.0 * (1.0 + gain[o]) + offset[o] + bow[o] * std::sin(kPi * x);
    return std::fmin(v, clip[o]);
  }
};

struct FakeOutputs : OutputPort {
  const SimDac *dac;
  uint16_t stored[8][CR::kNodes];
  int held = -1;
  uint16_t held_code = 0;
  int holds = 0, releases = 0;
  bool two_held = false;  // a second output was held while one already was
  explicit FakeOutputs(const SimDac *d) : dac(d) {
    for (int o = 0; o < 8; ++o)
      for (int k = 0; k < CR::kNodes; ++k)
        stored[o][k] = uint16_t(std::lround(k * 1200 * double(CR::kNominalCodesPerMv)));
  }
  void Hold(int out, uint16_t code) override {
    if (held >= 0 && held != out) two_held = true;
    held = out;
    held_code = code;
    ++holds;
  }
  void ReleaseAll() override { held = -1; ++releases; }
  uint16_t TableCode(int out, int k) const override { return stored[out][k]; }
  uint16_t CodeForMv(int, const uint16_t t[CR::kNodes], int32_t mv) const override {
    if (mv <= 0) return t[0];
    if (mv >= 12000) return t[10];
    const int k = mv / 1200;
    const double f = (mv - k * 1200) / 1200.0;
    return uint16_t(std::lround(t[k] + f * (double(t[k + 1]) - double(t[k]))));
  }
  double OutMv(int o) const { return held == o ? dac->Mv(o, held_code) : 0.0; }
};

// What a reading looks like after the meter's NR3 text and ParseReading.
static double via_text(double mv) {
  char b[48];
  snprintf(b, sizeof(b), "%+.9E", mv / 1000.0);
  return strtod(b, nullptr) * 1000.0;
}

struct FakeMeter : MeterPort {
  FakeOutputs *outs;
  FakeClock *clk;
  bool connected = true;
  bool never_answer = false;
  uint32_t read_ms = 400;   // NPLC 10 + autozero at 50 Hz
  uint32_t cmd_ms = 20;
  int fail_next = 0;        // next N completions report an error
  int refuse_begin = 0;     // next N Begin() calls are refused (driver recovering)
  double offset_mv = 0.0;   // reading offset (a replug moves it)
  double noise_mv = 0.02;   // uniform +/-
  std::function<double(int read_index)> extra;  // added to READ? #n (0-based)
  std::vector<std::string> sent;
  std::vector<std::pair<int, uint16_t>> read_at;  // (held output, code) at each READ?
  int reads = 0;
  bool busy = false;
  uint32_t ready_at = 0;
  std::string cur;
  char reply[64] = {};
  uint32_t lcg = 1;

  FakeMeter(FakeOutputs *o, FakeClock *c) : outs(o), clk(c) {}
  bool Begin(const char *scpi, uint32_t) override {
    if (!connected || busy) return false;
    if (refuse_begin > 0) { --refuse_begin; return false; }
    sent.push_back(scpi);
    cur = scpi;
    busy = true;
    ready_at = clk->t + (cur == "READ?" ? read_ms : cmd_ms);
    return true;
  }
  int Poll(const char **r) override {
    if (!busy) return -1;
    if (!connected) { busy = false; return -1; }
    if (never_answer) return 0;
    if (int32_t(clk->t - ready_at) < 0) return 0;
    busy = false;
    if (fail_next > 0) { --fail_next; return -1; }
    reply[0] = 0;
    if (cur == "READ?") {
      lcg = lcg * 1103515245u + 12345u;
      const double u = double((lcg >> 8) & 0xFFFF) / 65535.0 * 2.0 - 1.0;
      double mv = (outs->held >= 0 ? outs->OutMv(outs->held) : 0.0) + offset_mv + u * noise_mv;
      if (extra) mv += extra(reads);
      ++reads;
      read_at.push_back({outs->held, outs->held_code});
      snprintf(reply, sizeof(reply), "%+.9E\n", mv / 1000.0);
    }
    *r = reply;
    return 1;
  }
  bool Connected() const override { return connected; }
  const char *Identity() const override { return "Siglent Technologies,SDM3065X,SDM36GBX000000,1.01.01.25\n"; }
};

// Input i reads output A: raw = 4095 - v * 4096/12000 * (1 + gain) - offset + bend sin.
struct FakeInputs : InputPort {
  FakeOutputs *outs;
  FakeClock *clk;
  double gain[8] = {0.01, -0.012, 0.005, 0.0, -0.008, 0.015, -0.004, 0.009};
  double offset[8] = {2.0, -3.0, 1.0, 0.5, -1.5, 2.5, 0.0, -2.0};
  double bend[8] = {3.0, 2.0, 4.0, 1.0, 2.5, 3.5, 1.5, 2.0};
  double pp[8] = {2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
  int started = -1;
  uint32_t start_ms = 0;
  const CalRoutineFlow *flow = nullptr;  // when set: only its current input may be sampled
  int windows = 0;
  bool short_window = false;  // WindowDone asked before 600 ms
  FakeInputs(FakeOutputs *o, FakeClock *c) : outs(o), clk(c) {}
  double Raw(int i, double v) const {
    double r = 4095.0 - v * (4096.0 / 12000.0) * (1.0 + gain[i]) - offset[i] +
               bend[i] * std::sin(kPi * v / 12000.0);
    return std::fmax(0.0, std::fmin(4095.0, r));
  }
  void StartWindow(int in) override {
    if (flow) CHECK(in == flow->current_input());
    started = in;
    start_ms = clk->t;
  }
  bool WindowDone(CM::PointStats *s) override {
    if (clk->t - start_ms < 600) short_window = true;
    const double v = outs->OutMv(kSourceOutput);
    const double m = Raw(started, v);
    s->mean_raw = float(m);
    s->min_raw = float(std::fmax(0.0, m - pp[started] / 2));
    s->max_raw = float(std::fmin(4095.0, m + pp[started] / 2));
    s->samples = 3333;
    ++windows;
    return true;
  }
};

struct Rig {
  SimDac dac;
  FakeClock clk;
  FakeOutputs outs{&dac};
  FakeMeter meter{&outs, &clk};
  FakeInputs ins{&outs, &clk};
  CalRoutineFlow flow;
  uint32_t t_begin = 0;
  bool hold_rule_broken = false;  // a Hold on anything but the output under test
  Rig() {
    g_log.clear();
    flow.SetLog(capture);
    ins.flow = &flow;
  }
  void Begin() {
    t_begin = clk.t;
    flow.Begin(meter, outs, ins, clk);
  }
  void Step(uint32_t ms = 5) {
    flow.Tick();
    const Stage s = flow.stage();
    if (outs.held >= 0) {
      const bool out_stage = s == Stage::CalOutput || s == Stage::SweepOutput;
      const bool in_stage = s == Stage::CalInput;
      if (out_stage && outs.held != flow.current_output()) hold_rule_broken = true;
      if (in_stage && outs.held != kSourceOutput) hold_rule_broken = true;
      if (!out_stage && !in_stage) hold_rule_broken = true;
    }
    clk.t += ms;
  }
  // Runs, pressing Continue at every prompt, until `until` returns true or
  // the run ends. Returns false on the time limit.
  bool Run(std::function<bool()> until = nullptr, uint32_t limit_ms = 60u * 60u * 1000u) {
    const uint32_t t0 = clk.t;
    while (clk.t - t0 < limit_ms) {
      const Stage s = flow.stage();
      if (until && until()) return true;
      if (s == Stage::Results || s == Stage::Failed || s == Stage::Idle) return true;
      if (s == Stage::PromptOutput || s == Stage::PromptInput) flow.Continue();
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
  // The meter setup, in order, before anything else.
  CHECK(r.meter.sent.size() > 5);
  for (int i = 0; i < 5 && r.meter.sent.size() > 5; ++i) CHECK(r.meter.sent[i] == kMeterSetup[i]);
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
                          Stage::CalOutput, Stage::SweepOutput, Stage::PromptInput,
                          Stage::CalInput, Stage::Results, Stage::Failed};
  for (Stage want : stages) {
    Rig r;
    if (want == Stage::NoMeter) r.meter.connected = false;
    r.Begin();
    bool reached = false;
    const uint32_t t0 = r.clk.t;
    while (r.clk.t - t0 < 60u * 60u * 1000u) {
      const Stage s = r.flow.stage();
      if (s == want && (want != Stage::CalOutput || r.outs.held >= 0) &&
          (want != Stage::CalInput || r.outs.held >= 0)) { reached = true; break; }
      if (s == Stage::Results && want != Stage::Results) break;
      if (want == Stage::Failed && s == Stage::CalOutput) r.meter.never_answer = true;
      if (want != Stage::PromptOutput && s == Stage::PromptOutput) r.flow.Continue();
      if (want != Stage::PromptInput && s == Stage::PromptInput) r.flow.Continue();
      if (s == Stage::NoMeter && want == Stage::NoMeter) { reached = true; break; }
      r.Step();
    }
    CHECK(reached);
    r.flow.Cancel();
    CHECK(r.flow.stage() == Stage::Idle);
    CHECK(r.outs.held == -1);
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
    // Measurement #4 (READ? 15..19) of output A: a loose lead, 1 mV p-p.
    r.meter.extra = [](int n) { return (n >= 15 && n < 20) ? ((n & 1) ? 0.5 : -0.5) : 0.0; };
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
  CHECK(!r.outs.two_held);
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
  // Skip leaves an input uncalibrated too.
  Rig s;
  s.Begin();
  CHECK(s.Run([&] { return s.flow.stage() == Stage::PromptInput && s.flow.current_input() == 5; }));
  s.flow.Skip();
  CHECK(s.Run());
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
  CHECK(r.meter.sent.size() > sent_before + 5);
  for (int i = 0; i < 5 && r.meter.sent.size() > sent_before + 5; ++i)
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
    const double r = 4095.0 - mid * (4096.0 / 12000.0) * (1.0 + ins.gain[i]) - ins.offset[i] +
                     ins.bend[i] * std::sin(kPi * mid / 12000.0);
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
    const int32_t lo = bottom ? 1200 : 600, hi = 10800;
    char want[64];
    snprintf(want, sizeof(want), "CALR,IN,1,1,%ld,%ld", (long)lo, (long)hi);
    CHECK(count_prefix(want) == 1);
    for (const auto &l : g_log) {
      if (l.compare(0, 9, "CALR,IPT,") != 0) continue;
      int in, mv;
      if (sscanf(l.c_str(), "CALR,IPT,%d,%d,", &in, &mv) == 2) CHECK(mv >= lo && mv <= hi);
    }
    for (int i = 0; i < 8; ++i) {
      CHECK(res.in_pass[i]);
      CHECK(res.in_holdout_mv[i] <= 1.0f);
      // Every breakpoint inside the measured span is within 1 mV of the truth.
      double worst = 0;
      for (int b = 0; b < CR::kCurvePoints; ++b) {
        const double t = true_mv_at(r.ins, i, double(b << CR::kCurveShift));
        if (t < lo || t > hi) continue;
        worst = std::fmax(worst, std::fabs(res.curve_mv_x2[i][b] / 2.0 - t));
      }
      CHECK(worst <= 1.0);
      if (worst > 1.0) printf("  input %d: worst curve error %.2f mV in %ld..%ld mV\n", i + 1,
                              worst, (long)lo, (long)hi);
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
  CHECK(r.Run([&] { return r.flow.stage() == Stage::CalInput && r.outs.held >= 0; }));
  r.flow.Begin(r.meter, r.outs, r.ins, r.clk);
  CHECK(r.outs.held == -1);
  CHECK(r.flow.stage() == Stage::ConfigMeter);
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
  bool SaveLines(const CM::Fit[8], uint8_t m, const uint8_t[8]) override {
    line_mask = m;
    return Step("lines");
  }
  void WriteMarker() override { Step("marker"); }
  void ResetAutotune() override { Step("autotune"); }
  void ReloadInputs() override { Step("reload"); }
};

static CalRoutineFlow::Results passing_results() {
  CalRoutineFlow::Results r;
  memset(&r, 0, sizeof(r));
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
  for (int i = 0; i < 8; ++i) CHECK(s.src[i] == 1);
  CHECK(s.table00 == 6553);
  // Through the flow, after a real run.
  Rig rig;
  rig.Begin();
  CHECK(rig.Run());
  FakeSave fs;
  CHECK(rig.flow.Save(fs));
  CHECK(fs.calls == want);
  CHECK(fs.curve_mask == 0xFF);
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
  CHECK(rig.Run([&] { return rig.flow.stage() == Stage::PromptInput; }));
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
  char a[64], b[64], out[200];
  CM::FormatFixed(a, sizeof(a), CM::ScaleRound(s.mean_raw, 2), 2);
  CM::FormatFixed(b, sizeof(b), CM::ScaleRound(s.max_raw - s.min_raw, 2), 2);
  snprintf(out, sizeof(out), "INCAL,POINT,in=%d,%d,%ld,%s,%s,%lu", in + 1, k + 1, (long)mv, a, b,
           (unsigned long)s.samples);
  return out;
}
static std::string legacy_result(int in, int out, const CM::Fit &fit, float resid, float noise,
                                 CM::Verdict v) {
  char g[64], o[64], r[64], n[64], line[256];
  CM::FormatFixed(g, sizeof(g), CM::ScaleRound(fit.gain * 1000.f, 1), 1);
  CM::FormatFixed(o, sizeof(o), CM::ScaleRound(fit.offset_mv, 2), 2);
  CM::FormatFixed(r, sizeof(r), CM::ScaleRound(resid, 2), 2);
  CM::FormatFixed(n, sizeof(n), CM::ScaleRound(noise, 2), 2);
  snprintf(line, sizeof(line), "INCAL,RESULT,in=%d,out=%d,gain_uv=%s,offset_mv=%s,resid_mv=%s,"
           "noise_pp=%s,verdict=%s", in + 1, out + 1, g, o, r, n, CM::VerdictName(v));
  return line;
}

struct ScriptedInputs : InputPort {
  CM::PointStats pts[3];
  int i = 0;
  void StartWindow(int) override {}
  bool WindowDone(CM::PointStats *s) override { *s = pts[i++ % 3]; return true; }
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
  Lost(b, sizeof(b), 3); is("LOST");
  Back(b, sizeof(b), 3); is("BACK");
  Result(b, sizeof(b), true, 0xFF, 0xF7); is("RESULT");
  Save(b, sizeof(b), true); is("SAVE");
  Discard(b, sizeof(b)); is("DISCARD");
  Cancel(b, sizeof(b), "SWEEP_OUT"); is("CANCEL");
  Fail(b, sizeof(b), "METER_TIMEOUT"); is("FAIL");
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
  r.meter.extra = [](int n) { return (n >= 15 && n < 20) ? ((n & 1) ? 0.5 : -0.5) : 0.0; };
  r.Begin();
  CHECK(r.Run([&] { return r.meter.reads >= 40; }));
  r.meter.fail_next = 1;
  CHECK(r.Run([&] { return r.flow.stage() == Stage::PromptInput && r.flow.current_input() == 2; }));
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
                          "CALR,RETRY,", "CALR,RESULT,", "CALR,SAVE,"})
    CHECK(count_prefix(sub) > 0);
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
  test_save_sequence_order();
  test_save_refused_unless_can_save();
  test_save_stops_on_failure();
  test_q_lines_unchanged();
  test_q_commit_drops_curve();
  test_calr_lines_golden();
  printf("test_calroutine_flow: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
