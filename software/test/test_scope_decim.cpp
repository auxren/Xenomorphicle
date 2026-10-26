// Host tests for the zoom's decimator (src/ScopeDecim.h): DC gain, passband
// accuracy, alias rejection, output rate and the peak frequency found in the
// decimated spectrum.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <vector>

#include "../src/ScopeDecim.h"
#include "../src/ScopeFft.h"

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

static const double kPi = 3.14159265358979323846;
static ScopeFft::Work g_work;

// Peak-to-peak-free amplitude estimate: the rms of the settled output, as a sine amplitude.
static double Gain(int stages, double hz, double fs, double amp = 10000.0) {
  ScopeDecim::Cascade c;
  c.Reset(stages);
  double sum = 0;
  long n = 0;
  const long total = (long)(fs * 0.5) + 4096 * (1L << stages);
  for (long i = 0; i < total; ++i) {
    int16_t y;
    const int16_t x = (int16_t)lround(amp * sin(2 * kPi * hz * i / fs));
    if (c.Push(x, &y) && i > total / 2) { sum += (double)y * y; ++n; }
  }
  return sqrt(2.0 * sum / n) / amp;
}

static double Db(double g) { return 20.0 * log10(g + 1e-12); }

static void test_dc_gain_and_rate() {
  printf("test_dc_gain_and_rate\n");
  for (int st = 1; st <= 6; ++st) {
    ScopeDecim::Cascade c;
    c.Reset(st);
    int outs = 0;
    int16_t y = 0, last = 0;
    for (int i = 0; i < 100000; ++i)
      if (c.Push(12345, &y)) { ++outs; last = y; }
    CHECK(last == 12345);                       // DC gain exactly 1
    c.Reset(st);
    for (int i = 0; i < 4000; ++i) c.Push(30001, &y);
    CHECK(y == 30001);                          // (a coefficient sum one count off shows at large levels)
    CHECK(outs == 100000 >> st);                // one output per 2^stages inputs
    // negative DC and full scale do not wrap
    c.Reset(st);
    for (int i = 0; i < 4000; ++i) c.Push(-32768, &y);
    CHECK(y == -32768);
    c.Reset(st);
    for (int i = 0; i < 4000; ++i) c.Push(32767, &y);
    CHECK(y == 32767);
  }
  ScopeDecim::Cascade p;   // zero stages: pass through
  p.Reset(0);
  int16_t y;
  CHECK(!p.Push(5, &y) || y == 5);
}

// Pass band: flat (within 0.3 dB) to 0.8 of the final Nyquist, for the three zooms.
// Alias rejection: a tone that would fold into the shown band is 40 dB down or better.
static void test_passband_and_aliases() {
  printf("test_passband_and_aliases\n");
  const double fs = 48000.0;
  struct Z { int stages; } zooms[] = {{2}, {4}, {6}};
  for (auto z : zooms) {
    const double D = (double)(1 << z.stages);
    const double nyq = fs / D / 2.0, shown = 0.8 * nyq;
    for (double f : {0.05 * shown, 0.5 * shown, 0.9 * shown, shown}) {
      const double g = Db(Gain(z.stages, f, fs));
      printf("  /%g pass %.1f Hz: %.2f dB\n", D, f, g);
      CHECK(fabs(g) < 0.3);
    }
    // frequencies that alias into [0, shown] after the final /D: k*fs/D +- f
    double worst = -200;
    for (int k = 1; k <= (int)D / 2; ++k) {
      for (double f : {0.1 * shown, 0.5 * shown, shown}) {
        for (int sgn : {-1, 1}) {
          const double in = k * (fs / D) + sgn * f;
          if (in <= nyq * 1.05 || in >= fs / 2) continue;   // inside the final band or above the codec
          const double g = Db(Gain(z.stages, in, fs));
          if (g > worst) worst = g;
        }
      }
    }
    printf("  /%g worst alias into the shown band: %.1f dB\n", D, worst);
    CHECK(worst < -60.0);
  }
}

// Tones that fold through the first stage: with the plot reaching -90 dB, a near-full-scale
// tone anywhere between the first stage's passband and the input Nyquist must come out below
// -65 dB (the first version's 11-tap first stage let 19.5 kHz through at -43 dB: 21 px of plot).
static void test_first_stage_folding() {
  printf("test_first_stage_folding\n");
  struct Case { double fs; int stages; };
  for (Case c : {Case{48000.0, 2}, Case{48000.0, 4}, Case{48000.0, 6}, Case{37350.598, 2}, Case{37350.598, 4}, Case{37350.598, 6}}) {
    const double D = (double)(1 << c.stages);
    const double nyq = c.fs / D / 2.0, shown = 0.8 * nyq;
    double worst = -300;
    // every frequency from 0.3 of the input rate up to just below its Nyquist that lands in [0, shown]
    for (double f = 0.30 * c.fs; f < 0.4999 * c.fs; f += c.fs * 0.0037) {
      double r = fmod(f, c.fs / D);
      if (r > c.fs / D / 2.0) r = c.fs / D - r;       // where it lands after the final decimation
      if (r > shown) continue;
      const double g = Db(Gain(c.stages, f, c.fs, 30000.0));
      if (g > worst) worst = g;
    }
    printf("  fs %.0f /%g: worst fold-in from the upper band %.1f dB\n", c.fs, D, worst);
    CHECK(worst < -65.0);
  }
}

// Through the real FFT: a tone's peak in the decimated spectrum, and its resolution.
static void test_peak_frequency() {
  printf("test_peak_frequency\n");
  const double fs = 48000.0;
  struct { int stages; double hz; double tol; } cases[] = {
      {2, 1000.0, 1.0}, {2, 4400.0, 2.0}, {4, 100.0, 0.6}, {4, 440.0, 0.6},
      {6, 40.0, 0.2},   {6, 100.0, 0.2},  {6, 261.63, 0.2},
  };
  for (auto &c : cases) {
    ScopeDecim::Feed feed;
    feed.Reset(c.stages);
    const long need = (long)ScopeDecim::kRingLen * (1L << c.stages) + 4000;
    for (long i = 0; i < need; ++i) feed.Push((int16_t)lround(12000.0 * sin(2 * kPi * c.hz * i / fs)));
    CHECK(feed.Full());
    std::vector<float> in(1024), db(513);
    { std::vector<int16_t> raw(1024); CHECK(feed.Snapshot(raw.data())); for (int i = 0; i < 1024; ++i) in[i] = raw[i] / 32768.f; }
    ScopeFft::MagnitudeDb(in.data(), db.data(), g_work);
    const float rate = (float)(fs / (1 << c.stages));
    const float hz = ScopeFft::PeakHz(db.data(), rate, nullptr, 5.f * rate / 1024.f, 0.8f * rate / 2.f);
    printf("  /%d %.2f Hz -> %.3f Hz (bin %.2f Hz)\n", 1 << c.stages, c.hz, hz, rate / 1024.0);
    CHECK(fabs(hz - c.hz) < c.tol);
  }
}

static void test_feed_ring() {
  printf("test_feed_ring\n");
  ScopeDecim::Feed f;
  f.Reset(2);
  CHECK(f.Count() == 0 && !f.Full());
  for (int i = 0; i < 4 * 1023; ++i) f.Push(1000);
  CHECK(f.Count() == 1023 && !f.Full());
  for (int i = 0; i < 4; ++i) f.Push(1000);
  CHECK(f.Full() && f.Count() == 1024);
  // the oldest of the newest 1024 is first
  f.Reset(1);
  for (int i = 0; i < 3000; ++i) f.Push(7);
  for (int i = 0; i < 100; ++i) f.Push(9);
  std::vector<int16_t> snap0(1024);
  CHECK(f.Snapshot(snap0.data()));
  CHECK(snap0[0] == 7 && snap0[1023] == 9);
  // Reset forgets
  f.Reset(3);
  CHECK(f.Count() == 0 && f.stages() == 3);
}

// Q15 taps of the long filter (a copy: the test checks the code against it, not against itself)
static const int kLongOdd[12] = {10367, -3290, 1787, -1096, 693, -433, 261, -148, 77, -35, 13, -3};
static int LongTap(int k) {   // k = 0..46, centre 23
  const int d = k - 23;
  if (d == 0) return 16382;
  const int a = d < 0 ? -d : d;
  return (a & 1) ? kLongOdd[(a - 1) / 2] : 0;
}

// Full-scale overshoot: the long filter's worst-case gain is sum|h| = 1.195, so an input of
// +-32767 shaped to the taps' signs overshoots the int16 range. It must clamp, not wrap.
static void test_overshoot_clamps() {
  printf("test_overshoot_clamps\n");
  for (int sign : {1, -1}) {
    ScopeDecim::Cascade c;
    c.Reset(1);
    int16_t y = 0;
    for (int i = 0; i < 100; ++i) c.Push(0, &y);
    c.Push(0, &y);                                 // x(47): outside the 47 taps
    bool got = false;
    for (int k = 46; k >= 0; --k) {                // oldest first, newest (k = 0) last
      const int t = LongTap(k);
      const int16_t x = t == 0 ? 0 : (int16_t)((t > 0 ? 1 : -1) * sign * 32767);
      got = c.Push(x, &y);
    }
    CHECK(got);
    CHECK(y == (sign > 0 ? 32767 : -32768));
  }
  // the same shaped input against an int64 reference over many random +-full-scale streams
  srand(21);
  for (int trial = 0; trial < 20; ++trial) {
    ScopeDecim::Cascade c;
    c.Reset(1);
    std::vector<int> x;
    int clamped = 0;
    for (int n = 0; n < 4000; ++n) {
      const int v = (rand() & 1) ? 32767 : -32768;
      x.push_back(v);
      int16_t y;
      if (c.Push((int16_t)v, &y) && n >= 100) {
        long long acc = 0;
        for (int k = 0; k < 47; ++k) acc += (long long)LongTap(k) * x[n - k];
        long long r = (acc + 16384) >> 15;
        if (r > 32767) { r = 32767; ++clamped; }
        if (r < -32768) { r = -32768; ++clamped; }
        CHECK(y == r);
      }
    }
    (void)clamped;
  }
}

// Reset forgets filter history and phase: a used cascade after Reset is a new one.
static void test_reset_forgets() {
  printf("test_reset_forgets\n");
  for (int st = 1; st <= 6; ++st) {
    ScopeDecim::Cascade used, fresh;
    used.Reset(st);
    srand(st);
    int16_t y;
    for (int i = 0; i < 1001; ++i) used.Push((int16_t)(rand() - 16384), &y);   // odd count: phases mid-way
    used.Reset(st);
    fresh.Reset(st);
    srand(100 + st);
    int outs = 0;
    for (int i = 0; i < 3000; ++i) {
      const int16_t x = (int16_t)(rand() - 16384);
      int16_t ya = 0, yb = 0;
      const bool a = used.Push(x, &ya), b = fresh.Push(x, &yb);
      CHECK(a == b);
      if (a && b) { CHECK(ya == yb); ++outs; }
    }
    CHECK(outs == 3000 >> st);
  }
}

// A producer thread stands in for the ISR: a snapshot must never mix two moments.
static void test_snapshot_not_torn() {
  printf("test_snapshot_not_torn\n");
  ScopeDecim::Feed f;
  f.Reset(0);                                     // pass-through: the ring holds the counter
  std::atomic<bool> stop(false);
  std::thread prod([&] {
    uint32_t i = 0;
    while (!stop.load()) {
      f.Push((int16_t)i++);
      const auto t0 = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(25)) {}   // a decimated sample every 25 us: four times the CV /4 rate
    }
  });
  while (!f.Full()) {}                            // the producer needs 25 ms to fill the ring
  long ok = 0, torn = 0, refused = 0;
  std::vector<int16_t> snap(ScopeDecim::kRingLen);
  for (int rep = 0; rep < 40000; ++rep) {
    const bool got = f.Snapshot(snap.data());
    if (!got) { ++refused; continue; }
    ++ok;
    for (int i = 1; i < ScopeDecim::kRingLen; ++i)
      if ((int16_t)(snap[i] - snap[i - 1]) != 1) { ++torn; break; }
  }
  stop = true;
  prod.join();
  printf("  %ld snapshots, %ld refused, %ld torn\n", ok, refused, torn);
  CHECK(torn == 0);
  CHECK(ok > 0.95 * (ok + refused));   // a push every 25 us costs almost no snapshots with a fast copy
  // The deterministic version of the race: a push caught between writing its slot and bumping
  // the count (the reader runs in the middle) must not disturb the window being read.
  {
    ScopeDecim::Feed h;
    h.Reset(0);
    for (int i = 0; i < 3000; ++i) h.Push((int16_t)i);
    std::vector<int16_t> before(ScopeDecim::kRingLen), mid(ScopeDecim::kRingLen);
    CHECK(h.Snapshot(before.data()));
    h.WriteSlot((int16_t)-1);                       // sample 3000 stored, not yet published
    CHECK(h.Snapshot(mid.data()));
    CHECK(before == mid);
    h.Publish();                                    // now it is part of the window
    CHECK(h.Snapshot(mid.data()));
    CHECK(mid[ScopeDecim::kRingLen - 1] == -1);
  }
  // The retry rules, with a push injected between the copy and the recheck (what an ISR does):
  {
    ScopeDecim::Feed h;
    h.Reset(0);
    for (int i = 0; i < 3000; ++i) h.Push((int16_t)i);
    std::vector<int16_t> out(ScopeDecim::kRingLen);
    int calls = 0;
    // one push during the first copy: the snapshot is retried and is the NEW window
    CHECK(h.Snapshot(out.data(), [&](int attempt) { ++calls; if (attempt == 0) h.Push(7777); }));
    CHECK(calls == 2 && out[ScopeDecim::kRingLen - 1] == 7777);
    // pushes during the first three copies: the fourth attempt is clean and accepted
    calls = 0;
    CHECK(h.Snapshot(out.data(), [&](int attempt) { ++calls; if (attempt < 3) h.Push(5555); }));
    CHECK(calls == 4 && out[ScopeDecim::kRingLen - 1] == 5555);
    // pushes during every one of the four: refused, not accepted torn
    calls = 0;
    CHECK(!h.Snapshot(out.data(), [&](int) { ++calls; h.Push(4444); }));
    CHECK(calls == ScopeDecim::kReadAttempts && ScopeDecim::kReadAttempts == 4);
  }
  // not full yet: refused
  ScopeDecim::Feed g;
  g.Reset(2);
  for (int i = 0; i < 100; ++i) g.Push(1);
  { std::vector<int16_t> tmp(1024); CHECK(!g.Snapshot(tmp.data())); }
}

int main() {
  test_dc_gain_and_rate();
  test_passband_and_aliases();
  test_first_stage_folding();
  test_peak_frequency();
  test_feed_ring();
  test_overshoot_clamps();
  test_reset_forgets();
  test_snapshot_not_torn();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
