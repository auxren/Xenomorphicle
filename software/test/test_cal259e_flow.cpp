// Host tests for the Cal Routine's 259e target (src/Cal259eFlow.h): the step
// machine against simulated hardware. Xeno output A is a bowed DAC (SimDac)
// whose stored table is calibrated (as after an Outputs run), the USB meter
// (FakeMeter) probes it, and one or two Sim259e modules with an ADC model
// raw = (mv + err_mv) * k + noise read it on whichever input it is patched
// into. Also the CALR 259 line formats (fixtures/cal259e_lines.txt).
// Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_cal259e_flow
//     test_cal259e_flow.cpp ../src/Cal259eFlow.cpp ../src/Cal259eBus.cpp ../src/CalRec.cpp
//     ../src/CalRoutineMeasure.cpp ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp
//     ../src/UsbTmc.cpp
// (and not CalRoutineFlow.cpp: the 259e target links without the Self flow).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../src/Cal259eFlow.h"
#include "../src/CvMeterMath.h"
#include "fake_meter.h"
#include "sim259e.h"

typedef Cal259eFlow::Stage Stage;
typedef Cal259eFlow F;
namespace CM = CvMeterMath;

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
static bool logged(const char *l) {
  for (const auto &x : g_log)
    if (x == l) return true;
  return false;
}

// ---- simulated hardware ------------------------------------------------------

struct FakeBackup : BackupPort {
  bool begin_ok = true;
  int result = 1;       // what Poll ends with
  int delay_polls = 5;  // Poll returns 0 this often first
  int begins = 0, polls = 0, left = 0;
  std::vector<uint8_t> addrs;
  uint32_t bytes = 990;          // what the master moved (a 259e bank is exactly 990)
  BackupWhy why = BK_NONE;
  BackupWhy Why() const override { return why; }
  uint32_t Bytes() const override { return bytes; }
  uint32_t Crc() const override { return 0xA1B2C3D4u; }
  bool Begin(uint8_t a) override {
    ++begins;
    addrs.push_back(a);
    if (!begin_ok) return false;
    left = delay_polls;
    return true;
  }
  int Poll() override {
    ++polls;
    if (left > 0) { --left; return 0; }
    return result;
  }
};

struct FakeEntropy : EntropyPort {
  int fills = 0;
  void Fill(uint8_t *d, int n) override {
    ++fills;
    for (int i = 0; i < n; ++i) d[i] = uint8_t(0xA0 + i);
  }
  uint32_t UnixTime() override { return 1761264000u; }
};

// The 200e bus with every module on it: a frame reaches all of them, replies
// come back in module order. mangle (when set) edits a frame before it goes out.
struct MultiBus : BusPort {
  std::vector<Sim259e *> mods;
  std::function<void(uint8_t *, uint8_t)> mangle;
  int Send(const uint8_t *f, uint8_t n) override {
    uint8_t g[32];
    memcpy(g, f, n);
    if (mangle) mangle(g, n);
    int rc = 0;
    for (size_t i = 0; i < mods.size(); ++i) {
      const int r = mods[i]->Send(g, n);
      if (i == 0) rc = r;
    }
    return rc;
  }
  bool PopReply(uint8_t *f, uint8_t *n) override {
    for (Sim259e *m : mods)
      if (m->PopReply(f, n)) return true;
    return false;
  }
};

struct Rig {
  SimDac dac;
  FakeClock clk;
  FakeOutputs outs{&dac};
  FakeMeter meter{&outs, &clk};
  Sim259e a, b;  // 0x28, 0x29
  MultiBus bus;
  FakeBackup backup;
  FakeEntropy ent;
  Cal259eFlow flow;

  // Output A goes into `patched`'s input patch_ch (-1: nowhere). The UI sets
  // patch_ch at each PromptPatch: the prompted channel, or the other one when
  // wrong_jack. both_move: the other module sees output A too (on the same channel).
  Sim259e *patched = &a;
  int patch_ch = -1;
  bool wrong_jack = false;
  int wrong_jack_ch = -1;      // with wrong_jack: only this channel's patch is wrong (-1 both)
  bool hold_confirm = false;   // the UI leaves the write confirm unanswered
  bool both_move = false;
  bool auto_ui = true;
  bool bad_hold = false;       // an output other than A was ever held
  int mask_at_prompt[2] = {-1, -1};  // outs.mask when channel c was prompted

  double InputMv(const Sim259e *m, int ch) const {
    if (ch != patch_ch) return 0.0;
    if (m == patched || both_move) return outs.OutMv(F::kSourceOutput);
    return 0.0;
  }

  Rig() {
    outs.clk = &clk;
    // Output A's stored table, as an Outputs run leaves it: entry k is the code
    // the bowed DAC turns into k * 1200 mV.
    for (int k = 0; k < CR::kNodes; ++k) {
      int lo = 0, hi = 65535;
      while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (dac.Mv(0, uint16_t(mid)) < k * 1200.0) lo = mid + 1;
        else hi = mid;
      }
      outs.stored[0][k] = uint16_t(lo);
    }
    a.addr = 0x28;
    b.addr = 0x29;
    for (Sim259e *m : {&a, &b}) {
      m->clock = &clk;
      m->adc_noise = 2.0;
      m->input_mv = [this, m](int ch) { return InputMv(m, ch); };
    }
    b.noise_lcg = 99;
    bus.mods.push_back(&a);
  }
  void Multi() { bus.mods.push_back(&b); }

  void Begin(bool self_cal = true) {
    g_log.clear();
    flow.Begin(meter, outs, clk, bus, backup, ent, self_cal, capture);
  }
  void Ui() {
    switch (flow.stage()) {
      case Stage::Warmup: flow.Continue(); break;
      case Stage::PromptPatch:
        if (flow.channel() >= 0 && flow.channel() < 2 && mask_at_prompt[flow.channel()] < 0)
          mask_at_prompt[flow.channel()] = outs.mask;
        patch_ch = (wrong_jack && (wrong_jack_ch < 0 || wrong_jack_ch == flow.channel()))
                       ? 1 - flow.channel() : flow.channel();
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
    if (outs.mask & ~1u) bad_hold = true;
    if (auto_ui) Ui();
  }
  bool Terminal() const {
    const Stage s = flow.stage();
    return s == Stage::Done || s == Stage::Failed || s == Stage::NoSelfCal || s == Stage::Idle;
  }
  // Steps until pred() (true) or a terminal stage (pred()'s value); false on timeout.
  bool Run(std::function<bool()> pred = nullptr, uint32_t max_ms = 600000) {
    for (uint32_t i = 0; i < max_ms; ++i) {
      if (pred && pred()) return true;
      if (Terminal()) return pred ? pred() : true;
      Step();
    }
    return false;
  }
  void Idle(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) Step();
  }
};

static void good_adc(Sim259e &m) {
  for (int c = 0; c < 2; ++c) {
    m.adc_k[c] = Sim259e::kNominalK * 1.015;  // gain error +1.5 %
    m.adc_err_mv[c] = 20.0;                   // offset +20 mV
  }
}

static int last_commit_flags(const Sim259e &m) {
  for (size_t i = m.log.size(); i-- > 0;)
    if (m.log[i].size() >= 9 && m.log[i][3] == CalRec::kCmdCommit)
      return m.log[i].size() >= 10 ? m.log[i][9] : 0;
  return -1;
}

static void install(Sim259e &m, uint16_t seq, uint8_t id0) {
  CalRec::Record r;
  for (int i = 0; i < 8; ++i) r.unit_id[i] = uint8_t(id0 + i);
  r.seq = seq;
  r.date = 1700000000u;
  r.ch[0] = {0x9041, 0};
  r.ch[1] = {0x9041, 0};
  uint8_t rec[30];
  CalRec::Encode259e(r, rec);
  m.Install(rec);
}

// ---- tests -----------------------------------------------------------------------

// The run's 259 lines checked by value, not only by field count:
//   259FIT: gain, offset and both cents fields are result()'s, format-exact,
//           and the fit recomputed from points() gives the same channel;
//   259REC: the hex is record(), which is what the module now holds;
//   259PT:  one line per point, in kPointMv order, with points()'s values, and
//           those values agree with the sim: the meter median is the held
//           output's voltage, the raw median is (mv + err) * k within the noise.
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
static void check_run_lines_by_value(Rig &r) {
  const F::Result &res = r.flow.result();
  int fits = 0, recs = 0, pts[2] = {0, 0};
  for (const auto &l : g_log) {
    const auto f = split(l);
    if (f.size() < 2 || f[0] != "CALR") continue;
    if (f[1] == "259FIT") {
      CHECK(f.size() == 7);
      const int c = std::stoi(f[2]);
      CHECK(c == fits);
      ++fits;
      char g[8];
      snprintf(g, sizeof(g), "%04X", unsigned(res.ch[c].gain));
      CHECK(f[3] == g);
      CHECK(f[4] == std::to_string(res.ch[c].offset));
      CHECK(f[5] == fixed(res.worst_verify_cents[c], 2));
      CHECK(f[6] == fixed(res.nonlin_cents[c], 2));
      CalRec::Channel again;
      CHECK(CalRec::Fit259e(r.flow.points(c), F::kFitPoints, &again) == CalRec::FIT_OK);
      CHECK(again.gain == res.ch[c].gain && again.offset == res.ch[c].offset);
    } else if (f[1] == "259REC") {
      ++recs;
      CHECK(f.size() == 4 && f[2] == "28");
      std::string hex;
      char h[3];
      for (int i = 0; i < 30; ++i) { snprintf(h, sizeof(h), "%02X", r.flow.record()[i]); hex += h; }
      CHECK(f[3] == hex);
      CHECK(memcmp(r.flow.record(), r.a.active, 30) == 0);
      uint8_t enc[30];
      CalRec::Record rec;
      CHECK(CalRec::Decode259e(r.flow.record(), 30, &rec));
      CHECK(rec.ch[0].gain == res.ch[0].gain && rec.ch[1].offset == res.ch[1].offset);
      CalRec::Encode259e(rec, enc);
      CHECK(memcmp(enc, r.flow.record(), 30) == 0);
    } else if (f[1] == "259PT") {
      CHECK(f.size() == 7);
      const int c = std::stoi(f[2]);
      const int i = pts[c & 1]++;
      if (i >= F::kPoints) { CHECK(false); continue; }
      const CalRec::FitPoint &p = r.flow.points(c)[i];
      const int32_t mv = F::kPointMv[i];
      CHECK(f[3] == std::to_string(mv));
      CHECK(f[4] == fixed(p.mv, 3));
      CHECK(f[5] == std::to_string(p.raw));
      CHECK(f[6] == "8");
      // Independent of the flow: the sim's own model.
      const uint16_t code = r.outs.CodeForMv(0, r.outs.stored[0], mv);
      CHECK(std::fabs(p.mv - r.dac.Mv(0, code)) < 0.1);
      const double raw = (p.mv + r.a.adc_err_mv[c]) * r.a.adc_k[c];
      CHECK(std::fabs(double(p.raw) - raw) <= 3.0);
    }
  }
  CHECK(fits == 2 && recs == 1 && pts[0] == F::kPoints && pts[1] == F::kPoints);
}

static void good_unit_passes() {
  Rig r;
  good_adc(r.a);
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Done);
  CHECK(r.flow.fail() == F::F_NONE);
  CHECK(r.flow.target() == 0x28);
  const F::Result &res = r.flow.result();
  CHECK(res.valid_mask == 3);
  const double want_gain = 0x9041 / 1.015;
  for (int c = 0; c < 2; ++c) {
    CHECK(std::fabs(res.worst_verify_cents[c]) <= 3.0f);
    CHECK(res.nonlin_cents[c] >= 0.f && res.nonlin_cents[c] <= 3.0f);
    CHECK(std::fabs(res.ch[c].gain - want_gain) / want_gain < 0.003);
    // corrected = raw * g / 65536 + off; raw carries +20 mV: off ~ -20 * 4096 / 1200
    CHECK(std::fabs(res.ch[c].offset + 68.3) < 5.0);
  }
  // The record on the module decodes to the fitted values (no record before:
  // entropy unit ID, seq 1, the RTC date).
  CalRec::Record rec;
  CHECK(r.a.has_active && CalRec::Decode259e(r.a.active, 30, &rec));
  for (int c = 0; c < 2; ++c) {
    CHECK(rec.ch[c].gain == res.ch[c].gain);
    CHECK(rec.ch[c].offset == res.ch[c].offset);
  }
  CHECK(rec.seq == 1 && rec.date == 1761264000u);
  for (int i = 0; i < 8; ++i) CHECK(rec.unit_id[i] == uint8_t(0xA0 + i));
  CHECK(memcmp(r.flow.record(), r.a.active, 30) == 0);
  CHECK(r.a.count[CalRec::kCmdCommit] == 1);
  CHECK(last_commit_flags(r.a) == 0);
  CHECK(r.backup.begins == 1 && r.backup.addrs[0] == 0x28);
  CHECK(r.a.bad_frames == 0);
  // Holds: only output A, released after each channel and at the end.
  CHECK(!r.bad_hold);
  CHECK(r.mask_at_prompt[0] == 0 && r.mask_at_prompt[1] == 0);
  CHECK(r.outs.mask == 0);
  // The log: kPoints (8 fit + 5 verify) per channel, both fits, the record, OK.
  CHECK(count_prefix("CALR,259START,28") == 1);
  CHECK(count_prefix("CALR,259PT,0,") == F::kPoints && count_prefix("CALR,259PT,1,") == F::kPoints);
  CHECK(count_prefix("CALR,259FIT,") == 2);
  CHECK(count_prefix("CALR,259REC,28,") == 1);
  CHECK(logged("CALR,259END,OK"));
  // The meter was set up as Self does it, before any reading.
  CHECK(r.meter.sent.size() > 6);
  for (int i = 0; i < 6 && r.meter.sent.size() > 6; ++i)
    CHECK(r.meter.sent[size_t(i)] == CalRoutine::kMeterSetup[i]);
  // Each point: throwaway + 3 READ? (no noise beyond the rule).
  CHECK(r.meter.reads == 2 * F::kPoints * 4);
  check_run_lines_by_value(r);
}

static void scale_off_fails() {
  Rig r;
  for (int c = 0; c < 2; ++c) r.a.adc_k[c] = Sim259e::kNominalK * 0.8;  // k 20 % off
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_SCALE);
  CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  CHECK(r.backup.begins == 0);
  CHECK(count_prefix("CALR,259FIT,0,") == 1);  // the failing gain is logged
  CHECK(logged("CALR,259END,SCALE"));
  CHECK(r.outs.mask == 0);
}

static void wrong_jack_fails_identity() {
  Rig r;
  good_adc(r.a);
  r.wrong_jack = true;
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_IDENTITY);
  CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  CHECK(count_prefix("CALR,259PT,") == 0);
  CHECK(logged("CALR,259END,IDENTITY"));
  CHECK(r.outs.mask == 0 && !r.bad_hold);
}

static void several_259es_one_moves() {
  Rig r;
  r.Multi();
  good_adc(r.b);
  r.patched = &r.b;  // only 0x29 sees output A
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.candidates_mask() == 0x3);
  CHECK(r.flow.stage() == Stage::Done);
  CHECK(r.flow.target() == 0x29);
  CHECK(r.b.count[CalRec::kCmdCommit] == 1 && r.b.has_active);
  CHECK(!r.a.has_active);  // 0x28 was never written (frames to 0x29 only)
  CHECK(count_prefix("CALR,259START,29") == 1);
  CHECK(count_prefix("CALR,259REC,29,") == 1);
  CHECK(r.backup.addrs.size() == 1 && r.backup.addrs[0] == 0x29);
  for (const auto &f : r.a.log)
    if (f.size() > 4 && (f[3] == CalRec::kCmdPut || f[3] == CalRec::kCmdCommit)) CHECK(f[4] == 0x29);
}

static void several_259es_both_move() {
  Rig r;
  r.Multi();
  good_adc(r.a);
  good_adc(r.b);
  r.both_move = true;
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_SEVERAL_MOVED);
  CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.b.count[CalRec::kCmdPut] == 0);
  CHECK(r.outs.mask == 0);
}

static void meter_disagrees_fails_patch() {
  Rig r;
  good_adc(r.a);
  r.meter.offset_mv = 80.0;  // 80 mV away from the held code's voltage
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_PATCH);
  CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  CHECK(logged("CALR,259END,PATCH"));
  CHECK(r.outs.mask == 0);
}

static void noisy_meter_fails() {
  Rig r;
  good_adc(r.a);
  r.meter.noise_mv = 10.0;  // spread far above 0.5 mV
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_NOISY);
  CHECK(count_prefix("CALR,REREAD,") == 1);  // the existing rule: one re-read set, then NOISY
  CHECK(r.meter.reads == 1 + 3 + 3);
  CHECK(r.a.count[CalRec::kCmdPut] == 0);
  CHECK(logged("CALR,259END,NOISY"));
  CHECK(r.outs.mask == 0);
}

static void backup_fails_no_write() {
  {
    Rig r;
    good_adc(r.a);
    r.backup.result = -1;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_BACKUP);
    CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
    CHECK(r.a.count[CalRec::kCmdRead] > 0);  // only reads went out
    CHECK(logged("CALR,259END,BACKUP"));
  }
  {
    Rig r;
    good_adc(r.a);
    r.backup.begin_ok = false;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_BACKUP);
    CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  }
}

static void unit_id_kept() {
  {
    Rig r;
    good_adc(r.a);
    install(r.a, 5, 0x11);
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Done);
    CalRec::Record rec;
    CHECK(CalRec::Decode259e(r.a.active, 30, &rec));
    for (int i = 0; i < 8; ++i) CHECK(rec.unit_id[i] == uint8_t(0x11 + i));
    CHECK(rec.seq == 6);
    CHECK(r.ent.fills == 0);
  }
  {
    Rig r;
    good_adc(r.a);
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Done);
    CalRec::Record rec;
    CHECK(CalRec::Decode259e(r.a.active, 30, &rec));
    for (int i = 0; i < 8; ++i) CHECK(rec.unit_id[i] == uint8_t(0xA0 + i));
    CHECK(rec.seq == 1);
    CHECK(r.ent.fills == 1);
  }
}

// Decision a: compaction is unreachable while kAllowCompaction is false. The
// 8-slots-full COMMIT ends in a Failed screen (F_NO_SLOT), never a confirm, and
// ConfirmCompact() does nothing.
static void slots_full_is_a_failed_screen() {
#ifndef CAL259E_ALLOW_COMPACTION
  CHECK(!F::kAllowCompaction);   // the firmware build
#endif
  if (F::kAllowCompaction) return;
  Rig r;
  good_adc(r.a);
  install(r.a, 7, 0x31);
  r.a.slots_used = 8;
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_NO_SLOT);
  CHECK(logged("CALR,259END,NO_SLOT"));
  CHECK(r.a.count[CalRec::kCmdCommit] == 1 && last_commit_flags(r.a) == 0);
  CHECK(r.backup.begins == 1);
  CHECK(r.flow.commit_result() == E_NO_SLOT);
  const int frames = int(r.a.log.size());
  r.flow.ConfirmCompact();   // nothing, in any stage
  r.Idle(10000);
  CHECK(r.flow.stage() == Stage::Failed && int(r.a.log.size()) == frames);
  CHECK(r.a.slots_used == 8);
  CHECK(std::strcmp(F::FailName(F::F_NO_SLOT), "NO_SLOT") == 0);
}

// The compaction path itself, built and run with -DCAL259E_ALLOW_COMPACTION=1
// (a second run in run-host-tests.sh): kept working for when a restore path exists.
static void slots_full_needs_second_confirm() {
  if (!F::kAllowCompaction) return;
  auto full = [](Rig &r) {
    good_adc(r.a);
    install(r.a, 7, 0x31);
    r.a.slots_used = 8;
    r.Begin();
  };
  {
    Rig r;
    full(r);
    CHECK(r.Run([&] { return r.flow.stage() == Stage::ConfirmCompact; }));
    CHECK(r.a.count[CalRec::kCmdCommit] == 1 && last_commit_flags(r.a) == 0);
    CHECK(r.backup.begins == 1);
    // Without ConfirmCompact() nothing more goes out.
    const int frames = int(r.a.log.size());
    r.Idle(10000);
    CHECK(r.flow.stage() == Stage::ConfirmCompact);
    CHECK(int(r.a.log.size()) == frames);
    CHECK(r.a.count[CalRec::kCmdCommit] == 1);
    // With it: a second backup, then COMMIT flags 1.
    r.flow.ConfirmCompact();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Done);
    CHECK(r.backup.begins == 2);
    CHECK(r.a.count[CalRec::kCmdCommit] == 2 && last_commit_flags(r.a) == 1);
    CalRec::Record rec;
    CHECK(CalRec::Decode259e(r.a.active, 30, &rec) && rec.seq == 8);
    CHECK(r.a.slots_used == 1);
  }
  {
    Rig r;  // Cancel from ConfirmCompact sends nothing more.
    full(r);
    CHECK(r.Run([&] { return r.flow.stage() == Stage::ConfirmCompact; }));
    const int frames = int(r.a.log.size());
    r.flow.Cancel();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED);
    CHECK(int(r.a.log.size()) == frames && r.backup.begins == 1);
  }
  {
    Rig r;  // ConfirmCompact() anywhere else does nothing.
    good_adc(r.a);
    r.Begin();
    CHECK(r.Run([&] { return r.flow.stage() == Stage::Confirm; }));
    r.flow.ConfirmCompact();
    CHECK(r.flow.stage() == Stage::Confirm);
  }
}

static void cancel_every_stage() {
  struct Case {
    const char *name;
    std::function<void(Rig &)> setup;
    std::function<bool(Rig &)> at;
  };
  auto at_stage = [](Stage s) { return [s](Rig &r) { return r.flow.stage() == s; }; };
  const std::vector<Case> cases = {
    {"NoMeter", [](Rig &r) { r.meter.connected = false; }, at_stage(Stage::NoMeter)},
    {"Scan", nullptr, at_stage(Stage::Scan)},
    {"Warmup", nullptr, at_stage(Stage::Warmup)},
    {"PromptPatch0", nullptr, at_stage(Stage::PromptPatch)},
    {"Identify0", nullptr, at_stage(Stage::Identify)},
    {"Identify0-high", nullptr, [](Rig &r) {
       return r.flow.stage() == Stage::Identify && r.outs.mask == 1 &&
              r.outs.code[0] == r.outs.CodeForMv(0, r.outs.stored[0], 6000); }},
    {"Measure0", nullptr, at_stage(Stage::Measure)},
    {"Measure0-mid", nullptr, [](Rig &r) { return r.flow.stage() == Stage::Measure && r.flow.point_index() == 5; }},
    {"PromptPatch1", nullptr, [](Rig &r) { return r.flow.stage() == Stage::PromptPatch && r.flow.channel() == 1; }},
    {"Measure1", nullptr, [](Rig &r) { return r.flow.stage() == Stage::Measure && r.flow.channel() == 1; }},
    {"Results", nullptr, at_stage(Stage::Results)},
    {"Confirm", nullptr, at_stage(Stage::Confirm)},
    {"Backup", [](Rig &r) { r.backup.delay_polls = 100000; }, at_stage(Stage::Backup)},
    {"Write-readrec", nullptr, at_stage(Stage::Write)},
    {"Put-1", nullptr, [](Rig &r) { return r.a.count[CalRec::kCmdPut] == 1; }},
    {"Put-2", nullptr, [](Rig &r) { return r.a.count[CalRec::kCmdPut] == 2; }},
    {"Put-3", nullptr, [](Rig &r) { return r.a.count[CalRec::kCmdPut] == 3; }},
    {"Put-4", nullptr, [](Rig &r) { return r.a.count[CalRec::kCmdPut] == 4; }},
    {"Commit-busy", nullptr, nullptr},
  };
  for (const Case &c : cases) {
    Rig r;
    good_adc(r.a);
    if (c.setup) c.setup(r);
    r.Begin();
    bool reached;
    r.auto_ui = false;  // the prompts are answered by hand, after the stage is seen
    if (std::string(c.name) != "Commit-busy") {
      reached = r.Run([&] {
        if (c.at(r)) return true;
        r.Ui();
        return false;
      });
    } else {
      // All 4 PUT frames through, then the bus stays busy: the COMMIT never goes out.
      r.auto_ui = true;
      reached = r.Run([&] { return r.a.count[CalRec::kCmdPut] == 4; });
      r.a.busy_sends = -1;
      r.Idle(20);
      reached = reached && r.flow.stage() == Stage::Write;
    }
    CHECK(reached);
    if (!reached) { printf("  case %s not reached (stage %s)\n", c.name, F::StageName(r.flow.stage())); continue; }
    const int puts = r.a.count[CalRec::kCmdPut];
    r.flow.Cancel();
    CHECK(r.outs.mask == 0);  // released at once
    r.a.busy_sends = 0;
    r.Idle(20000);
    const bool ok = r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED &&
                    r.a.count[CalRec::kCmdCommit] == 0 && r.a.count[CalRec::kCmdPut] == puts &&
                    r.outs.mask == 0 && !r.bad_hold && logged("CALR,259END,CANCELLED");
    CHECK(ok);
    if (!ok) printf("  case %s: stage %s fail %s commits %d\n", c.name, F::StageName(r.flow.stage()),
                    F::FailName(r.flow.fail()), r.a.count[CalRec::kCmdCommit]);
    // Cancel again: back to Idle.
    r.flow.Cancel();
    CHECK(r.flow.stage() == Stage::Idle);
  }
  {
    // After the COMMIT went out: the status is still read and reported.
    Rig r;
    good_adc(r.a);
    r.Begin();
    CHECK(r.Run([&] { return r.a.count[CalRec::kCmdCommit] == 1; }));
    const int reads = r.a.count[CalRec::kCmdRead];
    r.flow.Cancel();
    CHECK(r.outs.mask == 0);
    CHECK(r.flow.cancel_pending());
    CHECK(r.flow.stage() == Stage::Write);
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED);
    CHECK(r.flow.commit_result() == 1);
    CHECK(r.a.count[CalRec::kCmdRead] > reads);  // the status READ after the cancel
    CHECK(r.a.count[CalRec::kCmdCommit] == 1);
    CHECK(logged("CALR,259END,CANCELLED_OK"));
    CHECK(!logged("CALR,259END,CANCELLED"));
  }
  {
    // After a COMMIT verdict: the end line names it, no further COMMIT goes
    // out, and record() / result() still describe what was put.
    struct After {
      const char *name;
      bool full;  // slots full: the compaction path
      std::function<bool(Rig &)> at;
      const char *end;
      int commits;
    };
    const std::vector<After> after = {
      {"Verify", false, [](Rig &r) { return r.flow.stage() == Stage::Verify; }, "CALR,259END,CANCELLED_OK", 1},
      {"ConfirmCompact", true, [](Rig &r) { return r.flow.stage() == Stage::ConfirmCompact; },
       "CALR,259END,CANCELLED_NO_SLOT", 1},
      {"CompactBackup", true, [](Rig &r) {
         if (r.flow.stage() == Stage::ConfirmCompact) r.flow.ConfirmCompact();
         return r.flow.stage() == Stage::Backup && r.backup.begins == 2; },
       "CALR,259END,CANCELLED_NO_SLOT", 1},
      {"CompactPut", true, [](Rig &r) {
         if (r.flow.stage() == Stage::ConfirmCompact) r.flow.ConfirmCompact();
         return r.a.count[CalRec::kCmdPut] == 6; },
       "CALR,259END,CANCELLED_NO_SLOT", 1},
    };
    for (const After &c : after) {
      if (c.full && !F::kAllowCompaction) continue;  // unreachable: see slots_full_is_a_failed_screen
      Rig r;
      good_adc(r.a);
      if (c.full) {
        install(r.a, 7, 0x31);
        r.a.slots_used = 8;
      }
      r.backup.delay_polls = 50;
      r.Begin();
      const bool reached = r.Run([&] { return c.at(r); });
      CHECK(reached);
      if (!reached) { printf("  case %s not reached (stage %s)\n", c.name, F::StageName(r.flow.stage())); continue; }
      uint8_t put[30];
      memcpy(put, r.flow.record(), 30);
      const uint16_t g0 = r.flow.result().ch[0].gain;
      const int puts = r.a.count[CalRec::kCmdPut];
      r.flow.Cancel();
      CHECK(r.outs.mask == 0);
      r.Idle(20000);
      const bool ok = r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED &&
                      logged(c.end) && !logged("CALR,259END,CANCELLED") &&
                      r.a.count[CalRec::kCmdCommit] == c.commits && r.a.count[CalRec::kCmdPut] == puts;
      CHECK(ok);
      if (!ok) printf("  after-commit case %s: stage %s commits %d last %s\n", c.name,
                      F::StageName(r.flow.stage()), r.a.count[CalRec::kCmdCommit], g_log.back().c_str());
      CHECK(memcmp(put, r.flow.record(), 30) == 0 && r.flow.result().ch[0].gain == g0);
      CHECK(r.flow.result().valid_mask == 3);
    }
  }
  {
    // NoSelfCal and Done: Cancel goes back to Idle and sends nothing.
    Rig r;
    r.Begin(false);
    CHECK(r.flow.stage() == Stage::NoSelfCal);
    r.flow.Cancel();
    CHECK(r.flow.stage() == Stage::Idle && r.a.log.empty());
  }
}

static void backup_times_out() {
  Rig r;
  good_adc(r.a);
  r.backup.delay_polls = 1 << 30;  // Poll returns 0 forever
  r.Begin();
  CHECK(r.Run([&] { return r.flow.stage() == Stage::Backup; }));
  const uint32_t t0 = r.clk.Abs();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_BACKUP);
  const uint32_t took = r.clk.Abs() - t0;
  CHECK(took >= F::kBackupTimeoutMs && took <= F::kBackupTimeoutMs + 5);
  CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  CHECK(logged("CALR,259END,BACKUP"));
}

static void preconditions() {
  {
    Rig r;
    r.Begin(false);
    CHECK(r.flow.stage() == Stage::NoSelfCal && r.flow.fail() == F::F_NO_SELF_CAL);
    r.Idle(5000);
    CHECK(r.flow.stage() == Stage::NoSelfCal);
    CHECK(r.a.log.empty() && r.meter.sent.empty() && r.outs.holds == 0);
    CHECK(logged("CALR,259END,NO_SELF_CAL"));
  }
  {
    Rig r;
    good_adc(r.a);
    r.meter.connected = false;
    r.Begin();
    CHECK(r.flow.stage() == Stage::NoMeter && r.flow.fail() == F::F_NO_METER);
    r.Idle(5000);
    CHECK(r.flow.stage() == Stage::NoMeter && r.a.log.empty());
    r.meter.connected = true;
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Done && r.flow.fail() == F::F_NONE);
  }
  {
    Rig r;
    r.a.drop_replies = true;  // nothing answers at 0x28..0x2B
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_NO_259E);
    CHECK(r.outs.holds == 0);
    for (const auto &f : r.a.log) CHECK(f[3] == CalRec::kCmdRead);
    int addrs = 0;
    for (const auto &f : r.a.log) addrs |= 1 << (f[4] - 0x28);
    CHECK(addrs == 0xF);
  }
  {
    Rig r;  // the meter rejects a setup line
    r.meter.err_reply = [](int) { return "-113,\"Undefined header\""; };
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_METER_CONFIG);
    CHECK(r.a.log.empty());
  }
}

static void meter_lost_fails() {
  Rig r;
  good_adc(r.a);
  r.Begin();
  CHECK(r.Run([&] { return r.flow.stage() == Stage::Measure && r.flow.point_index() == 3; }));
  r.meter.connected = false;
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_METER_LOST);
  CHECK(r.outs.mask == 0);
  CHECK(r.a.count[CalRec::kCmdPut] == 0);
  CHECK(logged("CALR,259END,METER_LOST"));
}

static void verify_fails() {
  Rig r;
  good_adc(r.a);
  // The 259e input bends: +-25 mV over the range, which no line absorbs.
  r.a.input_mv = [&r](int ch) {
    const double v = r.InputMv(&r.a, ch);
    return v + 25.0 * std::sin(kPi * v / 5000.0);
  };
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_VERIFY);
  CHECK(r.a.count[CalRec::kCmdPut] == 0);
  CHECK(r.outs.mask == 0);
}

static void adc_fresh_samples() {
  {
    Rig r;  // a slow module: 8 fresh samples at 100 ms each still fit in 2 s
    good_adc(r.a);
    r.a.sample_ms = 100;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Done);
    CHECK(count_prefix("CALR,259PT,0,1200,") == 1);
    for (const auto &l : g_log)
      if (l.compare(0, 11, "CALR,259PT,") == 0) CHECK(l.substr(l.rfind(',')) == ",8");
  }
  {
    Rig r;  // too slow: F_BUS, nothing written
    good_adc(r.a);
    r.a.sample_ms = 400;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_BUS);
    CHECK(r.a.count[CalRec::kCmdPut] == 0);
    CHECK(r.outs.mask == 0);
  }
}

static void commit_outcomes() {
  {
    Rig r;  // a PUT byte flipped on the wire: the module refuses the record
    good_adc(r.a);
    r.bus.mangle = [](uint8_t *f, uint8_t n) {
      if (n > 8 && f[3] == CalRec::kCmdPut && f[6] == 8) f[8] ^= 0x01;
    };
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_COMMIT_REFUSED);
    CHECK(r.a.count[CalRec::kCmdCommit] == 1);
    CHECK(logged("CALR,259END,COMMIT_REFUSED"));
  }
  {
    Rig r;  // the module goes silent after the COMMIT
    good_adc(r.a);
    r.Begin();
    CHECK(r.Run([&] { return r.a.count[CalRec::kCmdCommit] == 1; }));
    r.a.drop_replies = true;
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_COMMIT_UNKNOWN);
    CHECK(r.a.count[CalRec::kCmdCommit] == 1);
    CHECK(logged("CALR,259END,COMMIT_UNKNOWN"));
  }
  {
    Rig r;  // the stored record reads back different
    good_adc(r.a);
    r.Begin();
    CHECK(r.Run([&] { return r.a.count[CalRec::kCmdCommit] == 1; }));
    r.a.active[21] ^= 0x01;
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_READBACK);
    CHECK(r.a.count[CalRec::kCmdCommit] == 1);
  }
}

// ---- CALR line formats ---------------------------------------------------------

static std::vector<std::string> fixture_lines(const char *file, const char *prefix) {
  std::vector<std::string> out;
  std::ifstream f(file);
  std::string line;
  while (std::getline(f, line))
    if (line.compare(0, strlen(prefix), prefix) == 0) out.push_back(line);
  return out;
}

static size_t fields(const std::string &l) {
  size_t n = 1;
  for (char c : l) n += c == ',';
  return n;
}

static std::string sub_of(const std::string &l) {
  const size_t a = l.find(',') + 1, e = l.find(',', a);
  return l.substr(a, e == std::string::npos ? std::string::npos : e - a);
}

static void golden_lines() {
  const char *fx = "fixtures/cal259e_lines.txt";
  const auto heads = fixture_lines(fx, "CALR#,");
  CHECK(int(heads.size()) == Cal259eLines::HeaderCount());
  for (int i = 0; i < Cal259eLines::HeaderCount() && i < int(heads.size()); ++i)
    CHECK(heads[size_t(i)] == Cal259eLines::Header(i));
  // The meter rule's headers are the Self run's.
  const auto self_heads = fixture_lines("fixtures/calr_lines.txt", "CALR#,");
  for (const char *sub : {"CALR#,RETRY,", "CALR#,REREAD,", "CALR#,version"}) {
    std::string mine, theirs;
    for (const auto &h : heads) if (h.compare(0, strlen(sub), sub) == 0) mine = h;
    for (const auto &h : self_heads) if (h.compare(0, strlen(sub), sub) == 0) theirs = h;
    CHECK(!mine.empty() && mine == theirs);
  }

  std::map<std::string, std::string> want;
  for (const auto &l : fixture_lines(fx, "CALR,")) want[sub_of(l)] = l;
  char b[Cal259eLines::kMaxLine];
  auto is = [&](const char *sub) {
    const bool ok = want.count(sub) && want[sub] == b;
    CHECK(ok);
    if (!ok) printf("  %s got: %s\n  want: %s\n", sub, b, want.count(sub) ? want[sub].c_str() : "(none)");
  };
  using namespace Cal259eLines;
  Start(b, sizeof(b), 0x29); is("259START");
  Point(b, sizeof(b), 0, 4800, 4800.1234, 29076, 8); is("259PT");
  Fit(b, sizeof(b), 1, CalRec::Channel{0x8F2C, -120}, -1.2345, 0.449); is("259FIT");
  Backup(b, sizeof(b), 0x28, 990, 0x1A2B3C4Du, "OK"); is("259BK");
  CalRec::Record rec;
  for (int i = 0; i < 8; ++i) rec.unit_id[i] = uint8_t(i + 1);
  rec.seq = 2;
  rec.date = 1761264000u;
  rec.ch[0] = {0x8F2C, -120};
  rec.ch[1] = {0x9041, 15};
  uint8_t bytes[30];
  CalRec::Encode259e(rec, bytes);
  Rec(b, sizeof(b), 0x28, bytes); is("259REC");
  End(b, sizeof(b), F::FailName(F::F_COMMIT_UNKNOWN)); is("259END");
  CalRoutineLines::Retry(b, sizeof(b), "READ?", 2); is("RETRY");
  CalRoutineLines::Reread(b, sizeof(b), 0, 3600, 19661, 0.734); is("REREAD");

  std::map<std::string, size_t> nf;
  for (int i = 1; i < HeaderCount(); ++i) nf[sub_of(Header(i))] = fields(Header(i));
  for (const auto &kv : want) {
    CHECK(nf.count(kv.first));
    CHECK(nf[kv.first] == fields(kv.second));
  }
  CHECK(want.size() == nf.size());

  // A run with a meter retry and a re-read: the headers first, in order, then
  // only known SUBs with their field counts.
  Rig r;
  good_adc(r.a);
  r.meter.extra = [](int n) { return (n >= 9 && n < 12) ? ((n & 1) ? 0.5 : -0.5) : 0.0; };
  r.Begin();
  CHECK(r.Run([&] { return r.meter.reads >= 30; }));
  r.meter.fail_next = 1;
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Done);
  int heads_seen = 0;
  for (const auto &l : g_log) {
    if (l.compare(0, 6, "CALR#,") == 0) {
      CHECK(heads_seen < HeaderCount() && l == Header(heads_seen));
      ++heads_seen;
      continue;
    }
    CHECK(heads_seen == HeaderCount());  // no line before the headers are all out
    CHECK(l.compare(0, 5, "CALR,") == 0);
    const std::string sub = sub_of(l);
    const bool known = nf.count(sub) && nf[sub] == fields(l);
    CHECK(known);
    if (!known) printf("  irregular: %s\n", l.c_str());
    CHECK(l.size() < kMaxLine);
  }
  CHECK(heads_seen == HeaderCount());
  for (const char *sub : {"CALR,259START,", "CALR,259PT,", "CALR,259FIT,", "CALR,259REC,",
                          "CALR,259END,", "CALR,RETRY,", "CALR,REREAD,"})
    CHECK(count_prefix(sub) > 0);
}

static void names() {
  for (int s = int(Stage::Idle); s <= int(Stage::Failed); ++s)
    CHECK(strcmp(F::StageName(Stage(s)), "?") != 0);
  for (int f = F::F_NONE; f <= F::F_INTERNAL; ++f)
    CHECK(strcmp(F::FailName(F::FailCode(f)), "?") != 0);
  CHECK(std::fabs(F::IdentityThreshold() - 0.5 * 4800.0 * 4096.0 / 1200.0 / (0x9041 / 65536.0)) < 1e-6);
}

// ---- the reviewer's follow-ups ---------------------------------------------------

// S4: the COMMIT's Send returns a Wire error. Not "nothing went out": the status
// is polled, the end line is not BUS, and exactly one COMMIT is ever sent.
static void commit_wire_error() {
  for (int rc : {2, 3, 4, 5}) {
    {
      Rig r;  // the module never saw the frame
      good_adc(r.a);
      r.a.commit_send_rc = rc;
      r.a.commit_latches = false;
      r.Begin();
      CHECK(r.Run());
      CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_COMMIT_UNKNOWN);
      CHECK(r.a.count[CalRec::kCmdCommit] == 1 && r.a.commits_received == 0);
      CHECK(logged("CALR,259END,COMMIT_UNKNOWN") && !logged("CALR,259END,BUS"));
      CHECK(r.flow.commit_result() == E_COMMIT_UNKNOWN);
      CHECK(r.outs.mask == 0);
    }
    {
      Rig r;  // a NAK on the last byte, but the module latched it: the status says so
      good_adc(r.a);
      r.a.commit_send_rc = rc;
      r.a.commit_latches = true;
      r.Begin();
      CHECK(r.Run());
      CHECK(r.flow.stage() == Stage::Done);
      CHECK(r.a.count[CalRec::kCmdCommit] == 1 && r.a.commits_received == 1);
      CHECK(logged("CALR,259END,OK"));
    }
  }
  {
    Rig r;  // cancelled while the wire-error COMMIT's status is awaited: still one COMMIT, still reported
    good_adc(r.a);
    r.a.commit_send_rc = 3;
    r.a.commit_latches = true;
    r.Begin();
    CHECK(r.Run([&] { return r.a.count[CalRec::kCmdCommit] == 1; }));
    r.flow.Cancel();
    CHECK(r.flow.cancel_pending());
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED);
    CHECK(r.a.count[CalRec::kCmdCommit] == 1);
    CHECK(logged("CALR,259END,CANCELLED_OK"));
  }
  {
    Rig r;  // -1 (tx gate closed) before the COMMIT goes out is still nothing sent, then retried
    good_adc(r.a);
    r.Begin();
    CHECK(r.Run([&] { return r.flow.stage() == Stage::Write; }));
    r.a.busy_sends = 5;
    CHECK(r.Run());
    CHECK(r.a.count[CalRec::kCmdCommit] == 1);
  }
}

// SUSPEND (the app's Cancel, twice) after the COMMIT was ACKed: the verdict is
// still collected by whoever ticks the flow afterwards (CalRoutinePorts::Task259),
// one COMMIT, a CANCELLED_* line, CalRunActive-equivalent cancel_pending() until then.
static void suspend_in_write_after_ack() {
  Rig r;
  good_adc(r.a);
  r.Begin();
  CHECK(r.Run([&] { return r.a.count[CalRec::kCmdCommit] == 1; }));
  CHECK(r.flow.stage() == Stage::Write);
  r.flow.Cancel();                                   // HandleAppEvent(SUSPEND)
  if (r.flow.stage() != Stage::Idle) r.flow.Cancel();
  CHECK(r.flow.cancel_pending());                    // CalRunActive() stays true
  CHECK(r.outs.mask == 0);
  r.auto_ui = false;                                 // the app is gone: only the task ticks
  int guard = 0;
  while (r.flow.cancel_pending() && guard++ < 20000) r.Step();
  CHECK(!r.flow.cancel_pending());
  CHECK(r.a.count[CalRec::kCmdCommit] == 1 && r.a.commits_received == 1);
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_CANCELLED);
  CHECK(logged("CALR,259END,CANCELLED_OK"));
  CHECK(r.flow.commit_result() == 1);
  // and once it is collected a new run may start
  CHECK(r.flow.Begin(r.meter, r.outs, r.clk, r.bus, r.backup, r.ent, true, capture));
}

// S6: a new run is refused while a cancelled run's COMMIT verdict is pending.
static void begin_refused_while_cancel_pending() {
  Rig r;
  good_adc(r.a);
  r.Begin();
  CHECK(r.Run([&] { return r.a.count[CalRec::kCmdCommit] == 1; }));
  r.flow.Cancel();
  CHECK(r.flow.cancel_pending());
  const size_t logn = g_log.size();
  CHECK(!r.flow.Begin(r.meter, r.outs, r.clk, r.bus, r.backup, r.ent, true, capture));
  CHECK(g_log.size() == logn);                        // no headers: nothing was touched
  CHECK(r.flow.cancel_pending() && r.flow.stage() == Stage::Write);
  CHECK(r.a.count[CalRec::kCmdCommit] == 1);
  r.auto_ui = false;
  int guard = 0;
  while (r.flow.cancel_pending() && guard++ < 20000) r.Step();
  CHECK(logged("CALR,259END,CANCELLED_OK"));          // the verdict was not lost
  CHECK(r.a.count[CalRec::kCmdCommit] == 1);
}

// Decision c: nonlinearity is a pass criterion. A top-of-range kink that the line
// does not follow ends Failed/F_NONLIN, after the fit line is logged.
static void nonlinearity_is_a_pass_criterion() {
  CHECK(F::kMaxNonlinearityCents == 5.0);
  // F_NONLIN needs a fit-point residual over 5 cents with the verify points
  // within 3: a 10 mV bump at the 4800 mV fit point alone (1 mV = 1 cent at
  // 1.2 V/oct; the verify points are elsewhere).
  Rig r;
  good_adc(r.a);
  r.a.input_mv = [&r](int ch) {
    const double mv = r.InputMv(&r.a, ch);
    return std::fabs(mv - 4800.0) < 100.0 ? mv + 10.0 : mv;
  };
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed);
  CHECK(r.flow.fail() == F::F_NONLIN);
  CHECK(r.flow.result().nonlin_cents[0] > F::kMaxNonlinearityCents);
  CHECK(std::fabs(r.flow.result().worst_verify_cents[0]) <= F::kVerifyCents);
  CHECK(logged("CALR,259END,NONLIN"));
  CHECK(count_prefix("CALR,259FIT,0,") == 1);
  CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  CHECK(r.flow.result().valid_mask == 0);
}

// Decision c: the nonlinearity figure is the UNCLAMPED fit-point residual. A kink at
// the 9600 mV point pushes its corrected code past the 259e's 0x7FFF clamp; the
// clamped residual would show about -0.3 cents there and hide it. (The run ends
// F_VERIFY here, the kink being large enough to show at 9000 mV as well; the
// figures are stored before that check.)
static void nonlinearity_sees_past_the_clamp() {
  Rig r;
  good_adc(r.a);
  r.a.input_mv = [&r](int ch) {
    const double mv = r.InputMv(&r.a, ch);
    return mv > 9300.0 ? mv + 14.0 : mv;
  };
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.result().nonlin_cents[0] > 7.0);
}

// Decision c: 9000 mV verifies the top of the range, where the 9600 mV fit point's
// own residual is clamped (target 32768 > 0x7FFF).
static void top_of_range_is_verified() {
  CHECK(F::kPointMv[F::kPoints - 1] == 9000);
  CHECK(F::kPointMv[F::kFitPoints - 1] == 9600);
  // The clamped residual hides a positive error at 9600 mV; the unclamped shows it.
  const CalRec::Channel c = {0x9041, 25};
  const uint16_t raw = uint16_t(32768.0 / (0x9041 / 65536.0));
  CHECK(CalRec::ResidualCents(raw, 9600.0, c) < 0.5);
  CHECK(CalRec::ResidualCentsUnclamped(raw, 9600.0, c) > 5.0);
}

// Decision d/e: results expire; ConfirmWrite re-checks valid_mask and the age.
static void results_expire_and_confirm_rechecks() {
  {
    Rig r;
    good_adc(r.a);
    r.hold_confirm = true;
    r.Begin();
    CHECK(r.Run([&] { return r.flow.stage() == Stage::Confirm; }));
    // the age runs from channel 0's first point: the measurement time is in it
    const uint32_t age0 = r.flow.results_age_ms();
    CHECK(age0 > 1000 && age0 < F::kResultsMaxAgeMs - 10000);
    r.Idle(F::kResultsMaxAgeMs - 2000 - age0);
    CHECK(r.flow.stage() == Stage::Confirm);
    CHECK(r.flow.results_age_ms() >= F::kResultsMaxAgeMs - 2100);
    r.Idle(3000);
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_EXPIRED);
    CHECK(logged("CALR,259END,EXPIRED"));
    CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
    CHECK(r.backup.begins == 0);
  }
  {
    Rig r;  // expired on the Results screen too
    good_adc(r.a);
    r.auto_ui = false;
    r.Begin();
    for (int i = 0; i < 2000000 && r.flow.stage() != Stage::Results; ++i) {
      r.Step();
      if (r.flow.stage() == Stage::Warmup || r.flow.stage() == Stage::PromptPatch) r.Ui();
    }
    CHECK(r.flow.stage() == Stage::Results);
    r.Idle(F::kResultsMaxAgeMs + 1000);
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_EXPIRED);
  }
  {
    Rig r;  // the clock reads past the limit at the moment of the B press itself
    good_adc(r.a);
    r.hold_confirm = true;
    r.Begin();
    CHECK(r.Run([&] { return r.flow.stage() == Stage::Confirm; }));
    r.clk.t += F::kResultsMaxAgeMs + 1;
    r.flow.ConfirmWrite();
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_EXPIRED);
    CHECK(r.backup.begins == 0 && r.a.count[CalRec::kCmdPut] == 0);
  }
  {
    Rig r;  // valid_mask != 3 at the confirm: refused, nothing backed up or sent
    good_adc(r.a);
    r.hold_confirm = true;
    r.Begin();
    CHECK(r.Run([&] { return r.flow.stage() == Stage::Confirm; }));
    CHECK(r.flow.result().valid_mask == 3);
    const_cast<Cal259eFlow::Result &>(r.flow.result()).valid_mask = 1;
    r.flow.ConfirmWrite();
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_INTERNAL);
    CHECK(r.backup.begins == 0 && r.a.count[CalRec::kCmdPut] == 0 &&
          r.a.count[CalRec::kCmdCommit] == 0);
  }
  {
    Rig r;  // inside the limit the confirm goes on
    good_adc(r.a);
    r.hold_confirm = true;
    r.Begin();
    CHECK(r.Run([&] { return r.flow.stage() == Stage::Confirm; }));
    r.flow.ConfirmWrite();
    CHECK(r.flow.stage() == Stage::Backup);
    r.hold_confirm = false;
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Done);
  }
}

// M2: the age limit runs from channel 0's FIRST measured point (not from the last channel's
// fit), so a prompt left open before channel 1 expires, and a slow channel 1 expires the
// results even though channel 1 itself was quick. After expiry the run starts over.
static void expiry_counts_from_the_first_point() {
  {
    Rig r;  // an operator leaves the channel-1 patch prompt open for hours
    good_adc(r.a);
    r.auto_ui = false;
    r.Begin();
    for (int i = 0; i < 2000000 && count_prefix("CALR,259PT,0,") < 1; ++i) {
      r.Step();
      if (r.flow.stage() == Stage::Warmup || r.flow.stage() == Stage::PromptPatch) r.Ui();
    }
    const uint32_t t1 = r.clk.t;
    for (int i = 0; i < 2000000 && !(r.flow.stage() == Stage::PromptPatch && r.flow.channel() == 1); ++i) {
      r.Step();
      if (r.flow.stage() == Stage::Warmup || (r.flow.stage() == Stage::PromptPatch && r.flow.channel() == 0)) r.Ui();
    }
    CHECK(r.flow.stage() == Stage::PromptPatch && r.flow.channel() == 1);
    CHECK(r.clk.t - t1 < F::kResultsMaxAgeMs - 10000);          // the first channel is quick
    r.Idle(F::kResultsMaxAgeMs - 5000 - (r.clk.t - t1));
    CHECK(r.flow.stage() == Stage::PromptPatch);                 // still inside the limit
    r.Idle(10000);
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_EXPIRED);
    CHECK(logged("CALR,259END,EXPIRED"));
    r.flow.Cancel();                                              // dismissed: Idle, start over
    CHECK(r.flow.stage() == Stage::Idle);
    g_log.clear();
    r.Begin();                                                    // a fresh run measures again
    CHECK(r.flow.stage() != Stage::Failed && r.flow.result().valid_mask == 0);
    r.auto_ui = true;
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Confirm || r.flow.stage() == Stage::Done);
    CHECK(count_prefix("CALR,259PT,0,") == F::kPoints);
  }
  {
    Rig r;  // a slow channel 1 (prompt answered at 9m55 after the first point) expires the run
    good_adc(r.a);
    r.auto_ui = false;
    r.hold_confirm = true;
    r.Begin();
    for (int i = 0; i < 2000000 && count_prefix("CALR,259PT,0,") < 1; ++i) {
      r.Step();
      if (r.flow.stage() == Stage::Warmup || r.flow.stage() == Stage::PromptPatch) r.Ui();
    }
    const uint32_t t1 = r.clk.t;
    for (int i = 0; i < 2000000 && !(r.flow.stage() == Stage::PromptPatch && r.flow.channel() == 1); ++i) {
      r.Step();
      if (r.flow.stage() == Stage::Warmup || (r.flow.stage() == Stage::PromptPatch && r.flow.channel() == 0)) r.Ui();
    }
    r.Idle(F::kResultsMaxAgeMs - 5000 - (r.clk.t - t1));
    r.Ui();                                                       // answer the prompt
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_EXPIRED);
    CHECK(r.backup.begins == 0 && r.a.count[CalRec::kCmdPut] == 0);
  }
}

// Wrong jack on the second channel only (channel 0 is patched right): the
// per-channel input model sees A on the principal input while the mod input is
// asked: nothing moves, F_IDENTITY, nothing is written.
static void wrong_jack_on_channel_one() {
  Rig r;
  good_adc(r.a);
  r.wrong_jack = true;
  r.wrong_jack_ch = 1;
  r.Begin();
  CHECK(r.Run());
  CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_IDENTITY);
  CHECK(r.flow.result().valid_mask == 1);
  CHECK(count_prefix("CALR,259PT,0,") == F::kPoints && count_prefix("CALR,259PT,1,") == 0);
  CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  CHECK(r.backup.begins == 0);
  CHECK(r.outs.mask == 0 && !r.bad_hold);
}

// Decision b and S1: the backup's reason reaches the Failed screen and the log, its
// bytes are logged, and the flow refuses a bank that is not 990 bytes whatever the port said.
static void backup_reason_size_and_line() {
  {
    Rig r;
    good_adc(r.a);
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Done);
    CHECK(logged("CALR,259BK,28,990,A1B2C3D4,OK"));
    CHECK(r.flow.backup_why() == BK_NONE);
  }
  {
    Rig r;  // refused at Begin with a reason (firmware card served)
    good_adc(r.a);
    r.backup.begin_ok = false;
    r.backup.why = BK_FW_CARD;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_BACKUP);
    CHECK(r.flow.backup_why() == BK_FW_CARD);
    CHECK(logged("CALR,259BK,28,990,A1B2C3D4,FW_CARD"));
    CHECK(logged("CALR,259END,BACKUP"));
    CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  }
  for (uint32_t n : {989u, 991u}) {
    Rig r;  // a port that says "saved" for a bank of the wrong size
    good_adc(r.a);
    r.backup.bytes = n;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.stage() == Stage::Failed && r.flow.fail() == F::F_BACKUP);
    CHECK(r.flow.backup_why() == BK_SIZE);
    char want[64];
    snprintf(want, sizeof(want), "CALR,259BK,28,%u,A1B2C3D4,SIZE", unsigned(n));
    CHECK(logged(want));
    CHECK(r.a.count[CalRec::kCmdPut] == 0 && r.a.count[CalRec::kCmdCommit] == 0);
  }
  {
    Rig r;  // the port fails during the transfer
    good_adc(r.a);
    r.backup.result = -1;
    r.backup.why = BK_VERIFY;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.fail() == F::F_BACKUP && r.flow.backup_why() == BK_VERIFY);
    CHECK(logged("CALR,259BK,28,990,A1B2C3D4,VERIFY"));
  }
  {
    Rig r;  // gives up: BK_TIMEOUT
    good_adc(r.a);
    r.backup.delay_polls = 1 << 30;
    r.Begin();
    CHECK(r.Run());
    CHECK(r.flow.fail() == F::F_BACKUP && r.flow.backup_why() == BK_TIMEOUT);
  }
}

int main() {
  good_unit_passes();
  scale_off_fails();
  wrong_jack_fails_identity();
  several_259es_one_moves();
  several_259es_both_move();
  meter_disagrees_fails_patch();
  noisy_meter_fails();
  backup_fails_no_write();
  unit_id_kept();
  slots_full_is_a_failed_screen();
  slots_full_needs_second_confirm();
  cancel_every_stage();
  backup_times_out();
  preconditions();
  meter_lost_fails();
  verify_fails();
  adc_fresh_samples();
  commit_outcomes();
  backup_reason_size_and_line();
  commit_wire_error();
  suspend_in_write_after_ack();
  begin_refused_while_cancel_pending();
  nonlinearity_is_a_pass_criterion();
  top_of_range_is_verified();
  nonlinearity_sees_past_the_clamp();
  results_expire_and_confirm_rechecks();
  expiry_counts_from_the_first_point();
  wrong_jack_on_channel_one();
  golden_lines();
  names();
  printf("test_cal259e_flow: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
