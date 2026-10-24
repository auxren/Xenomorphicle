// Host tests for the Cal Routine's meter rule as a class of its own
// (src/CalRoutineMeasure.h): CalRoutine::MeterMeasure against the simulated
// DMM and DAC of fake_meter.h. Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_calroutine_measure
//     test_calroutine_measure.cpp ../src/CalRoutineMeasure.cpp ../src/CalRoutineFlow.cpp
//     ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
// (CalRoutineFlow.cpp is linked for the CALR line formats the rule logs with.)
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/CalRoutineMeasure.h"
#include "fake_meter.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static std::vector<std::string> g_log;
static void capture(const char *line) { g_log.push_back(line); }

struct Rig {
  SimDac dac;
  FakeClock clk;
  FakeOutputs outs{&dac};
  FakeMeter meter{&outs, &clk};
  MeterMeasure m;
  Rig() {
    g_log.clear();
    outs.clk = &clk;
    m.Bind(meter, outs, capture);
  }
  // Steps every 5 ms until the measurement leaves BUSY (or 60 s pass).
  MeterMeasure::Result Run(uint32_t *ms_taken = nullptr) {
    const uint32_t t0 = clk.t;
    MeterMeasure::Result r = MeterMeasure::BUSY;
    for (int i = 0; i < 12000 && r == MeterMeasure::BUSY; ++i) {
      r = m.Step(clk.Ms());
      if (r == MeterMeasure::BUSY) clk.t += 5;
    }
    if (ms_taken) *ms_taken = clk.t - t0;
    return r;
  }
  int Reads() const {
    int n = 0;
    for (const auto &s : meter.sent) n += s == "READ?";
    return n;
  }
};

static void steady_meter_done() {
  Rig r;
  r.dac.custom = [](int, uint16_t) { return 5000.0; };
  r.meter.noise_mv = 0.02;
  r.m.Start(2, 30000, 5000, r.clk.Ms());
  CHECK(r.outs.holds == 1 && r.outs.held == 2 && r.outs.held_code == 30000);
  CHECK(r.meter.sent.empty());
  const MeterMeasure::Result res = r.Run();
  CHECK(res == MeterMeasure::DONE);
  CHECK(r.Reads() == 1 + kReadings);  // the throwaway, then kReadings
  CHECK(r.meter.read_mv.size() == size_t(1 + kReadings));
  // The median of the readings after the throwaway.
  std::vector<double> used(r.meter.read_mv.begin() + 1, r.meter.read_mv.end());
  const CalRoutineMath::Reading want = CalRoutineMath::Summarize(used.data(), int(used.size()));
  CHECK(std::fabs(r.m.reading().median_mv - want.median_mv) < 1e-6);  // the reply's NR3 text
  CHECK(std::fabs(r.m.reading().spread_mv - want.spread_mv) < 1e-6);
  CHECK(std::fabs(r.m.reading().median_mv - 5000.0) < 0.05);
  CHECK(CalRoutineMath::ReadingTrusted(r.m.reading()));
  CHECK(r.outs.holds == 1);  // one measurement = one hold
  for (const auto &l : g_log) CHECK(l.compare(0, 11, "CALR,REREAD") != 0);
}

static void noisy_then_reread_then_noisy() {
  Rig r;
  r.dac.custom = [](int, uint16_t) { return 5000.0; };
  // Every READ? after the throwaway alternates by 4 mV: spread > 0.5 mV, twice.
  r.meter.extra = [](int n) { return (n & 1) ? 2.0 : -2.0; };
  r.m.Start(0, 1000, 5000, r.clk.Ms());
  CHECK(r.Run() == MeterMeasure::NOISY);
  CHECK(r.Reads() == 1 + 2 * kReadings);  // throwaway, set, one re-read set
  int rereads = 0;
  for (const auto &l : g_log) rereads += l.compare(0, 11, "CALR,REREAD") == 0;
  CHECK(rereads == 1);
  CHECK(!CalRoutineMath::ReadingTrusted(r.m.reading()));
  // Idle after NOISY: a further Step has nothing to do.
  CHECK(r.m.Step(r.clk.Ms()) == MeterMeasure::FAIL);
}

static void noisy_then_clean_reread_is_done() {
  Rig r;
  r.dac.custom = [](int, uint16_t) { return 5000.0; };
  // Only the first set (reads 1..3) is noisy; the re-read set is clean.
  r.meter.extra = [](int n) { return (n >= 1 && n <= 3) ? ((n & 1) ? 2.0 : -2.0) : 0.0; };
  r.m.Start(0, 1000, 5000, r.clk.Ms());
  CHECK(r.Run() == MeterMeasure::DONE);
  CHECK(r.Reads() == 1 + 2 * kReadings);
  CHECK(CalRoutineMath::ReadingTrusted(r.m.reading()));
}

static void final_readings_count() {
  Rig r;
  r.dac.custom = [](int, uint16_t) { return 7000.0; };
  r.m.Start(1, 40000, 7000, r.clk.Ms(), kFinalReadings);
  CHECK(r.Run() == MeterMeasure::DONE);
  CHECK(r.Reads() == 1 + kFinalReadings);
  CHECK(std::fabs(r.m.reading().median_mv - 7000.0) < 0.05);
}

static void meter_unplug_lost() {
  Rig r;
  r.dac.custom = [](int, uint16_t) { return 5000.0; };
  r.m.Start(0, 1000, 5000, r.clk.Ms());
  // Let the first READ? start, then pull the meter.
  MeterMeasure::Result res = MeterMeasure::BUSY;
  for (int i = 0; i < 400 && r.meter.sent.empty(); ++i) { res = r.m.Step(r.clk.Ms()); r.clk.t += 5; }
  CHECK(res == MeterMeasure::BUSY && r.meter.sent.size() == 1);
  r.meter.connected = false;
  CHECK(r.Run() == MeterMeasure::LOST);
  // Idle: no further meter traffic.
  const size_t n = r.meter.sent.size();
  r.clk.t += 1000;
  CHECK(r.m.Step(r.clk.Ms()) == MeterMeasure::FAIL);
  CHECK(r.meter.sent.size() == n);
}

static void meter_never_answers_fails() {
  Rig r;
  r.dac.custom = [](int, uint16_t) { return 5000.0; };
  r.meter.never_answer = true;
  r.m.Start(0, 1000, 5000, r.clk.Ms());
  CHECK(r.Run() == MeterMeasure::FAIL);
  // Initial try plus kQueryRetries retries, each logged.
  int retries = 0;
  for (const auto &l : g_log) retries += l.compare(0, 10, "CALR,RETRY") == 0;
  CHECK(retries == kQueryRetries);
}

static void settle_respected() {
  Rig r;
  r.dac.custom = [](int, uint16_t) { return 5000.0; };
  r.m.Start(0, 1000, 5000, r.clk.Ms());
  const uint32_t t0 = r.clk.t;
  uint32_t first_read_ms = 0;
  for (int i = 0; i < 1000; ++i) {
    r.m.Step(r.clk.Ms());
    if (!r.meter.sent.empty()) { first_read_ms = r.clk.t - t0; break; }
    r.clk.t += 1;
  }
  CHECK(!r.meter.sent.empty() && r.meter.sent[0] == "READ?");
  // The step at kOutSettleMs starts the query, the next one sends it; never before.
  CHECK(first_read_ms >= kOutSettleMs && first_read_ms <= kOutSettleMs + 1);
  // A second measurement settles again, from its own Start.
  r.Run();
  const size_t before = r.meter.sent.size();
  r.m.Start(0, 2000, 5000, r.clk.Ms());
  for (uint32_t i = 0; i + 1 < kOutSettleMs; ++i) { r.clk.t += 1; r.m.Step(r.clk.Ms()); }
  CHECK(r.meter.sent.size() == before);
}

int main() {
  steady_meter_done();
  noisy_then_reread_then_noisy();
  noisy_then_clean_reread_is_done();
  final_readings_count();
  meter_unplug_lost();
  meter_never_answers_fails();
  settle_respected();
  printf("test_calroutine_measure: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
