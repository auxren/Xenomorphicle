// Force-included (-include) when compiling firmware formatting code on the host:
// snprintf aborts on a float conversion, the way the module's newlib-nano prints
// nothing for it. The host's libc would happily print the number and hide the
// bug. See src/ScopeFmt.h.
#ifndef NOFLOAT_PRINTF_H_
#define NOFLOAT_PRINTF_H_
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static inline int nofloat_snprintf(char *buf, size_t n, const char *fmt, ...) {
  for (const char *p = fmt; *p; ++p) {
    if (*p != '%') continue;
    ++p;
    if (*p == '%') continue;
    while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) ++p;   // flags, width, precision, '*'
    while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') ++p;
    if (*p == 'f' || *p == 'F' || *p == 'e' || *p == 'E' || *p == 'g' || *p == 'G' || *p == 'a' || *p == 'A') {
      fprintf(stderr, "nofloat_snprintf: float conversion in \"%s\" (blank on the module)\n", fmt);
      abort();
    }
    if (!*p) break;
  }
  va_list ap;
  va_start(ap, fmt);
  const int r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
#define snprintf nofloat_snprintf
#endif
