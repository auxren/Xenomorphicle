// The host side of the simulator. See sim_host.h -- in particular the
// determinism rules, which this file is responsible for keeping.

#include "sim_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <EEPROM.h>
#include <FS.h>
#include <LittleFS.h>
#include <MIDI.h>
#include <SD.h>
#include <SPI.h>
#include <USBHost_t36.h>
#include <Wire.h>

// --- clock -----------------------------------------------------------------
// One 64-bit microsecond counter. The millisecond and microsecond views are
// both derived from it, so they can never disagree -- a long-press timed in UI
// ticks and a hold timed in millis() are reading the same instant.
namespace {
uint64_t g_now_us = 0;
}

uint32_t SimNowMs() { return (uint32_t)(g_now_us / 1000u); }
uint32_t SimNowUs() { return (uint32_t)g_now_us; }
void SimAdvanceMs(uint32_t dt) { g_now_us += (uint64_t)dt * 1000u; }
void SimAdvanceUs(uint32_t dt) { g_now_us += dt; }

// --- pins ------------------------------------------------------------------
namespace {
uint8_t g_pins[kSimPinCount];
IMXRT_GPIO_t g_gpio_port;
uint16_t g_cv_raw[8];
float g_id_voltage = 0.10f;   // see SimSetIdVoltage
}

uint8_t *SimPinLevels() { return g_pins; }
IMXRT_GPIO_t *SimGpioPort() { return &g_gpio_port; }
uint16_t *SimCvRaw() { return g_cv_raw; }
float SimIdVoltage() { return g_id_voltage; }
void SimSetIdVoltage(float v) { g_id_voltage = v; }

// --- i.MXRT register stand-ins ---------------------------------------------
uint32_t SIM_ARM_DWT_CYCCNT = 0;
uint32_t SIM_ARM_DWT_CTRL = 0;
uint32_t SIM_ARM_DEMCR = 0;
uint32_t SIM_LPSPI4_TDR = 0;
uint32_t SIM_LPSPI4_TCR = 0;
uint32_t SIM_LPSPI4_SR = 0;
uint32_t SIM_IOMUX[8] = {0};

// --- peripherals -----------------------------------------------------------
SimSerial Serial;
SimSPI SPI;
SimSPI SPI1;
SimTwoWire Wire;
SimTwoWire Wire1;
SimTwoWire Wire2;
SimEEPROMClass EEPROM;
SimSDClass SD;

namespace {
// Set once from the command line before boot. NOT cleared by SimHostReset():
// whether a card is seated is a property of the bench, not of the run, and a
// replay that re-boots must find the same machine it recorded on.
bool g_card_present = false;
}
void SimSetCardPresent(bool present) { g_card_present = present; }
bool SimCardPresent() { return g_card_present; }

namespace {
uint8_t g_eeprom[E2END + 1];
}
uint8_t *SimEepromBytes() { return g_eeprom; }
size_t SimEepromSize() { return sizeof(g_eeprom); }

// --- MIDI ports ------------------------------------------------------------
SimUsbMidi usbMIDI;
MIDIDevice_BigBuffer usbHostMIDI[2];
midi::MidiInterface<midi::SerialMIDI<HardwareSerial> > MIDI1;

void SimMidiPortBase::Push(uint8_t type, uint8_t d1, uint8_t d2, uint8_t ch) {
  const int next = (head_ + 1) % kCap;
  if (next == tail_) return;   // full: drop, exactly as a real ring would
  q_[head_][0] = type;
  q_[head_][1] = d1;
  q_[head_][2] = d2;
  q_[head_][3] = ch;
  head_ = next;
}

bool SimMidiPortBase::read() {
  if (tail_ == head_) return false;
  type_ = q_[tail_][0];
  d1_ = q_[tail_][1];
  d2_ = q_[tail_][2];
  ch_ = q_[tail_][3];
  tail_ = (tail_ + 1) % kCap;
  return true;
}

uint8_t SimMidiPortBase::getType() const { return type_; }
uint8_t SimMidiPortBase::getData1() const { return d1_; }
uint8_t SimMidiPortBase::getData2() const { return d2_; }
uint8_t SimMidiPortBase::getChannel() const { return ch_; }
uint8_t *SimMidiPortBase::getSysExArray() const { return nullptr; }
unsigned SimMidiPortBase::getSysExArrayLength() const { return 0; }

// Transmitted messages are counted, and the first few of each kind are logged.
// Past that they are silent, so a running sequencer cannot bury the log.
void SimMidiPortBase::note_tx(const char *what, int a, int b, int c) {
  ++tx_count_;
  if (tx_count_ <= 8)
    SimLog("MIDI TX %s: %s %d %d ch%d (goes nowhere)", name_, what, a, b, c);
  else if (tx_count_ == 9)
    SimLog("MIDI TX %s: ...further messages not logged", name_);
}

void SimMidiPortBase::sendNoteOn(uint8_t n, uint8_t v, uint8_t ch) { note_tx("note-on", n, v, ch); }
void SimMidiPortBase::sendNoteOff(uint8_t n, uint8_t v, uint8_t ch) { note_tx("note-off", n, v, ch); }
void SimMidiPortBase::sendControlChange(uint8_t cc, uint8_t v, uint8_t ch) { note_tx("cc", cc, v, ch); }
void SimMidiPortBase::sendProgramChange(uint8_t pc, uint8_t ch) { note_tx("pgm", pc, 0, ch); }
void SimMidiPortBase::sendAfterTouch(uint8_t v, uint8_t ch) { note_tx("aftertouch", v, 0, ch); }
void SimMidiPortBase::sendPitchBend(int b, uint8_t ch) { note_tx("bend", b, 0, ch); }
void SimMidiPortBase::sendRealTime(uint8_t t) { note_tx("realtime", t, 0, 0); }
void SimMidiPortBase::sendSysEx(unsigned len, const uint8_t *, bool) { note_tx("sysex", (int)len, 0, 0); }
void SimMidiPortBase::send(uint8_t t, uint8_t d1, uint8_t d2, uint8_t ch) { note_tx("raw", t, d1, d2 | (ch << 8)); }

// --- the panel -------------------------------------------------------------
void SimPanelVisible(uint8_t out[1024]) {
  const uint8_t *src = SimPanelBytes();
  if (SimPanelInverted())
    for (int i = 0; i < 1024; ++i) out[i] = (uint8_t)~src[i];
  else
    memcpy(out, src, 1024);
}

// --- deferred frame capture ------------------------------------------------
// See sim_host.h. Armed from the command line before boot, fired from the
// SH1106 shim's page-7 write so the captured frame is whole.
//
// NOT cleared by SimHostReset(): the arm is a property of the run, set before
// boot, and a reset that dropped it would silently disarm --snap-at.
namespace {
uint32_t g_snap_at = 0;
bool g_snap_armed = false;
bool g_snap_taken = false;
uint8_t g_snap[1024];
}

void SimSnapArm(uint32_t at_ms) {
  g_snap_at = at_ms;
  g_snap_armed = true;
  g_snap_taken = false;
}

void SimPanelFrameComplete() {
  if (!g_snap_armed || g_snap_taken || SimNowMs() < g_snap_at) return;
  SimPanelVisible(g_snap);
  g_snap_taken = true;
}

bool SimSnapTaken() { return g_snap_taken; }
const uint8_t *SimSnapBytes() { return g_snap; }

// --- log -------------------------------------------------------------------
namespace {
std::vector<std::string> g_log;
constexpr size_t kLogCap = 400;
}

void SimLog(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_log.push_back(buf);
  if (g_log.size() > kLogCap) g_log.erase(g_log.begin(), g_log.begin() + 64);
}

const std::vector<std::string> &SimLogLines() { return g_log; }
void SimLogClear() { g_log.clear(); }

// Everything the firmware prints goes to the log, not to stdout: stdout is the
// frame, or the --stdio protocol, or a --dump-fb capture.
size_t SimPrint::write(const uint8_t *b, size_t n) {
  static std::string pending;
  for (size_t i = 0; i < n; ++i) {
    const char c = (char)b[i];
    if (c == '\n') { SimLog("fw: %s", pending.c_str()); pending.clear(); }
    else if (c != '\r') pending += c;
  }
  return n;
}

size_t SimPrint::printf(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return 0;
  return write((const uint8_t *)buf, (size_t)n);
}

// --- lifecycle -------------------------------------------------------------
void SimHostReset() {
  g_now_us = 0;
  memset(g_pins, 1, sizeof(g_pins));      // every pin idles high
  memset(&g_gpio_port, 0, sizeof(g_gpio_port));
  memset(g_cv_raw, 0, sizeof(g_cv_raw));
  memset(g_eeprom, 0xff, sizeof(g_eeprom));
  g_log.clear();
  // Fixed seed: the splash screen picks an icon with random(), and a replay
  // that picked a different one would diff against the recording.
  srandom(1);
}
