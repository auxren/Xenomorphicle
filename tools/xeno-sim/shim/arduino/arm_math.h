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

#endif
