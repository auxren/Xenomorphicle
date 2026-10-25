// Host tests for the measurement line and the console line
// (src/ScopeFreqLine.cpp): the two-gate acceptance rule, the Meter's
// confirmed/unconfirmed states, the line text and its width, the decimals
// following sigma, and the parseable SFREQ line.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/ScopeFreqLine.h"

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

#define CHECK_STR(got, want)                                                              \
  do {                                                                                    \
    ++checks;                                                                             \
    if (std::strcmp((got), (want)) != 0) {                                                \
      ++failures;                                                                         \
      printf("  FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, (got), (want)); \
    }                                                                                     \
  } while (0)

using namespace ScopeFreqLine;
using ScopeFreq::Result;
using ScopeFreq::Status;

static Result Ok(double hz, double sigma_c, uint32_t seq) {
  Result r;
  r.status = Status::OK;
  r.hz = hz;
  r.cents_sigma = sigma_c;
  r.seq = seq;
  r.gate_s = 0.1;
  return r;
}

static double Shift(double hz, double cents) { return hz * pow(2.0, cents / 1200.0); }

static std::string Line(const Reading &r, bool cal, bool unverified = false) {
  char b[64];
  FormatLine(r, cal, b, sizeof(b), unverified);
  return b;
}

static Reading Rd(Status s, double hz, double sig, bool confirmed, uint32_t seq = 1) {
  Reading r;
  r.have = true;
  r.status = s;
  r.hz = hz;
  r.sigma_cents = sig;
  r.confirmed = confirmed;
  r.seq = seq;
  return r;
}

static void test_agree() {
  printf("test_agree\n");
  const double f = 440.0;
  // sigma 0.001 each: combined 0.001414, 3 sigma = 0.004243, plus the 0.01 floor
  const double lim = 3.0 * sqrt(2.0) * 0.001 + 0.01;
  CHECK(Agree(Ok(f, 0.001, 1), Ok(f, 0.001, 2)));
  CHECK(Agree(Ok(f, 0.001, 1), Ok(Shift(f, lim * 0.95), 0.001, 2)));
  CHECK(!Agree(Ok(f, 0.001, 1), Ok(Shift(f, lim * 1.05), 0.001, 2)));
  CHECK(Agree(Ok(f, 0.001, 1), Ok(Shift(f, -lim * 0.95), 0.001, 2)));   // either sign
  CHECK(!Agree(Ok(f, 0.001, 1), Ok(Shift(f, -lim * 1.05), 0.001, 2)));
  // symmetric
  CHECK(Agree(Ok(Shift(f, lim * 0.95), 0.001, 2), Ok(f, 0.001, 1)));
  CHECK(!Agree(Ok(Shift(f, lim * 1.05), 0.001, 2), Ok(f, 0.001, 1)));
  // sigma widens the window: 0.3 cent sigma each allows about 1.27 cent
  const double lim2 = 3.0 * sqrt(2.0) * 0.3 + 0.01;
  CHECK(Agree(Ok(f, 0.3, 1), Ok(Shift(f, lim2 * 0.95), 0.3, 2)));
  CHECK(!Agree(Ok(f, 0.3, 1), Ok(Shift(f, lim2 * 1.05), 0.3, 2)));
  // the two sigmas combine in quadrature, not by their larger or their sum
  const double lim3 = 3.0 * sqrt(0.2 * 0.2 + 0.4 * 0.4) + 0.01;
  CHECK(Agree(Ok(f, 0.2, 1), Ok(Shift(f, lim3 * 0.98), 0.4, 2)));
  CHECK(!Agree(Ok(f, 0.2, 1), Ok(Shift(f, lim3 * 1.02), 0.4, 2)));
  // the DC-step case from the spec: sigma 0.05, off by 0.93 cent
  CHECK(!Agree(Ok(110.0, 0.05, 1), Ok(Shift(110.0, 0.93), 0.05, 2)));
  // only OK gates agree with anything
  Result u = Ok(f, 0.001, 2);
  u.status = Status::UNSTABLE;
  CHECK(!Agree(Ok(f, 0.001, 1), u));
  CHECK(!Agree(u, Ok(f, 0.001, 1)));
  Result z = Ok(0.0, 0.001, 2);
  CHECK(!Agree(Ok(f, 0.001, 1), z));
  CHECK(!Agree(z, z));
}

static void test_meter_states() {
  printf("test_meter_states\n");
  Meter m;
  CHECK(!m.reading().have);
  // first gate: shown, not confirmed
  CHECK(m.Update(Ok(440.0, 0.002, 1)));
  CHECK(m.reading().have && m.reading().status == Status::OK && !m.reading().confirmed);
  CHECK(m.reading().hz == 440.0);
  // the same seq again is not news
  CHECK(!m.Update(Ok(440.0, 0.002, 1)));
  // second agrees: confirmed
  CHECK(m.Update(Ok(Shift(440.0, 0.003), 0.002, 2)));
  CHECK(m.reading().confirmed);
  // third disagrees (a step): shown, unconfirmed
  CHECK(m.Update(Ok(Shift(440.0, 1.5), 0.002, 3)));
  CHECK(m.reading().status == Status::OK && !m.reading().confirmed);
  // and the next, at the new value, confirms against the one before it
  CHECK(m.Update(Ok(Shift(440.0, 1.5), 0.002, 4)));
  CHECK(m.reading().confirmed);
  // a gate that is not OK breaks the chain
  Result u = Ok(440.0, 0.5, 5);
  u.status = Status::UNSTABLE;
  CHECK(m.Update(u));
  CHECK(m.reading().status == Status::UNSTABLE && !m.reading().confirmed && m.reading().hz == 440.0);
  CHECK(m.Update(Ok(Shift(440.0, 1.5), 0.002, 6)));
  CHECK(!m.reading().confirmed);   // the UNSTABLE gate in between is not a witness
  CHECK(m.Update(Ok(Shift(440.0, 1.5), 0.002, 7)));
  CHECK(m.reading().confirmed);
  Result n;
  n.status = Status::NO_SIGNAL;
  n.seq = 8;
  CHECK(m.Update(n));
  CHECK(m.reading().status == Status::NO_SIGNAL && !m.reading().confirmed);
  CHECK(m.Update(Ok(440.0, 0.002, 9)));
  CHECK(!m.reading().confirmed);
  // Reset forgets everything
  m.Reset();
  CHECK(!m.reading().have);
  CHECK(m.Update(Ok(440.0, 0.002, 9)));   // even the same seq counts as news after a Reset
  CHECK(!m.reading().confirmed);
  // flags and gate pass through
  Result c = Ok(440.0, 0.002, 10);
  c.flags = ScopeFreq::FLAG_CLIPPED;
  c.gate_s = 0.1042;
  m.Update(c);
  CHECK(m.reading().flags == ScopeFreq::FLAG_CLIPPED && m.reading().gate_s == 0.1042 && m.reading().seq == 10);
}

static void test_line_text() {
  printf("test_line_text\n");
  CHECK(Line(Rd(Status::OK, 440.012, 0.0003, true), true) == "440.012Hz A4 +0.05c");
  CHECK(Line(Rd(Status::OK, 440.012, 0.0003, false), true) == "?440.012Hz A4 +0.05c");
  CHECK(Line(Rd(Status::OK, 440.012, 0.0003, true), false) == "*440.012Hz A4 +0.05c");
  CHECK(Line(Rd(Status::OK, 440.012, 0.0003, false), false) == "?440.012Hz A4 +0.05c");   // unconfirmed wins
  CHECK(Line(Rd(Status::OK, 440.0, 0.0003, true), true) == "440.000Hz A4 +0.00c");
  CHECK(Line(Rd(Status::OK, Shift(440.0, -0.001), 0.0003, true), true).find("-0.00") == std::string::npos);
  CHECK(Line(Rd(Status::OK, Shift(440.0, -0.001), 0.0003, true), true).find("+0.00c") != std::string::npos);
  CHECK(Line(Rd(Status::OK, Shift(440.0, -12.34), 0.0003, true), true).find("-12.34c") != std::string::npos);
  // fewer digits when sigma is large: 0.8 cent sigma on 100 Hz is 0.046 Hz
  CHECK(Line(Rd(Status::OK, 98.30, 0.8, true), true) == "98.30Hz G2 +5c");
  CHECK(Line(Rd(Status::OK, 98.30, 0.2, true), true).find("Hz G2 +") != std::string::npos);
  // one decimal in cents between 0.05 and 0.5 of sigma
  CHECK(Line(Rd(Status::OK, 261.63, 0.2, true), true) == "261.63Hz C4 +0.0c");
  // unstable: marked, number still there
  CHECK(Line(Rd(Status::UNSTABLE, 440.0, 3.0, false), true) == "!440.0Hz A4 +0c");
  // an LFO: no note name under 16 Hz
  CHECK(Line(Rd(Status::OK, 1.0001, 0.2, true), true) == "1.000Hz");
  CHECK(Line(Rd(Status::OK, 1.0001, 0.2, true), true).find("A") == std::string::npos);
  // text-only statuses
  CHECK(Line(Rd(Status::NO_SIGNAL, 0, 0, false), true) == "NO SIGNAL");
  CHECK(Line(Rd(Status::TOO_QUIET, 0, 0, false), true) == "TOO QUIET");
  CHECK(Line(Rd(Status::LOST, 0, 0, false), true) == "SIGNAL LOST");
  CHECK(Line(Rd(Status::TOO_FEW, 0, 0, false), true) == "TOO LOW");
  CHECK(Line(Rd(Status::TOO_HIGH, 0, 0, false), true) == "TOO HIGH");
  CHECK(Line(Rd(Status::WARMING, 0, 0, false), true) == "WAIT");
  {  // every fixed text and mark uses glyphs the screen font has (it ends at 'z': no tilde)
    const Status all[] = {Status::NO_SIGNAL, Status::TOO_QUIET, Status::LOST, Status::TOO_FEW, Status::TOO_HIGH, Status::WARMING};
    for (Status st : all)
      for (char ch : Line(Rd(st, 0, 0, false), true)) CHECK(ch >= ' ' && ch <= 'z');
    char na2[32];
    FormatNa(na2, sizeof(na2));
    for (const char *p = na2; *p; ++p) CHECK(*p >= ' ' && *p <= 'z');
  }
  Reading none;
  CHECK(Line(none, true) == "WAIT");
  char na[32];
  FormatNa(na, sizeof(na));
  CHECK_STR(na, "freq: --");
  // a flat note below the next semitone rounds to the nearer one
  CHECK(Line(Rd(Status::OK, 452.0, 0.0003, true), true).find("A4 +") != std::string::npos);
  CHECK(Line(Rd(Status::OK, 460.0, 0.0003, true), true).find("A#4 ") != std::string::npos);
  CHECK(Line(Rd(Status::OK, 27.5, 0.0003, true), true).find("A0 ") != std::string::npos);
  CHECK(Line(Rd(Status::OK, 4978.03, 0.0003, true), true).find("D#8 ") != std::string::npos);
}

// A CV input's frame rate is not yet confirmed on hardware: its readings say so.
static void test_rate_unverified_mark() {
  printf("test_rate_unverified_mark\n");
  CHECK(Line(Rd(Status::OK, 440.012, 0.0003, true), true, true) == "#440.012Hz A4 +0.05c");
  CHECK(Line(Rd(Status::OK, 440.012, 0.0003, true), false, true) == "#440.012Hz A4 +0.05c");   // beats the * mark
  CHECK(Line(Rd(Status::OK, 440.012, 0.0003, false), true, true) == "?440.012Hz A4 +0.05c");   // ? beats #
  CHECK(Line(Rd(Status::UNSTABLE, 440.0, 3.0, false), true, true) == "!440.0Hz A4 +0c");       // ! beats #
  CHECK(Line(Rd(Status::NO_SIGNAL, 0, 0, false), true, true) == "NO SIGNAL");                // text unchanged
  CHECK(Line(Rd(Status::OK, 4978.123, 0.0001, true), true, true).size() <= (size_t)kLineChars);
  CHECK(Line(Rd(Status::OK, 1.0001, 0.2, true), true, true) == "#1.000Hz");
}

static void test_line_width_and_truncation() {
  printf("test_line_width_and_truncation\n");
  // a tiny buffer never overflows and is always terminated
  char b[8];
  FormatLine(Rd(Status::OK, 440.012, 0.0003, true), true, b, sizeof(b));
  CHECK(std::strlen(b) <= 7);
  FormatLine(Rd(Status::OK, 440.012, 0.0003, true), true, b, 1);
  CHECK(b[0] == 0);
  // across the audio range, sigma and flags: never wider than the screen
  size_t worst = 0;
  srand(7);
  for (int i = 0; i < 20000; ++i) {
    const double hz = 10.0 * pow(8000.0 / 10.0, rand() / (double)RAND_MAX);
    const double sig = pow(10.0, -5.0 + 7.0 * rand() / (double)RAND_MAX);
    const Status st = (rand() & 3) ? Status::OK : Status::UNSTABLE;
    const bool cal = rand() & 1, conf = rand() & 1;
    const std::string s = Line(Rd(st, hz, sig, conf), cal);
    if (s.size() > worst) worst = s.size();
    // whole, not cut off by the width limit: a number with a note ends in the cents unit
    if (hz >= 16.0 && s.back() != 'c') {
      printf("  cut off: \"%s\"\n", s.c_str());
      CHECK(false);
      break;
    }
    for (char ch : s)   // the 6x8 font runs from space to 'z'
      if (ch < ' ' || ch > 'z') { printf("  glyph out of the font: \"%s\"\n", s.c_str()); CHECK(false); break; }
    if (s.size() > (size_t)kLineChars) {
      printf("  too wide: \"%s\" (%zu)\n", s.c_str(), s.size());
      CHECK(false);
      break;
    }
    if (s.find("-0.0c") != std::string::npos || s.find("-0c") != std::string::npos ||
        s.find("-0.00c") != std::string::npos) {
      printf("  negative zero: \"%s\"\n", s.c_str());
      CHECK(false);
      break;
    }
  }
  CHECK(worst >= 18);   // the test did reach the long forms
  CHECK(worst <= (size_t)kLineChars);
  // the longest realistic one, with the longest mark, fits as is
  CHECK(Line(Rd(Status::OK, 4978.123, 0.0001, false), false).size() <= (size_t)kLineChars);
  CHECK(Line(Rd(Status::UNSTABLE, 2093.123, 0.0001, false), false).size() <= (size_t)kLineChars);
}

static void test_status_token_and_sfreq() {
  printf("test_status_token_and_sfreq\n");
  CHECK_STR(StatusToken(Rd(Status::OK, 440, 0.1, true)), "OK");
  CHECK_STR(StatusToken(Rd(Status::OK, 440, 0.1, false)), "SETTLING");
  CHECK_STR(StatusToken(Rd(Status::UNSTABLE, 440, 0.1, false)), "UNSTABLE");
  CHECK_STR(StatusToken(Rd(Status::NO_SIGNAL, 0, 0, false)), "NO_SIGNAL");
  CHECK_STR(StatusToken(Rd(Status::TOO_QUIET, 0, 0, false)), "TOO_QUIET");
  CHECK_STR(StatusToken(Rd(Status::LOST, 0, 0, false)), "LOST");
  CHECK_STR(StatusToken(Rd(Status::TOO_FEW, 0, 0, false)), "TOO_LOW");
  CHECK_STR(StatusToken(Rd(Status::TOO_HIGH, 0, 0, false)), "TOO_HIGH");
  CHECK_STR(StatusToken(Rd(Status::WARMING, 0, 0, false)), "WAIT");
  Reading none;
  CHECK_STR(StatusToken(none), "WAIT");

  char b[160];
  Reading r = Rd(Status::OK, 440.01234, 0.00123, true, 17);
  r.gate_s = 0.1042;
  r.flags = ScopeFreq::FLAG_CLIPPED;
  int n = FormatSfreq(b, sizeof(b), "INL", &r, true);
  CHECK_STR(b, "SFREQ,INL,440.01234,0.00123,OK,104,1,17,1,1");
  CHECK(n == (int)std::strlen(b));
  r.confirmed = false;
  FormatSfreq(b, sizeof(b), "CV3", &r, false, false);
  CHECK_STR(b, "SFREQ,CV3,440.01234,0.00123,SETTLING,104,1,17,0,0");
  // a result with no number prints 0 for hz and sigma
  Reading q = Rd(Status::NO_SIGNAL, 0, 0, false, 9);
  FormatSfreq(b, sizeof(b), "INR", &q, true);
  CHECK_STR(b, "SFREQ,INR,0.00000,0.00000,NO_SIGNAL,0,0,9,1,1");
  // closed or off a countable source
  FormatSfreq(b, sizeof(b), "-", nullptr, false);
  CHECK_STR(b, "SFREQ,-,0.00000,0.00000,CLOSED,0,0,0,0,1");
  // no result yet
  FormatSfreq(b, sizeof(b), "INL", &none, true);
  CHECK_STR(b, "SFREQ,INL,0.00000,0.00000,WAIT,0,0,0,1,1");
  // always five-plus fields, no spaces, and never past the buffer
  char tiny[12];
  FormatSfreq(tiny, sizeof(tiny), "INL", &r, true);
  CHECK(std::strlen(tiny) < sizeof(tiny));
  // an LFO's hz keeps its digits
  Reading lf = Rd(Status::OK, 0.99987, 0.3, true, 2);
  lf.gate_s = 2.0001;
  FormatSfreq(b, sizeof(b), "CV1", &lf, true);
  CHECK_STR(b, "SFREQ,CV1,0.99987,0.30000,OK,2000,0,2,1,1");
}

int main() {
  test_agree();
  test_meter_states();
  test_line_text();
  test_rate_unverified_mark();
  test_line_width_and_truncation();
  test_status_token_and_sfreq();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
