#ifndef SCOPEFREQLINE_H_
#define SCOPEFREQLINE_H_
// ---------------------------------------------------------------------------
// What the Scope and the console say about the counter's results. Pure,
// host-tested in test/test_scope_freq_line.cpp.
//
// The screen shows every gate at once (0.1 s), and says how far to trust it:
//   440.012Hz A4 +0.05c    two consecutive gates agreed, clock calibrated
//   *440.012Hz A4 +0.05c   agreed, but the crystal has not been calibrated:
//                          the absolute value can be 30 ppm (0.05 cent) off
//   ?440.012Hz A4 +0.05c   not (yet) confirmed by a second gate that agrees
//   #440.012Hz A4 +0.05c   steady, but the source's sample rate is not yet confirmed
//   !440.01Hz A4 +3c       UNSTABLE: an average of something that moved
//   NO SIGNAL, TOO QUIET, SIGNAL LOST, TOO LOW, TOO HIGH, WAIT
// The number of decimals follows the result's own sigma, so a coarse 0.1 s
// reading of a low tone prints fewer digits than a clean one.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "ScopeFreq.h"

namespace ScopeFreqLine {

constexpr int kLineChars = 21;           // the 128-pixel screen, 6-pixel font
constexpr double kAgreeSigmas = 3.0;
constexpr double kAgreeFloorCents = 0.01;

// Two OK gates agree when their frequencies differ by no more than 3 sigma
// combined plus 0.01 cent. Anything that moves every crossing together
// mid-gate (a DC step, an amplitude change on an asymmetric wave) is invisible
// to the sigma of one gate and shows here.
bool Agree(const ScopeFreq::Result &a, const ScopeFreq::Result &b);

struct Reading {
  bool have = false;       // a result has arrived since Reset
  ScopeFreq::Status status = ScopeFreq::Status::WARMING;
  bool confirmed = false;  // OK, and the gate before it was OK and agrees
  double hz = 0;           // valid for OK and UNSTABLE
  double sigma_cents = 0;
  double gate_s = 0;
  uint8_t flags = 0;
  uint32_t seq = 0;
};

class Meter {
public:
  void Reset();
  // Feed the newest Result; true when the reading changed (a new seq).
  bool Update(const ScopeFreq::Result &r);
  const Reading &reading() const { return cur_; }

private:
  Reading cur_;
  ScopeFreq::Result prev_;
  bool prev_ok_ = false;
};

// buf of at least kLineChars + 1 bytes; never longer than kLineChars.
// rate_unverified: the source's sample rate has not been confirmed on hardware
// (the CV inputs, ScopeFreqService.h kCvRateVerified): a # mark.
void FormatLine(const Reading &r, bool clock_calibrated, char *buf, size_t n, bool rate_unverified = false);
// The screen text when the Scope's source cannot be counted or the counter is off.
void FormatNa(char *buf, size_t n);

// OK, SETTLING (OK but unconfirmed), UNSTABLE, NO_SIGNAL, TOO_QUIET, LOST,
// TOO_LOW, TOO_HIGH, WAIT; no spaces, for the console.
const char *StatusToken(const Reading &r);
// SFREQ,<src>,<hz>,<sigma_cents>,<status>,<gate_ms>,<flags>,<seq>,<cal>,<rate>
// src is the source's label (INL, CV3) or "-"; r may be null (CLOSED). rate is 1
// when the source's sample rate is confirmed, 0 when not (CV inputs, for now).
int FormatSfreq(char *buf, size_t n, const char *src, const Reading *r, bool clock_calibrated,
                bool rate_verified = true);

}  // namespace ScopeFreqLine

#endif  // SCOPEFREQLINE_H_
