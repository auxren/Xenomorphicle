#ifndef XENOSIM_FW_ARDUINO_H_
#define XENOSIM_FW_ARDUINO_H_
// Angle-bracket <Arduino.h> for the one translation unit that compiles the real
// PresetBusUI.cpp (see README.md in this directory). software/test/host_stubs/
// Arduino.h is deliberately near-empty -- it exists for bjorklund.cpp's PROGMEM
// and nothing else -- and this file needs a little more. Same discipline: this
// is what PresetBusUI.cpp uses, not a general Arduino layer.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif
#ifndef DMAMEM
#define DMAMEM
#endif
#ifndef FASTRUN
#define FASTRUN
#endif

// The simulator's virtual millisecond clock -- the same one shim/oc_shim.h
// hands the app, so the overlay's hold timing and the app's bus timing share
// one clock. Defined in sim_bus.cpp.
uint32_t SimNowMs();
static inline uint32_t millis() { return SimNowMs(); }

// Arduino's constrain() is a macro; a template is the same thing without the
// double evaluation, and PresetBusUI.cpp only ever calls it on ints.
template <typename T>
static inline T constrain(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

#endif  // XENOSIM_FW_ARDUINO_H_
