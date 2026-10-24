#ifndef SCOPETRIGGER_H_
#define SCOPETRIGGER_H_
// ---------------------------------------------------------------------------
// Period estimate and triggering for the Scope. Pure; host-tested in
// test/test_scope_trigger.cpp against synthetic ring-mod, sine, clipped
// square and flat inputs.
//
// The hard case is Oren's ring mod (a folded wave times a sine). A
// sum-and-difference spectrum crosses its mean several times per true
// period, so "first rising edge" hops between sidebands every frame. Two
// rules make it hold still:
//   1. The period is the FIRST autocorrelation peak within 10 % of the best,
//      which is the full repeat of the composite, not a sideband.
//   2. A fresh lock starts on the steepest rising mean crossing in the newest
//      period. Once locked, the frame start is wherever (within one period)
//      the locked template correlates best: it tracks the feature itself, so
//      a morphing wave stays on it even when its crossings fade. The template
//      stays anchored while it still matches (correlation >= 0.9), so a
//      steady wave cannot drift by accumulated rounding. Only a period change
//      of more than 10 % or a poor match (< 0.3) re-locks.
//
// Inputs are oldest-first float arrays in any unit (the app passes mV).
// Loop context only; no allocation.
// ---------------------------------------------------------------------------

namespace ScopeTrigger {

constexpr int kMaxWindow = 1024;

// Period in samples (sub-sample, parabolic), or 0 when there is none: a flat
// input, peak-to-peak below min_pp, or no lag correlating at 0.5 or better.
float EstimatePeriod(const float *x, int n, int min_lag, int max_lag, float min_pp = 0.f);

// Start of the window-long frame beginning at the LATEST qualifying crossing
// of `level` that still leaves `window` samples after it, or -1. A crossing
// needs the signal to have been beyond level -/+ hyst first.
int LevelTrigger(const float *x, int n, int window, float level, float hyst, bool rising);

class AutoTrigger {
public:
  void Reset();
  // Start index of the frame to display. Untriggered (Locked() false) it is
  // the frame ending at the newest sample, n - window.
  int Find(const float *x, int n, int window, float min_pp = 0.f);
  bool Locked() const { return locked_; }
  float Period() const { return period_; }

private:
  float prev_[kMaxWindow];
  int prev_len_ = 0;
  float period_ = 0.f;
  bool locked_ = false;
};

}  // namespace ScopeTrigger

#endif  // SCOPETRIGGER_H_
