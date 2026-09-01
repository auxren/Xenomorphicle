#ifndef XENOSIM_SIM_UI_H_
#define XENOSIM_SIM_UI_H_

#include <stdint.h>

#include <string>

#include "shim/fw/OC_ui.h"

class SimAppBase;

// ---------------------------------------------------------------------------
// The panel's button state machine and event dispatcher.
//
// This exists because the firmware dispatches on event.mask -- the set of
// buttons held at the moment one of them changed -- not on the one button that
// moved. Three gestures on this module are chords:
//
//   both encoder pushes        -> the 200e preset-bus overlay
//   A (or Z) + right encoder   -> the app menu
//   A + B                      -> screen flip, inside the Setup app
//
// A simulator that fires one fire-and-forget press per click can never build a
// mask with two bits in it, so none of those screens are reachable. So the
// simulator models held state: press and release are separate, OC::Ui::Poll()
// runs once per simulated millisecond and emits the same EVENT_BUTTON_DOWN /
// _PRESS / _LONG_PRESS / _LONG_RELEASE stream the hardware does, and
// ui.read_immediate() reports what is held -- which the preset overlay polls
// directly to time its STORE and RECALL holds.
//
// sim_ui.cpp is a deliberate mirror of software/src/OC_ui.cpp. Keep them in
// sync; the mirror names the lines it is following.
// ---------------------------------------------------------------------------

// Panel token ("a", "b", "x", "y", "l"/"encl", "r"/"encr", "z") -> control
// mask. Returns 0 for anything that is not a button.
uint16_t SimUiControlForToken(const std::string &tok);

// The short token for a control mask, for logs and the front end's held list.
const char *SimUiTokenForControl(uint16_t control);

// Currently-held buttons as "a+r", or "" when nothing is held.
std::string SimUiHeldTokens();
uint16_t SimUiHeld();

void SimUiSetButton(uint16_t control, bool down);
void SimUiReleaseAll();

// An encoder turn, queued with the currently-held mask attached exactly as
// OC::Ui::Poll() attaches it.
void SimUiEncoder(uint16_t control, int delta);

// One simulated millisecond of the ISR half (OC::Ui::Poll).
void SimUiPoll();

// The loop half (OC::Ui::DispatchEvents): drains the queue, applies the global
// chords, and hands what is left to the overlay or the app.
OC::UiMode SimUiDispatch(SimAppBase *app);

// The mode the last dispatch settled on. The simulator only ever draws the app
// or the preset overlay for real; UI_MODE_APP_SETTINGS and UI_MODE_SCREENSAVER
// get a stand-in screen, because the app menu needs an app registry this
// simulator does not have.
OC::UiMode SimUiMode();
void SimUiSetMode(OC::UiMode mode);

#endif  // XENOSIM_SIM_UI_H_
