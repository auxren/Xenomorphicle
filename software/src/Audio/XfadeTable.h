#pragma once

// Equal-power crossfade tables, built by the compiler and kept in flash.
//
// AudioDelayExtF32 and AudioTweightyF32 each used to `new float[N]` twice per
// instance on Acquire() and fill the arrays with EqualPowerFade(). At the
// shipped N = 2048 that is 16 KB of RAM2 heap per Tweighty, and 16 KB per
// delay channel (32 KB for a stereo Delay), every one of them holding the
// identical numbers. The heap does not return freed memory to the untouched
// top, and a later large request (the 229 KB firmware card) needs one
// contiguous run, so these small blocks cost more than their size.
//
// Here the table is a constexpr object: it occupies flash, costs no heap, no
// allocation can fail, and there is one copy however many instances read it.
// Entry i is EqualPowerFade at t = i / (N - 1), the same argument the run-time
// loop used (float division by float(N - 1)).
//
// Flash, not DTCM: see kTable below.
//
// The values differ slightly from the old run-time fill: the compiler
// evaluates the curve without fused multiply-add, the device build contracted
// it (vfma). Measured against that fill: up to 3 ulp, 692 of 2048 entries
// differ, largest absolute difference 2.4e-7; the end points are exact
// (out[0] == 1.0, in[0] == 0.0, in[N-1] == 1.0, out[N-1] == 0.0).

#include <array>
#include <cstddef>
#include "../EqualPowerFade.h"

namespace XfadeLut {

template <size_t N>
struct Table {
  std::array<float, N> in;   // rises 0 -> 1
  std::array<float, N> out;  // falls 1 -> 0
};

template <size_t N>
constexpr Table<N> Build() {
  static_assert(N >= 2, "a crossfade needs two points");
  Table<N> t{};
  const float n = static_cast<float>(N - 1);
  for (size_t i = 0; i < N; i++) {
    EqualPowerFade(t.out[i], t.in[i], static_cast<float>(i) / n);
  }
  return t;
}

// Section .progmem (PROGMEM itself is empty on Teensy 4), and it matters: the Teensy 4 linker script moves every .rodata*
// into DTCM (.data) for speed, and DTCM is the scarce RAM on this module (see
// the T41_audio_dbg note in platformio.ini). Without the attribute this table
// is 16 KB of DTCM instead of 16 KB of RAM2 heap -- measured: RAM1 variables
// grew by exactly 16384 -- which only moves the problem. .progmem stays in
// flash and is read through the instruction cache, like any other flash const.
// externally_visible for the same reason OC_flashmem.h uses it: LTO drops the
// section attribute of a COMDAT (inline/template) object without it, and the
// table silently lands in DTCM again.
#if defined(__IMXRT1062__) && defined(__arm__)
#define XFADE_LUT_SECTION __attribute__((section(".progmem"), externally_visible))
#else
#define XFADE_LUT_SECTION
#endif
template <size_t N>
inline constexpr Table<N> kTable XFADE_LUT_SECTION = Build<N>();

}  // namespace XfadeLut
