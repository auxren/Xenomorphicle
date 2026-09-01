// ---------------------------------------------------------------------------
// The last few firmware symbols with nowhere else to come from.
//
// Each one is defined in a file the simulator cannot compile -- Main.cpp (USB,
// audio, MTP, the crash handler, a 400-line serial console),
// HemisphereApplet.cpp (which does not build under a compiler as strict about
// const member calls as this one), applets/_config.h (Hemisphere, which this
// build excludes) -- and each is either inert here or unreachable.
//
// This is the file to check when something in the simulator behaves as though
// a global were missing: it probably is, and this is where it is faked.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <Arduino.h>

#include "HSClockManager.h"
#include "HSIOFrame.h"
#include "src/drivers/ADC/OC_util_ADC.h"

// --- HemisphereApplet.cpp:6-7 ----------------------------------------------
// The IO frame and the clock manager are shared state, not applet code, and
// the apps this build carries do use them: HS::frame is what an app reads its
// inputs and MIDI state through, and clock_m is the internal clock. Real
// classes, real behaviour -- only their home is different.
namespace HS {
IOFrame frame;
ClockManager clock_m;
}

// --- applets/_config.h:258-280 ---------------------------------------------
// The applet registry. NO_HEMISPHERE means there are no applets, so nothing
// reaches these: HSUtils.cpp's DrawAppletList is the only caller and the
// screen that opens it is not in this build. If one is ever hit, the empty
// list it reports is the honest answer.
namespace HS {
bool applet_is_hidden(const int &) { return true; }
const char *get_applet_name(const int) { return "-"; }
const uint8_t *get_applet_icon(const int) { return nullptr; }
}

// --- Main.cpp:315, 345 ------------------------------------------------------
// The watchdog and the DTCM stack-paint high-water mark. There is no watchdog
// here (a wedged loop just wedges) and no DTCM to run out of, so the low-water
// figure the preset engine reports after a save is a fixed number and means
// nothing.
uint32_t stack_low_water() { return 16384; }
void watchdog_feed() {}

// --- Teensyduino runtime ----------------------------------------------------
// Linker-provided memory-map symbols and the bootloader reboot vector. Nothing
// in the simulator reboots, and the free-RAM figures are answered by
// shim/src/OC_core.cpp instead.
extern "C" {
char _heap_end[1] = {0};
char *__brkval = _heap_end;
char _ebss[1] = {0};
char _extram_start[1] = {0};
uint32_t external_psram_size = 0;
void _reboot_Teensyduino_() {}
}

