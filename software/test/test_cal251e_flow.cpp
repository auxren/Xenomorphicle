// Host tests for the Cal Routine's 251e target (src/Cal251eFlow.h): the step machine against
// simulated hardware. Xeno outputs E-H are bowed DACs (SimDac) whose stored tables are
// calibrated (as after an Outputs run); the Xeno's inputs 1-4 are a model of the calibrated
// input stage (a per-input gain/offset error between the true chain and the calibration, noise,
// a top); the 251e is Sim251e, with a configurable analog error model and an injectable
// wrong-scale stage. Also the CALR 251 line formats (fixtures/cal251e_lines.txt).
// Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_cal251e_flow
//     test_cal251e_flow.cpp ../src/Cal251eFlow.cpp ../src/Cal251eBus.cpp ../src/CalRec.cpp
//     ../src/CalRec251e.cpp ../src/CalRoutineFlow.cpp ../src/CalRoutineMeasure.cpp
//     ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
// The write is compiled out of the firmware (Cal251eFlow::kAllowWrite); the write checks run
// in the variant built with -DCAL251E_ALLOW_WRITE=1 (run-host-tests.sh).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "fake_meter.h"
#include "sim251e.h"
// Two tests poke the flow's private state (the fit's target constant; the result between
// Results and ConfirmWrite). Every standard header is in by now.
#include <algorithm>
#include <deque>
#define private public
#include "../src/Cal251eFlow.h"
#undef private

typedef Cal251eFlow::Stage Stage;
typedef Cal251eFlow F;
namespace CM = CvMeterMath;
namespace R = CalRec251e;

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
static bool has_line(const std::string &prefix) {
  for (const auto &x : g_log)
    if (x.compare(0, prefix.size(), prefix) == 0) return true;
  return false;
}

// ---- simulated hardware ------------------------------------------------------

struct FakeEntropy : EntropyPort {
  int fills = 0;
  void Fill(uint8_t *d, int n) override {
    ++fills;
    for (int i = 0; i < n; ++i) d[i] = uint8_t(0xA0 + i);
  }
  uint32_t UnixTime() override { return 1761264000u; }
};

struct Rig;

// The Xeno's input stage. The true chain turns the voltage at the jack into a raw count; the
// calibration (what Cal Routine stored) turns a raw count back into millivolts. They agree
// unless a test makes them differ (gain_err, off_mv: what the loopback is there to catch).
struct Chain {
  double top_true = 10600.0, top_cal = 10600.0;   // mV at raw 0
  double g = 2.918;                                // mV per raw count
  double gain_err[8] = {};                         // the true chain reads V*(1+e)+off
  double off_mv[8] = {};
  double noise_mv = 0.15;                          // uniform +-, on each window's mean
  double pp_counts = 6.0;                          // peak-to-peak of the smoothed reading
  double drift_mv_per_s = 0.0;                     // all inputs, from the clock
  uint32_t lcg = 5;
  double Raw(int k, double v, double drift) const {
    return (top_true - (v * (1.0 + gain_err[k]) + off_mv[k] + drift)) / g;
  }
  double Noise() {
    lcg = lcg * 1103515245u + 12345u;
    return (double((lcg >> 8) & 0xFFFF) / 65535.0 * 2.0 - 1.0) * noise_mv;
  }
};

enum { SRC_NONE = 0, SRC_XENO = 1, SRC_251 = 2 };

struct FakeInputs : CalRoutine::InputPort {
  Rig *rig;
  double v0[8] = {};
  uint32_t t_start = 0;
  int windows = 0;
  bool dead = false;       // the port never delivers samples
  explicit FakeInputs(Rig *r) : rig(r) {}
  void StartWindow() override;
  bool WindowDone(CM::PointStats s[8]) override;
};

struct FakeMv : F::MvPort {
  Chain *chain;
  uint8_t mask = 0xFF;
  explicit FakeMv(Chain *c) : chain(c) {}
  bool Calibrated(int label) const override { return mask & (1u << label); }
  float Millivolts(int, float raw) const override { return float(chain->top_cal - double(raw) * chain->g); }
};

struct Rig {
  SimDac dac;
  FakeClock clk;
  FakeOutputs outs{&dac};
  Sim251e m;
  FakeEntropy ent;
  Chain chain;
  FakeInputs ins{this};
  FakeMv mvp{&chain};
  Cal251eFlow flow;
  uint16_t stored_before[8][CR::kNodes];

  int src_kind[8] = {};
  int src_idx[8] = {};
  bool auto_ui = true;
  bool hold_confirm = false;     // the UI leaves the write confirm unanswered
  bool bad_hold = false;         // an output other than E-H was ever held
  bool to_251 = true;            // the UI moves the cables to the 251e at the second prompt
  int swap_a = -1, swap_b = -1;  // 251e channels whose cables are crossed
  int skip_ch = -1;              // a 251e channel with no cable
  int prompts_loop = 0, prompts_patch = 0;

  Rig() {
    outs.clk = &clk;
    for (int o = 0; o < 8; ++o)
      for (int k = 0; k < CR::kNodes; ++k) {
        int lo = 0, hi = 65535;
        while (lo < hi) {
          const int mid = (lo + hi) / 2;
          if (dac.Mv(o, uint16_t(mid)) < k * 1200.0) lo = mid + 1;
          else hi = mid;
        }
        outs.stored[o][k] = uint16_t(lo);
      }
    memcpy(stored_before, outs.stored, sizeof(stored_before));
    m.clock = &clk;
    m.drift_t0 = clk.t;
  }

  void PatchLoop() {
    for (int k = 0; k < 4; ++k) { src_kind[k] = SRC_XENO; src_idx[k] = 4 + k; }
  }
  void Patch251() {
    for (int k = 0; k < 4; ++k) {
      src_kind[k] = k == skip_ch ? SRC_NONE : SRC_251;
      src_idx[k] = k;
    }
    if (swap_a >= 0) { src_idx[swap_a] = swap_b; src_idx[swap_b] = swap_a; }
  }
  double VoltAt(int k) {
    if (src_kind[k] == SRC_XENO) return outs.OutMv(src_idx[k]);
    if (src_kind[k] == SRC_251) return m.OutMv(src_idx[k]);
    return 0.0;
  }

  void Begin(bool self_cal = true) {
    g_log.clear();
    flow.Begin(outs, ins, mvp, clk, m, ent, self_cal, capture);
  }
  void Ui() {
    switch (flow.stage()) {
      case Stage::Warmup: flow.Continue(); break;
      case Stage::PromptLoop: ++prompts_loop; PatchLoop(); flow.Continue(); break;
      case Stage::PromptPatch:
        ++prompts_patch;
        if (to_251) Patch251();
        flow.Continue();
        break;
      case Stage::Results: flow.Continue(); break;
      case Stage::Confirm:
        if (!hold_confirm) flow.ConfirmWrite();
        break;
      default: break;
    }
  }
  void Step() {
    clk.t += 1;
    flow.Tick();
    if (outs.mask & ~0xF0u) bad_hold = true;
    if (auto_ui) Ui();
  }
  bool Terminal() const {
    const Stage s = flow.stage();
    return s == Stage::Done || s == Stage::Failed || s == Stage::NoSelfCal || s == Stage::Idle;
  }
  // Steps until pred() or a terminal stage; false on timeout.
  bool Run(std::function<bool()> pred = nullptr, uint32_t max_ms = 900000) {
    for (uint32_t i = 0; i < max_ms; ++i) {
      if (pred && pred()) return true;
      if (Terminal()) return pred ? pred() : true;
      Step();
    }
    return false;
  }
  // The end of a read-only run, or of a write run (Done).
  bool RunToEnd() {
    return Run([this] {
      const Stage s = flow.stage();
      if (F::kAllowWrite) return s == Stage::Done || s == Stage::Failed;
      return s == Stage::Results || s == Stage::Failed || s == Stage::NoSelfCal;
    });
  }
  void Idle(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) Step();
  }
};

void FakeInputs::StartWindow() {
  for (int k = 0; k < 8; ++k) v0[k] = rig->VoltAt(k);
  t_start = rig->clk.t;
}
bool FakeInputs::WindowDone(CM::PointStats s[8]) {
  ++windows;
  const double drift = rig->chain.drift_mv_per_s * double(rig->clk.Abs()) / 1000.0;
  for (int k = 0; k < 8; ++k) {
    // The mean of the voltage over the window: its two ends (a hold that lapsed or a code that
    // changed mid-window shows as a mean between them).
    const double v = 0.5 * (v0[k] + rig->VoltAt(k));
    const double raw = rig->chain.Raw(k, v, drift) + rig->chain.Noise() / rig->chain.g;
    s[k].samples = dead ? 0 : 800;
    s[k].mean_raw = float(raw);
    s[k].min_raw = float(raw - rig->chain.pp_counts / 2);
    s[k].max_raw = float(raw + rig->chain.pp_counts / 2);
  }
  return true;
}

// What the module really puts on a jack for a stage code through a record: the truth the flow
// never sees. Worst |V - code * K| over the codes the analog stage can reach.
static double TruthWorst(Sim251e &m, int ch, const CalRec::Channel &c) {
  double w = 0.0;
  for (int code = 0; code <= R::kStockCodeAtTop; code += 5) {
    const double want = double(code) * R::kMvPerCode;
    if (want < m.AnalogMv(ch, 0)) continue;   // below the stage's own floor
    const double e = std::fabs(m.AnalogMv(ch, R::Apply((uint16_t)code, c.gain, c.offset)) - want);
    if (e > w) w = e;
  }
  return w;
}
static double TruthWorstSeq(Sim251e &m, int ch) {   // through the module's own sequencer path
  double w = 0.0;
  const int save = m.seq_code[ch];
  for (int code = 0; code <= R::kStockCodeAtTop; code += 5) {
    const double want = double(code) * R::kMvPerCode;
    if (want < m.AnalogMv(ch, 0)) continue;
    m.seq_code[ch] = code;
    const double e = std::fabs(m.OutMv(ch) - want);
    if (e > w) w = e;
  }
  m.seq_code[ch] = save;
  return w;
}

static void ExpectNoWrite(Rig &r) {
  CHECK(r.m.count[CalRec::kCmdPut] == 0);
  CHECK(r.m.count[CalRec::kCmdCommit] == 0);
}
static void ExpectQuiet(Rig &r) {   // every hold let go, nothing left to send
  CHECK(r.outs.mask == 0);
  CHECK(!r.flow.background_work());
  for (int c = 0; c < 4; ++c) CHECK(r.m.ovr[c] == -1);
  CHECK(r.m.bad_frames == 0);
  CHECK(!r.bad_hold);
  CHECK(memcmp(r.outs.stored, r.stored_before, sizeof(r.stored_before)) == 0);  // Xeno cal untouched
}

static void install(Sim251e &m, uint16_t seq, uint8_t id0) {
  R::Record rec;
  for (int i = 0; i < 8; ++i) rec.unit_id[i] = uint8_t(id0 + i);
  rec.seq = seq;
  rec.date = 1700000000u;
  for (int c = 0; c < 4; ++c) rec.ch[c] = {R::kGainOne, 0};
  uint8_t b[38];
  R::Encode(rec, b);
  m.Install(b);
}

// ---- tests -----------------------------------------------------------------------

static void good_unit_passes() {
  Rig r;
  r.Begin();
  CHECK(r.RunToEnd());
  CHECK(r.flow.fail() == F::F_NONE);
  const F::Result &res = r.flow.result();
  CHECK(res.valid_mask == 15);
  CHECK(res.loop_worst_mv < 1.0f);
  for (int c = 0; c < 4; ++c) {
    const double want_a = R::kMvPerCode * (1.0 + r.m.gain_err[c]);
    CHECK(std::fabs(res.a_mv[c] - want_a) / want_a < 0.003);
    CHECK(std::fabs(res.b_mv[c] - r.m.zero_mv[c]) < 4.0);
    CHECK(res.nonlin_mv[c] <= F::kNonlinMv);
    CHECK(std::fabs(res.verify_mv[c]) <= F::kVerifyMvLimit);
    CHECK(res.anchor_mv[c] <= R::kAnchorTolMv);
    // The truth, which the flow cannot see: the stage was 1-2 % off and the record fixes it.
    const CalRec::Channel unity = {R::kGainOne, 0};
    CHECK(TruthWorst(r.m, c, unity) > 40.0);
    CHECK(TruthWorst(r.m, c, res.ch[c]) < 4.0);
  }
  // Only Xeno E-H held, all let go; the 251e's overrides refreshed in time and released.
  CHECK(r.m.lapses == 0);
  r.Idle(50);
  CHECK(r.m.releases >= 1);
  ExpectQuiet(r);
  // The record, frames and CALR lines.
  CHECK(count_prefix("CALR,251START,5C") == 1);
  CHECK(count_prefix("CALR,251LOOP,") == 4 * F::kLoopPoints);
  CHECK(count_prefix("CALR,251PT,") == 4 * F::kPoints);
  CHECK(count_prefix("CALR,251LINE,") == 4);
  CHECK(count_prefix("CALR,251FIT,") == 4);
  CHECK(count_prefix("CALR,251STAT,") == 4 && count_prefix("CALR,251WARN,") == 0);
  CHECK(res.warn_nonlin == 0 && res.warn_verify == 0 && res.warn_anchor == 0);
  for (int c = 0; c < 4; ++c) {   // the report: shape of the stage, noise and the DAC step
    CHECK(res.bow_mv[c] > 0.05f && res.bow_mv[c] < 3.0f);
    CHECK(res.inl_mv[c] < 3.0f);
    // the chain's window noise is uniform +-0.15 mV: rms 0.087, and the report must say so (not 2x)
    CHECK(res.noise_mv[c] > 0.02f && res.noise_mv[c] < 0.16f);
    CHECK(std::fabs(res.a_mv[c] - R::kMvPerCode * (1.0 + r.m.gain_err[c])) < 0.01);   // lsb in 251STAT
  }
  CHECK(has_line("CALR,251OLD,NONE"));
  const double mean_noise = (res.noise_mv[0] + res.noise_mv[1] + res.noise_mv[2] + res.noise_mv[3]) / 4.0;
  CHECK(mean_noise > 0.05 && mean_noise < 0.11);     // 0.087 expected; a doubled figure is 0.17
  CHECK(r.prompts_loop >= 1 && r.prompts_patch >= 1);
  if (!F::kAllowWrite) {
    CHECK(r.flow.stage() == Stage::Results);
    ExpectNoWrite(r);
    CHECK(!r.m.has_active);
    CHECK(count_prefix("CALR,251REC,") == 0);
  }
}

// The default build is measuring-only: Continue at Results goes nowhere, ConfirmWrite does
// nothing at any stage, and the module sees no PUT or COMMIT whatever the UI does.
static void read_only_build_never_writes() {
  if (F::kAllowWrite) return;
  Rig r;
  r.Begin();
  r.auto_ui = true;
  const bool ok = r.Run([&] { r.flow.ConfirmWrite(); return r.flow.stage() == Stage::Results; });
  CHECK(ok);
  for (int i = 0; i < 20; ++i) { r.flow.Continue(); r.flow.ConfirmWrite(); r.Idle(100); }
  CHECK(r.flow.stage() == Stage::Results);
  CHECK(r.flow.result().valid_mask == 15);
  ExpectNoWrite(r);
  r.Idle(11u * 60u * 1000u);     // a Results screen left open stays readable (no expiry read-only)
  CHECK(r.flow.stage() == Stage::Results);
  r.flow.Cancel();
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED);
  r.flow.Cancel();
  CHECK(r.flow.stage() == Stage::Idle);
  ExpectNoWrite(r);
  ExpectQuiet(r);
}

static void no_self_cal_or_uncalibrated_inputs() {
  Rig r;
  r.Begin(false);
  CHECK(r.flow.stage() == Stage::NoSelfCal && r.flow.fail() == F::F_NO_SELF_CAL);
  r.Idle(100);
  CHECK(r.m.log.empty() && r.outs.holds == 0);
  Rig u;
  u.mvp.mask = 0xFF & ~(1u << 2);       // input 3 never calibrated
  u.Begin();
  CHECK(u.flow.stage() == Stage::NoSelfCal && u.flow.fail() == F::F_INPUT_UNCAL);
  u.Idle(100);
  CHECK(u.m.log.empty() && u.outs.holds == 0);
  u.flow.Continue();
  CHECK(u.flow.stage() == Stage::NoSelfCal);   // nothing moves it on
  u.flow.Cancel();
  CHECK(u.flow.stage() == Stage::Idle);
  Rig o;                                  // inputs 5-8 uncalibrated do not matter
  o.mvp.mask = 0x0F;
  o.Begin();
  CHECK(o.RunToEnd() && o.flow.fail() == F::F_NONE);
}

static void no_251e_on_the_bus() {
  { Rig r; r.m.drop_replies = true; r.Begin(); r.Run(); CHECK(r.flow.fail() == F::F_NO_251E); ExpectNoWrite(r);
    CHECK(r.m.count[CalRec::kCmdOverride] == 0); CHECK(r.outs.holds == 0); }
  { Rig r; r.m.status_fmt = 2; r.Begin(); r.Run(); CHECK(r.flow.fail() == F::F_NO_251E); }
  { Rig r; r.m.addr = 0x5D; r.Begin(); r.Run(); CHECK(r.flow.fail() == F::F_NO_251E); }
  { Rig r; r.m.busy_sends = -1; r.Begin(); r.Run(); CHECK(r.flow.fail() == F::F_BUS); }
  { Rig r; r.m.nak = 2; r.Begin(); r.Run(); CHECK(r.flow.fail() == F::F_BUS); }
}

// The previous record is read and logged before anything else (a measuring-only run exercises
// the READ path), and a record that reads wrong is logged as an error, not fatal.
static void existing_record_is_logged() {
  Rig r;
  install(r.m, 5, 0x40);
  r.Begin();
  CHECK(r.RunToEnd());
  CHECK(r.flow.fail() == F::F_NONE);
  CHECK(r.flow.had_record() && r.flow.record_seq() == 5);
  std::string hex;
  char h[3];
  for (int i = 0; i < 38; ++i) { snprintf(h, sizeof h, "%02X", r.m.active[i]); hex += h; }
  if (!F::kAllowWrite) CHECK(has_line("CALR,251OLD," + hex));
  Rig b;
  install(b.m, 5, 0x40);
  b.m.corrupt_read_arg = 30;
  b.Begin();
  CHECK(b.RunToEnd());
  CHECK(has_line("CALR,251OLD,ERR_9"));       // E_INCONSISTENT
  if (!F::kAllowWrite) CHECK(b.flow.fail() == F::F_NONE);
}

// ---- the loopback: the measurement chain is checked before the 251e is touched -----------

static void loopback_catches_a_bad_chain() {
  for (int which = 0; which < 4; ++which) {
    Rig r;
    r.chain.gain_err[which] = 0.0003;      // 0.03 %: 2.9 mV at 9.6 V
    r.Begin();
    r.Run();
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_LOOPBACK);
    CHECK(r.flow.result().loop_worst_mv > 2.0f);
    CHECK(r.m.count[CalRec::kCmdOverride] == 0);     // the 251e was never driven
    ExpectNoWrite(r);
    ExpectQuiet(r);
  }
  Rig o;                                   // an offset instead
  o.chain.off_mv[2] = 2.5;
  o.Begin();
  o.Run();
  CHECK(o.flow.fail() == F::F_LOOPBACK);
  Rig p;                                   // just inside the limit passes the loopback
  p.chain.off_mv[1] = 1.4;
  p.Begin();
  CHECK(p.RunToEnd() && p.flow.fail() == F::F_NONE);
  CHECK(p.flow.result().loop_worst_mv > 1.0f && p.flow.result().loop_worst_mv <= 2.0f);
}

static void loopback_wrong_cable() {
  Rig r;
  r.Begin();
  r.Run([&] { return r.flow.stage() == Stage::PromptLoop; });
  // the operator leaves input 3 unpatched
  r.auto_ui = false;
  r.PatchLoop();
  r.src_kind[2] = SRC_NONE;
  r.flow.Continue();
  r.auto_ui = true;
  r.Run();
  CHECK(r.flow.fail() == F::F_PATCH);
  CHECK(r.m.count[CalRec::kCmdOverride] == 0);
  ExpectQuiet(r);
}

static void noisy_input_stops_the_run() {
  Rig r;
  r.chain.pp_counts = 30.0;
  r.Begin();
  r.Run();
  CHECK(r.flow.fail() == F::F_NOISY);
  ExpectNoWrite(r);
  ExpectQuiet(r);
  Rig d;
  d.ins.dead = true;
  d.Begin();
  d.Run();
  CHECK(d.flow.fail() == F::F_NO_INPUT);
  ExpectQuiet(d);
}

// ---- identity: each 251e channel reaches its own input ------------------------------------

static void identity_checks() {
  {
    Rig r;
    r.swap_a = 0; r.swap_b = 1;             // CV1 and CV2 crossed
    r.Begin();
    r.Run();
    CHECK(r.flow.fail() == F::F_IDENTITY);
    CHECK(r.flow.identity_channel() == 0 && r.flow.identity_input() == 1);
    ExpectNoWrite(r);
    r.Idle(50);
    ExpectQuiet(r);
  }
  {
    Rig r;
    r.skip_ch = 2;                           // CV3 not patched
    r.Begin();
    r.Run();
    CHECK(r.flow.fail() == F::F_IDENTITY);
    CHECK(r.flow.identity_channel() == 2 && r.flow.identity_input() == -1);
    r.Idle(50);
    ExpectQuiet(r);
  }
  {
    Rig r;                                  // the module never took the overrides
    r.m.ignore_override = true;
    r.Begin();
    r.Run();
    CHECK(r.flow.fail() == F::F_NO_OVERRIDE);   // the status bit speaks before the identity check can
    r.Idle(50);
    ExpectQuiet(r);
  }
}

// ---- the physical sanity checks --------------------------------------------------------

// THE REGRESSION: hardware whose stage is 4096 codes per 10 V (6.5 % low). Its fit and its
// verify points are perfectly self-consistent; only the physical scale check sees it.
static void wrong_scale_hardware_is_rejected() {
  Rig r;
  r.m.scale = 3831.0 / 4096.0;
  r.Begin();
  r.Run();
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_SCALE);
  ExpectNoWrite(r);
  r.Idle(50);
  ExpectQuiet(r);
  // The line it measured is in the log: that is the data for the owner.
  CHECK(count_prefix("CALR,251LINE,0,") == 1);
  // The same unit, were the scale check not there, would have fitted perfectly: its line is
  // straight and a gain of about 1.07 corrects it (proves the failure is the scale, not the fit).
  R::FitPoint p[8];
  for (int i = 0; i < 8; ++i) {
    const int c = R::CodeOfMv(F::kFitMv[i]);
    p[i].code = (uint16_t)c;
    p[i].mv = r.m.AnalogMv(0, c);
  }
  CalRec::Channel ch;
  R::Analog an;
  double w = 0;
  CHECK(R::Fit(p, 8, R::kMvPerCode, &ch, &an, &w) == R::FIT_OK);
  CHECK(ch.gain > 0x4000 + 800);
}

// THE REPLAY: the fit is run with the 259e's mistake (a target of 4096 codes per 10 V instead of
// 3831) on a perfectly good unit. The fit and its own residuals are self-consistent; the run must
// still be rejected, before anything is written, by the physical checks (the literal anchors).
static void wrong_scale_fit_is_rejected() {
  Rig r;
  r.Begin();
  r.flow.fit_k_ = 10000.0 / 4096.0;
  r.RunToEnd();
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_RECORD);
  ExpectNoWrite(r);
  r.Idle(50);
  ExpectQuiet(r);
  // Its fit line was fine (the unit is good): the failure is the record's scale only.
  CHECK(count_prefix("CALR,251LINE,0,") == 1 && count_prefix("CALR,251FIT,0,") == 1);
  // The same replay with a smaller mistake (3 % low) is also caught: the anchor limit is 8 mV.
  Rig s;
  s.Begin();
  s.flow.fit_k_ = R::kMvPerCode * 0.97;
  s.RunToEnd();
  CHECK(s.flow.fail() == F::F_RECORD);
  // A 0.1 % error (10 mV at 10 V) is over the write limit but not gross: the write build refuses
  // it, a measuring-only run reports it; 0.02 % (2 mV) passes either.
  Rig t;
  t.Begin();
  t.flow.fit_k_ = R::kMvPerCode * 1.001;
  t.RunToEnd();
  if (F::kAllowWrite) {
    CHECK(t.flow.fail() == F::F_RECORD);
  } else {
    CHECK(t.flow.stage() == Stage::Results && t.flow.result().warn_anchor != 0);
    CHECK(has_line("CALR,251WARN,0,ANCHOR,"));
  }
  ExpectNoWrite(t);
  Rig u;
  u.Begin();
  u.flow.fit_k_ = R::kMvPerCode * 1.0002;
  CHECK(u.RunToEnd() && u.flow.fail() == F::F_NONE);
}

// The 251e drops its overrides after 5 s; the flow refreshes them before every window. A module
// with a short timer (1.5 s, tighter than a point's two windows) shows a missing refresh.
static void overrides_are_refreshed_before_every_window() {
  Rig r;
  r.m.override_ms = 1500;
  r.Begin();
  CHECK(r.RunToEnd());
  CHECK(r.flow.fail() == F::F_NONE);
  CHECK(r.m.lapses == 0);
}

static void scale_window_edges() {
  struct C { double scale; bool pass; };
  const C cases[] = {{0.97, true}, {1.03, true}, {0.95, false}, {1.05, false}, {0.935, false}, {1.10, false}};
  for (const C &c : cases) {
    Rig r;
    r.m.scale = c.scale;
    r.m.gain_err[0] = r.m.gain_err[1] = r.m.gain_err[2] = r.m.gain_err[3] = 0.0;
    r.Begin();
    r.RunToEnd();
    if (c.pass) {
      CHECK(r.flow.fail() == F::F_NONE);
    } else {
      // A stage 10 % high drives the 9.6 V point past the Xeno input's top first: also a refusal.
      CHECK(r.flow.fail() == F::F_SCALE || (c.scale > 1.08 && r.flow.fail() == F::F_INPUT_RANGE));
      ExpectNoWrite(r);
    }
  }
  Rig z;                               // a zero floor of 150 mV (|b| > 100)
  z.m.zero_mv[1] = 150.0;
  z.Begin();
  z.RunToEnd();
  CHECK(z.flow.fail() == F::F_SCALE);
  Rig n;                               // and a negative one
  n.m.zero_mv[3] = -120.0;
  n.Begin();
  n.RunToEnd();
  CHECK(n.flow.fail() == F::F_SCALE);
}

static void nonlinear_dac_is_reported_then_rejected() {
  Rig r;
  r.m.inl_mv[2] = 9.0;                  // more INL than the write limit (3 mV) allows, not gross
  r.Begin();
  r.RunToEnd();
  if (F::kAllowWrite) {                 // the write path needs the limit
    CHECK(r.flow.fail() == F::F_NONLIN);
  } else {                              // a measuring-only run reports it and goes on
    CHECK(r.flow.stage() == Stage::Results && r.flow.fail() == F::F_NONE);
    CHECK(r.flow.result().warn_nonlin == (1u << 2));
    CHECK(has_line("CALR,251WARN,2,NONLIN,"));
    CHECK(r.flow.result().inl_mv[2] > 2.0f);   // and the report carries the figure
  }
  ExpectNoWrite(r);
  Rig g;                                // gross INL stops either build
  g.m.inl_mv[2] = 30.0;
  g.Begin();
  g.RunToEnd();
  CHECK(g.flow.fail() == F::F_NONLIN);
  Rig ok;                               // a little INL is fine
  ok.m.inl_mv[2] = 1.5;
  ok.Begin();
  CHECK(ok.RunToEnd() && ok.flow.fail() == F::F_NONE && ok.flow.result().warn_nonlin == 0);
}

// A glitch at the corrected codes (the fit points are clean): only the verify points see it.
static void verify_catches_a_glitch_the_fit_cannot() {
  Rig r;
  r.m.spike_lo = 2040; r.m.spike_hi = 2110;   // the corrected code of stage 5400 mV (2069) is in here
  r.m.spike_mv = 7.0;
  r.Begin();
  r.RunToEnd();
  if (F::kAllowWrite) {
    CHECK(r.flow.fail() == F::F_VERIFY);
  } else {
    CHECK(r.flow.stage() == Stage::Results);
    CHECK(r.flow.result().warn_verify != 0 && has_line("CALR,251WARN,") && has_line("CALR,251STAT,"));
  }
  ExpectNoWrite(r);
  r.Idle(50);
  ExpectQuiet(r);
  Rig g;                                      // a gross glitch stops either build, and the data is logged
  g.m.spike_lo = 2040; g.m.spike_hi = 2110;
  g.m.spike_mv = 40.0;
  g.Begin();
  g.RunToEnd();
  CHECK(g.flow.fail() == F::F_VERIFY);
  CHECK(count_prefix("CALR,251STAT,") == 4);
  g.Idle(50);
  ExpectQuiet(g);
}

// The noise figure of the report is the rms of the window-to-window difference over sqrt 2: with
// the chain's noise raised to +-0.6 mV (rms 0.346) it follows.
static void report_noise_follows_the_chain() {
  Rig r;
  r.chain.noise_mv = 0.6;
  r.Begin();
  r.RunToEnd();
  CHECK(r.flow.stage() == Stage::Results || r.flow.stage() == Stage::Done || r.flow.fail() == F::F_DRIFT);
  if (r.flow.fail() == F::F_NONE) {
    const F::Result &x = r.flow.result();
    const double mean = (x.noise_mv[0] + x.noise_mv[1] + x.noise_mv[2] + x.noise_mv[3]) / 4.0;
    CHECK(mean > 0.25 && mean < 0.44);               // 0.346 expected
  }
}

// The gross non-linearity limit (12 mV) separates a warning from a stop. INL amplitudes 11.0 and
// 11.7 mV give 11.8 and 12.4 mV about the line on the sim's fit points.
static void gross_nonlinearity_limit() {
  Rig below;
  below.m.inl_mv[2] = 11.0;
  below.Begin();
  below.RunToEnd();
  if (!F::kAllowWrite) {
    CHECK(below.flow.stage() == Stage::Results && below.flow.result().warn_nonlin == (1u << 2));
    CHECK(below.flow.result().nonlin_mv[2] > 11.0f && below.flow.result().nonlin_mv[2] < 12.0f);
  } else {
    CHECK(below.flow.fail() == F::F_NONLIN);
  }
  Rig above;
  above.m.inl_mv[2] = 11.7;
  above.Begin();
  above.RunToEnd();
  CHECK(above.flow.fail() == F::F_NONLIN);
  CHECK(above.flow.result().nonlin_mv[2] > 12.0f);
}

// A failed channel does not hide the others: all four are measured, fitted and logged, and the run
// ends with the first failure.
static void every_channel_is_reported_even_after_a_failure() {
  Rig r;
  r.m.zero_mv[1] = 150.0;      // channel 1: scale/zero failure (no record can be fitted)
  r.m.inl_mv[3] = 30.0;        // channel 3: gross non-linearity
  r.Begin();
  r.RunToEnd();
  CHECK(r.flow.fail() == F::F_SCALE);
  CHECK(count_prefix("CALR,251LINE,") == 4);
  CHECK(count_prefix("CALR,251FIT,") == 3);        // channels 0, 2 and 3 got a record; 1 did not
  CHECK(has_line("CALR,251LINE,3,") && has_line("CALR,251END,SCALE,warn="));
  ExpectNoWrite(r);
  r.Idle(50);
  ExpectQuiet(r);
}

// A warned run is marked on its END line.
static void end_line_carries_the_warnings() {
  if (F::kAllowWrite) return;
  Rig clean;
  clean.Begin();
  CHECK(clean.RunToEnd());
  clean.flow.Cancel();
  CHECK(has_line("CALR,251END,CANCELLED,warn=0"));
  Rig w;
  w.m.inl_mv[2] = 9.0;                       // channel 2 over the write non-linearity limit
  w.Begin();
  CHECK(w.RunToEnd());
  const unsigned ww = w.flow.warn_word();
  CHECK((ww & 0xF) == 0x4 && w.flow.result().warn_nonlin == 0x4);   // channel 2, non-linearity
  w.flow.Cancel();
  char want[48];
  snprintf(want, sizeof want, "CALR,251END,CANCELLED,warn=%X", ww);
  CHECK(has_line(want) && ww != 0);
}

static void drift_is_detected() {
  Rig r;
  r.m.drift_mv_per_s = 0.2;              // the 251e warms up: 0.2 mV/s, ~6 mV over the run
  r.m.drift_t0 = r.clk.t;
  r.Begin();
  r.RunToEnd();
  // A line absorbs a steady drift as a slope error; the verify points (measured later) and the
  // drift repeat see it. Whichever sees it first, the run never passes.
  CHECK(r.flow.stage() == Stage::Failed);
  CHECK(r.flow.fail() == F::F_VERIFY || r.flow.fail() == F::F_DRIFT || r.flow.fail() == F::F_NONLIN);
  Rig e;                                  // a step after the verify points: only the repeat sees it
  e.Begin();
  e.Run([&] { return e.flow.point_index() == F::kPoints - 1; });
  e.m.extra_mv = 3.0;
  e.RunToEnd();
  CHECK(e.flow.fail() == F::F_DRIFT);
  e.Idle(50);
  CHECK(e.m.releases >= 1);             // the overrides are let go on this path too
  ExpectQuiet(e);
  Rig w;                                  // and a step between the two windows of one point
  w.Begin();
  w.Run([&] { return w.flow.point_index() == 2; });
  w.Idle(1100);
  w.m.extra_mv = 2.5;
  w.RunToEnd();
  CHECK(w.flow.fail() == F::F_DRIFT || w.flow.fail() == F::F_NONLIN || w.flow.fail() == F::F_VERIFY);
  Rig c;                                  // slow drift (the Xeno side) is tolerated
  c.chain.drift_mv_per_s = 0.01;
  c.Begin();
  CHECK(c.RunToEnd() && c.flow.fail() == F::F_NONE);
}

static void input_near_its_top_is_refused() {
  Rig r;
  r.chain.top_true = r.chain.top_cal = 9650.0;    // an input that tops out at 9.65 V
  r.Begin();
  r.RunToEnd();
  CHECK(r.flow.fail() == F::F_INPUT_RANGE);
}

static void a_ground_offset_inside_the_budget_is_tolerated() {
  Rig r;
  r.m.zero_mv[0] += 1.0;                 // 1 mV of common offset between the units (not in the loopback)
  r.m.zero_mv[1] += 1.0;
  r.Begin();
  CHECK(r.RunToEnd() && r.flow.fail() == F::F_NONE);
  for (int c = 0; c < 4; ++c) CHECK(TruthWorst(r.m, c, r.flow.result().ch[c]) < 4.0);
}

static void expiry() {
  Rig r;
  r.auto_ui = false;
  r.Begin();
  r.Run([&] { return r.flow.stage() == Stage::Warmup; });
  r.flow.Continue();
  r.Run([&] { return r.flow.stage() == Stage::PromptLoop; });
  r.PatchLoop();
  r.flow.Continue();
  r.Run([&] { return r.flow.stage() == Stage::PromptPatch; });
  r.Idle(10u * 60u * 1000u + 100u);        // the patch prompt left open past the limit
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_EXPIRED);
  ExpectNoWrite(r);
  ExpectQuiet(r);
}

// ---- cancel at every point -----------------------------------------------------------------

static void cancel_anywhere_releases_everything() {
  const uint32_t offsets[] = {3, 40, 350, 1200, 2500, 5000, 9000, 14000, 20000, 28000, 36000, 44000};
  for (uint32_t off : offsets) {
    Rig r;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Loopback; });
    r.Idle(off);
    if (r.Terminal()) continue;
    const Stage was = r.flow.stage();
    r.flow.Cancel();
    if (was == Stage::Results) { r.flow.Cancel(); r.Idle(100); ExpectNoWrite(r); ExpectQuiet(r); continue; }
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED);
    r.Idle(100);
    ExpectNoWrite(r);
    ExpectQuiet(r);
    if (r.m.count[CalRec::kCmdOverride] > 0) CHECK(r.m.count[CalRec::kCmdRelease] >= 1);
    r.flow.Cancel();
    CHECK(r.flow.stage() == Stage::Idle);
  }
  // A RELEASE that cannot get onto the bus gives up and leaves the module's 5 s timer.
  Rig b;
  b.Begin();
  b.Run([&] { return b.flow.stage() == Stage::Measure; });
  b.Idle(300);
  b.flow.Cancel();
  b.m.busy_sends = -1;
  b.Idle(2000);
  CHECK(!b.flow.background_work());
  b.m.busy_sends = 0;
  b.Idle(5200);
  CHECK(!b.m.OverrideActive());                           // the module let go by itself
}

// Safe to repeat: two measuring-only runs back to back leave the module and the Xeno as they were.
static void measuring_only_is_repeatable() {
  Rig r;
  if (F::kAllowWrite) return;
  r.Begin();
  CHECK(r.RunToEnd());
  const F::Result first = r.flow.result();
  r.flow.Cancel();
  r.flow.Cancel();
  CHECK(r.flow.stage() == Stage::Idle);
  r.Begin();
  CHECK(r.RunToEnd());
  const F::Result &second = r.flow.result();
  for (int c = 0; c < 4; ++c) {
    CHECK(std::abs(int(first.ch[c].gain) - int(second.ch[c].gain)) <= 1);
    CHECK(std::abs(int(first.ch[c].offset) - int(second.ch[c].offset)) <= 1);
    CHECK(std::fabs(first.a_mv[c] - second.a_mv[c]) < 0.003f);
  }
  r.flow.Cancel();
  r.flow.Cancel();
  r.Idle(100);
  ExpectNoWrite(r);
  ExpectQuiet(r);
  CHECK(!r.m.has_active);
  CHECK(r.outs.holds == 2 * 4 * F::kLoopPoints);   // only the loopback holds, 4 outputs x 5 points x 2 runs
}

// ---- line formats --------------------------------------------------------------------------

static std::vector<std::string> split(const std::string &l) {
  std::vector<std::string> f;
  size_t a = 0;
  for (;;) {
    const size_t e = l.find(',', a);
    f.push_back(l.substr(a, e == std::string::npos ? std::string::npos : e - a));
    if (e == std::string::npos) return f;
    a = e + 1;
  }
}
static std::string fixed(double v, int d) {
  char t[32];
  CM::FormatFixed(t, sizeof(t), CM::ScaleRound(v, d), d);
  return t;
}

// The run's lines checked by value against result() and the sim.
static void check_run_lines_by_value(Rig &r) {
  const F::Result &res = r.flow.result();
  int fits = 0, lines = 0;
  int pts[4] = {};
  for (const auto &l : g_log) {
    const auto f = split(l);
    if (f.size() < 2 || f[0] != "CALR") continue;
    if (f[1] == "251FIT") {
      CHECK(f.size() == 7);
      const int c = std::stoi(f[2]);
      CHECK(c == fits);
      ++fits;
      char g[8];
      snprintf(g, sizeof g, "%04X", unsigned(res.ch[c].gain));
      CHECK(f[3] == g);
      CHECK(f[4] == std::to_string(res.ch[c].offset));
      CHECK(f[6] == fixed(res.anchor_mv[c], 2));
      CalRec::Channel again;
      R::Analog an;
      CHECK(R::Fit(r.flow.fit_points(c), F::kFitPoints, R::kMvPerCode, &again, &an, nullptr) == R::FIT_OK);
      CHECK(again.gain == res.ch[c].gain && again.offset == res.ch[c].offset);
    } else if (f[1] == "251LINE") {
      CHECK(f.size() == 8);
      const int c = std::stoi(f[2]);
      CHECK(c == lines);
      CHECK(std::fabs(std::stod(f[6]) - res.bow_mv[c]) < 0.01 && std::fabs(std::stod(f[7]) - res.inl_mv[c]) < 0.01);
      ++lines;
      CHECK(f[3] == fixed(res.a_mv[c], 5) || std::fabs(std::stod(f[3]) - res.a_mv[c]) < 2e-5);
      CHECK(std::fabs(std::stod(f[5]) - res.nonlin_mv[c]) < 0.01);
    } else if (f[1] == "251STAT") {
      CHECK(f.size() == 8);
      const int c = std::stoi(f[2]);
      CHECK(std::fabs(std::stod(f[3]) - res.noise_mv[c]) < 0.002);
      CHECK(std::fabs(std::stod(f[4]) - res.a_mv[c]) < 1e-4);
      CHECK(std::fabs(std::stod(f[5]) - res.verify_mv[c]) < 0.006);
      CHECK(f[7].size() >= 1);
    } else if (f[1] == "251PT") {
      CHECK(f.size() == 6);
      const int c = std::stoi(f[2]);
      const int i = pts[c & 3]++;
      CHECK(i < F::kPoints);
      const char kind = i < F::kFitPoints ? 'F' : i < F::kPoints - 1 ? 'V' : 'D';
      CHECK(f[3].size() == 1 && f[3][0] == kind);
      if (kind == 'F') {
        const R::FitPoint &p = r.flow.fit_points(c)[i];
        CHECK(f[4] == std::to_string(p.code));
        CHECK(f[5] == fixed(p.mv, 3));
        // against the sim's own model: the reading is the module's voltage at that raw code
        CHECK(std::fabs(p.mv - r.m.AnalogMv(c, p.code)) < 1.0);
      }
    }
  }
  CHECK(fits == 4 && lines == 4);
  for (int c = 0; c < 4; ++c) CHECK(pts[c] == F::kPoints);
}

static void lines_by_value() {
  Rig r;
  r.Begin();
  CHECK(r.RunToEnd());
  check_run_lines_by_value(r);
}

// The fixture: golden examples for each formatter, the header lines the flow logs, and the
// field counts of every line of a run.
static void fixture_lines() {
  const char *fx = "fixtures/cal251e_lines.txt";
  std::ifstream in(fx);
  if (!in) in.open(std::string("software/test/") + fx);
  CHECK(in.good());
  std::vector<std::string> headers, examples;
  std::string l;
  while (std::getline(in, l)) {
    if (l.empty() || (l[0] == '#' && l.compare(0, 6, "CALR#,") != 0)) continue;
    if (l.compare(0, 6, "CALR#,") == 0) headers.push_back(l);
    else examples.push_back(l);
  }
  CHECK(int(headers.size()) == Cal251eLines::HeaderCount());
  for (size_t i = 0; i < headers.size() && int(i) < Cal251eLines::HeaderCount(); ++i)
    CHECK(headers[i] == Cal251eLines::Header(int(i)));
  CHECK(std::string(Cal251eLines::Header(0)) == "CALR#,version=2");
  bool have_end_examples = false;
  for (const auto &e : examples)
    if (e == "CALR,251END,SCALE,warn=0" || e == "CALR,251END,CANCELLED,warn=24") have_end_examples = true;
  std::map<std::string, size_t> fields;
  for (const auto &h : headers) {
    const auto f = split(h);
    if (f.size() >= 2 && f[0] == "CALR#") fields[f[1]] = f.size() - 2;
  }
  char buf[Cal251eLines::kMaxLine];
  std::map<std::string, std::string> want;
  for (const auto &e : examples)
    if (split(e)[1] != "251END") want[split(e)[1]] = e;
  Cal251eLines::Start(buf, sizeof buf, 0x5C);
  CHECK(want["251START"] == buf);
  Cal251eLines::Loop(buf, sizeof buf, 3, 4800, 4801.234);
  CHECK(want["251LOOP"] == buf);
  Cal251eLines::Point(buf, sizeof buf, 2, 'V', 1839, 4799.5);
  CHECK(want["251PT"] == buf);
  Cal251eLines::Line(buf, sizeof buf, 1, 2.64156, 6.927, 1.63, 0.85, 0.40);
  CHECK(want["251LINE"] == buf);
  Cal251eLines::Stat(buf, sizeof buf, 2, 0.087, 2.6103, -1.42, 0.65, 5);
  CHECK(want["251STAT"] == buf);
  Cal251eLines::Warn(buf, sizeof buf, 2, "NONLIN", 4.12, 3.0);
  CHECK(want["251WARN"] == buf);
  Cal251eLines::Commit(buf, sizeof buf, 3, 7);
  CHECK(want["251COMMIT"] == buf);
  Cal251eLines::Fit(buf, sizeof buf, 3, CalRec::Channel{0x3FA5, -12}, 1.63, 0.25);
  CHECK(want["251FIT"] == buf);
  R::Record rec;
  for (int i = 0; i < 8; ++i) rec.unit_id[i] = uint8_t(i + 1);
  rec.seq = 7; rec.date = 1761264000u;
  rec.ch[0] = {0x4012, -7}; rec.ch[1] = {0x3FF0, 3}; rec.ch[2] = {0x41A0, -12}; rec.ch[3] = {0x4000, 0};
  uint8_t b[38];
  R::Encode(rec, b);
  Cal251eLines::Rec(buf, sizeof buf, 0x5C, b);
  CHECK(want["251REC"] == buf);
  Cal251eLines::End(buf, sizeof buf, "SCALE", 0);
  CHECK(std::string(buf) == "CALR,251END,SCALE,warn=0");
  Cal251eLines::End(buf, sizeof buf, "CANCELLED", 0x24);
  CHECK(std::string(buf) == "CALR,251END,CANCELLED,warn=24");
  CHECK(have_end_examples);
  Cal251eLines::Old(buf, sizeof buf, nullptr, 0);
  CHECK(want["251OLD"] == buf);
  // a whole run: every CALR line has its header's field count, the headers come first
  Rig r;
  r.Begin();
  CHECK(r.RunToEnd());
  CHECK(int(g_log.size()) > Cal251eLines::HeaderCount());
  for (int i = 0; i < Cal251eLines::HeaderCount(); ++i) CHECK(g_log[size_t(i)] == Cal251eLines::Header(i));
  for (const auto &x : g_log) {
    const auto f = split(x);
    if (f[0] != "CALR" || !fields.count(f[1])) continue;
    if (f[1] == "251OLD") { CHECK(f.size() == 3); continue; }
    CHECK(f.size() - 2 == fields[f[1]] || f[1] == "251END");
  }
}

// ---- the write (the variant built with -DCAL251E_ALLOW_WRITE=1) --------------------------------

static int commits_in(const Sim251e &m) { return m.count[CalRec::kCmdCommit]; }

static void write_path() {
  if (!F::kAllowWrite) return;
  Rig r;
  r.Begin();
  CHECK(r.RunToEnd());
  CHECK(r.flow.stage() == Stage::Done && r.flow.fail() == F::F_NONE);
  CHECK(commits_in(r.m) == 1);                     // exactly one COMMIT
  CHECK(r.m.count[CalRec::kCmdPut] == 5);
  CHECK(r.m.has_active && memcmp(r.m.active, r.flow.record(), 38) == 0);
  R::Record rec;
  CHECK(R::Decode(r.flow.record(), 38, &rec));
  CHECK(rec.seq == 1 && rec.date == 1761264000u && rec.unit_id[0] == 0xA0);
  for (int c = 0; c < 4; ++c) {
    CHECK(rec.ch[c].gain == r.flow.result().ch[c].gain && rec.ch[c].offset == r.flow.result().ch[c].offset);
    // End to end through the module's own sequencer path: the truth after the write.
    CHECK(TruthWorstSeq(r.m, c) < 4.0);
  }
  CHECK(count_prefix("CALR,251REC,5C,") == 1 && has_line("CALR,251END,OK"));
  CHECK(has_line("CALR,251COMMIT,1,1") && r.flow.commits_since_power_up() == 1);   // count and seq per COMMIT
  check_run_lines_by_value(r);
  r.Idle(50);
  ExpectQuiet(r);
  // A second run: the unit keeps its ID, the sequence number counts, the old record is logged.
  r.flow.Cancel();
  r.Begin();
  const uint8_t first_rec[38] = {0};
  (void)first_rec;
  uint8_t before[38];
  memcpy(before, r.m.active, 38);
  CHECK(r.RunToEnd());
  CHECK(r.flow.stage() == Stage::Done);
  CHECK(R::Decode(r.flow.record(), 38, &rec) && rec.seq == 2 && rec.unit_id[0] == 0xA0);
  CHECK(commits_in(r.m) == 2);
  CHECK(has_line("CALR,251COMMIT,2,2") && r.flow.commits_since_power_up() == 2);   // not reset by Begin
  std::string hex;
  char h[3];
  for (int i = 0; i < 38; ++i) { snprintf(h, sizeof h, "%02X", before[i]); hex += h; }
  CHECK(has_line("CALR,251OLD," + hex));
}

// The same run on both firmware behaviours: C5 clears the commit-result bits when the COMMIT is
// accepted and sets them when it ends (here after 700 ms); C4 leaves the old ones.
static void write_on_c4_and_c5() {
  if (!F::kAllowWrite) return;
  for (int c5 = 0; c5 < 2; ++c5) {
    Rig r;
    r.m.fw_c5 = c5;
    r.m.commit_ms = 700;
    install(r.m, 9, 0x30);
    r.Begin();
    CHECK(r.RunToEnd());
    CHECK(r.flow.stage() == Stage::Done);
    CHECK(commits_in(r.m) == 1);
    R::Record rec;
    CHECK(R::Decode(r.flow.record(), 38, &rec) && rec.seq == 10 && rec.unit_id[0] == 0x30);
  }
  Rig s;                                   // C5 with a COMMIT that never ends: unknown, once
  s.m.fw_c5 = true;
  s.m.stuck_commit = true;
  s.Begin();
  s.RunToEnd();
  CHECK(s.flow.fail() == F::F_COMMIT_UNKNOWN && commits_in(s.m) == 1);
}

// The per-power-up cap on COMMITs: the 21st is refused before anything is sent.
static void write_cap() {
  if (!F::kAllowWrite) return;
  Rig r;
  for (int i = 1; i <= F::kWriteCap; ++i) {
    r.Begin();
    CHECK(r.RunToEnd());
    CHECK(r.flow.stage() == Stage::Done);
    CHECK(r.flow.commits_since_power_up() == i);
    r.flow.Cancel();
  }
  CHECK(commits_in(r.m) == F::kWriteCap);
  char want[40];
  snprintf(want, sizeof want, "CALR,251COMMIT,%d,%d", F::kWriteCap, F::kWriteCap);
  CHECK(has_line(want));
  const int puts = r.m.count[CalRec::kCmdPut];
  r.Begin();
  r.RunToEnd();
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_WRITE_CAP);
  CHECK(commits_in(r.m) == F::kWriteCap && r.m.count[CalRec::kCmdPut] == puts);
  r.Idle(50);
  ExpectQuiet(r);
}

static void write_failures() {
  if (!F::kAllowWrite) return;
  {   // the module refuses (a torn rewrite leaves no verdict)
    Rig r;
    r.m.tear_commit = true;
    r.Begin();
    r.RunToEnd();
    CHECK(r.flow.fail() == F::F_COMMIT_UNKNOWN);
    CHECK(commits_in(r.m) == 1);                   // never resent
    ExpectQuiet(r);
  }
  {   // the status read before the COMMIT gets no answer: F_BUS, and nothing is committed
    Rig r;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Write && r.flow.sub_ == Cal251eFlow::S_PRESTAT; });
    r.m.drop_replies = true;
    r.RunToEnd();
    CHECK(r.flow.fail() == F::F_BUS);
    CHECK(commits_in(r.m) == 0 && r.flow.commits_since_power_up() == 0);
    r.m.drop_replies = false;
    r.Idle(50);
    ExpectQuiet(r);
  }
  {   // the read-back differs
    Rig r;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Write; });
    r.m.corrupt_read_arg = 8;
    r.RunToEnd();
    CHECK(r.flow.fail() == F::F_READBACK);
    CHECK(commits_in(r.m) == 1);
  }
  {   // Wire error on the COMMIT frame, latched: ok, still one COMMIT
    Rig r;
    r.m.commit_send_rc = 4;
    r.Begin();
    r.RunToEnd();
    CHECK(r.flow.stage() == Stage::Done && commits_in(r.m) == 1);
  }
  {   // the write needs the confirm: nothing is sent while it is unanswered, and it expires
    Rig r;
    r.hold_confirm = true;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Confirm; });
    r.Idle(5000);
    ExpectNoWrite(r);
    r.Idle(10u * 60u * 1000u);
    CHECK(r.flow.fail() == F::F_EXPIRED);
    ExpectNoWrite(r);
  }
  {   // ConfirmWrite checks the age itself, whenever the UI calls it (Tick may not have run)
    Rig r;
    r.hold_confirm = true;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Confirm; });
    r.clk.t += 11u * 60u * 1000u;
    r.flow.ConfirmWrite();
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_EXPIRED);
    ExpectNoWrite(r);
  }
  {   // the write's own re-check: a record that fails the absolute anchors is not sent
    Rig r;
    r.hold_confirm = true;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Confirm; });
    r.flow.result_.ch[1].gain = uint16_t(r.flow.result_.ch[1].gain * 3831 / 4096);   // a wrong-scale record
    r.flow.ConfirmWrite();
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_RECORD);
    ExpectNoWrite(r);
  }
  {   // cancel after the COMMIT went out: the verdict is collected in the background
    Rig r;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Write && commits_in(r.m) == 1; });
    r.flow.Cancel();
    CHECK(r.flow.cancel_pending());
    CHECK(!r.flow.Begin(r.outs, r.ins, r.mvp, r.clk, r.m, r.ent, true, capture));   // refused
    for (int i = 0; i < 6000 && r.flow.cancel_pending(); ++i) { r.clk.t += 1; r.flow.Tick(); }
    CHECK(!r.flow.cancel_pending() && r.flow.cancelled_verdict() == 1);
    CHECK(has_line("CALR,251END,CANCELLED_OK"));
    CHECK(commits_in(r.m) == 1);
  }
  {   // a cancel before the COMMIT goes out writes nothing
    Rig r;
    r.Begin();
    r.Run([&] { return r.flow.stage() == Stage::Write; });
    r.flow.Cancel();
    r.Idle(3000);
    ExpectNoWrite(r);
    CHECK(r.flow.stage() == Stage::Failed);
  }
}

int main() {
  // The write is compiled out unless asked for: the default build, and the ones with the macro 0,
  // must be read-only.
#if !(defined(CAL251E_ALLOW_WRITE) && CAL251E_ALLOW_WRITE)
  CHECK(!F::kAllowWrite);
#else
  CHECK(F::kAllowWrite);
#endif
  good_unit_passes();
  read_only_build_never_writes();
  no_self_cal_or_uncalibrated_inputs();
  no_251e_on_the_bus();
  existing_record_is_logged();
  loopback_catches_a_bad_chain();
  loopback_wrong_cable();
  noisy_input_stops_the_run();
  identity_checks();
  wrong_scale_hardware_is_rejected();
  wrong_scale_fit_is_rejected();
  overrides_are_refreshed_before_every_window();
  scale_window_edges();
  nonlinear_dac_is_reported_then_rejected();
  verify_catches_a_glitch_the_fit_cannot();
  report_noise_follows_the_chain();
  gross_nonlinearity_limit();
  every_channel_is_reported_even_after_a_failure();
  end_line_carries_the_warnings();
  drift_is_detected();
  input_near_its_top_is_refused();
  a_ground_offset_inside_the_budget_is_tolerated();
  expiry();
  cancel_anywhere_releases_everything();
  measuring_only_is_repeatable();
  lines_by_value();
  fixture_lines();
  write_path();
  write_on_c4_and_c5();
  write_cap();
  write_failures();
  printf("test_cal251e_flow: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
