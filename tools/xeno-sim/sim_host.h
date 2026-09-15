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
bool SimPanelDisplayOn();

// The panel as the eye sees it: the same bytes with the panel's invert command
// applied. This is what the front end draws and what --dump-fb emits.
void SimPanelVisible(uint8_t out[1024]);

// --- deferred frame capture ------------------------------------------------
// Capture the first COMPLETE frame drawn at or after `at_ms`, and keep it.
//
// This is the only way to see a screen the firmware draws from inside its own
// blocking loop -- Ui::DebugStats(), ConfirmReset(), the calibration wizard.
// Those never return to the simulator's driver, so by the time --dump-fb runs,
// whatever was on screen has been redrawn over: the app menu repaints as soon
// as DebugStats exits, and the frame the page actually showed is gone. The
// scheduled-tap trick (`<button>-inN`) can get a press INTO such a loop; this
// is the same idea pointed the other way, and it is what makes those pages'
// layout checkable at all.
//
// "Complete" is load-bearing. The panel is filled one page of eight at a time,
// so g_panel between page 0 and page 7 is a blend of two frames. The capture
// therefore fires from the page-7 write, not from a timer, so it can never
// return a torn picture -- see SendPage() in the SH1106 shim.
void SimSnapArm(uint32_t at_ms);
void SimPanelFrameComplete();
bool SimSnapTaken();
const uint8_t *SimSnapBytes();   // 1024 bytes, as SimPanelVisible emits them

// --- log -------------------------------------------------------------------
void SimLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const std::vector<std::string> &SimLogLines();
void SimLogClear();

// --- lifecycle -------------------------------------------------------------
// Zeroes everything the host layer owns and reseeds the RNG. Called before
// boot, and again before a replay, so a replay never inherits a previous run.
void SimHostReset();

#endif  // XENOSIM_SIM_HOST_H_
