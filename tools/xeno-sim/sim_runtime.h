#ifndef XENOSIM_SIM_RUNTIME_H_
#define XENOSIM_SIM_RUNTIME_H_
// ---------------------------------------------------------------------------
// The firmware's boot and main loop, on the host.
//
// software/src/Main.cpp cannot be compiled here -- it is USB descriptors, the
// audio graph, MTP, the watchdog, the crash handler and a 400-line serial
// console, none of which exist on a host. So sim_runtime.cpp mirrors the two
// functions from it that decide what you see: setup()'s init order and
// loop()'s redraw/dispatch order. It is a deliberate transcription and it
// names the Main.cpp line ranges it follows; if loop() changes, this must.
//
// EVERYTHING IT CALLS IS THE REAL THING. Ui::Poll, Ui::DispatchEvents,
// Ui::AppSettings, AppBase::Draw, PresetBusUI, the app switcher, the display
// pipeline and every app are compiled from software/src/. This file is the
// scheduler around them, and the scheduler is the one part of the running
// system that is not the firmware's own.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <string>

// Boot: everything from setup() that has a meaning off-chip, in the order
// Main.cpp does it. `reset_settings` forces the first-run path -- it holds A+B
// through the splash, which is the module's own gesture.
//
// `answer_cancel` decides how the ConfirmReset prompt that gesture opens is
// answered: false is OK (erase), true is CANCEL. Both are real button presses
// scheduled into the firmware's own blocking prompt; nothing reaches inside it.
// CANCEL is not a curiosity -- "a reset the user backed out of leaves storage
// exactly as it was" is a property nothing else in the simulator can ask.
void SimRuntimeBoot(bool reset_settings, bool answer_cancel = false);

// One simulated millisecond: 17 core ISR passes (16.6 kHz), one UI poll
// (1 kHz), then TWO passes of loop() -- hardware runs loop() hundreds of times
// per millisecond, and a timer stamped in one pass and re-checked in the same
// millisecond by the next is a bug class the simulator must be able to see
// (see SimRuntimeTickMs). Returns after advancing the clock by 1 ms.
void SimRuntimeTickMs();

// Advance n simulated milliseconds.
void SimRuntimeAdvanceMs(uint32_t n);

// What the UI is showing, as a short word: "app", "menu", "preset", "saver",
// "cal". For the status line and the snapshot.
const char *SimRuntimeScreen();

// The current app's name, from the real app container.
const char *SimRuntimeAppName();

// A one-line summary of the instrument: screen, app, held buttons, clock.
std::string SimRuntimeStatusLine();

// The app container (TU-local to OC_apps.cpp; see shim/src/apps/_config.h).
namespace OC {
size_t SimAppCount();
const char *SimAppNameAt(size_t i);
uint16_t SimAppIdAt(size_t i);
}

// Force a redraw on the next loop pass, as the firmware's MENU_REDRAW does.
void SimRuntimePoke();

#endif  // XENOSIM_SIM_RUNTIME_H_
