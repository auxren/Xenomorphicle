#ifndef XENOSIM_MALLOC_H_
#define XENOSIM_MALLOC_H_
// glibc/newlib's <malloc.h>. macOS has no such header; the firmware wants
// mallinfo() to report free RAM, which the simulator answers with a fixed
// number (see shim/src/OC_core.cpp).
#include <stdlib.h>
struct mallinfo {
  int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks;
  int fordblks, keepcost;
};
static inline struct mallinfo mallinfo(void) { struct mallinfo m = {}; return m; }
#endif
