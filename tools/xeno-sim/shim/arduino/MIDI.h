#ifndef XENOSIM_MIDI_H_
#define XENOSIM_MIDI_H_
// ---------------------------------------------------------------------------
// Stand-in for the Teensy MIDI library and usbMIDI.
//
// Every port is an independent FIFO. That is enough to prove which port an app
// polls and that its parser consumes what arrives -- the question the Captain
// MIDI and 200e Rec screens exist to answer -- and it is nothing at all like a
// USB device: no enumeration, no bandwidth, no jitter, no packet coalescing.
// Transmitted messages are counted and logged, then dropped.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

namespace midi {

enum MidiType : uint8_t {
  InvalidType           = 0x00,
  NoteOff               = 0x80,
  NoteOn                = 0x90,
  AfterTouchPoly        = 0xA0,
  ControlChange         = 0xB0,
  ProgramChange         = 0xC0,
  AfterTouchChannel     = 0xD0,
  PitchBend             = 0xE0,
  SystemExclusive       = 0xF0,
  TimeCodeQuarterFrame  = 0xF1,
  SongPosition          = 0xF2,
  SongSelect            = 0xF3,
  TuneRequest           = 0xF6,
  Clock                 = 0xF8,
  Start                 = 0xFA,
  Continue              = 0xFB,
  Stop                  = 0xFC,
  ActiveSensing         = 0xFE,
  SystemReset           = 0xFF,
};

}  // namespace midi

// Named by HSMIDI.h in MIDI1's declared type; never instantiated here.
class HardwareSerial;

// One port. See SimMidiPortImpl in sim_midi.cpp for the queue itself.
class SimMidiPortBase {
public:
  // Teensyduino's usbMIDI carries the message numbers as members; the
  // firmware writes usbMIDI.Start rather than midi::Start in places.
  static constexpr uint8_t NoteOff = midi::NoteOff;
  static constexpr uint8_t NoteOn = midi::NoteOn;
  static constexpr uint8_t AfterTouchPoly = midi::AfterTouchPoly;
  static constexpr uint8_t ControlChange = midi::ControlChange;
  static constexpr uint8_t ProgramChange = midi::ProgramChange;
  static constexpr uint8_t AfterTouchChannel = midi::AfterTouchChannel;
  static constexpr uint8_t PitchBend = midi::PitchBend;
  static constexpr uint8_t SystemExclusive = midi::SystemExclusive;
  static constexpr uint8_t Clock = midi::Clock;
  static constexpr uint8_t Start = midi::Start;
  static constexpr uint8_t Continue = midi::Continue;
  static constexpr uint8_t Stop = midi::Stop;
  static constexpr uint8_t SystemReset = midi::SystemReset;

  bool read();
  uint8_t getType() const;
  uint8_t getData1() const;
  uint8_t getData2() const;
  uint8_t getChannel() const;
  uint8_t *getSysExArray() const;
  unsigned getSysExArrayLength() const;

  void sendNoteOn(uint8_t note, uint8_t vel, uint8_t ch);
  void sendNoteOff(uint8_t note, uint8_t vel, uint8_t ch);
  void sendControlChange(uint8_t cc, uint8_t val, uint8_t ch);
  void sendProgramChange(uint8_t pc, uint8_t ch);
  void sendAfterTouch(uint8_t val, uint8_t ch);
  void sendPitchBend(int bend, uint8_t ch);
  void sendRealTime(uint8_t type);
  void sendSysEx(unsigned length, const uint8_t *data, bool hdr = false);
  void send(uint8_t type, uint8_t d1, uint8_t d2, uint8_t ch);
  void send_now() {}
  void begin(int = 1) {}
  void turnThruOn() {}
  void turnThruOff() {}
  void setHandleSystemExclusive(void (*)(uint8_t *, unsigned)) {}

  // simulator side
  void Push(uint8_t type, uint8_t d1, uint8_t d2, uint8_t ch = 1);
  const char *name() const { return name_; }
  void set_name(const char *n) { name_ = n; }
  unsigned tx_count() const { return tx_count_; }

protected:
  static constexpr int kCap = 64;
  uint8_t q_[kCap][4] = {};
  int head_ = 0, tail_ = 0;
  uint8_t type_ = 0, d1_ = 0, d2_ = 0, ch_ = 1;
  unsigned tx_count_ = 0;
  const char *name_ = "?";
  void note_tx(const char *what, int a, int b, int c);
};

class SimUsbMidi : public SimMidiPortBase {};
extern SimUsbMidi usbMIDI;

namespace midi {
// HSMIDI.h declares the DIN port as midi::MidiInterface<midi::SerialMIDI<...>>.
// Both are FIFOs here like every other port.
template <typename Transport> class SerialMIDI {};
template <typename T> class MidiInterface : public SimMidiPortBase {};
}  // namespace midi

#endif  // XENOSIM_MIDI_H_
