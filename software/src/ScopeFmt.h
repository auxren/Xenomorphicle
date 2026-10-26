#ifndef SCOPEFMT_H_
#define SCOPEFMT_H_
// ---------------------------------------------------------------------------
// Fixed-point number text with integer arithmetic only.
//
// The firmware's snprintf is newlib-nano without float support: %f, %.*f and
// %+.*f print NOTHING on the module (the codebase avoids float printf on purpose:
// it would pull the float-printf tables into DTCM). The host's libc prints them
// fine, so a test that formats with %f passes on the host and shows an empty
// field on the OLED. Every number the Scope or the console prints from a double
// goes through Fixed() instead; test_scope_fmt.cpp checks it against printf, and
// the float-format guard in run-host-tests.sh and the abort-on-%f snprintf shim
// (test/host_stubs/nofloat_printf.h) keep %f out of these sources.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace ScopeFmt {

// v with `decimals` (0..9) digits after the point, rounded half away from zero.
// force_sign puts a '+' before a non-negative value (never "-0.00": a value that
// rounds to zero has no minus). NaN is "nan", beyond +-9e9 "ovf". Writes at most
// n bytes including the NUL; returns the length the full text has (like snprintf).
inline int Fixed(char *out, size_t n, double v, int decimals, bool force_sign = false) {
  char t[48];
  int k = 0;
  if (decimals < 0) decimals = 0;
  if (decimals > 9) decimals = 9;
  if (v != v) {
    t[k++] = 'n'; t[k++] = 'a'; t[k++] = 'n';
  } else {
    bool neg = v < 0;
    const double a = neg ? -v : v;
    uint64_t sc = 1;
    for (int i = 0; i < decimals; ++i) sc *= 10;
    if (a > 9e9) {
      if (neg) t[k++] = '-';
      else if (force_sign) t[k++] = '+';
      t[k++] = 'o'; t[k++] = 'v'; t[k++] = 'f';
    } else {
      const uint64_t q = (uint64_t)(a * (double)sc + 0.5);
      if (q == 0) neg = false;
      if (neg) t[k++] = '-';
      else if (force_sign) t[k++] = '+';
      char d[24];
      int nd = 0;
      for (uint64_t ip = q / sc; ; ip /= 10) {
        d[nd++] = (char)('0' + (int)(ip % 10));
        if (ip < 10) break;
      }
      while (nd) t[k++] = d[--nd];
      if (decimals) {
        t[k++] = '.';
        uint64_t fp = q % sc;
        for (int i = decimals - 1; i >= 0; --i) {
          uint64_t p = 1;
          for (int j = 0; j < i; ++j) p *= 10;
          t[k++] = (char)('0' + (int)((fp / p) % 10));
        }
      }
    }
  }
  t[k] = 0;
  if (out && n) {
    size_t c = (size_t)k < n - 1 ? (size_t)k : n - 1;
    for (size_t i = 0; i < c; ++i) out[i] = t[i];
    out[c] = 0;
  }
  return k;
}

// The spectrum peak for the Scope's header: "pk 79Hz", "pk 808Hz", "pk 1.1k",
// "pk 12.3k". At most 8 characters (the field the header gives it). Below 999.5 Hz
// whole hertz, from there kilohertz with one decimal.
inline int PeakText(char *out, size_t n, double hz) {
  char num[24];
  int k = 0;
  char t[40];
  t[k++] = 'p'; t[k++] = 'k'; t[k++] = ' ';
  if (!(hz >= 0)) hz = 0;
  if (hz < 999.5) {
    Fixed(num, sizeof(num), hz, 0);
    for (const char *c = num; *c; ++c) t[k++] = *c;
    t[k++] = 'H'; t[k++] = 'z';
  } else {
    Fixed(num, sizeof(num), hz / 1000.0, 1);
    for (const char *c = num; *c; ++c) t[k++] = *c;
    t[k++] = 'k';
  }
  t[k] = 0;
  if (out && n) {
    size_t c = (size_t)k < n - 1 ? (size_t)k : n - 1;
    for (size_t i = 0; i < c; ++i) out[i] = t[i];
    out[c] = 0;
  }
  return k;
}

}  // namespace ScopeFmt

#endif  // SCOPEFMT_H_
