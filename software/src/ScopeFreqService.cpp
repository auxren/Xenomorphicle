// The frequency counter service. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define FREQ_IRQ_OFF() __disable_irq()
#define FREQ_IRQ_ON() __enable_irq()
#define FREQ_CODE FLASHMEM
#else
#define FREQ_IRQ_OFF() ((void)0)
#define FREQ_IRQ_ON() ((void)0)
#define FREQ_CODE
#endif

// No jump tables: on the Teensy build the linker folds .rodata into RAM1 data,
// and a few switch tables here would push the next 4 KB alignment boundary.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-jump-tables")
#endif

#include "ScopeFreqService.h"

namespace ScopeFreqSvc {

namespace detail {
// RAM1 (DTCM) like any other static: the ISR touches it every sample.
ScopeFreq::FreqCounter g_counter;
volatile uint8_t g_audio_tap1 = 0;
volatile uint8_t g_cv_index1 = 0;
}  // namespace detail

namespace {

using ScopeFreq::Result;
using ScopeFreq::Status;
using namespace ScopeSources;

// Hand-off slot. One writer (whichever ISR feeds the counter: only one is ever
// armed), one reader (loop context). g_pub_seq is odd while the slot is being
// written; the reader retries when it moved or was odd.
Result g_slot;
volatile uint32_t g_pub_seq = 0;
volatile bool g_have = false;

unsigned g_src1 = 0;   // 1 + the source, 0 = none (zero-initialised: .bss)
Profile g_profile = Profile::NONE;
Gate g_gate = Gate::FAST;
bool g_clock_cal = false;
int g_clock_ppm10 = 0;
AutoRange g_auto;
ScopeFreqLine::Meter g_meter;
uint32_t g_polled_seq = 0;      // the counter's own result number
uint32_t g_polled_pub = 0;      // the hand-off slot's number, to skip a read when nothing is new

inline void Fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

void WriteSlot(const Result &r) {
  const uint32_t s = g_pub_seq;
  g_pub_seq = s + 1;  // odd: writing
  Fence();
  g_slot = r;
  Fence();
  g_have = true;
  g_pub_seq = s + 2;
}

void ForgetPolled(bool keep_meter) {
  g_polled_seq = 0;
  g_polled_pub = 0;
  g_auto = AutoRange();
  if (!keep_meter) g_meter.Reset();
}

// Loop context, interrupts off by the caller.
void Apply(Profile p, bool keep_meter = false) {
  g_profile = p;
  detail::g_counter.Configure(MakeConfig(p, g_gate, g_clock_ppm10 * (g_clock_cal ? 1 : 0)));
  g_have = false;
  g_pub_seq += 2;  // a reader mid-copy must retry and then see "nothing"
  ForgetPolled(keep_meter);
}

void Stop() {
  detail::g_audio_tap1 = 0;
  detail::g_cv_index1 = 0;
  g_src1 = 0;
  g_profile = Profile::NONE;
  g_have = false;
  g_pub_seq += 2;
  ForgetPolled(false);
}

}  // namespace

namespace detail {
void Publish() { WriteSlot(g_counter.Last()); }
void PublishForTest(const Result &r) { WriteSlot(r); }
}  // namespace detail

FREQ_CODE bool Countable(Source s) {
  const Kind k = KindOf(s);
  return k == KIND_AUDIO_IN || k == KIND_AUDIO_OUT || k == KIND_CV_IN;
}

FREQ_CODE const char *ProfileName(Profile p) {
  switch (p) {
    case Profile::NONE: return "NONE";
    case Profile::AUDIO: return "AUDIO";
    case Profile::CV: return "CV";
    case Profile::CV_LOW: return "CV_LOW";
  }
  return "?";
}

// rounded up, so a "2 s" gate is never 5 ppm short of two seconds
static uint32_t Samples(double seconds, double fs) {
  const double n = seconds * fs;
  const uint32_t k = (uint32_t)n;
  return (double)k < n ? k + 1 : k;
}

FREQ_CODE ScopeFreq::Config MakeConfig(Profile p, Gate g, int clock_ppm10) {
  ScopeFreq::Config c;
  c.clock_ratio = 1.0 + (double)clock_ppm10 * 1e-7;
  const bool slow = g == Gate::SLOW;
  switch (p) {
    case Profile::CV:
      c.fs = kCvFsHz;
      c.learn_samples = Samples(0.1, kCvFsHz);   // two cycles at 20 Hz
      c.max_period = Samples(0.1, kCvFsHz);      // 10 Hz
      c.gate_samples = Samples(slow ? 1.0 : 0.1, kCvFsHz);
      break;
    case Profile::CV_LOW:
      c.fs = kCvFsHz;
      c.learn_samples = Samples(2.0, kCvFsHz);
      c.max_period = Samples(2.0, kCvFsHz);      // 0.5 Hz
      c.gate_samples = Samples(slow ? 10.0 : 2.0, kCvFsHz);
      break;
    default:                       // AUDIO, NONE
      c.fs = kAudioFsHz;
      c.learn_samples = 4800;
      c.max_period = 4800;
      c.gate_samples = slow ? 48000 : 4800;
      break;
  }
  return c;
}

FREQ_CODE Profile AutoRange::Next(Profile cur, const Result &r) {
  if (cur == Profile::CV) {
    const bool moving = r.pp > kMovingLsb;
    const bool nothing = ((r.status == Status::NO_SIGNAL || r.status == Status::TOO_QUIET) && moving) ||
                         r.status == Status::TOO_FEW || r.status == Status::LOST;
    if (!nothing) { streak = 0; return cur; }
    if (++streak >= 2) { streak = 0; return Profile::CV_LOW; }
    return cur;
  }
  streak = 0;
  if (cur == Profile::CV_LOW) {
    if (r.status == Status::NO_SIGNAL || r.status == Status::TOO_QUIET) return Profile::CV;
    if (r.status == Status::OK && r.hz > 30.0) return Profile::CV;
  }
  return cur;
}

FREQ_CODE void Select(int src) {
  if (src + 1 == (int)g_src1) return;
  FREQ_IRQ_OFF();
  Stop();
  if (src >= 0 && src < SRC_COUNT && Countable((Source)src)) {
    g_src1 = (unsigned)src + 1;
    const Source s = (Source)src;
    if (KindOf(s) == KIND_CV_IN) {
      const int cv = SubIndex(s);
      detail::g_cv_index1 = (uint8_t)((cv < 4 ? cv + 4 : cv - 4) + 1);  // Scan_DMA's word order
      Apply(Profile::CV);
    } else {
      detail::g_audio_tap1 = (uint8_t)((KindOf(s) == KIND_AUDIO_IN ? SubIndex(s) : 2 + SubIndex(s)) + 1);
      Apply(Profile::AUDIO);
    }
  }
  FREQ_IRQ_ON();
}

FREQ_CODE void Deselect() { Select(-1); }
FREQ_CODE int ActiveSource() { return (int)g_src1 - 1; }
FREQ_CODE Profile ActiveProfile() { return g_profile; }

FREQ_CODE void SetGate(Gate g) {
  if (g == g_gate) return;
  FREQ_IRQ_OFF();
  g_gate = g;
  if (g_src1) Apply(g_profile);
  FREQ_IRQ_ON();
}

FREQ_CODE Gate GetGate() { return g_gate; }

FREQ_CODE bool SetClock(bool calibrated, int ppm10) {
  if (calibrated && (ppm10 > kClockMaxPpm10 || ppm10 < -kClockMaxPpm10)) return false;
  FREQ_IRQ_OFF();
  g_clock_cal = calibrated;
  g_clock_ppm10 = calibrated ? ppm10 : 0;
  if (g_src1) Apply(g_profile);
  FREQ_IRQ_ON();
  return true;
}

FREQ_CODE bool ClockCalibrated() { return g_clock_cal; }
FREQ_CODE int ClockPpm10() { return g_clock_ppm10; }

FREQ_CODE bool Read(Result *out) {
  for (int tries = 0; tries < 8; ++tries) {
    const uint32_t s1 = g_pub_seq;
    Fence();
    if (s1 & 1) continue;
    if (!g_have) return false;
    *out = g_slot;
    Fence();
    if (s1 == g_pub_seq) return true;
  }
  return false;
}

FREQ_CODE void Poll() {
  if (!g_src1) return;
  const uint32_t pub = g_pub_seq;
  if ((pub & 1) || pub == g_polled_pub) return;  // nothing new (or mid-write: next pass)
  Result r;
  if (!Read(&r)) return;
  g_polled_pub = pub;
  g_polled_seq = r.seq;
  g_meter.Update(r);
  if (g_profile != Profile::CV && g_profile != Profile::CV_LOW) return;
  const Profile next = g_auto.Next(g_profile, r);
  if (next != g_profile) {
    FREQ_IRQ_OFF();
    Apply(next, true);   // the screen keeps its last word while the new range learns
    FREQ_IRQ_ON();
  }
}

FREQ_CODE const ScopeFreqLine::Reading &CurrentReading() { return g_meter.reading(); }

FREQ_CODE void ResetForTest() {
  FREQ_IRQ_OFF();
  Stop();
  g_gate = Gate::FAST;
  g_clock_cal = false;
  g_clock_ppm10 = 0;
  detail::g_counter.Configure(MakeConfig(Profile::AUDIO, Gate::FAST, 0));
  FREQ_IRQ_ON();
}

}  // namespace ScopeFreqSvc
