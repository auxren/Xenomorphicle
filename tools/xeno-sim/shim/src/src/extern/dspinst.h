#ifndef XENOSIM_DSPINST_H_
#define XENOSIM_DSPINST_H_
// ---------------------------------------------------------------------------
// The Cortex-M4 DSP intrinsics, on a host.
//
// Every function in the real header is a single ARM instruction wrapped in
// inline asm, with a portable fallback only where the header already carries
// one for Cortex-M0 (KINETISL). Defining KINETISL for the duration of the
// include takes those fallbacks, which is the right answer where they exist.
//
// Where the real header has NO fallback the function has no body on this
// platform. Those are all audio-path helpers, the simulator compiles no audio,
// and an inline function that is never called is never emitted -- so this is
// safe as long as that stays true. If one is ever called from UI code the
// answer will be garbage, which is why the warning is suppressed here and
// nowhere else: this file is the record of that trade.
// ---------------------------------------------------------------------------

#ifndef KINETISL
#define KINETISL 1
#define XENOSIM_DEFINED_KINETISL
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#pragma clang diagnostic ignored "-Wasm-operand-widths"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#endif

#include <real/src/extern/dspinst.h>

#ifdef __clang__
#pragma clang diagnostic pop
#else
#pragma GCC diagnostic pop
#endif

#ifdef XENOSIM_DEFINED_KINETISL
#undef KINETISL
#undef XENOSIM_DEFINED_KINETISL
#endif

#endif  // XENOSIM_DSPINST_H_
