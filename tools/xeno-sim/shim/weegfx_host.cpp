// Compiles the REAL renderer -- software/src/src/drivers/weegfx.cpp, byte for
// byte -- into the simulator, so the screens the simulator draws are produced
// by the same clipping, the same 6x8 font and the same vertical pixel packing
// the module uses. Nothing here reimplements any drawing.
//
// Two host adjustments, both confined to this translation unit:
//
//   FLASHMEM     is a Teensy placement attribute with no host meaning.
//
//   size_t       weegfx.h declares `print(uint32_t, unsigned)` while
//                weegfx.cpp defines `print(uint32_t, size_t)`. On the Teensy 4
//                those are the same type (ILP32); on a 64-bit host they are
//                not, and the out-of-line definition matches no declaration.
//                Redefining size_t to `unsigned` makes them agree again, which
//                is why every system header this file needs is pulled in FIRST,
//                above the #define -- their own typedefs of size_t must be seen
//                before the macro exists. Do not move these includes below it.
//
// The alternative was editing weegfx.cpp, which is firmware.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string.h>
#include <utility>

#include <Arduino.h>   // software/test/host_stubs/Arduino.h (PROGMEM only)

#define FLASHMEM
#define size_t unsigned

#include "src/drivers/weegfx.cpp"  // NOLINT -- deliberately including the .cpp
