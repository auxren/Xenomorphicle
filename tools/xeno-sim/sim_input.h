#ifndef XENOSIM_SIM_INPUT_H_
#define XENOSIM_SIM_INPUT_H_
// ---------------------------------------------------------------------------
// The panel, as pins.
//
// The simulator does NOT synthesise UI events. It drives the seven button pins
// and the four encoder quadrature pins, and the firmware's own OC_ui.cpp reads
// them: UI::Button's 8-bit debounce shift register, UI::Encoder's quadrature
// decoder and its acceleration state, Ui::Poll's DOWN / PRESS / LONG_PRESS /
// LONG_RELEASE sequencing and its kLongPressTicks threshold are all the
// firmware's, unmodified, running on the simulated clock.
//
// That matters for three things a reviewer will notice immediately:
//   * a press has to survive ~7 UI ticks of debounce before it is a press;
//   * a long press is kLongPressTicks (500) UI ticks, counted in UI ticks and
//     not in wall-clock milliseconds;
//   * encoder acceleration is real -- a fast turn moves further per detent,
//     because the real decoder's acceleration accumulator is being fed real
//     detents at a real rate.
//
// A detent is four pin phases, one per UI tick, which is a brisk-but-possible
// 4 ms per detent. Queue several and they arrive back to back, which is what
// makes acceleration build; a single detent never accelerates. So the fastest
// turn the simulator can express is at the fast end of what a hand can do, and
// nothing slower than 4 ms/detent is reachable through the multi-detent path.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <string>

// Binds the control masks to the pin numbers OC::Pinout_Detect() chose. Call
// after Pinout_Detect and before the first tick.
void SimInputInit();

// One UI tick of pin state: applies the next queued encoder phase.
void SimInputTick();

// Hold or release a button. The control is an OC::UiControl mask.
void SimInputSetButton(uint16_t control, bool down);
void SimInputReleaseAll();

// Queue |delta| detents on an encoder, four phases each.
void SimInputEncoder(bool right, int delta);

// True while encoder phases are still queued -- a click sent now would land
// mid-detent, so scripted mode waits this out.
bool SimInputEncoderBusy();

// Panel token ("a" "b" "x" "y" "l" "r" "z") -> control mask, and back.
uint16_t SimInputControlForToken(const std::string &tok);
const char *SimInputTokenForControl(uint16_t control);

// What is held, as "a+r", or "" for nothing.
uint16_t SimInputHeld();
std::string SimInputHeldTokens();

// A press scheduled for a future moment on the simulated clock. The boot
// sequence needs it: AppSwitcher::Init() opens a blocking "reset settings?"
// prompt on a virgin config and will sit there until a button is pressed, so
// the simulator answers it the way a person would rather than reaching inside
// the firmware to skip it. See SimRuntimeBoot.
void SimInputScheduleTap(uint16_t control, uint32_t at_ms, uint32_t dur_ms);

// The 225e last/next pulse jacks. Level, not edge: PresetBusUI::Task does its
// own edge detection, which is the code under test.
void SimInputSetTrigger(int index /*0-3*/, bool high);

#endif  // XENOSIM_SIM_INPUT_H_
