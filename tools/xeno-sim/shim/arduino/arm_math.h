#ifndef XENOSIM_ARM_MATH_H_
#define XENOSIM_ARM_MATH_H_
// CMSIS-DSP stand-in. Only the pieces the firmware's non-audio code reaches:
// the fixed-point typedefs and the Cortex-M synchronisation intrinsics that
// util_sync.h's critical section is built from.
//
// The exclusive-access pair below is NOT atomic. The simulator runs every
// context -- ISR, bus, main loop -- on one thread from one loop, so there is
// nothing to be atomic against. No concurrency bug is reproducible here.
#include <math.h>
#include <stdint.h>

typedef float float32_t;
typedef double float64_t;
typedef int8_t q7_t;
typedef int16_t q15_t;
typedef int32_t q31_t;
typedef int64_t q63_t;

static inline void __DMB() {}
static inline void __DSB() {}
static inline void __ISB() {}
static inline void __NOP() {}
static inline void __CLREX() {}
static inline uint32_t __LDREXW(volatile uint32_t *p) { return *p; }
static inline uint32_t __STREXW(uint32_t v, volatile uint32_t *p) { *p = v; return 0; }
static inline uint8_t __LDREXB(volatile uint8_t *p) { return *p; }
static inline uint32_t __STREXB(uint8_t v, volatile uint8_t *p) { *p = v; return 0; }

// A handful of CMSIS-DSP float32 vector ops, portable C++ standing in for
// the Cortex-M SIMD/FPU instructions the real arm_math.h compiles these down
// to. Used by software/src/Audio/AudioTweightyF32.h's update() -- which
// never runs here (no audio-rate ISR exists in this simulator at all, see
// shim/arduino/Audio.h), so these only need to type-check and link, not to
// be fast. Signatures match the real CMSIS-DSP functions of the same name.
static inline void arm_scale_f32(const float32_t *pSrc, float32_t scale,
                                  float32_t *pDst, uint32_t blockSize) {
  for (uint32_t i = 0; i < blockSize; i++) pDst[i] = pSrc[i] * scale;
}
static inline void arm_add_f32(const float32_t *pSrcA, const float32_t *pSrcB,
                                float32_t *pDst, uint32_t blockSize) {
  for (uint32_t i = 0; i < blockSize; i++) pDst[i] = pSrcA[i] + pSrcB[i];
}
static inline void arm_mult_f32(const float32_t *pSrcA, const float32_t *pSrcB,
                                 float32_t *pDst, uint32_t blockSize) {
  for (uint32_t i = 0; i < blockSize; i++) pDst[i] = pSrcA[i] * pSrcB[i];
}

#endif
