#ifndef XENOSIM_WIRE_H_
#define XENOSIM_WIRE_H_
// Inert I2C. The 200e bus master's traffic is answered by sim_bus.cpp, not here.
#include <stdint.h>
#include <stddef.h>
class SimTwoWire {
public:
  void begin() {} void begin(uint8_t) {} void end() {}
  void setClock(uint32_t) {} void setSDA(uint8_t) {} void setSCL(uint8_t) {}
  void beginTransmission(uint8_t) {}
  uint8_t endTransmission(uint8_t = 1) { return 0; }
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t *, size_t n) { return n; }
  uint8_t requestFrom(uint8_t, uint8_t, uint8_t = 1) { return 0; }
  int available() { return 0; }
  int read() { return -1; }
};
extern SimTwoWire Wire;
extern SimTwoWire Wire1;
extern SimTwoWire Wire2;
#endif
