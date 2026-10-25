// Host tests for the frequency counter and edge statistics
// (src/ScopeFreq.h, header-only). Synthetic signals are rounded to int16 the
// way the audio tap delivers them. Error numbers are in cents against the
// exact generator frequency; the bounds are the design numbers in
// docs/specs/xeno-frequency-counter.md, section "Measured on the host".
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#include "../src/ScopeFreq.h"

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

using namespace ScopeFreq;

static const double kPi = 3.14159265358979323846;
static const double kFs = 48000.0;

// ---- generators ----------------------------------------------------------
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return (uint32_t)(s >> 16);
  }
  double uni() { return (next() + 0.5) / 4294967296.0; }
  double gauss() {
    double a = 0;
    for (int i = 0; i < 12; ++i) a += (next() & 0xFFFFF) / 1048576.0;
    return a - 6.0;  // sum of 12 uniforms: unit variance
  }
};

enum Wave { SINE, TRI, SAW, SQUARE };

// Band-limited to 22 kHz by additive synthesis, as the codec's filter would
// leave them: a naive square or saw puts its edge inside one sample and the
// test would measure the aliasing of the generator, not the counter.
static double wave(Wave w, double ph, double f) {
  const int kmax = (int)(22000.0 / f);
  double s = 0;
  switch (w) {
    case SINE: return sin(2 * kPi * ph);
    case TRI:
      for (int k = 1; k <= kmax; k += 2)
        s += ((k % 4 == 1) ? 1.0 : -1.0) * 8.0 / (kPi * kPi) / ((double)k * k) * sin(2 * kPi * k * ph);
      return s;
    case SAW:
      for (int k = 1; k <= kmax; ++k) s += ((k % 2) ? 1.0 : -1.0) * (-2.0 / kPi) / k * sin(2 * kPi * k * ph);
      return s;
    default:
      for (int k = 1; k <= kmax; k += 2) s += 4.0 / kPi / k * sin(2 * kPi * k * ph);
      return s;
  }
}

static int16_t q(double v) {
  v = floor(v + 0.5);
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return (int16_t)v;
}

// Feeds `gen(i)` until the counter publishes. Returns that Result.
static Result Run(FreqCounter &c, const std::function<double(uint64_t)> &gen,
                  double max_s = 8.0, uint64_t *used = nullptr) {
  const uint64_t n = (uint64_t)(max_s * kFs);
  for (uint64_t i = 0; i < n; ++i) {
    if (c.Push(q(gen(i)))) {
      if (used) *used = i + 1;
      return c.Last();
    }
  }
  if (used) *used = n;
  return c.Last();
}

// Collects the first `n` published results.
static std::vector<Result> RunN(FreqCounter &c, const std::function<double(uint64_t)> &gen, int n, double max_s = 12.0) {
  std::vector<Result> out;
  const uint64_t cnt = (uint64_t)(max_s * kFs);
  for (uint64_t i = 0; i < cnt && (int)out.size() < n; ++i)
    if (c.Push(q(gen(i)))) out.push_back(c.Last());
  return out;
}

// The acceptance rule Cal Routine will use: two consecutive OK gates that agree.
static bool Agree(const Result &a, const Result &b) {
  if (a.status != Status::OK || b.status != Status::OK) return false;
  const double d = fabs(Cents(a.hz, b.hz));
  return d <= 3.0 * sqrt(a.cents_sigma * a.cents_sigma + b.cents_sigma * b.cents_sigma) + 0.01;
}

static double ErrCents(const Result &r, double f) {
  return r.status == Status::OK ? Cents(r.hz, f) : 1e9;
}

// ---- 1. tones 20 Hz .. 5 kHz, all four shapes ------------------------------
static void test_tone_table() {
  printf("test_tone_table  (clean, band-limited, about 0.33 FS peak, odd phase; error in cents)\n");
  const double freqs[] = {20, 27.5, 55, 110, 220, 440, 987.77, 1000, 1760, 3000, 5000};
  const char *names[] = {"sine", "tri", "saw", "square"};
  printf("  %8s %10s %10s %10s %10s\n", "Hz", names[0], names[1], names[2], names[3]);
  double worst = 0;
  for (double f : freqs) {
    printf("  %8.2f", f);
    for (int w = 0; w < 4; ++w) {
      FreqCounter c;
      const double ph0 = 0.123;
      Result r = Run(c, [&](uint64_t i) { return 11000.0 * wave((Wave)w, ph0 + f * i / kFs, f); });
      const double e = ErrCents(r, f);
      printf(" %+10.5f", e);
      CHECK(r.status == Status::OK);
      CHECK(fabs(e) < 0.001);  // design: 0.01 cent; measured worst 0.0002
      CHECK(r.gate_s >= 1.0);
      if (fabs(e) > worst) worst = fabs(e);
    }
    printf("\n");
  }
  printf("  worst clean error %.5f cent\n", worst);
}

// ---- 2. noise: measured sigma vs the model ----------------------------------
// Model (spec): sigma_f/f = sqrt(12) * (sigma_v/A) / (2 pi sqrt(N) * N) * sqrt(2)?
// For a sine: crossing jitter sigma_t = sigma_v / (2 pi f A); LSQ over N
// crossings gives sigma_f/f = sqrt(12) * sigma_t * f / N^1.5.
static double model_cents(double f, double gate_s, double snr_amp_ratio) {
  const double N = f * gate_s;
  return 1731.234 * sqrt(12.0) * snr_amp_ratio / (2 * kPi) / pow(N, 1.5);
}

static void test_noise() {
  printf("test_noise  (white noise, 30 trials per row; rms error vs model, cents)\n");
  printf("  %8s %8s %12s %12s %12s\n", "Hz", "SNR dB", "rms err", "model", "reported");
  const double freqs[] = {20, 100, 1000, 5000};
  const double snrs[] = {60, 40};
  for (double f : freqs) {
    for (double snr : snrs) {
      const double A = 16384.0;
      const double sig = A / sqrt(2.0) * pow(10.0, -snr / 20.0);  // noise rms for SNR = (A^2/2) / sig^2
      double acc = 0, rep = 0, gate = 0;
      int ok = 0;
      const int trials = 30;
      for (int t = 0; t < trials; ++t) {
        Rng rng(1000 + t);
        FreqCounter c;
        const double ph0 = rng.uni();
        // Noise is generated per sample and must not depend on call order.
        Result r = Run(c, [&](uint64_t i) { return A * sin(2 * kPi * (ph0 + f * i / kFs)) + sig * rng.gauss(); });
        if (r.status != Status::OK) continue;
        ++ok;
        const double e = Cents(r.hz, f);
        acc += e * e;
        rep += r.cents_sigma;
        gate += r.gate_s;
      }
      const double rms = sqrt(acc / (ok ? ok : 1));
      const double m = model_cents(f, gate / (ok ? ok : 1), sig / A);
      printf("  %8.1f %8.0f %12.5f %12.5f %12.5f\n", f, snr, rms, m, rep / (ok ? ok : 1));
      CHECK(ok == trials);
      CHECK(rms < 2.0 * m + 1e-4);          // never worse than twice the model
      CHECK(rms > 0.2 * m);                 // and the model is not wildly pessimistic
      CHECK(rep / ok < 3.0 * m + 1e-4 && rep / ok > m / 3.0);  // self-reported sigma is honest
    }
  }
}

// Heavy noise: where the hysteresis earns its keep. Whatever the noise, a result marked OK
// must be within a few of its own sigmas; past about 0.15 A rms the status must degrade instead.
static void test_heavy_noise() {
  printf("test_heavy_noise  (noise rms as a fraction of the sine amplitude; hysteresis is 0.3 A)\n");
  printf("  %8s %8s %10s %12s %10s\n", "Hz", "noise/A", "status", "err cent", "sigma");
  int ok_at_low = 0;
  for (double f : {100.0, 440.0}) {
    for (double k : {0.05, 0.10, 0.15, 0.20, 0.30, 0.40}) {
      Rng rng(77);
      FreqCounter c;
      Result r = Run(c, [&](uint64_t i) { return 12000.0 * sin(2 * kPi * (f * i / kFs + 0.3)) + k * 12000.0 * rng.gauss(); });
      printf("  %8.0f %8.2f %10s %+12.4f %10.4f\n", f, k, StatusText(r.status), r.status == Status::OK ? ErrCents(r, f) : 0.0,
             r.cents_sigma);
      if (r.status == Status::OK) CHECK(fabs(ErrCents(r, f)) < 4.0 * r.cents_sigma + 0.05);
      if (k <= 0.15 && r.status == Status::OK) ++ok_at_low;
      if (k >= 0.30) CHECK(r.status != Status::OK || fabs(ErrCents(r, f)) < 1.0);
    }
  }
  CHECK(ok_at_low == 6);  // both tones, 0.05 / 0.10 / 0.15
}

// ---- 3. DC offset -----------------------------------------------------------
static void test_dc_offset() {
  printf("test_dc_offset  (440 and 20 Hz sine, offset in %% of FS)\n");
  const double offs[] = {0, 5, 20, 40};
  for (double f : {440.0, 20.0}) {
    for (double off : offs) {
      const double A = 8000.0;
      const double dc = off / 100.0 * 32768.0;
      FreqCounter c;
      Result r = Run(c, [&](uint64_t i) { return dc + A * sin(2 * kPi * (0.3 + f * i / kFs)); });
      const double e = ErrCents(r, f);
      printf("  f=%6.1f offset %2.0f%%: err %+9.5f cent  level %.0f hyst %.0f\n", f, off, e, r.level, r.hyst);
      CHECK(r.status == Status::OK);
      CHECK(fabs(e) < 0.002);
    }
  }
  // A fixed level far off the signal's middle still counts (crossings are of the
  // wrong point of the cycle, but all alike).
  Config cfg;
  cfg.auto_center = false;
  cfg.fixed_level = 3000;
  FreqCounter c(cfg);
  Result r = Run(c, [&](uint64_t i) { return 8000.0 * sin(2 * kPi * (0.3 + 440.0 * i / kFs)); });
  CHECK(r.status == Status::OK);
  CHECK(fabs(ErrCents(r, 440.0)) < 0.005);
}

// A slow DC ramp (a drifting offset, 1500 LSB/s) walks the signal away from a fixed level:
// the level is re-learned at every gate, so the counter keeps reading instead of going LOST.
static void test_dc_ramp() {
  printf("test_dc_ramp  (440 Hz, amplitude 6000, DC ramps 0 -> 12000 LSB over 8 s)\n");
  FreqCounter c;
  std::vector<Result> r = RunN(c, [](uint64_t i) { return 1500.0 * i / kFs + 6000.0 * sin(2 * kPi * (0.2 + 440.0 * i / kFs)); }, 7, 8.0);
  int ok = 0;
  double worst = 0;
  for (const Result &x : r) {
    if (x.status == Status::OK) { ++ok; worst = std::max(worst, fabs(ErrCents(x, 440.0))); }
  }
  printf("  %zu gates, %d OK, worst error %.4f cent (a ramp is a slow time shift of every crossing)\n", r.size(), ok, worst);
  CHECK(r.size() >= 6 && ok == (int)r.size());
  CHECK(worst < 0.5);
}

// ---- 4. harmonics -----------------------------------------------------------
static void test_harmonics() {
  printf("test_harmonics\n");
  struct H { double f; double a2, a3, a5; };
  const H cases[] = {{440, 0, 0.3, 0}, {440, 0.3, 0, 0}, {1000, 0.2, 0.2, 0.2}, {110, 0.4, 0.4, 0.1},
                     {5000, 0.1, 0.05, 0}};
  for (const H &h : cases) {
    FreqCounter c;
    Result r = Run(c, [&](uint64_t i) {
      const double p = 0.37 + h.f * i / kFs;
      return 12000.0 * (sin(2 * kPi * p) + h.a2 * sin(4 * kPi * p + 1.0) + h.a3 * sin(6 * kPi * p + 2.0) +
                        h.a5 * sin(10 * kPi * p + 0.5));
    });
    const double e = ErrCents(r, h.f);
    printf("  %6.0f Hz h2=%.2f h3=%.2f h5=%.2f: err %+9.5f cent\n", h.f, h.a2, h.a3, h.a5, e);
    CHECK(r.status == Status::OK);
    CHECK(fabs(e) < 0.003);
  }
  // A strong 2nd harmonic that makes extra rising crossings through the hysteresis
  // band's neighbourhood: must be caught or correct, never silently an octave off.
  {
    FreqCounter c;
    Result r = Run(c, [&](uint64_t i) {
      const double p = 0.1 + 300.0 * i / kFs;
      return 12000.0 * (0.5 * sin(2 * kPi * p) + 1.0 * sin(4 * kPi * p + 1.3));
    });
    printf("  300 Hz fundamental 0.5, 2nd 1.0: status %s hz %.3f\n", StatusText(r.status), r.hz);
    CHECK(r.status != Status::OK || fabs(Cents(r.hz, 300.0)) < 0.01 || fabs(Cents(r.hz, 600.0)) < 0.01);
  }
}

// ---- 5. amplitude steps -----------------------------------------------------
static void test_amplitude_steps() {
  printf("test_amplitude_steps  (gate of 1 s, step at 0.5 s)\n");
  struct S { double f; double a0, a1, dc; };
  const S cases[] = {{440, 12000, 6000, 0}, {440, 1200, 12000, 0}, {20, 12000, 6000, 0},
                     {1000, 12000, 6000, 4000}, {110, 3000, 12000, 4000}};
  for (const S &s : cases) {
    FreqCounter c;
    Result r = Run(c, [&](uint64_t i) {
      const double a = (i > 24000 + 4800) ? s.a1 : s.a0;  // after learning, mid-gate
      return s.dc + a * sin(2 * kPi * (0.2 + s.f * i / kFs));
    });
    const double e = ErrCents(r, s.f);
    printf("  %5.0f Hz amp %6.0f -> %6.0f dc %5.0f: status %-9s err %+9.5f cent  sigma %.5f\n", s.f, s.a0, s.a1, s.dc,
           StatusText(r.status), r.status == Status::OK ? e : 0.0, r.cents_sigma);
    // No DC: level 0 is the middle of the wave at both amplitudes, nothing moves.
    if (s.dc == 0) {
      CHECK(r.status == Status::OK);
      CHECK(fabs(e) < 0.003);
    } else {
      // With DC the learned level is not the middle of the new amplitude: a one-off
      // time shift of the crossings. It must be small and visible in the fit.
      CHECK(r.status == Status::OK || r.status == Status::UNSTABLE);
      if (r.status == Status::OK) CHECK(fabs(e) < 0.2);
    }
  }
  // The two ways a step does hurt: the fixed level stops being the middle of the cycle.
  // An asymmetric wave (sine plus a 2nd harmonic) whose amplitude halves mid-gate, and
  // a DC step on a symmetric one. Each moves every later crossing by one fixed amount,
  // an error the fit residual (sigma) under-reports; the next gate is clean, and two
  // consecutive gates disagreeing is what catches it.
  printf("  hurting steps (one jump in crossing times, in the first gate):\n");
  for (double f : {110.0, 440.0, 1760.0}) {
    FreqCounter c1, c2;
    auto ga = [&](uint64_t i) {
      const double k = (i > 24000 + 4800) ? 0.5 : 1.0;
      const double p = 0.2 + f * i / kFs;
      return k * 12000.0 * (sin(2 * kPi * p) + 0.5 * sin(4 * kPi * p + 1.0)); };
    auto gb = [&](uint64_t i) {
      const double dc = (i > 24000 + 4800) ? 3000.0 : 0.0;
      return dc + 12000.0 * sin(2 * kPi * (0.2 + f * i / kFs)); };
    std::vector<Result> a = RunN(c1, ga, 3), b = RunN(c2, gb, 3);
    printf("   %6.0f Hz  asym amp x0.5: gate1 %+8.4f gate2 %+8.4f cent (sigma %.4f)   DC +9%% FS: gate1 %+8.4f gate2 %+8.4f cent (sigma %.4f)\n",
           f, ErrCents(a[0], f), ErrCents(a[1], f), a[0].cents_sigma, ErrCents(b[0], f), ErrCents(b[1], f), b[0].cents_sigma);
    CHECK(a.size() == 3 && b.size() == 3);
    CHECK(fabs(ErrCents(a[0], f)) < 1.5 && fabs(ErrCents(b[0], f)) < 1.5);  // bounded, but far above sigma
    CHECK(fabs(ErrCents(a[1], f)) < 0.003 && fabs(ErrCents(b[1], f)) < 0.003);  // the next gate is clean
    CHECK(!Agree(a[0], a[1]) && !Agree(b[0], b[1]));                             // and the pair is refused
    CHECK(Agree(a[1], a[2]) && Agree(b[1], b[2]));                               // the clean pair is accepted
  }
  // Frequency stepped by a semitone mid-gate (a CV step): the straddling gate must not be trusted.
  {
    FreqCounter c;
    const double f0 = 440.0, f1 = 440.0 * 1.0594630943592953;
    auto gen = [&](uint64_t i) {
      const double t = i / kFs, ts = 0.55 + 4800.0 / kFs;
      const double ph = t < ts ? f0 * t : f0 * ts + f1 * (t - ts);
      return 12000.0 * sin(2 * kPi * (0.3 + ph)); };
    std::vector<Result> r = RunN(c, gen, 3);
    printf("  semitone step mid-gate: gate1 %s %.3f Hz, gate2 %s %.3f Hz, gate3 %s %.3f Hz\n", StatusText(r[0].status), r[0].hz,
           StatusText(r[1].status), r[1].hz, StatusText(r[2].status), r[2].hz);
    CHECK(r.size() == 3);
    CHECK(!Agree(r[0], r[1]));
    CHECK(Agree(r[1], r[2]) && fabs(Cents(r[2].hz, f1)) < 0.003);
  }
  // A step to below the hysteresis: the signal is gone as far as the counter can tell.
  {
    FreqCounter c;
    Result r = Run(c, [&](uint64_t i) {
      const double a = (i > 4800 + 12000) ? 20.0 : 12000.0;
      return a * sin(2 * kPi * (0.2 + 440.0 * i / kFs));
    });
    printf("  step to 20 LSB: status %s\n", StatusText(r.status));
    CHECK(r.status == Status::LOST);
  }
}

// ---- 6. 1 V/oct sweep -------------------------------------------------------
static void test_octave_sweep() {
  printf("test_octave_sweep  (1.2 V/oct, 0 V = 27.5 Hz, source slope error injected; V in volts)\n");
  printf("  %6s %10s %12s %12s\n", "V", "Hz", "err cent", "err mV");
  const double slope_err[] = {0.0, 0.01, -0.0035};  // fractional error of the source's V/oct
  for (double se : slope_err) {
    std::vector<double> vs, ls;
    double worst = 0;
    for (int k = 0; k <= 7; ++k) {
      const double V = k * 1.2;
      const double f = 27.5 * pow(2.0, V / 1.2 * (1.0 + se));
      FreqCounter c;
      Result r = Run(c, [&](uint64_t i) { return 14000.0 * sin(2 * kPi * (0.77 + f * i / kFs)); });
      CHECK(r.status == Status::OK);
      const double e = ErrCents(r, f);
      if (se == 0.0) printf("  %6.2f %10.3f %+12.5f %+12.5f\n", V, r.hz, e, e * 1.2 / 1200.0 * 1000.0);
      if (fabs(e) > worst) worst = fabs(e);
      vs.push_back(V);
      ls.push_back(log2(r.hz / 27.5));
    }
    // Fit log2(f) = a + b V and compare 1/b (V per octave) with the truth.
    double sv = 0, sl = 0, svv = 0, svl = 0;
    const int n = (int)vs.size();
    for (int i = 0; i < n; ++i) { sv += vs[i]; sl += ls[i]; svv += vs[i] * vs[i]; svl += vs[i] * ls[i]; }
    const double b = (n * svl - sv * sl) / (n * svv - sv * sv);
    const double vpo = 1.0 / b;
    const double want = 1.2 / (1.0 + se);
    printf("  slope error %+.4f: fitted %.6f V/oct, truth %.6f V/oct (%.4f mV off), worst point %.5f cent\n", se, vpo,
           want, (vpo - want) * 1000, worst);
    CHECK(fabs(vpo - want) < 2e-6);  // 2 uV per octave
    CHECK(worst < 0.003);
  }
}

// ---- 7. failure modes -------------------------------------------------------
static void test_failures() {
  printf("test_failures\n");
  { FreqCounter c; Result r = Run(c, [](uint64_t) { return 0.0; }, 3);
    printf("  silence: %s\n", StatusText(r.status)); CHECK(r.status == Status::NO_SIGNAL); }
  { FreqCounter c; Result r = Run(c, [](uint64_t i) { return 9.0 * ((i / 7) & 1 ? 1 : -1); }, 3);
    printf("  +-9 LSB hiss: %s\n", StatusText(r.status)); CHECK(r.status == Status::NO_SIGNAL); }
  { FreqCounter c; Result r = Run(c, [](uint64_t i) { return 120.0 * sin(2 * kPi * 440.0 * i / kFs); }, 3);
    printf("  440 Hz at 120 LSB: %s\n", StatusText(r.status)); CHECK(r.status == Status::TOO_QUIET); }
  { FreqCounter c; Result r = Run(c, [](uint64_t i) { return 10000.0 + 100.0 * sin(2 * kPi * 440.0 * i / kFs); }, 3);
    printf("  dc with ripple: %s\n", StatusText(r.status)); CHECK(r.status != Status::OK); }
  { FreqCounter c; Result r = Run(c, [](uint64_t i) { return 12000.0 * sin(2 * kPi * 3.0 * i / kFs); }, 6);
    printf("  3 Hz: %s\n", StatusText(r.status)); CHECK(r.status != Status::OK); }
  { FreqCounter c; Result r = Run(c, [](uint64_t i) { return 12000.0 * sin(2 * kPi * 9000.0 * i / kFs); }, 6);
    printf("  9 kHz: %s\n", StatusText(r.status)); CHECK(r.status == Status::TOO_HIGH); }
  { Rng rng(5); FreqCounter c;
    Result r = Run(c, [&](uint64_t) { return 6000.0 * rng.gauss(); }, 6);
    printf("  white noise: %s\n", StatusText(r.status)); CHECK(r.status != Status::OK); }
  { // signal stops mid-gate
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) { return i > 4800 + 20000 ? 0.0 : 12000.0 * sin(2 * kPi * 440.0 * i / kFs); }, 6);
    printf("  stops mid-gate: %s\n", StatusText(r.status)); CHECK(r.status == Status::LOST); }
  { // the tone stops for a second and comes back a fifth higher: the counter re-learns and reports it
    FreqCounter c;
    auto gen = [](uint64_t i) {
      const double t = i / kFs;
      if (t > 2.0 && t < 3.5) return 0.0;
      return 12000.0 * sin(2 * kPi * (t < 2.0 ? 440.0 * t : 660.0 * t + 0.3)); };
    std::vector<Result> r = RunN(c, gen, 40, 9.0);
    bool lost = false, back = false;
    for (const Result &x : r) {
      if (x.status == Status::LOST || x.status == Status::NO_SIGNAL) lost = true;
      if (lost && x.status == Status::OK && fabs(Cents(x.hz, 660.0)) < 0.01) back = true;
    }
    printf("  gap then 660 Hz: lost=%d recovered=%d (%zu results)\n", lost, back, r.size());
    CHECK(lost && back); }
  { // one cycle missing just before the gate closes: the fit barely moves (jitter 4% of a period),
    // only the interval check sees it
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) {
      const double t = i / kFs;
      return (t > 1.098 && t < 1.0991) ? 0.0 : 12000.0 * sin(2 * kPi * (1000.0 * t + 0.3)); });
    printf("  dropped cycle at the end of the gate: %s jitter %.4f ratio %.2f\n", StatusText(r.status), r.jitter, r.interval_ratio);
    CHECK(r.jitter < 0.05 && r.interval_ratio > 1.9);
    CHECK(r.status == Status::UNSTABLE); }
  { // hard clipped sine: counts correctly, flags it
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) { return 90000.0 * sin(2 * kPi * (0.2 + 440.0 * i / kFs)); });
    printf("  clipped: %s err %+.5f flags %d\n", StatusText(r.status), ErrCents(r, 440.0), r.flags);
    CHECK(r.status == Status::OK); CHECK(r.flags & FLAG_CLIPPED); CHECK(fabs(ErrCents(r, 440.0)) < 0.003); }
  { // 50.37 Hz hum at -30 dBFS on a 20 Hz tone at -6 dBFS: a counter's worst day
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) {
      return 16384.0 * sin(2 * kPi * (0.2 + 20.0 * i / kFs)) + 500.0 * sin(2 * kPi * (0.9 + 50.37 * i / kFs)); });
    printf("  20 Hz + 50.37 Hz hum -30 dB: %s err %+.5f cent sigma %.5f\n", StatusText(r.status), ErrCents(r, 20.0), r.cents_sigma);
    CHECK(r.status == Status::OK); CHECK(fabs(ErrCents(r, 20.0)) < 0.5); }
  { // 1 Hz vibrato of +-10 cents: that is FM, not a pitch, and the counter must say so
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) {
      const double t = i / kFs;
      const double dev = 440.0 * (pow(2.0, 10.0 / 1200.0) - 1.0);
      const double ph = 440.0 * t - dev / (2 * kPi * 1.0) * cos(2 * kPi * 1.0 * t);
      return 12000.0 * sin(2 * kPi * ph); });
    printf("  vibrato +-10 cent: %s\n", StatusText(r.status));
    CHECK(r.status == Status::UNSTABLE); }
  { // slow drift, 3 cents per second: the answer is the middle of the gate, and sigma shows it moved
    FreqCounter c;
    const double rate = 3.0 / 1200.0 * 0.69314718055994531;  // ln(2) * octaves per second
    Result r = Run(c, [&](uint64_t i) {
      const double t = i / kFs;
      return 12000.0 * sin(2 * kPi * (440.0 * (exp(rate * t) - 1.0) / rate)); });
    const double mid = 4800.0 / kFs + r.gate_s * 0.5;  // gate starts after learning
    const double want = 440.0 * exp(rate * mid);
    printf("  drift 3 cent/s: %s err vs mid-gate %+.4f cent  sigma %.4f\n", StatusText(r.status), Cents(r.hz, want), r.cents_sigma);
    CHECK(r.status == Status::OK); CHECK(fabs(Cents(r.hz, want)) < 0.2); CHECK(r.cents_sigma > 0.002); }
  { // 40 cent/s: flagged, and the number is still the mid-gate average
    FreqCounter c;
    const double rate = 40.0 / 1200.0 * 0.69314718055994531;
    Result r = Run(c, [&](uint64_t i) {
      const double t = i / kFs;
      return 12000.0 * sin(2 * kPi * (440.0 * (exp(rate * t) - 1.0) / rate)); });
    const double mid = 4800.0 / kFs + r.gate_s * 0.5;
    const double want = 440.0 * exp(rate * mid);
    printf("  drift 40 cent/s: %s err vs mid-gate %+.4f cent\n", StatusText(r.status), Cents(r.hz, want));
    CHECK(r.status == Status::UNSTABLE); CHECK(fabs(Cents(r.hz, want)) < 1.0); }
}

// ---- 8. behaviours the Scope and Cal Routine will lean on ------------------
static void test_chunking_and_gate() {
  printf("test_chunking_and_gate\n");
  auto gen = [](uint64_t i) { return 9000.0 * sin(2 * kPi * (0.1 + 523.25 * i / kFs)); };
  std::vector<int16_t> sig(200000);
  for (size_t i = 0; i < sig.size(); ++i) sig[i] = q(gen(i));
  FreqCounter a, b, d;
  std::vector<double> ra, rb, rd;
  for (size_t i = 0; i < sig.size(); ++i) if (a.Push(sig[i])) ra.push_back(a.Last().hz);
  for (size_t i = 0; i + 128 <= sig.size(); i += 128) if (b.Push(&sig[i], 128)) rb.push_back(b.Last().hz);
  size_t pos = 0; int chunk = 1;
  while (pos < sig.size()) {
    const int n = (int)std::min<size_t>(chunk, sig.size() - pos);
    if (d.Push(&sig[pos], n)) rd.push_back(d.Last().hz);
    pos += n; chunk = chunk % 997 + 1;
  }
  CHECK(!ra.empty());
  CHECK(ra.size() >= 3);
  CHECK(ra.size() >= rb.size());  // 128-blocks stop at a block edge: at most one gate less
  for (size_t i = 0; i < rb.size() && i < ra.size(); ++i) CHECK(ra[i] == rb[i]);
  CHECK(ra.size() == rd.size());
  for (size_t i = 0; i < rd.size(); ++i) CHECK(ra[i] == rd[i]);
  printf("  %zu results per 4.2 s, identical for 1-sample, 128-sample and ragged chunks\n", ra.size());
  // Successive gates are non-overlapping and each at least 1 s.
  uint64_t used = 0;
  FreqCounter c;
  Run(c, [&](uint64_t i) { return 9000.0 * sin(2 * kPi * (523.25 * i / kFs)); }, 8, &used);
  CHECK(used >= 48000 + 4800 - 100);  // learning (0.1 s) + a gate (1 s)
  CHECK(used < 48000 + 4800 + 3000);
  CHECK(c.Last().gate_s >= 1.0 && c.Last().gate_s < 1.0 + 1.0 / 523.25 + 1e-9);
  // A shorter gate is a configuration, and its error is what the design table says.
  Config cfg; cfg.gate_samples = 4800;
  FreqCounter s(cfg);
  Result r = Run(s, [&](uint64_t i) { return 9000.0 * sin(2 * kPi * (0.31 + 1234.5 * i / kFs)); });
  printf("  0.1 s gate at 1234.5 Hz: err %+.5f cent sigma %.5f\n", ErrCents(r, 1234.5), r.cents_sigma);
  CHECK(r.status == Status::OK && fabs(ErrCents(r, 1234.5)) < 0.05);
}

static void test_clock_ratio_and_notes() {
  printf("test_clock_ratio_and_notes\n");
  // A crystal 20 ppm fast makes every tone read 20 ppm low (samples arrive faster than assumed):
  // generate with the real rate, correct with the ratio.
  const double ppm = 20e-6;
  const double fs_true = kFs * (1.0 + ppm);
  Config cfg;
  FreqCounter plain(cfg);
  cfg.clock_ratio = 1.0 + ppm;
  FreqCounter fixed(cfg);
  auto gen = [&](uint64_t i) { return 9000.0 * sin(2 * kPi * (0.1 + 1000.0 * i / fs_true)); };
  Result a = Run(plain, gen), b = Run(fixed, gen);
  printf("  20 ppm crystal: uncorrected %+.5f cent (1 ppm = 0.00173 cent), corrected %+.5f\n", ErrCents(a, 1000.0), ErrCents(b, 1000.0));
  CHECK(fabs(ErrCents(a, 1000.0) - 1731.234 * (-ppm)) < 0.002 || fabs(ErrCents(a, 1000.0) - 1731.234 * ppm) < 0.002);
  CHECK(fabs(ErrCents(b, 1000.0)) < 0.002);
  CHECK(fabs(fabs(ErrCents(a, 1000.0)) - 0.0346) < 0.003);

  int m; double c;
  CHECK(NearestNote(440.0, 440.0, &m, &c) && m == 69 && fabs(c) < 1e-9);
  CHECK(NearestNote(442.0, 440.0, &m, &c) && m == 69 && fabs(c - 7.8475) < 0.01);
  CHECK(NearestNote(261.6256, 440.0, &m, &c) && m == 60 && fabs(c) < 0.01);
  CHECK(NearestNote(452.9, 440.0, &m, &c) && m == 70 && c < 0 && c > -50);  // just over the half-way point
  CHECK(NearestNote(432.0, 432.0, &m, &c) && m == 69);
  CHECK(!NearestNote(0.0, 440.0, &m, &c));
  char nm[8];
  NoteName(69, nm); CHECK(nm[0] == 'A' && nm[1] == '4' && !nm[2]);
  NoteName(61, nm); CHECK(nm[0] == 'C' && nm[1] == '#' && nm[2] == '4');
  NoteName(21, nm); CHECK(nm[0] == 'A' && nm[1] == '0');
  NoteName(0, nm);  CHECK(nm[0] == 'C' && nm[1] == '-' && nm[2] == '1');
}

// ---- 9. interpolation matters when the gate is short -------------------------
static void test_interp_value() {
  printf("test_interp_value  (0.05 s gate, sines 1 kHz..5 kHz with offset; cents)\n");
  double worst = 0;
  for (double f : {1000.0, 1777.7, 3001.3, 5000.0, 2345.6}) {
    Config a; a.gate_samples = 2400;
    FreqCounter ca(a);
    auto gen = [&](uint64_t i) { return 6000.0 + 9000.0 * sin(2 * kPi * (0.61 + f * i / kFs)); };
    const double ea = ErrCents(Run(ca, gen), f);
    printf("  %7.1f Hz  err %+9.5f\n", f, ea);
    worst = std::max(worst, fabs(ea));
  }
  // Whole-sample timestamps would scatter by +-0.5 sample and give about 0.06 cent here.
  CHECK(worst < 0.01);
}

// ---- 10. TR edge timestamps --------------------------------------------------
static void test_edges() {
  printf("test_edges\n");
  const double hz = 600e6;
  // 1 kHz clock, 600e6/1000 = 600000 cycles per edge, with the 32-bit counter wrapping
  // many times and 2 us (1200 cycle) peak interrupt-latency jitter.
  Rng rng(9);
  EdgeStats e;
  uint64_t t = 4000000000ull;  // starts near a wrap
  for (int i = 0; i < 10000; ++i) {
    t += 600000;
    const int64_t jit = (int64_t)((rng.uni() - 0.5) * 2400.0);
    e.NoteCycles((uint32_t)(t + jit));
  }
  EdgeResult r = e.Measure(hz);
  printf("  1 kHz over 10 s, +-2 us jitter, counter wraps: %.6f Hz, sigma %.3f ppm, interval %.0f..%.0f\n", r.hz, r.sigma_ppm,
         r.min_interval, r.max_interval);
  CHECK(r.edges == 10000);
  CHECK(fabs(r.hz - 1000.0) < 1000.0 * 1e-6);
  CHECK(r.min_interval > 600000 - 2400 && r.max_interval < 600000 + 2400);
  CHECK(r.sigma_ppm < 0.2);
  // Wrap extension exactness: intervals just under 2^32.
  Wrap32 w;
  uint64_t base = 123;
  uint64_t t0 = w.Extend((uint32_t)base);
  uint64_t t1 = w.Extend((uint32_t)(base + 4294967000ull));
  uint64_t t2 = w.Extend((uint32_t)(base + 2 * 4294967000ull));
  CHECK(t1 - t0 == 4294967000ull);
  CHECK(t2 - t1 == 4294967000ull);
  // Two edges only.
  EdgeStats two;
  two.Note(0); two.Note(1000);
  CHECK(fabs(two.Measure(1e6).hz - 1000.0) < 1e-9);
  CHECK(two.Measure(1e6).edges == 2);
  EdgeStats one; one.Note(5);
  CHECK(one.Measure(1e6).hz == 0.0);
  // A missed edge shows as a double interval.
  EdgeStats m;
  for (int i = 0; i < 20; ++i) if (i != 7) m.Note((uint64_t)i * 1000);
  CHECK(m.Measure(1e6).max_interval == 2000 && m.Measure(1e6).min_interval == 1000);
}

static void test_lsq() {
  printf("test_lsq\n");
  LsqLine l;
  for (int k = 0; k < 1000; ++k) l.Add(7.25 + 3.5 * k);
  CHECK(fabs(l.Slope() - 3.5) < 1e-12);
  CHECK(l.ResidualRms() < 1e-9);
  LsqLine big;  // huge offsets: the plain sum-of-squares form loses everything here
  for (int k = 0; k < 50000; ++k) big.Add(1e9 + 9.6 * k + ((k * 7919) % 13 - 6) * 0.01);
  CHECK(fabs(big.Slope() - 9.6) < 1e-6);
  CHECK(big.ResidualRms() > 0.03 && big.ResidualRms() < 0.05);
}

// Log2 stands in for libm's log2 (whose tables cost 3.3 KB of RAM1 on the target).
static void test_log2() {
  printf("test_log2\n");
  double worst = 0;
  for (double x = 1e-6; x < 1e9; x *= 1.0173) {
    const double e = fabs(Log2(x) - log2(x));
    if (e > worst) worst = e;
  }
  CHECK(worst < 1e-13);
  CHECK(Log2(1.0) == 0.0);
  CHECK(fabs(Log2(2.0) - 1.0) < 1e-15);
  CHECK(fabs(Log2(0.5) + 1.0) < 1e-15);
  CHECK(fabs(Log2(440.0 / 440.0 * 1.0000001) - log2(1.0000001)) < 1e-18);
  // the cent and note helpers the screen uses
  CHECK(fabs(Cents(880.0, 440.0) - 1200.0) < 1e-10);
  CHECK(fabs(Cents(440.0 * pow(2.0, 0.37 / 1200), 440.0) - 0.37) < 1e-10);
  int midi; double c;
  CHECK(NearestNote(466.1637615, 440.0, &midi, &c) && midi == 70 && fabs(c) < 1e-6);
}

// Result::pp, the peak-to-peak behind a result: what lets the CV ranging tell
// an idle input from a slow one.
static void test_result_pp() {
  printf("test_result_pp\n");
  {
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) { return 9000.0 * sin(2 * kPi * (0.1 + 440.0 * i / kFs)); });
    CHECK(r.status == Status::OK);
    CHECK(r.pp > 17900 && r.pp <= 18000);
  }
  {
    FreqCounter c;
    Result r = Run(c, [](uint64_t) { return 0.0; });
    CHECK(r.status == Status::NO_SIGNAL && r.pp == 0);
  }
  {  // hiss of a few LSB: NO SIGNAL, and a small pp
    FreqCounter c;
    Rng rng(5);
    Result r = Run(c, [&](uint64_t) { return 3.0 * rng.gauss(); });
    CHECK(r.status == Status::NO_SIGNAL && r.pp > 0 && r.pp < 40);
  }
  {  // a tone too quiet to count: TOO QUIET with the pp it had
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) { return 90.0 * sin(2 * kPi * (0.1 + 440.0 * i / kFs)); });
    CHECK(r.status == Status::TOO_QUIET && r.pp > 170 && r.pp <= 180);
  }
  {  // a slow ramp the learning window sees a slice of
    FreqCounter c;
    Result r = Run(c, [](uint64_t i) { return -3000.0 + 0.5 * (double)i; });
    CHECK((r.status == Status::NO_SIGNAL || r.status == Status::TOO_QUIET || r.status == Status::LOST) && r.pp > 1000);
  }
}

int main() {
  test_lsq();
  test_log2();
  test_result_pp();
  test_tone_table();
  test_noise();
  test_heavy_noise();
  test_dc_offset();
  test_dc_ramp();
  test_harmonics();
  test_amplitude_steps();
  test_octave_sweep();
  test_failures();
  test_chunking_and_gate();
  test_clock_ratio_and_notes();
  test_interp_value();
  test_edges();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
