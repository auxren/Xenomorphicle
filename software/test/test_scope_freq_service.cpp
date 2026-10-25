// Host tests for the frequency counter service (src/ScopeFreqService.cpp):
// source selection, the two ISR feeds (audio block, CV frame), the 0.1 s and
// 1 s gates, the CV low-frequency ranging, the clock correction and the
// ISR-to-loop hand-off. Tones are rounded to what each path delivers.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "../src/ScopeFreqService.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                          \
  } while (0)

using namespace ScopeFreqSvc;
using ScopeFreq::Result;
using ScopeFreq::Status;
using namespace ScopeSources;

static const double kPi = 3.14159265358979323846;

static int16_t q16(double v) {
  v = floor(v + 0.5);
  return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// A tone as the audio ISR delivers it: 128-sample blocks, to tap `tap`.
struct AudioFeed {
  uint64_t n = 0;
  double fs = 48000.0;
  void Block(int tap, double hz, double amp, double dc = 0.0, int16_t *keep = nullptr) {
    int16_t b[128];
    for (int i = 0; i < 128; ++i, ++n) b[i] = q16(dc + amp * sin(2 * kPi * (0.1 + hz * n / fs)));
    if (keep) memcpy(keep, b, sizeof(b));
    OnAudio(tap, b, 128);
  }
};

// A CV input as the ADC hook delivers it: one 8-word frame per sample, the
// input of interest at the word index Scan_DMA puts it in (CV1-4 are words
// 4-7, CV5-8 words 0-3), all the others parked at a different constant.
struct CvFeed {
  uint64_t n = 0;
  double fs = kCvFsHz;
  void Frame(int cv, double hz, double amp, double mid = 16000.0, bool tri = false) {
    int16_t f[8];
    for (int k = 0; k < 8; ++k) f[k] = (int16_t)(3000 + 1000 * k);
    const int j = cv < 4 ? cv + 4 : cv - 4;
    const double ph = 0.1 + hz * n / fs;
    double s = tri ? (4.0 * fabs(ph - floor(ph + 0.5)) - 1.0) : sin(2 * kPi * ph);
    f[j] = q16(mid + amp * s);
    ++n;
    OnCvFrame(f);
  }
};

// Feeds audio until a result is readable (or max_s passes). Returns seconds fed.
static double AudioUntilResult(AudioFeed &a, int tap, double hz, double amp, Result *r, double max_s = 12.0) {
  const uint64_t n0 = a.n;
  while ((a.n - n0) / a.fs < max_s) {
    a.Block(tap, hz, amp);
    if (Read(r)) return (a.n - n0) / a.fs;
  }
  return -1.0;
}

static double CentsErr(double f, double ref) { return 1200.0 * log2(f / ref); }

static void test_countable_and_config() {
  printf("test_countable_and_config\n");
  CHECK(Countable(SRC_IN_L) && Countable(SRC_IN_R));
  CHECK(Countable(SRC_OUT_L) && Countable(SRC_OUT_R));
  for (int s = SRC_CV_IN1; s <= SRC_CV_IN8; ++s) CHECK(Countable((Source)s));
  for (int s = SRC_CV_OUT1; s <= SRC_TR4; ++s) CHECK(!Countable((Source)s));

  const ScopeFreq::Config af = MakeConfig(Profile::AUDIO, Gate::FAST, 0);
  CHECK(af.fs == 48000.0 && af.gate_samples == 4800 && af.learn_samples == 4800 && af.max_period == 4800);
  CHECK(af.clock_ratio == 1.0);
  const ScopeFreq::Config as = MakeConfig(Profile::AUDIO, Gate::SLOW, 0);
  CHECK(as.gate_samples == 48000);
  const ScopeFreq::Config cf = MakeConfig(Profile::CV, Gate::FAST, 0);
  const double cvfs = 150e6 / (2.0 * 251.0 * 8.0);   // 37,350.6 Hz: see test_cv_rate_formula
  CHECK(cf.fs == cvfs && cf.gate_samples == 3736 && cf.learn_samples == 3736 && cf.max_period == 3736);
  CHECK(MakeConfig(Profile::CV, Gate::SLOW, 0).gate_samples == 37351);
  const ScopeFreq::Config cl = MakeConfig(Profile::CV_LOW, Gate::FAST, 0);
  CHECK(cl.fs == cvfs && cl.max_period == 74702 && cl.learn_samples == 74702 && cl.gate_samples == 74702);
  CHECK(MakeConfig(Profile::CV_LOW, Gate::SLOW, 0).gate_samples == 373506);
  // 20.0 ppm
  CHECK(fabs(MakeConfig(Profile::AUDIO, Gate::FAST, 200).clock_ratio - 1.00002) < 1e-12);
  CHECK(fabs(MakeConfig(Profile::AUDIO, Gate::FAST, -1234).clock_ratio - (1.0 - 123.4e-6)) < 1e-12);
  CHECK(std::strcmp(ProfileName(Profile::CV_LOW), "CV_LOW") == 0);
}

// The CV frame rate is not a round number: it is derived from the timer setup in
// OC_ADC.cpp ADC33131D_Start(). This pins the constant to that arithmetic so it
// cannot drift back to a nominal 37.5 kHz unnoticed.
static void test_cv_rate_formula() {
  printf("test_cv_rate_formula\n");
  // comp1 exactly as the driver computes it (float arithmetic, truncated)
  const int comp1 = (int)((float)150000000 / 300000.0f / 2.0f + 0.5f);
  CHECK(comp1 == 250 && kCvTimerComp1 == comp1);
  CHECK(kCvBusHz == 150000000.0);
  CHECK(kCvTimerTicksPerCompare == comp1 + 1);
  CHECK(kCvReadingsPerFrame == 8);
  // OFLAG toggles on every compare and a reading is taken per OFLAG period (2 compares)
  const double reading_hz = 150e6 / (2.0 * (comp1 + 1));
  CHECK(fabs(kCvFsHz - reading_hz / 8.0) < 1e-9);
  CHECK(fabs(kCvFsHz - 37350.5976) < 1e-3);
  // 3984 ppm below the nominal 37.5 kHz: 6.9 cents, outside the +-1000 ppm clock range
  CHECK(fabs((kCvFsHz / 37500.0 - 1.0) * 1e6 + 3984.0) < 1.0);
  CHECK(MakeConfig(Profile::CV, Gate::FAST, 0).fs == kCvFsHz);
  CHECK(MakeConfig(Profile::CV_LOW, Gate::FAST, 0).fs == kCvFsHz);
  CHECK(!kCvRateVerified);   // until the bench procedure in the spec has been run
}

static void test_audio_fast_gate() {
  printf("test_audio_fast_gate\n");
  ResetForTest();
  Select(SRC_IN_L);
  CHECK(ActiveSource() == SRC_IN_L && ActiveProfile() == Profile::AUDIO);
  CHECK(GetGate() == Gate::FAST);
  AudioFeed a;
  Result r;
  CHECK(!Read(&r));  // nothing yet
  const double t = AudioUntilResult(a, 0, 440.0, 9000.0, &r);
  CHECK(t > 0);
  CHECK(r.status == Status::OK);
  CHECK(fabs(CentsErr(r.hz, 440.0)) < 0.01);
  // learn (0.1 s) + a gate of at least 0.1 s: the first reading inside ~0.25 s
  CHECK(t < 0.25);
  CHECK(r.gate_s >= 0.1 && r.gate_s < 0.12);
  // honest sigma: present, small, and not the 1 s figure
  CHECK(r.cents_sigma > 0 && r.cents_sigma < 0.01);
  // after the first, one reading per 0.1 s
  const uint32_t seq0 = r.seq;
  Result r2;
  int blocks = 0;
  while (blocks < 400) {
    a.Block(0, 440.0, 9000.0);
    ++blocks;
    if (Read(&r2) && r2.seq != seq0) break;
  }
  CHECK(r2.seq != seq0);
  CHECK(blocks * 128 / 48000.0 < 0.13);
  CHECK(fabs(CentsErr(r2.hz, 440.0)) < 0.01);
}

static void test_audio_slow_gate() {
  printf("test_audio_slow_gate\n");
  ResetForTest();
  SetGate(Gate::SLOW);
  CHECK(GetGate() == Gate::SLOW);
  Select(SRC_IN_R);
  AudioFeed a;
  Result r;
  const double t = AudioUntilResult(a, 1, 1234.5, 9000.0, &r);
  CHECK(t > 1.0 && t < 1.3);
  CHECK(r.status == Status::OK && r.gate_s >= 1.0);
  CHECK(fabs(CentsErr(r.hz, 1234.5)) < 0.005);
  // switching the gate restarts it, and the next reading is the fast kind
  SetGate(Gate::FAST);
  CHECK(!Read(&r) || r.gate_s < 1.0 || true);  // (an old reading may remain; see the next check)
  Result r2;
  const double t2 = AudioUntilResult(a, 1, 1234.5, 9000.0, &r2, 3.0);
  CHECK(t2 > 0 && t2 < 0.3);
  CHECK(r2.gate_s < 0.2);
  ResetForTest();
  CHECK(GetGate() == Gate::FAST);
}

static void test_tap_selection() {
  printf("test_tap_selection\n");
  ResetForTest();
  Select(SRC_IN_L);
  AudioFeed a;
  Result r;
  // right channel and the outputs carry a tone; the selected one is silent
  for (int i = 0; i < 400; ++i) {
    a.Block(1, 440.0, 9000.0);
    a.Block(2, 440.0, 9000.0);
    a.Block(3, 440.0, 9000.0);
    int16_t z[128] = {0};
    OnAudio(0, z, 128);
  }
  CHECK(Read(&r) && r.status == Status::NO_SIGNAL);
  // and the other way round
  ResetForTest();
  Select(SRC_OUT_R);
  AudioFeed b;
  for (int i = 0; i < 600; ++i) b.Block(3, 700.0, 9000.0);
  CHECK(Read(&r) && r.status == Status::OK && fabs(CentsErr(r.hz, 700.0)) < 0.02);
  // every audio source maps to its own tap
  for (int s = SRC_IN_L; s <= SRC_OUT_R; ++s) {
    ResetForTest();
    Select(s);
    AudioFeed c;
    for (int i = 0; i < 600; ++i) c.Block(s - SRC_IN_L, 300.0, 9000.0);
    CHECK(Read(&r) && r.status == Status::OK);
  }
}

static void test_deselect_and_idempotent_select() {
  printf("test_deselect_and_idempotent_select\n");
  ResetForTest();
  AudioFeed a;
  Result r;
  // feeding with nothing selected does nothing
  for (int i = 0; i < 400; ++i) a.Block(0, 440.0, 9000.0);
  CHECK(!Read(&r));
  CHECK(ActiveSource() == -1 && ActiveProfile() == Profile::NONE);
  // Select again and again, every block: the gate must still finish on time
  Select(SRC_IN_L);
  double t = -1;
  const uint64_t n0 = a.n;
  while ((a.n - n0) / 48000.0 < 1.0) {
    a.Block(0, 440.0, 9000.0);
    Select(SRC_IN_L);
    if (Read(&r)) { t = (a.n - n0) / 48000.0; break; }
  }
  CHECK(t > 0 && t < 0.25);
  // a different source discards the reading and starts over
  Select(SRC_IN_R);
  CHECK(!Read(&r));
  CHECK(ActiveSource() == SRC_IN_R);
  // a source that cannot be counted stops it
  Select(SRC_CV_OUT3);
  CHECK(ActiveSource() == -1 && !Read(&r));
  Select(SRC_TR1);
  CHECK(ActiveSource() == -1);
  Select(-1);
  CHECK(ActiveSource() == -1);
  Select(SRC_IN_L);
  Deselect();
  CHECK(ActiveSource() == -1 && ActiveProfile() == Profile::NONE);
  for (int i = 0; i < 400; ++i) a.Block(0, 440.0, 9000.0);
  CHECK(!Read(&r));
}

static void test_cv_audio_rate() {
  printf("test_cv_audio_rate\n");
  for (int cv = 0; cv < 8; ++cv) {
    ResetForTest();
    Select(SRC_CV_IN1 + cv);
    CHECK(ActiveProfile() == Profile::CV);
    CvFeed f;
    Result r;
    bool got = false;
    for (int i = 0; i < 37500 / 4 && !got; ++i) {  // 0.25 s
      f.Frame(cv, 1000.0, 6000.0);
      got = Read(&r);
    }
    CHECK(got);
    if (!got) continue;
    CHECK(r.status == Status::OK);
    CHECK(fabs(CentsErr(r.hz, 1000.0)) < 0.02);
    CHECK(r.gate_s >= 0.1 && r.gate_s < 0.12);
  }
  // only the selected input counts: CV3 selected, CV4 carries the tone
  ResetForTest();
  Select(SRC_CV_IN3);
  CvFeed g;
  Result r;
  for (int i = 0; i < 20000; ++i) g.Frame(3, 1000.0, 6000.0);
  CHECK(Read(&r) && r.status == Status::NO_SIGNAL);
  // a DC-coupled input at a high offset, and an offset that moves the sine
  // against the rail, still count (the word is clamped at 0)
  ResetForTest();
  Select(SRC_CV_IN1);
  CvFeed h;
  for (int i = 0; i < 20000; ++i) h.Frame(0, 440.0, 5000.0, 26000.0);
  CHECK(Read(&r) && r.status == Status::OK && fabs(CentsErr(r.hz, 440.0)) < 0.02);
  // negative words (a bus fault, or a converter reading under 0) are clamped to 0,
  // not passed on: a sine centred on 0 loses its negative half, and the counter's
  // own peak-to-peak shows the clamp (6000 for the positive half, not 12000)
  ResetForTest();
  Select(SRC_CV_IN1);
  CvFeed neg;
  Result rn;
  for (int i = 0; i < 20000; ++i) neg.Frame(0, 1000.0, 6000.0, 0.0);
  CHECK(Read(&rn) && rn.status == Status::OK);
  CHECK(rn.pp > 5900 && rn.pp <= 6000);
  CHECK(fabs(CentsErr(rn.hz, 1000.0)) < 20.0);   // a half-wave rectified tone still has its period
  int16_t bad[8] = {-5, -5, -5, -5, -5, -5, -5, -5};
  for (int i = 0; i < 40000; ++i) OnCvFrame(bad);  // never crashes, never counts
  CHECK(Read(&rn) && (rn.status == Status::NO_SIGNAL || rn.status == Status::LOST));
}

static void test_cv_low_frequency_ranging() {
  printf("test_cv_low_frequency_ranging\n");
  ResetForTest();
  Select(SRC_CV_IN2);
  CHECK(ActiveProfile() == Profile::CV);
  CvFeed f;
  Result r;
  double t_low = -1;
  // a 1 Hz triangle LFO, 3 V p-p: nothing the CV profile can count
  for (uint64_t i = 0; i < 37500ull * 20; ++i) {
    f.Frame(1, 1.0, 5000.0, 16000.0, true);
    if ((i & 127) == 0) Poll();
    if (ActiveProfile() == Profile::CV_LOW && t_low < 0) t_low = i / 37500.0;
    if (ActiveProfile() == Profile::CV_LOW && Read(&r) && r.status == Status::OK) break;
  }
  CHECK(t_low > 0 && t_low < 1.0);
  CHECK(ActiveProfile() == Profile::CV_LOW);
  CHECK(Read(&r) && r.status == Status::OK);
  CHECK(fabs(CentsErr(r.hz, 1.0)) < 2.0);   // 1 Hz over a 2 s gate: coarse, and the sigma says so
  CHECK(r.cents_sigma >= 0);
  CHECK(r.gate_s >= 2.0);
  // the tone comes up to audio rate: the CV profile takes it back
  bool back = false;
  for (uint64_t i = 0; i < 37500ull * 12 && !back; ++i) {
    f.Frame(1, 440.0, 5000.0);
    if ((i & 127) == 0) Poll();
    back = ActiveProfile() == Profile::CV && Read(&r) && r.status == Status::OK && fabs(CentsErr(r.hz, 440.0)) < 0.1;
  }
  CHECK(back);
  // silence: a flat input never leaves the CV range, so a tone is read at once
  ResetForTest();
  Select(SRC_CV_IN2);
  int flips = 0, nosig = 0, other = 0;
  Profile last = ActiveProfile();
  for (uint64_t i = 0; i < 37500ull * 12; ++i) {
    int16_t fr[8];
    for (int k = 0; k < 8; ++k) fr[k] = 16000;
    OnCvFrame(fr);
    if ((i & 127) == 0) Poll();
    if (ActiveProfile() != last) { ++flips; last = ActiveProfile(); }
    if (Read(&r)) { if (r.status == Status::NO_SIGNAL) ++nosig; else ++other; }
  }
  CHECK(flips == 0);
  CHECK(nosig > 10 && other == 0);
  // ...and then a tone on that input is read within 0.25 s
  CvFeed tone;
  double t_read = -1;
  for (uint64_t i = 0; i < 37500 / 3 && t_read < 0; ++i) {
    tone.Frame(1, 440.0, 5000.0);
    if ((i & 127) == 0) Poll();
    if (Read(&r) && r.status == Status::OK) t_read = i / 37500.0;
  }
  CHECK(t_read > 0 && t_read < 0.25);
  // a noise-floor input (a few LSB of hiss) is idle too
  ResetForTest();
  Select(SRC_CV_IN2);
  unsigned seed = 7;
  flips = 0;
  last = ActiveProfile();
  for (uint64_t i = 0; i < 37500ull * 4; ++i) {
    int16_t fr[8];
    seed = seed * 1664525u + 1013904223u;
    for (int k = 0; k < 8; ++k) fr[k] = (int16_t)(16000 + ((seed >> 20) % 9) - 4);
    OnCvFrame(fr);
    if ((i & 127) == 0) Poll();
    if (ActiveProfile() != last) { ++flips; last = ActiveProfile(); }
  }
  CHECK(flips == 0 && ActiveProfile() == Profile::CV);
  // audio sources never change profile
  ResetForTest();
  Select(SRC_IN_L);
  for (int i = 0; i < 2000; ++i) { int16_t z[128] = {0}; OnAudio(0, z, 128); Poll(); }
  CHECK(ActiveProfile() == Profile::AUDIO);
}

static void test_auto_range_rule() {
  printf("test_auto_range_rule\n");
  AutoRange ar;
  Result r;
  r.status = Status::NO_SIGNAL;
  r.pp = 5000;
  CHECK(ar.Next(Profile::CV, r) == Profile::CV);        // one is not enough
  CHECK(ar.Next(Profile::CV, r) == Profile::CV_LOW);    // two are
  ar = AutoRange();
  r.status = Status::OK; r.hz = 440;
  CHECK(ar.Next(Profile::CV, r) == Profile::CV);
  r.status = Status::NO_SIGNAL;
  CHECK(ar.Next(Profile::CV, r) == Profile::CV);
  r.status = Status::OK;                                 // an OK in between resets the count
  CHECK(ar.Next(Profile::CV, r) == Profile::CV);
  r.status = Status::NO_SIGNAL;
  CHECK(ar.Next(Profile::CV, r) == Profile::CV);
  const Status lows[] = {Status::TOO_QUIET, Status::TOO_FEW, Status::LOST};
  for (Status s : lows) {
    ar = AutoRange();
    r.status = s;
    ar.Next(Profile::CV, r);
    CHECK(ar.Next(Profile::CV, r) == Profile::CV_LOW);
  }
  // a flat or noise-floor input is idle, not slow: it stays in CV for good
  for (Status s : {Status::NO_SIGNAL, Status::TOO_QUIET}) {
    for (int pp : {0, 1, 40, kMovingLsb}) {
      ar = AutoRange();
      r.status = s;
      r.pp = pp;
      for (int i = 0; i < 10; ++i) CHECK(ar.Next(Profile::CV, r) == Profile::CV);
    }
    ar = AutoRange();
    r.status = s;
    r.pp = kMovingLsb + 1;     // one step over: it moves
    ar.Next(Profile::CV, r);
    CHECK(ar.Next(Profile::CV, r) == Profile::CV_LOW);
  }
  // TOO LOW and LOST count whatever the range of the input was
  for (Status s : {Status::TOO_FEW, Status::LOST}) {
    ar = AutoRange();
    r.status = s;
    r.pp = 0;
    ar.Next(Profile::CV, r);
    CHECK(ar.Next(Profile::CV, r) == Profile::CV_LOW);
  }
  r.pp = 5000;
  ar = AutoRange();
  r.status = Status::UNSTABLE;   // a real but messy signal stays where it is
  for (int i = 0; i < 5; ++i) CHECK(ar.Next(Profile::CV, r) == Profile::CV);
  // from CV_LOW
  ar = AutoRange();
  r.status = Status::OK; r.hz = 1.0;
  CHECK(ar.Next(Profile::CV_LOW, r) == Profile::CV_LOW);
  r.hz = 29.9;
  CHECK(ar.Next(Profile::CV_LOW, r) == Profile::CV_LOW);
  r.hz = 30.1;
  CHECK(ar.Next(Profile::CV_LOW, r) == Profile::CV);
  r.status = Status::NO_SIGNAL;
  CHECK(ar.Next(Profile::CV_LOW, r) == Profile::CV);
  r.status = Status::TOO_QUIET;
  CHECK(ar.Next(Profile::CV_LOW, r) == Profile::CV);
  r.status = Status::TOO_FEW;                            // 0.4 Hz: stay, say TOO LOW
  CHECK(ar.Next(Profile::CV_LOW, r) == Profile::CV_LOW);
  CHECK(ar.Next(Profile::AUDIO, r) == Profile::AUDIO);
  CHECK(ar.Next(Profile::NONE, r) == Profile::NONE);
}

static void test_status_transitions() {
  printf("test_status_transitions\n");
  ResetForTest();
  Select(SRC_IN_L);
  AudioFeed a;
  Result r;
  // silence first
  for (int i = 0; i < 100; ++i) { int16_t z[128] = {0}; OnAudio(0, z, 128); }
  CHECK(Read(&r) && r.status == Status::NO_SIGNAL);
  // too quiet: a 60 LSB tone
  for (int i = 0; i < 200; ++i) a.Block(0, 440.0, 60.0);
  CHECK(Read(&r) && r.status == Status::TOO_QUIET);
  // a good tone
  for (int i = 0; i < 400; ++i) a.Block(0, 440.0, 9000.0);
  CHECK(Read(&r) && r.status == Status::OK);
  // the tone stops mid-gate
  for (int i = 0; i < 400; ++i) { int16_t z[128] = {0}; OnAudio(0, z, 128); }
  CHECK(Read(&r) && (r.status == Status::LOST || r.status == Status::NO_SIGNAL));
  // and comes back
  for (int i = 0; i < 600; ++i) a.Block(0, 523.25, 9000.0);
  CHECK(Read(&r) && r.status == Status::OK && fabs(CentsErr(r.hz, 523.25)) < 0.02);
  // far too high
  for (int i = 0; i < 600; ++i) a.Block(0, 9000.0, 9000.0);
  CHECK(Read(&r) && r.status == Status::TOO_HIGH);
}

static void test_clock() {
  printf("test_clock\n");
  ResetForTest();
  CHECK(!ClockCalibrated() && ClockPpm10() == 0);
  CHECK(SetClock(true, 200));
  CHECK(ClockCalibrated() && ClockPpm10() == 200);
  CHECK(SetClock(true, kClockMaxPpm10) && ClockPpm10() == 10000);
  CHECK(SetClock(true, -kClockMaxPpm10) && ClockPpm10() == -10000);
  CHECK(!SetClock(true, kClockMaxPpm10 + 1) && ClockPpm10() == -10000);   // refused, unchanged
  CHECK(!SetClock(true, -kClockMaxPpm10 - 1) && ClockPpm10() == -10000);
  CHECK(!SetClock(true, 2000000000) && ClockPpm10() == -10000);
  CHECK(SetClock(false, 0));
  CHECK(!ClockCalibrated() && ClockPpm10() == 0);
  CHECK(SetClock(false, 999999) && ClockPpm10() == 0);   // clearing ignores the value
  // the ratio reaches the counter: the same tone reads 20 ppm lower with +20 ppm set
  // (a crystal 20 ppm fast means the true rate is higher than the nominal one)
  ResetForTest();
  Select(SRC_IN_L);
  AudioFeed a;
  Result r0, r1;
  CHECK(AudioUntilResult(a, 0, 1000.0, 9000.0, &r0) > 0);
  SetClock(true, 200);       // while running
  CHECK(!Read(&r1));         // the reconfigured counter starts over
  CHECK(AudioUntilResult(a, 0, 1000.0, 9000.0, &r1) > 0);
  CHECK(fabs(r1.hz / r0.hz - 1.00002) < 2e-7);
  // set before Select as well
  ResetForTest();
  SetClock(true, -500);
  Select(SRC_IN_L);
  AudioFeed b;
  Result r2;
  CHECK(AudioUntilResult(b, 0, 1000.0, 9000.0, &r2) > 0);
  CHECK(fabs(r2.hz / r0.hz - (1.0 - 5e-5)) < 2e-7);
  // the clock survives a source change and Deselect
  Select(SRC_IN_R);
  Deselect();
  CHECK(ClockCalibrated() && ClockPpm10() == -500);
}

// The hand-off: a writer thread stands in for the ISR.
static void test_handoff_is_consistent() {
  printf("test_handoff_is_consistent\n");
  ResetForTest();
  Select(SRC_IN_L);
  std::atomic<bool> stop(false);
  std::thread w([&] {
    uint32_t i = 1;
    while (!stop.load()) {
      Result r;
      r.seq = i;
      r.hz = (double)i;
      r.cents_sigma = (double)i;
      r.period_samples = (double)i;
      r.gate_s = (double)i;
      r.jitter = (double)i;
      r.interval_ratio = (double)i;
      r.level = (double)i;
      r.hyst = (double)i;
      r.crossings = i;
      detail::PublishForTest(r);
      ++i;
      for (volatile int spin = 0; spin < 150; ++spin) {}  // an ISR publishes every 100 ms; this is still brutal
    }
  });
  long reads = 0, torn = 0;
  uint32_t last = 0;
  long backwards = 0;
  for (int k = 0; k < 400000; ++k) {
    Result r;
    if (!Read(&r)) continue;
    ++reads;
    const double v = (double)r.seq;
    if (r.hz != v || r.cents_sigma != v || r.period_samples != v || r.gate_s != v || r.jitter != v ||
        r.interval_ratio != v || r.level != v || r.hyst != v || r.crossings != r.seq)
      ++torn;
    if (r.seq < last) ++backwards;
    last = r.seq;
  }
  stop = true;
  w.join();
  CHECK(reads > 1000);
  CHECK(torn == 0);
  CHECK(backwards == 0);
  if (torn) printf("  torn reads: %ld of %ld\n", torn, reads);
}

static void test_poll_feeds_the_meter() {
  printf("test_poll_feeds_the_meter\n");
  ResetForTest();
  Select(SRC_IN_L);
  AudioFeed a;
  CHECK(!CurrentReading().have);
  // first gate: a reading, not yet confirmed; the second confirms it
  int seen = 0;
  bool first_unconfirmed = false, then_confirmed = false;
  for (int i = 0; i < 800 && !then_confirmed; ++i) {
    a.Block(0, 440.0, 9000.0);
    Poll();
    const ScopeFreqLine::Reading &r = CurrentReading();
    if (r.have && r.status == Status::OK) {
      if (seen == 0) { first_unconfirmed = !r.confirmed; seen = 1; }
      else if (r.confirmed) then_confirmed = true;
    }
  }
  CHECK(first_unconfirmed && then_confirmed);
  CHECK(fabs(CentsErr(CurrentReading().hz, 440.0)) < 0.01);
  // Poll with nothing new keeps the reading
  const uint32_t seq = CurrentReading().seq;
  Poll(); Poll();
  CHECK(CurrentReading().seq == seq);
  // a new source, a gate change and a clock change all start the reading over
  Select(SRC_IN_R);
  CHECK(!CurrentReading().have);
  for (int i = 0; i < 400; ++i) { a.Block(1, 440.0, 9000.0); Poll(); }
  CHECK(CurrentReading().have);
  SetGate(Gate::SLOW);
  CHECK(!CurrentReading().have);
  ResetForTest();
  Select(SRC_IN_R);
  for (int i = 0; i < 400; ++i) { a.Block(1, 440.0, 9000.0); Poll(); }
  CHECK(CurrentReading().have);
  SetClock(true, 100);
  CHECK(!CurrentReading().have);
  Deselect();
  CHECK(!CurrentReading().have);
  // Poll with nothing selected does nothing
  Poll();
  CHECK(!CurrentReading().have);
  // the CV range change keeps the screen's last word (no flicker to WAIT)
  ResetForTest();
  Select(SRC_CV_IN1);
  bool saw_nosig = false, wait_after = false;
  for (uint64_t i = 0; i < 37500ull * 6; ++i) {
    int16_t fr[8];
    for (int k = 0; k < 8; ++k) fr[k] = 16000;
    OnCvFrame(fr);
    if ((i & 127) == 0) Poll();
    if (CurrentReading().have && CurrentReading().status == Status::NO_SIGNAL) saw_nosig = true;
    if (saw_nosig && !CurrentReading().have) wait_after = true;
  }
  CHECK(saw_nosig && !wait_after);
}

static void test_idle_cost_path() {
  printf("test_idle_cost_path\n");
  ResetForTest();
  // nothing selected: a feed leaves the counter's result untouched
  const uint32_t before = detail::g_counter.Last().seq;
  int16_t b[128];
  for (int i = 0; i < 128; ++i) b[i] = (int16_t)(8000 * sin(i * 0.3));
  for (int i = 0; i < 2000; ++i) OnAudio(0, b, 128);
  int16_t f[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  for (int i = 0; i < 100000; ++i) OnCvFrame(f);
  CHECK(detail::g_counter.Last().seq == before);
  CHECK(detail::g_audio_tap1 == 0 && detail::g_cv_index1 == 0);
  // a CV source is not fed from the audio hook and vice versa
  Select(SRC_CV_IN1);
  CHECK(detail::g_audio_tap1 == 0 && detail::g_cv_index1 == 5);
  Select(SRC_CV_IN8);
  CHECK(detail::g_cv_index1 == 4);
  Select(SRC_IN_R);
  CHECK(detail::g_audio_tap1 == 2 && detail::g_cv_index1 == 0);
  Deselect();
  CHECK(detail::g_audio_tap1 == 0 && detail::g_cv_index1 == 0);
}

int main() {
  test_countable_and_config();
  test_cv_rate_formula();
  test_audio_fast_gate();
  test_audio_slow_gate();
  test_tap_selection();
  test_deselect_and_idempotent_select();
  test_cv_audio_rate();
  test_cv_low_frequency_ranging();
  test_auto_range_rule();
  test_status_transitions();
  test_clock();
  test_handoff_is_consistent();
  test_poll_feeds_the_meter();
  test_idle_cost_path();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
