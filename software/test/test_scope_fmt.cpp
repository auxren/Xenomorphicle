// Host tests for the integer number formatter (src/ScopeFmt.h) against printf,
// and for the float-printf shim itself.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/ScopeFmt.h"

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

static std::string Fx(double v, int d, bool plus = false) {
  char b[64];
  const int n = ScopeFmt::Fixed(b, sizeof(b), v, d, plus);
  if ((size_t)n != std::strlen(b)) return "LENGTH MISMATCH";
  return b;
}

static void test_known() {
  printf("test_known\n");
  CHECK(Fx(440.012, 3) == "440.012");
  CHECK(Fx(0.0, 2) == "0.00");
  CHECK(Fx(0.0, 0) == "0");
  CHECK(Fx(-0.001, 2) == "0.00");                 // never "-0.00"
  CHECK(Fx(-0.001, 2, true) == "+0.00");
  CHECK(Fx(-0.004, 2, true) == "+0.00");
  CHECK(Fx(-0.006, 2, true) == "-0.01");
  CHECK(Fx(5.5, 0) == "6");
  CHECK(Fx(-5.5, 0) == "-6");
  CHECK(Fx(0.5, 0) == "1");
  CHECK(Fx(0.49, 0) == "0");
  CHECK(Fx(12.34, 1, true) == "+12.3");
  CHECK(Fx(-12.34, 1, true) == "-12.3");
  CHECK(Fx(1000.0, 2) == "1000.00");
  CHECK(Fx(99.995, 2) == "100.00" || Fx(99.995, 2) == "99.99");   // a representation tie
  CHECK(Fx(0.99999, 3) == "1.000");
  CHECK(Fx(4978.123, 3) == "4978.123");
  CHECK(Fx(0.00001, 5) == "0.00001");
  CHECK(Fx(1.0, 9) == "1.000000000");
  CHECK(Fx(123456789.0, 0) == "123456789");
  CHECK(Fx(0.0, 3, true) == "+0.000");
  CHECK(Fx(NAN, 2) == "nan");
  CHECK(Fx(1e12, 2) == "ovf");
  CHECK(Fx(-1e12, 2) == "-ovf");
  CHECK(Fx(INFINITY, 1) == "ovf");
  CHECK(Fx(-INFINITY, 1) == "-ovf");
  CHECK(Fx(1.5, -3) == "2");                      // negative decimals mean 0
  CHECK(Fx(1.5, 40) == "1.500000000");            // and more than 9 mean 9
}

static void test_truncation() {
  printf("test_truncation\n");
  char b[8];
  int n = ScopeFmt::Fixed(b, sizeof(b), 440.012, 3);
  CHECK(n == 7 && std::strcmp(b, "440.012") == 0);
  n = ScopeFmt::Fixed(b, 4, 440.012, 3);
  CHECK(n == 7 && std::strcmp(b, "440") == 0);    // the length the full text has, like snprintf
  n = ScopeFmt::Fixed(b, 1, 440.012, 3);
  CHECK(n == 7 && b[0] == 0);
  n = ScopeFmt::Fixed(nullptr, 0, 1.5, 1);
  CHECK(n == 3);
  n = ScopeFmt::Fixed(b, 0, 1.5, 1);
  CHECK(n == 3);
}

// Against the host's printf over the ranges the Scope uses. A value whose scaled
// fraction is within 1e-9 of one half is a rounding tie that the two may break
// either way; every other value must match exactly.
static void test_against_printf() {
  printf("test_against_printf\n");
  srand(11);
  long ties = 0, n = 0;
  struct Range { double lo, hi; const char *what; } ranges[] = {
      {16.0, 5000.0, "Hz"}, {-50.0, 50.0, "cents"}, {0.0, 100.0, "sigma"},
      {0.0, 24.0, "kHz"},   {-15.0, 15.0, "volts"}, {0.0, 8000.0, "wide"}, {-1.0, 1.0, "small"},
  };
  for (const Range &r : ranges) {
    for (int dec = 0; dec <= 5; ++dec) {
      for (int i = 0; i < 4000; ++i) {
        const double v = r.lo + (r.hi - r.lo) * (rand() / (double)RAND_MAX);
        const double s = std::fabs(v) * std::pow(10.0, dec);
        const double frac = s - std::floor(s);
        char want[64], got[64];
        for (int plus = 0; plus < 2; ++plus) {
          if (plus) snprintf(want, sizeof(want), "%+.*f", dec, v);
          else snprintf(want, sizeof(want), "%.*f", dec, v);
          ScopeFmt::Fixed(got, sizeof(got), v, dec, plus != 0);
          ++n;
          if (std::fabs(frac - 0.5) < 1e-9) { ++ties; continue; }
          std::string w = want;
          // printf keeps the sign of a value that rounds to zero; Fixed does not
          if (w == "-0" || w.rfind("-0.", 0) == 0) {
            bool allzero = true;
            for (char c : w) if (c != '-' && c != '0' && c != '.') allzero = false;
            if (allzero) w = w.substr(1);
          }
          if (plus && (w[0] != '-' && w[0] != '+')) w = "+" + w;
          if (w[0] == '+' && !plus) w = w.substr(1);
          if (w != got) {
            if (failures < 10) printf("  v=%.17g dec=%d plus=%d printf=%s fixed=%s\n", v, dec, plus, want, got);
            CHECK(false);
          }
        }
      }
    }
  }
  CHECK(n > 100000);
  CHECK(ties < n / 1000);
  printf("  %ld comparisons, %ld rounding ties skipped\n", n, ties);
  ++checks;
}

// Exact multiples of the last digit round-trip, which is what the line prints.
static void test_grid_values() {
  printf("test_grid_values\n");
  for (int dec = 0; dec <= 3; ++dec) {
    const double unit = std::pow(10.0, -dec);
    for (long i = -50000; i <= 50000; i += 7) {
      const double v = (double)i * unit;
      char want[64], got[64];
      snprintf(want, sizeof(want), "%.*f", dec, v);
      ScopeFmt::Fixed(got, sizeof(got), v, dec);
      std::string w = want;
      if (w[0] == '-' && w.find_first_of("123456789") == std::string::npos) w = w.substr(1);
      CHECK(w == got);
    }
  }
}

static void test_peak_text() {
  printf("test_peak_text\n");
  auto pk = [](double hz) { char b[16]; int n = ScopeFmt::PeakText(b, sizeof(b), hz); if ((size_t)n != std::strlen(b)) return std::string("LEN"); return std::string(b); };
  CHECK(pk(79.0) == "pk 79Hz");
  CHECK(pk(98.4) == "pk 98Hz");
  CHECK(pk(808.0) == "pk 808Hz");
  CHECK(pk(999.4) == "pk 999Hz");
  CHECK(pk(999.5) == "pk 1.0k");        // never "pk 1000Hz": a wider field
  CHECK(pk(1100.0) == "pk 1.1k");
  CHECK(pk(12345.0) == "pk 12.3k");
  CHECK(pk(23999.0) == "pk 24.0k");
  CHECK(pk(0.0) == "pk 0Hz");
  CHECK(pk(-5.0) == "pk 0Hz");
  CHECK(pk(NAN) == "pk 0Hz");
  auto fine = [](double hz) { char b[16]; ScopeFmt::PeakText(b, sizeof(b), hz, true); return std::string(b); };
  CHECK(fine(98.43) == "pk 98.4");          // the 0-300 Hz span: 0.73 Hz bins
  CHECK(fine(5.04) == "pk 5.0");
  CHECK(fine(299.96) == "pk 300.0");
  CHECK(fine(999.96) == "pk 1.0k");
  CHECK(fine(1100.0) == "pk 1.1k");
  for (double hz = 1.0; hz < 24000.0; hz *= 1.01) { char b[16]; ScopeFmt::PeakText(b, sizeof(b), hz, true); CHECK(std::strlen(b) <= 8); }
  for (double hz = 20.0; hz < 24000.0; hz *= 1.0007) CHECK(pk(hz).size() <= 8);   // the header's field
}

static void test_span_text() {
  printf("test_span_text\n");
  auto sp = [](double hz) { char b[16]; ScopeFmt::SpanText(b, sizeof(b), hz); return std::string(b); };
  CHECK(sp(4800.0) == "0-4.8k");
  CHECK(sp(1200.0) == "0-1.2k");
  CHECK(sp(300.0) == "0-300");
  CHECK(sp(3735.06) == "0-3.7k");        // the CV spans, from the verified frame rate
  CHECK(sp(933.76) == "0-934");
  CHECK(sp(233.44) == "0-233");
  CHECK(sp(999.4) == "0-999");
  CHECK(sp(999.5) == "0-1.0k");
  CHECK(sp(-1.0) == "0-0");
  for (double hz = 10.0; hz < 24000.0; hz *= 1.02) CHECK(sp(hz).size() <= 8);
}

int main() {
  test_known();
  test_truncation();
  test_against_printf();
  test_grid_values();
  test_peak_text();
  test_span_text();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
