#ifndef XENOSIM_USBHOST_T36_H_
#define XENOSIM_USBHOST_T36_H_
// Stand-in for the USB host stack. There is no host stack here: the two
// "devices" are FIFOs (see MIDI.h). Nothing in the simulator says anything
// about USB host enumeration, which is the actual open question about the
// k-board -- see the README.
#include <MIDI.h>

class USBHost {
public:
  void begin() {}
  void Task() {}
};

class MIDIDevice_BigBuffer : public SimMidiPortBase {
public:
  MIDIDevice_BigBuffer() {}
  MIDIDevice_BigBuffer(USBHost &) {}
  operator bool() const { return true; }
  const uint8_t *manufacturer() { return (const uint8_t *)""; }
  const uint8_t *product() { return (const uint8_t *)""; }
  const uint8_t *serialNumber() { return (const uint8_t *)""; }
  // No device is ever attached, so both IDs stay 0 and every device-identity
  // screen reads "none". USB host enumeration is the one thing this simulator
  // is least able to say anything about -- see README.md.
  uint16_t idVendor() { return 0; }
  uint16_t idProduct() { return 0; }
};

typedef MIDIDevice_BigBuffer MIDIDevice;

#endif  // XENOSIM_USBHOST_T36_H_
