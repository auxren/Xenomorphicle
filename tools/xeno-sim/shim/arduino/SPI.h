#ifndef XENOSIM_SPI_H_
#define XENOSIM_SPI_H_
// Inert SPI: the simulator has no DAC and no display bus. Transfers are
// accepted and dropped.
#include <Arduino.h>
#include <stdint.h>
class SPISettings {
public:
  SPISettings() {}
  SPISettings(uint32_t, uint8_t, uint8_t) {}
};
class SimSPI {
public:
  void begin() {}
  void end() {}
  void beginTransaction(const SPISettings &) {}
  void endTransaction() {}
  uint8_t transfer(uint8_t) { return 0; }
  uint16_t transfer16(uint16_t) { return 0; }
  void transfer(const void *, void *, size_t) {}
  void setMOSI(uint8_t) {} void setMISO(uint8_t) {} void setSCK(uint8_t) {}
};
extern SimSPI SPI;
extern SimSPI SPI1;
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3
#endif
