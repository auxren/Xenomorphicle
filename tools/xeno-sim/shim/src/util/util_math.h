#ifndef XENOSIM_UTIL_MATH_H_
#define XENOSIM_UTIL_MATH_H_
// ---------------------------------------------------------------------------
// The real util/util_math.h, with its two ARM-assembly helpers replaced.
//
// SSAT16/USAT16 are single `ssat`/`usat` instructions written as inline asm.
// x86 has no such instruction, so the real definitions are renamed out of the
// way -- never called, never emitted -- and re-declared here in portable C++
// with the same saturating behaviour. Everything else in the header, which is
// the fixed-point division and interpolation the UI actually uses, is the real
// file included below.
// ---------------------------------------------------------------------------
#define SSAT16 SSAT16_arm_asm_unused_on_host
#define USAT16 USAT16_arm_asm_unused_on_host
#ifdef __clang__
// The renamed originals still have to parse; clang has an opinion about the
// operand widths of an ARM instruction it is never going to emit.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wasm-operand-widths"
#endif
#include <real/util/util_math.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#undef SSAT16
#undef USAT16

inline int32_t SSAT16(int32_t value) {
  return value > 32767 ? 32767 : (value < -32768 ? -32768 : value);
}
inline uint32_t USAT16(int32_t value) {
  return value < 0 ? 0u : (value > 65535 ? 65535u : (uint32_t)value);
}
inline uint32_t USAT16(uint32_t value) { return value > 65535 ? 65535u : value; }

#endif  // XENOSIM_UTIL_MATH_H_
