// The Scope's measurement line and the console's SFREQ line. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define FREQ_LINE_CODE FLASHMEM
#else
#define FREQ_LINE_CODE
#endif

// No jump tables: on the Teensy build the linker folds .rodata into RAM1 data,
// and a few switch tables here would push the next 4 KB alignment boundary.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-jump-tables")
#endif

#include "ScopeFreqLine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ScopeFreqLine {

using ScopeFreq::Result;
using ScopeFreq::Status;

namespace {

constexpr double kCentsPerLn = 1200.0 / 0.69314718055994531;  // cents per unit of ln(f ratio), for sigma_hz
constexpr double kNoteMinHz = 16.0;                           // below this it is a rate, not a pitch

int DecimalsForSigma(double sigma_abs, int max_dec) {
  if (!(sigma_abs > 0)) return max_dec;
  // the last printed digit (1, 0.1, 0.01 ...) is no coarser than the sigma;
  // a loop, not log10, which would pull libm's log tables into RAM
  int d = 0;
  double unit = 1.0;
  while (d < max_dec && unit > sigma_abs) {
    unit *= 0.1;
    ++d;
  }
  return d;
}

const char *TextFor(Status s) {
  switch (s) {
    case Status::NO_SIGNAL: return "NO SIGNAL";
    case Status::TOO_QUIET: return "TOO QUIET";
    case Status::LOST: return "SIGNAL LOST";
    case Status::TOO_FEW: return "TOO LOW";
    case Status::TOO_HIGH: return "TOO HIGH";
    default: return "WAIT";
  }
}

void Copy(char *buf, size_t n, const char *s) {
  size_t k = strlen(s);
  const size_t cap = n - 1 < (size_t)kLineChars ? n - 1 : (size_t)kLineChars;
  if (k > cap) k = cap;
  memcpy(buf, s, k);
  buf[k] = 0;
}

}  // namespace

FREQ_LINE_CODE bool Agree(const Result &a, const Result &b) {
  if (a.status != Status::OK || b.status != Status::OK) return false;
  if (!(a.hz > 0) || !(b.hz > 0)) return false;
  const double diff = fabs(ScopeFreq::kCentsPerOctave * ScopeFreq::Log2(a.hz / b.hz));
  const double comb = sqrt(a.cents_sigma * a.cents_sigma + b.cents_sigma * b.cents_sigma);
  return diff <= kAgreeSigmas * comb + kAgreeFloorCents;
}

FREQ_LINE_CODE void Meter::Reset() {
  cur_ = Reading();
  prev_ = Result();
  prev_ok_ = false;
}

FREQ_LINE_CODE bool Meter::Update(const Result &r) {
  if (cur_.have && r.seq == cur_.seq) return false;
  Reading n;
  n.have = true;
  n.status = r.status;
  n.hz = r.hz;
  n.sigma_cents = r.cents_sigma;
  n.gate_s = r.gate_s;
  n.flags = r.flags;
  n.seq = r.seq;
  if (r.status == Status::OK) {
    n.confirmed = prev_ok_ && Agree(prev_, r);
    prev_ = r;
    prev_ok_ = true;
  } else {
    prev_ok_ = false;
  }
  cur_ = n;
  return true;
}

FREQ_LINE_CODE void FormatNa(char *buf, size_t n) {
  if (!buf || n == 0) return;
  Copy(buf, n, "freq: --");
}

FREQ_LINE_CODE void FormatLine(const Reading &r, bool clock_calibrated, char *buf, size_t n,
                               bool rate_unverified) {
  if (!buf || n == 0) return;
  if (!r.have || (r.status != Status::OK && r.status != Status::UNSTABLE)) {
    Copy(buf, n, r.have ? TextFor(r.status) : "WAIT");
    return;
  }
  char mark[2] = {0, 0};
  if (r.status == Status::UNSTABLE) mark[0] = '!';
  else if (!r.confirmed) mark[0] = '?';
  else if (rate_unverified) mark[0] = '#';
  else if (!clock_calibrated) mark[0] = '*';  // the 6x8 font has no tilde (it ends at 'z')

  int hz_dec = DecimalsForSigma(r.hz * r.sigma_cents / kCentsPerLn, 3);
  int c_dec = r.sigma_cents < 0.05 ? 2 : r.sigma_cents < 0.5 ? 1 : 0;
  char note[8] = "";
  double cents = 0;
  bool has_note = false;
  if (r.hz >= kNoteMinHz) {
    int midi = 0;
    if (ScopeFreq::NearestNote(r.hz, 440.0, &midi, &cents)) {
      ScopeFreq::NoteName(midi, note);
      has_note = true;
    }
  }
  char out[48];
  for (;;) {
    int k = snprintf(out, sizeof(out), "%s%.*fHz", mark, hz_dec, r.hz);
    if (has_note) {
      double c = cents;
      double unit = 1.0;
      for (int i = 0; i < c_dec; ++i) unit *= 0.1;
      if (fabs(c) < 0.5 * unit) c = 0.0;  // no "-0.00"
      snprintf(out + k, sizeof(out) - (size_t)k, " %s %+.*fc", note, c_dec, c);
    }
    if (strlen(out) <= (size_t)kLineChars) break;
    if (hz_dec > 0) --hz_dec;
    else if (c_dec > 0) --c_dec;
    else break;
  }
  Copy(buf, n, out);
}

FREQ_LINE_CODE const char *StatusToken(const Reading &r) {
  if (!r.have) return "WAIT";
  switch (r.status) {
    case Status::OK: return r.confirmed ? "OK" : "SETTLING";
    case Status::UNSTABLE: return "UNSTABLE";
    case Status::NO_SIGNAL: return "NO_SIGNAL";
    case Status::TOO_QUIET: return "TOO_QUIET";
    case Status::LOST: return "LOST";
    case Status::TOO_FEW: return "TOO_LOW";
    case Status::TOO_HIGH: return "TOO_HIGH";
    default: return "WAIT";
  }
}

FREQ_LINE_CODE int FormatSfreq(char *buf, size_t n, const char *src, const Reading *r, bool clock_calibrated,
                               bool rate_verified) {
  if (!buf || n == 0) return 0;
  if (!r) {
    return snprintf(buf, n, "SFREQ,%s,0.00000,0.00000,CLOSED,0,0,0,%d,%d", src, clock_calibrated ? 1 : 0,
                    rate_verified ? 1 : 0);
  }
  const bool num = r->have && (r->status == Status::OK || r->status == Status::UNSTABLE);
  return snprintf(buf, n, "SFREQ,%s,%.5f,%.5f,%s,%ld,%u,%lu,%d,%d", src, num ? r->hz : 0.0,
                  num ? r->sigma_cents : 0.0, StatusToken(*r), (long)lround(r->have ? r->gate_s * 1000.0 : 0.0),
                  (unsigned)(r->have ? r->flags : 0), (unsigned long)(r->have ? r->seq : 0),
                  clock_calibrated ? 1 : 0, rate_verified ? 1 : 0);
}

}  // namespace ScopeFreqLine
