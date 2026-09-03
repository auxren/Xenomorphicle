#ifndef XENOSIM_EEPROM_H_
#define XENOSIM_EEPROM_H_
// RAM-backed EEPROM, with the EERef/EEPtr pair Teensyduino's EEPROM.h defines
// (EEPROMStorage.h walks the address space through EEPtr). It persists for the
// life of the process and no longer: nothing the simulator "saves" survives a
// restart, so a reviewer must not read a working Save screen here as evidence
// that settings come back after a power cycle.
#include <stddef.h>
#include <stdint.h>

// T4.1's emulated EEPROM: 4284 bytes.
#define E2END 4283

uint8_t *SimEepromBytes();
size_t SimEepromSize();

struct EERef {
  EERef(int idx) : index(idx) {}
  operator uint8_t() const {
    return (index >= 0 && (size_t)index < SimEepromSize()) ? SimEepromBytes()[index] : 0;
  }
  EERef &operator=(uint8_t v) {
    if (index >= 0 && (size_t)index < SimEepromSize()) SimEepromBytes()[index] = v;
    return *this;
  }
  EERef &update(uint8_t v) { return *this = v; }
  int index;
};

struct EEPtr {
  EEPtr(int idx = 0) : index(idx) {}
  operator int() const { return index; }
  EERef operator*() const { return EERef(index); }
  EEPtr &operator++() { ++index; return *this; }
  EEPtr operator++(int) { EEPtr t(*this); ++index; return t; }
  int index;
};

class SimEEPROMClass {
public:
  uint8_t read(int addr) { return EERef(addr); }
  void write(int addr, uint8_t v) { EERef{addr} = v; }
  void update(int addr, uint8_t v) { EERef{addr} = v; }
  EERef operator[](int addr) { return EERef(addr); }
  size_t length() const { return SimEepromSize(); }
};

extern SimEEPROMClass EEPROM;

#endif
