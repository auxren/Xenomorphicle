#ifndef XENOSIM_SIM_HOST_H_
#define XENOSIM_SIM_HOST_H_
// ---------------------------------------------------------------------------
// The host side of the simulator: the virtual clock, the pins, the log, and
// the storage/peripheral globals the Teensyduino stand-ins in shim/arduino/
// declare.
//
// DETERMINISM IS A HARD REQUIREMENT HERE. Replay is only worth having if the
// same session produces the same frames, so:
//
//   * SimNowMs()/SimNowUs() are the ONLY time source any firmware code can
//     observe. Nothing in the compiled firmware calls a wall clock: millis(),
//     micros(), delay(), elapsedMillis and elapsedMicros all read this counter
//     (shim/arduino/Arduino.h), and it only ever moves when the simulator
//     advances it.
//   * random() is seeded to a fixed value at startup.
//   * All state is either zero-initialised statics or explicitly reset in
//     SimHostReset().
//
// Wall-clock time is read in exactly one place -- the interactive terminal
// loop's pacing, in main -- and never by anything the firmware can see.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <string>
#include <vector>

// --- clock -----------------------------------------------------------------
uint32_t SimNowMs();
uint32_t SimNowUs();
void SimAdvanceMs(uint32_t dt);
void SimAdvanceUs(uint32_t dt);

// --- pins ------------------------------------------------------------------
uint8_t *SimPinLevels();

// --- CV / hardware identity ------------------------------------------------
uint16_t *SimCvRaw();
float SimIdVoltage();
void SimSetIdVoltage(float v);

// --- the panel -------------------------------------------------------------
// The bytes the OLED was last sent, page-packed exactly as the SH1106 driver
// hands them over: bit (y & 7) of buf[(y >> 3) * 128 + x].
const uint8_t *SimPanelBytes();
bool SimPanelInverted();

// The panel as the eye sees it: the same bytes with the panel's invert command
// applied. This is what the front end draws and what --dump-fb emits.
void SimPanelVisible(uint8_t out[1024]);

// --- log -------------------------------------------------------------------
void SimLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const std::vector<std::string> &SimLogLines();
void SimLogClear();

// --- lifecycle -------------------------------------------------------------
// Zeroes everything the host layer owns and reseeds the RNG. Called before
// boot, and again before a replay, so a replay never inherits a previous run.
void SimHostReset();

#endif  // XENOSIM_SIM_HOST_H_
