// The panel's button state machine and event dispatcher. See sim_ui.h.
//
// Everything below follows software/src/OC_ui.cpp. Where it does, the comment
// says which function it is mirroring, so the two can be diffed by eye.

#include "sim_ui.h"

#include "shim/oc_shim.h"

#include "PresetBusUI.h"     // the real overlay; shim/fw/ compiles its .cpp
#include "sim_bus.h"
#include "sim_preset.h"

extern uint_fast8_t MENU_REDRAW;

namespace OC {

Ui ui;

void Ui::Init() {
  ticks_ = 0;
  physical_ = 0;
  button_state_ = 0;
  button_ignore_mask_ = 0;
  for (size_t i = 0; i < CONTROL_BUTTON_LAST; ++i) button_press_time_[i] = 0;
  head_ = tail_ = 0;
  screensaver_ = false;
  jump_to_menu_ = false;
}

void Ui::PushEvent(UI::EventType t, uint16_t c, int16_t v, uint16_t m) {
  const size_t next = (head_ + 1) % kEventQueueDepth;
  if (next == tail_) return;      // full: drop, as the real ring does
  queue_[head_].type = (uint8_t)t;
  queue_[head_].control = c;
  queue_[head_].value = v;
  queue_[head_].mask = m;
  head_ = next;
}

UI::Event Ui::Pull() {
  const Queued &q = queue_[tail_];
  tail_ = (tail_ + 1) % kEventQueueDepth;
  return UI::Event((UI::EventType)q.type, q.control, q.value, q.mask);
}

// OC_ui.h's IgnoreEvent(), verbatim in behaviour.
bool Ui::IgnoreEvent(const UI::Event &event) {
  bool ignore = false;
  if (button_ignore_mask_ & event.control) {
    button_ignore_mask_ = (uint16_t)(button_ignore_mask_ & ~event.control);
    ignore = true;
  } else if (screensaver_) {
    screensaver_ = false;
    SetButtonIgnoreMask();
    ignore = true;
  }
  return ignore;
}

void Ui::SetButton(uint16_t control, bool down) {
  if (down) physical_ = (uint16_t)(physical_ | control);
  else physical_ = (uint16_t)(physical_ & ~control);
}

void Ui::ReleaseAll() { physical_ = 0; }

// OC::Ui::Poll(), the ISR half. On target this is the 1kHz timer; here it is
// called once per simulated millisecond from the simulator's Tick(), so
// kLongPressTicks (500) is 500 simulated ms, the same as on the module.
//
// The only difference from OC_ui.cpp is where the level comes from: there it
// is UI::Button's debounced pin read, here it is physical_, set by the front
// end. The edge detection, the press timing and -- crucially -- the mask
// attached to every event are the firmware's.
void Ui::Poll() {
  const uint32_t now = ++ticks_;

  // The ORN8 profile in OC_gpio.cpp gives every button a pin (but_mid = 20),
  // so OC_ui.cpp's `(but_mid == 0xFF) ? 4 : CONTROL_BUTTON_LAST` picks all
  // seven here. Only six are on the front end; Z exists so the Z chords are
  // dispatched by the same code path they are on the module.
  const uint16_t button_state =
      (uint16_t)(physical_ & ((1 << CONTROL_BUTTON_LAST) - 1));

  for (size_t i = 0; i < CONTROL_BUTTON_LAST; ++i) {
    const uint16_t m = control_mask(i);
    const bool is_down = (button_state & m) != 0;
    const bool was_down = (button_state_ & m) != 0;

    if (is_down && !was_down) {                 // just_pressed()
      button_press_time_[i] = now;
      PushEvent(UI::EVENT_BUTTON_DOWN, m, 0, button_state);
    } else if (!is_down && was_down) {          // released()
      if (now - button_press_time_[i] < kLongPressTicks)
        PushEvent(UI::EVENT_BUTTON_PRESS, m, 0, button_state);
      else
        PushEvent(UI::EVENT_BUTTON_LONG_RELEASE, m, 0, button_state);
      button_press_time_[i] = 0;
    } else if (is_down && (now - button_press_time_[i] == kLongPressTicks)) {
      PushEvent(UI::EVENT_BUTTON_LONG_PRESS, m, 0, button_state);
    }
  }

  button_state_ = button_state;
}

}  // namespace OC

// ---------------------------------------------------------------------------

namespace {

struct TokenMap { const char *tok; uint16_t control; };

// The panel, in the order the held list reads out.
const TokenMap kButtons[] = {
  { "a", OC::CONTROL_BUTTON_A },
  { "b", OC::CONTROL_BUTTON_B },
  { "x", OC::CONTROL_BUTTON_X },
  { "y", OC::CONTROL_BUTTON_Y },
  { "z", OC::CONTROL_BUTTON_Z },
  { "l", OC::CONTROL_BUTTON_L },
  { "r", OC::CONTROL_BUTTON_R },
};

OC::UiMode g_mode = OC::UI_MODE_MENU;

}  // namespace

uint16_t SimUiControlForToken(const std::string &tok) {
  for (const auto &e : kButtons)
    if (tok == e.tok) return e.control;
  if (tok == "encl") return OC::CONTROL_BUTTON_L;
  if (tok == "encr") return OC::CONTROL_BUTTON_R;
  return 0;
}

const char *SimUiTokenForControl(uint16_t control) {
  for (const auto &e : kButtons)
    if (control == e.control) return e.tok;
  if (control == OC::CONTROL_ENCODER_L) return "encL";
  if (control == OC::CONTROL_ENCODER_R) return "encR";
  return "?";
}

uint16_t SimUiHeld() { return OC::ui.physical(); }

std::string SimUiHeldTokens() {
  std::string out;
  for (const auto &e : kButtons) {
    if (!(OC::ui.physical() & e.control)) continue;
    if (!out.empty()) out += "+";
    out += e.tok;
  }
  return out;
}

void SimUiSetButton(uint16_t control, bool down) {
  OC::ui.SetButton(control, down);
}

void SimUiReleaseAll() { OC::ui.ReleaseAll(); }

void SimUiEncoder(uint16_t control, int delta) {
  if (!delta) return;
  if (delta > 64) delta = 64;
  if (delta < -64) delta = -64;
  // OC::Ui::Poll() pushes encoder events with the same button_state mask it
  // gives the button events, which is how "hold A and turn" reaches an app.
  OC::ui.PushEvent(UI::EVENT_ENCODER, control, (int16_t)delta,
                   OC::ui.button_state());
}

void SimUiPoll() { OC::ui.Poll(); }

OC::UiMode SimUiMode() { return g_mode; }
void SimUiSetMode(OC::UiMode mode) { g_mode = mode; }

// OC::Ui::DispatchEvents(), the loop half. Mirrored branch for branch; the
// only substitutions are the two things this simulator has no registry for
// (the app menu and the per-app IO settings screen), which log instead.
OC::UiMode SimUiDispatch(SimAppBase *app) {
  using namespace OC;
  if (!app) return UI_MODE_APP_SETTINGS;

  while (ui.available()) {
    const UI::Event event = ui.Pull();
    if (ui.screensaver() && UI::EVENT_BUTTON_LONG_RELEASE == event.type)
      continue;
    if (ui.IgnoreEvent(event))
      continue;

    MENU_REDRAW = 1;

    // On the module UI_MODE_APP_SETTINGS is not a value that gets
    // re-evaluated next pass: Main.cpp hands control to OC::Ui::AppSettings(),
    // which blocks in its own loop over the app registry until you leave. This
    // simulator runs one app and has no registry, so it holds the mode itself
    // and shows a stand-in screen until any press dismisses it.
    if (g_mode == UI_MODE_APP_SETTINGS) {
      if (UI::EVENT_BUTTON_PRESS == event.type) g_mode = UI_MODE_MENU;
      continue;
    }

    // 200e preset-bus overlay: it owns all input while open, and holding BOTH
    // encoder buttons opens it.
    if (OC::PresetBusUI::Active()) {
      if (OC::PresetBusUI::HandleEvent(event)) continue;
    }
    if (UI::EVENT_BUTTON_DOWN == event.type &&
        (CONTROL_BUTTON_L == event.control ||
         CONTROL_BUTTON_R == event.control) &&
        (event.mask & (CONTROL_BUTTON_L | CONTROL_BUTTON_R))
            == (CONTROL_BUTTON_L | CONTROL_BUTTON_R)) {
      OC::PresetBusUI::Enter();
      ui.SetButtonIgnoreMask();  // swallow the releases
      SimLog("chord encL+encR -> PresetBusUI::Enter() (mask %03x)",
             (unsigned)event.mask);
      continue;
    }

    const bool z_hold = (event.mask & CONTROL_BUTTON_Z);
    const bool a_hold = (event.mask & CONTROL_BUTTON_A);

    if (UI::EVENT_BUTTON_DOWN == event.type) {
      // Hold Z or A and push right encoder for main menu
      if (CONTROL_BUTTON_R == event.control && (z_hold || a_hold)) {
        ui.set_jump_to_menu(true);
        SimLog("chord %s+encR -> app menu (mask %03x)",
               a_hold ? "A" : "Z", (unsigned)event.mask);
        break;
      }
      // Hold Z or A and push left encoder for IO settings menu
      if (CONTROL_BUTTON_L == event.control && (z_hold || a_hold)) {
        SimLog("chord %s+encL -> AppBase::EditIOSettings(): the simulator has "
               "no IO settings screen, so nothing opens",
               a_hold ? "A" : "Z");
        ui.SetButtonIgnoreMask();
        continue;
      }
      // Hold Z and push A for screensaver
      if (CONTROL_BUTTON_A == event.control && z_hold) {
        ui.set_screensaver(true);
        ui.SetButtonIgnoreMask();
        SimLog("chord Z+A -> screensaver");
        break;
      }
    }

    // AppBase::DispatchEvent(): encoders one way, buttons the other.
    if (event.IsEncoder()) app->HandleEncoderEvent(event);
    else app->HandleButtonEvent(event);
  }

  // The real DispatchEvents also blanks on an idle timeout. The simulator
  // deliberately never does: an idle browser tab is not an idle module, and a
  // screensaver nobody asked for would look like a crash.
  if (ui.screensaver()) {
    g_mode = UI_MODE_SCREENSAVER;
  } else if (ui.jump_to_menu()) {
    ui.SetButtonIgnoreMask();   // ignore release
    ui.set_jump_to_menu(false);
    g_mode = UI_MODE_APP_SETTINGS;
  } else if (g_mode != UI_MODE_APP_SETTINGS) {
    g_mode = UI_MODE_MENU;
  }
  return g_mode;
}
