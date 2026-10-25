#ifndef SCOPEFREQ_H_
#define SCOPEFREQ_H_
// ---------------------------------------------------------------------------
// Frequency counter for the Xenomorpher used as a measuring instrument.
// Header-only, no Arduino or hardware dependency, host-tested in
// test/test_scope_freq.cpp. Design and error analysis:
// docs/specs/xeno-frequency-counter.md.
//
// FreqCounter::Push() takes audio samples one at a time (any block size, the
// result does not depend on how the samples are chunked). It finds rising
// crossings of a fixed level with a Schmitt trigger, timestamps each one to a
// fraction of a sample by linear interpolation (a cubic was tried and bought
// nothing measurable; see the spec), and after a gate of at least
// gate_samples fits a straight line through (crossing number, crossing time).
// The slope is the period in samples; frequency = fs / slope. The fit also
// gives the residual, which becomes a 1-sigma uncertainty in cents and the
// basis of the failure messages.
//
// EdgeStats / Wrap32 do the same fit for TR edge timestamps (cycle counter).
//
// Cost: per sample a few integer compares; per crossing one division and a
// handful of double operations.
// No allocation, no stdio, no FLASHMEM concerns: it is meant to run in the
// audio update() ISR, one instance, about 200 bytes.
// ---------------------------------------------------------------------------

#include <math.h>
#include <stdint.h>

// The floating-point work (interpolation, the fit, the result) is kept out of
// line. A function that contains any double arithmetic saves d8-d15 and builds a
// big frame on every call, integer-only path included, and the first FPU use in
// an interrupt makes the core stack the FP context; Push() itself is therefore
// kept free of it.
#if defined(__GNUC__)
#define SCOPEFREQ_NOINLINE __attribute__((noinline))
#else
#define SCOPEFREQ_NOINLINE
#endif

namespace ScopeFreq {

constexpr double kCentsPerOctave = 1200.0;

enum class Status : uint8_t {
  WARMING,     // no result yet
  OK,          // hz, cents_sigma valid
  NO_SIGNAL,   // nothing crossed the level: silence, or below the lowest frequency
  TOO_QUIET,   // there is a signal, but too small for the hysteresis
  LOST,        // the signal stopped during the gate
  UNSTABLE,    // crossings do not form a clean line: noise, double triggering, hum, FM
  TOO_FEW,     // fewer than 3 crossings fit in the gate: too low for this gate
  TOO_HIGH,    // fewer than 6 samples per cycle: interpolation no longer trustworthy
};

// At most 16 characters, for the 128x64 screen.
inline const char *StatusText(Status s) {
  switch (s) {
    case Status::WARMING:   return "WAIT";
    case Status::OK:        return "OK";
    case Status::NO_SIGNAL: return "NO SIGNAL";
    case Status::TOO_QUIET: return "TOO QUIET";
    case Status::LOST:      return "SIGNAL LOST";
    case Status::UNSTABLE:  return "UNSTABLE";
    case Status::TOO_FEW:   return "TOO LOW";
    case Status::TOO_HIGH:  return "TOO HIGH";
  }
  return "?";
}

enum Flags : uint8_t {
  FLAG_CLIPPED = 1,  // samples hit the rails during the gate (harmless to the count, but check the level)
};

// Least-squares line t = a + P*k through (k, t) points, k = 0,1,2.. in order,
// by Welford updates: sums of squares of 1e5-sample times would cancel to
// nothing in a plain double implementation.
struct LsqLine {
  uint32_t n = 0;
  double mean_t = 0, ckt = 0, ctt = 0;
  void Reset() { n = 0; mean_t = ckt = ctt = 0; }
  void Add(double t) {
    const double k = (double)n;
    // mean of k before this point: (n-1)/2, and 0 for the first.
    const double mk_old = n ? (k - 1.0) * 0.5 : 0.0;
    const double dt = t - mean_t;
    ++n;
    mean_t += dt / (double)n;
    ckt += (k - mk_old) * (t - mean_t);
    ctt += dt * (t - mean_t);
  }
  double Ckk() const { const double m = (double)n; return m * (m * m - 1.0) / 12.0; }
  // Slope (units of t per k), or 0 with fewer than 2 points.
  double Slope() const { return n >= 2 ? ckt / Ckk() : 0.0; }
  // RMS residual of the fit, in units of t; needs n >= 3.
  double ResidualRms() const {
    if (n < 3) return 0.0;
    double v = (ctt - ckt * ckt / Ckk()) / (double)(n - 2);
    return v > 0 ? sqrt(v) : 0.0;
  }
  // 1-sigma of the slope.
  double SlopeSigma() const { return n >= 3 ? ResidualRms() / sqrt(Ckk()) : 0.0; }
};

struct Config {
  double fs = 48000.0;             // sample rate, Hz
  double clock_ratio = 1.0;        // true_rate / assumed_rate: the one-time crystal correction
  uint32_t gate_samples = 48000;   // at least this long between first and last crossing
  uint32_t learn_samples = 4800;   // level/amplitude learning, 2 cycles at 20 Hz
  uint32_t max_period = 4800;      // longest period accepted (10 Hz)
  int hyst_min = 48;               // LSB: never less than this half-width
  double hyst_frac = 0.15;         // half-width as a fraction of the peak-to-peak
  double min_pp_hyst = 8.0;        // pp below this many hyst_min: TOO_QUIET
  bool auto_center = true;         // level = middle of the learned range, else fixed_level
  double fixed_level = 0.0;
  double max_interval_ratio = 1.3; // longest/shortest interval; above it: UNSTABLE
  double max_jitter = 0.05;        // residual rms / period; above it: UNSTABLE
  double min_samples_per_cycle = 6.0;
};

struct Result {
  Status status = Status::WARMING;
  uint8_t flags = 0;
  uint32_t seq = 0;          // increments on every new result of any status
  double hz = 0;             // valid when status is OK; filled, to be shown dimmed, when UNSTABLE
  double cents_sigma = 0;    // 1-sigma estimate from the fit residual
  double period_samples = 0;
  double gate_s = 0;         // first to last crossing
  uint32_t crossings = 0;
  double jitter = 0;         // residual rms / period
  double interval_ratio = 0; // longest / shortest interval
  double level = 0, hyst = 0;
  int32_t pp = 0;            // peak-to-peak of the samples behind this result, LSB (0 when none seen)
};

// log2 for x > 0 without libm's log/log2: the newlib in the Teensy toolchain
// keeps 3.3 KB of lookup tables for them in RAM1 (data, not flash), which is
// far more than the whole counter. frexp splits off the exponent; the mantissa
// in [0.707, 1.414) goes through ln(m) = 2 atanh((m-1)/(m+1)), whose series
// converges as 0.17^(2k): ten terms are good to 1e-15, about 1e-12 cent.
inline double Log2(double x) {
  int e = 0;
  double m = frexp(x, &e);  // [0.5, 1)
  if (m < 0.70710678118654752) { m *= 2.0; --e; }
  const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
  double term = s, sum = 0;
  for (int k = 1; k <= 21; k += 2) {
    sum += term / (double)k;
    term *= s2;
  }
  return (double)e + 2.0 * sum * 1.4426950408889634;
}

// Cents of f above ref (both > 0).
inline double Cents(double f, double ref) { return kCentsPerOctave * Log2(f / ref); }

// Nearest equal-tempered note; midi 69 = a4_hz. Returns false when f <= 0.
inline bool NearestNote(double f, double a4_hz, int *midi, double *cents) {
  if (!(f > 0) || !(a4_hz > 0)) return false;
  const double semis = 12.0 * Log2(f / a4_hz) + 69.0;
  const double r = floor(semis + 0.5);
  *midi = (int)r;
  *cents = (semis - r) * 100.0;
  return true;
}

// "A4", "C#3": buf of at least 5 bytes.
inline void NoteName(int midi, char *buf) {
  // one string literal (flash) rather than a table of pointers (RAM1 data on the target)
  const char *const kNames = "C  C# D  D# E  F  F# G  G# A  A# B  ";
  const int o = midi / 12 - 1, n = ((midi % 12) + 12) % 12;
  int i = 0;
  buf[i++] = kNames[3 * n];
  if (kNames[3 * n + 1] != ' ') buf[i++] = kNames[3 * n + 1];
  if (o < 0) { buf[i++] = '-'; buf[i++] = (char)('0' - o); }
  else if (o >= 10) { buf[i++] = '1'; buf[i++] = (char)('0' + o - 10); }
  else buf[i++] = (char)('0' + o);
  buf[i] = 0;
}

class FreqCounter {
public:
  explicit FreqCounter(const Config &c = Config()) { Configure(c); }

  void Configure(const Config &c) {
    cfg_ = c;
    Reset();
  }
  const Config &config() const { return cfg_; }

  // Back to learning; the last result stays readable until the next one.
  void Reset() {
    phase_ = LEARN;
    learn_n_ = 0;
    lo_ = hi_ = 0;
    hist_n_ = 0;
    st_ = ST_ABOVE;
    line_.Reset();
    clip_ = 0;
  }

  // True when a new Result was produced by this sample.
  bool Push(int16_t x) {
    ++pos_;
    if (x >= 32760 || x <= -32760) clip_ = 1;
    if (phase_ == LEARN) return Learn(x);
    if (x < lo_) lo_ = x;
    if (x > hi_) hi_ = x;
    prev_ = cur_;
    cur_ = x;
    if (hist_n_ < 2) { ++hist_n_; return false; }
    return Detect();
  }

  bool Push(const int16_t *x, int n) {
    bool got = false;
    for (int i = 0; i < n; ++i) got |= Push(x[i]);
    return got;
  }

  const Result &Last() const { return res_; }

private:
  enum Phase : uint8_t { LEARN, RUN };
  enum State : uint8_t { ST_ABOVE, ST_ARMED, ST_PENDING };  // not HIGH: Arduino.h defines that

  bool Learn(int16_t x) {
    if (learn_n_ == 0) lo_ = hi_ = x;
    if (x < lo_) lo_ = x;
    if (x > hi_) hi_ = x;
    if (++learn_n_ < cfg_.learn_samples) return false;
    return EndLearn();
  }

  // Levels are kept as integers in half-LSB units (2 * LSB) so the per-sample
  // path never touches the FPU: ls_ is twice the level, hhi_/hlo_ twice the
  // upper and lower hysteresis thresholds.
  SCOPEFREQ_NOINLINE
  bool SetLevels() {
    const int32_t pp = (int32_t)hi_ - (int32_t)lo_;
    ls_ = cfg_.auto_center ? (int32_t)hi_ + (int32_t)lo_ : (int32_t)floor(2.0 * cfg_.fixed_level + 0.5);
    double h = cfg_.hyst_frac * pp;
    if (h < cfg_.hyst_min) h = cfg_.hyst_min;
    const int32_t hi2 = (int32_t)ceil(h);
    hhi_ = ls_ + 2 * hi2;
    hlo_ = ls_ - 2 * hi2;
    return pp >= cfg_.min_pp_hyst * cfg_.hyst_min;
  }

  SCOPEFREQ_NOINLINE
  bool EndLearn() {
    const bool ok = SetLevels();
    learn_n_ = 0;
    if (!ok) {
      // Nothing worth counting: say why, and keep learning.
      const int pp = (int)hi_ - (int)lo_;
      return Publish(pp < cfg_.hyst_min ? Status::NO_SIGNAL : Status::TOO_QUIET);
    }
    StartRun();
    return false;
  }

  void StartRun() {
    phase_ = RUN;
    hist_n_ = 0;
    st_ = ST_ABOVE;  // must see a low half first
    line_.Reset();
    lo_ = 32767; hi_ = -32768;
    idle_ = 0;
    clip_ = 0;
    t_first_ = 0;
    prev_t_ = 0;
    imin_ = 1e300; imax_ = 0;
  }

  SCOPEFREQ_NOINLINE void Interpolate(int32_t a2, int32_t b2) {
    cand_ = (double)(pos_ - 2) + (double)(ls_ - a2) / (double)(b2 - a2);
  }

  // One step of the Schmitt trigger on the pair (prev_, cur_): samples number
  // pos_-2 and pos_-1 (0-based).
  bool Detect() {
    const int32_t a2 = 2 * (int32_t)prev_, b2 = 2 * (int32_t)cur_;
    bool ret = false;
    if (st_ != ST_ABOVE && a2 < ls_ && b2 >= ls_) {
      // Linear interpolation: where the chord between the two samples meets the level.
      Interpolate(a2, b2);
      st_ = ST_PENDING;
    }
    if (st_ == ST_PENDING && b2 >= hhi_) {
      st_ = ST_ABOVE;
      ret = Commit(cand_);
    } else if (b2 < hlo_) {
      st_ = ST_ARMED;
    }
    if (!ret) {
      ++idle_;  // samples since the last commit (or since the run began)
      if (line_.n == 0) {
        if (idle_ > 2u * cfg_.max_period) {
          ret = Publish(Status::NO_SIGNAL);
          RestartLearn();
        }
      } else if (idle_ > cfg_.max_period) {
        ret = Publish(line_.n < 3 ? Status::NO_SIGNAL : Status::LOST);
        RestartLearn();
      }
    }
    return ret;
  }

  void RestartLearn() {
    phase_ = LEARN;
    learn_n_ = 0;
    hist_n_ = 0;
    line_.Reset();
  }

  SCOPEFREQ_NOINLINE
  bool Commit(double t_abs) {
    idle_ = 0;
    if (line_.n == 0) {
      t_first_ = t_abs;
      line_.Add(0.0);
      prev_t_ = 0.0;
      return false;
    }
    const double t = t_abs - t_first_;
    const double iv = t - prev_t_;
    prev_t_ = t;
    if (iv < imin_) imin_ = iv;
    if (iv > imax_) imax_ = iv;
    line_.Add(t);
    if (t >= (double)cfg_.gate_samples) return Finish();
    return false;
  }

  SCOPEFREQ_NOINLINE
  bool Finish() {
    Result r;
    r.level = ls_ * 0.5;
    r.hyst = (hhi_ - ls_) * 0.5;
    r.pp = hi_ >= lo_ ? (int32_t)hi_ - (int32_t)lo_ : 0;
    r.crossings = line_.n;
    r.gate_s = prev_t_ / cfg_.fs;
    r.flags = clip_ ? FLAG_CLIPPED : 0;
    const double P = line_.Slope();
    r.period_samples = P;
    r.jitter = P > 0 ? line_.ResidualRms() / P : 0;
    r.interval_ratio = imin_ > 0 ? imax_ / imin_ : 1e9;
    if (line_.n < 3) {
      r.status = Status::TOO_FEW;
    } else if (!(P > 0)) {
      r.status = Status::UNSTABLE;
    } else if (P < cfg_.min_samples_per_cycle) {
      r.status = Status::TOO_HIGH;
    } else {
      // hz and sigma are filled even when UNSTABLE: the screen can show the number
      // dimmed, and the average of a moving pitch is still the average.
      r.hz = cfg_.fs * cfg_.clock_ratio / P;
      r.cents_sigma = kCentsPerOctave / 0.69314718055994531 * (line_.SlopeSigma() / P);
      r.status = (r.interval_ratio > cfg_.max_interval_ratio || r.jitter > cfg_.max_jitter) ? Status::UNSTABLE
                                                                                           : Status::OK;
    }
    // Next gate: levels from this gate's own range, which follows the signal.
    if (hi_ >= lo_ && (int)hi_ - (int)lo_ >= (int)(cfg_.min_pp_hyst * cfg_.hyst_min)) SetLevels();
    res_ = r;
    ++seq_;
    res_.seq = seq_;
    StartRunKeepState();
    return true;
  }

  SCOPEFREQ_NOINLINE
  void StartRunKeepState() {
    const State keep = st_;
    const int16_t p = prev_, c = cur_;
    const uint8_t hn = hist_n_;
    StartRun();
    st_ = keep;
    prev_ = p; cur_ = c;
    hist_n_ = hn;
  }

  SCOPEFREQ_NOINLINE
  bool Publish(Status s) {
    Result r;
    r.status = s;
    r.level = ls_ * 0.5;
    r.hyst = (hhi_ - ls_) * 0.5;
    r.pp = hi_ >= lo_ ? (int32_t)hi_ - (int32_t)lo_ : 0;
    res_ = r;
    ++seq_;
    res_.seq = seq_;
    return true;
  }

  Config cfg_;
  Result res_;
  uint32_t seq_ = 0;
  uint64_t pos_ = 0;
  uint32_t idle_ = 0;
  uint32_t learn_n_ = 0;
  Phase phase_ = LEARN;
  State st_ = ST_ABOVE;
  uint8_t hist_n_ = 0;
  uint8_t clip_ = 0;
  int16_t prev_ = 0, cur_ = 0;
  int16_t lo_ = 0, hi_ = 0;
  int32_t ls_ = 0, hhi_ = 0, hlo_ = 0;
  double cand_ = 0, t_first_ = 0, prev_t_ = 0;
  double imin_ = 1e300, imax_ = 0;
  LsqLine line_;
};

// ---------------------------------------------------------------------------
// TR edges. The pin interrupt stores the raw 32-bit cycle counter; loop
// context feeds them here in order. Wrap32 widens them to 64 bits, which is
// exact as long as consecutive edges are less than 2^32 cycles (7.16 s at
// 600 MHz) apart.
// ---------------------------------------------------------------------------
class Wrap32 {
public:
  uint64_t Extend(uint32_t c) {
    if (started_ && c < last_) hi_ += 1ull << 32;
    started_ = true;
    last_ = c;
    return hi_ | c;
  }
  void Reset() { started_ = false; hi_ = 0; last_ = 0; }

private:
  bool started_ = false;
  uint64_t hi_ = 0;
  uint32_t last_ = 0;
};

struct EdgeResult {
  uint32_t edges = 0;
  double period_ticks = 0;  // LSQ slope
  double hz = 0;            // tick_hz / period_ticks
  double min_interval = 0, max_interval = 0;  // ticks
  double jitter_ticks = 0;  // residual rms
  double sigma_ppm = 0;     // 1-sigma of the period estimate
};

class EdgeStats {
public:
  void Reset() { line_.Reset(); w_.Reset(); first_ = 0; prev_ = 0; imin_ = 1e300; imax_ = 0; }
  // Raw 32-bit cycle counter value of one edge.
  void NoteCycles(uint32_t c) { Note(w_.Extend(c)); }
  // Any monotonically increasing 64-bit tick.
  void Note(uint64_t ticks) {
    if (line_.n == 0) first_ = ticks;
    const double t = (double)(ticks - first_);
    if (line_.n) {
      const double iv = t - prev_;
      if (iv < imin_) imin_ = iv;
      if (iv > imax_) imax_ = iv;
    }
    prev_ = t;
    line_.Add(t);
  }
  uint32_t Count() const { return line_.n; }
  EdgeResult Measure(double tick_hz) const {
    EdgeResult r;
    r.edges = line_.n;
    if (line_.n < 2) return r;
    r.period_ticks = line_.Slope();
    r.hz = r.period_ticks > 0 ? tick_hz / r.period_ticks : 0;
    r.min_interval = imin_;
    r.max_interval = imax_;
    r.jitter_ticks = line_.ResidualRms();
    r.sigma_ppm = r.period_ticks > 0 ? 1e6 * line_.SlopeSigma() / r.period_ticks : 0;
    return r;
  }

private:
  LsqLine line_;
  Wrap32 w_;
  uint64_t first_ = 0;
  double prev_ = 0, imin_ = 1e300, imax_ = 0;
};

}  // namespace ScopeFreq

#endif  // SCOPEFREQ_H_
