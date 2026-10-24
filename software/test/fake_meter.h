// Simulated hardware for the meter rule's host tests: a bowed DAC per output,
// the output port that records what it held and when, and the Siglent DMM
// (NPLC 10, autozero ON) with latency, noise, transient errors and unplugging.
// Shared by test_calroutine_flow.cpp and test_calroutine_measure.cpp.
// Header-only, one translation unit per test.
#ifndef FAKE_METER_H_
#define FAKE_METER_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../src/CalRoutineFlow.h"

using namespace CalRoutine;
namespace CR = CalRoutineMath;

static const double kPi = 3.14159265358979323846;

struct FakeClock : Clock {
  uint32_t t = 0xFFFF0000u;  // wraps during every run
  uint32_t Ms() const override { return t; }
  uint32_t Abs() const { return t - 0xFFFF0000u; }  // ms since the start, no wrap
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
  uint8_t mask = 0;           // outputs held
  uint16_t code[8] = {};      // ... and their codes
  int held = -1;              // the one held output; -1 none, -2 several
  uint16_t held_code = 0;     // its code
  int holds = 0, releases = 0;
  // What the output was holding since the start, for a meter whose reading
  // spans a code change (the meter is on one output; several held = none).
  const FakeClock *clk = nullptr;
  struct Seg { int64_t t; int out; uint16_t code; };
  std::vector<Seg> hist{{0, -1, 0}};
  // Mean mV over [a, b) ms; *changed when the output changed inside it.
  double AvgMv(int64_t a, int64_t b, bool *changed) const {
    double sum = 0.0;
    int n_seg = 0;
    for (size_t i = 0; i < hist.size(); ++i) {
      const int64_t s0 = hist[i].t, s1 = i + 1 < hist.size() ? hist[i + 1].t : INT64_MAX;
      const int64_t lo = std::max(a, s0), hi = std::min(b, s1);
      if (hi <= lo) continue;
      ++n_seg;
      sum += (hist[i].out >= 0 ? dac->Mv(hist[i].out, hist[i].code) : 0.0) * double(hi - lo);
    }
    if (changed) *changed = n_seg > 1;
    return sum / double(b - a);
  }
  explicit FakeOutputs(const SimDac *d) : dac(d) {
    for (int o = 0; o < 8; ++o)
      for (int k = 0; k < CR::kNodes; ++k)
        stored[o][k] = uint16_t(std::lround(k * 1200 * double(CR::kNominalCodesPerMv)));
  }
  void Recount() {
    held = -1;
    for (int o = 0; o < 8; ++o)
      if (mask & (1u << o)) held = held == -1 ? o : -2;
    held_code = held >= 0 ? code[held] : 0;
    if (clk) hist.push_back({int64_t(clk->Abs()), held >= 0 ? held : -1, held_code});
  }
  void Hold(int out, uint16_t c) override {
    mask = uint8_t(mask | (1u << out));
    code[out] = c;
    ++holds;
    Recount();
  }
  void ReleaseAll() override {
    mask = 0;
    ++releases;
    Recount();
  }
  uint16_t TableCode(int out, int k) const override { return stored[out][k]; }
  uint16_t CodeForMv(int, const uint16_t t[CR::kNodes], int32_t mv) const override {
    if (mv <= 0) return t[0];
    if (mv >= 12000) return t[10];
    const int k = mv / 1200;
    const double f = (mv - k * 1200) / 1200.0;
    return uint16_t(std::lround(t[k] + f * (double(t[k + 1]) - double(t[k]))));
  }
  double OutMv(int o) const { return o >= 0 && (mask & (1u << o)) ? dac->Mv(o, code[o]) : 0.0; }
};

struct FakeMeter : MeterPort {
  FakeOutputs *outs;
  FakeClock *clk;
  bool connected = true;
  bool never_answer = false;
  // The SDM3065X at NPLC 10, autozero ON (kMeterSetup), as measured on the
  // bench 2026-10-24: each READ? takes ~530 ms and is a real conversion. The
  // meter converts continuously, so the reading a READ? returns was already
  // in progress when it was asked (it began conv_lead_ms earlier) and spans
  // any code change in that window: its value is the mean over the window,
  // about the midpoint of a step. Below NPLC 10 the meter returns a smoothed,
  // stale stream; that is not modelled because the flow never uses it.
  uint32_t read_ms = 530;
  int conv_lead_ms = -1;     // < 0: half a conversion
  int64_t begin_abs = 0;
  uint32_t cmd_ms = 20;
  int fail_next = 0;        // next N completions report an error
  int refuse_begin = 0;     // next N Begin() calls are refused (driver recovering)
  double offset_mv = 0.0;   // reading offset (a replug moves it)
  double noise_mv = 0.02;   // uniform +/-
  std::function<double(int read_index)> extra;  // added to READ? #n (0-based)
  // SYST:ERR? #n (0-based) answers this instead of the empty queue's reply.
  std::function<const char *(int err_index)> err_reply;
  int err_queries = 0;
  std::vector<std::string> sent;
  std::vector<std::pair<int, uint16_t>> read_at;  // (held output, code) at each READ?
  std::vector<int> read_hold;       // outs.holds at each READ? (one measurement = one hold)
  std::vector<double> read_mv;      // the value each READ? returned
  std::vector<bool> read_straddled; // its window spanned a code change
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
    begin_abs = clk->Abs();
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
      const int64_t lead = conv_lead_ms < 0 ? int64_t(read_ms / 2) : int64_t(conv_lead_ms);
      bool straddled = false;
      double mv = outs->AvgMv(begin_abs - lead, begin_abs - lead + read_ms, &straddled) + offset_mv +
                  u * noise_mv;
      if (extra) mv += extra(reads);
      ++reads;
      read_at.push_back({outs->held, outs->held_code});
      read_hold.push_back(outs->holds);
      read_mv.push_back(mv);
      read_straddled.push_back(straddled);
      snprintf(reply, sizeof(reply), "%+.9E\n", mv / 1000.0);
    } else if (cur == "SYST:ERR?") {
      const char *e = err_reply ? err_reply(err_queries) : nullptr;
      ++err_queries;
      snprintf(reply, sizeof(reply), "%s\n", e ? e : "0,\"No error\"");  // as the SDM3065X
    }
    *r = reply;
    return 1;
  }
  bool Connected() const override { return connected; }
  const char *Identity() const override { return "Siglent Technologies,SDM3065X,SDM36GBX000000,1.01.01.25\n"; }
};

#endif  // FAKE_METER_H_
