// Minimal host-build stub, NOT a real Arduino.h. Exists solely so
// src/src/extern/bjorklund.cpp -- which unconditionally #includes <Arduino.h>
// for the PROGMEM attribute -- can compile in a host g++ test build without
// the Teensyduino framework. bjorklund.cpp uses nothing else from Arduino.h
// (no digitalWrite, no millis, nothing beyond the PROGMEM keyword on its
// lookup table), so this is deliberately empty otherwise. If a future
// version of bjorklund.cpp (or anything else routed through this stub)
// starts using more of the real Arduino.h surface, either extend this stub
// or stop routing that file through it -- do not silently grow this into a
// general-purpose Arduino emulation layer.
#ifndef HOST_STUB_ARDUINO_H_
#define HOST_STUB_ARDUINO_H_

#ifndef PROGMEM
#define PROGMEM
#endif

#endif  // HOST_STUB_ARDUINO_H_
