#ifndef OC_UI_H_
#define OC_UI_H_
// ---------------------------------------------------------------------------
// Host stand-in for software/src/OC_ui.h.
//
// The real header drags OC_config.h, ui_button.h, ui_encoder.h and the ADC in
// behind it; none of that survives a host build. What the simulator needs from
// it is the panel's control bitmasks and an OC::ui whose read_immediate() and
// Poke() the real PresetBusUI.cpp calls -- so this declares exactly that.
//
// OC::Ui below is NOT a stub: sim_ui.cpp implements Poll() and DispatchEvents()
// as a line-for-line mirror of OC_ui.cpp, because the mask semantics ARE the
// thing under test. Every chord on this panel (both encoders -> preset bus,
// A + encR -> app menu, A + B -> screen flip) is dispatched on event.mask, the
// set of buttons held at the moment one of them changed. Keep the two in sync;
// sim_ui.cpp names the lines it is mirroring.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "src/UI/ui_events.h"   // real UI::Event / UI::EventType

namespace OC {

// Copied verbatim from OC_ui.h's non-NORTHERNLIGHT_2OC_LEFTSIDE branch, which
// is what T41_console / T41_audio / T40 build. If the panel mapping ever
// changes, this must change with it.
enum UiControl : uint16_t {
  CONTROL_BUTTON_UP    = 1 << 0,
  CONTROL_BUTTON_DOWN  = 1 << 1,
  CONTROL_BUTTON_L     = 1 << 2,
  CONTROL_BUTTON_R     = 1 << 3,
  CONTROL_BUTTON_M     = 1 << 4,   // "Z": NOT wired on this hardware
  CONTROL_BUTTON_UP2   = 1 << 5,
  CONTROL_BUTTON_DOWN2 = 1 << 6,

  CONTROL_ENCODER_L    = 1 << 8,
  CONTROL_ENCODER_R    = 1 << 9,

  CONTROL_BUTTON_LAST  = 7,        // ARDUINO_TEENSY41

  CONTROL_BUTTON_A = CONTROL_BUTTON_UP,
  CONTROL_BUTTON_B = CONTROL_BUTTON_DOWN,
  CONTROL_BUTTON_X = CONTROL_BUTTON_UP2,
  CONTROL_BUTTON_Y = CONTROL_BUTTON_DOWN2,
  CONTROL_BUTTON_Z = CONTROL_BUTTON_M,
};

static inline uint16_t control_mask(unsigned i) {
  return (uint16_t)(1 << i);
}

enum UiMode {
  UI_MODE_SCREENSAVER,
  UI_MODE_MENU,
  UI_MODE_APP_SETTINGS,
  UI_MODE_CALIBRATE
};

class Ui {
public:
  static const size_t kEventQueueDepth = 16;
  static const uint32_t kLongPressTicks = 500;   // OC_ui.h's own value

  void Init();

  // One simulated millisecond of the real Poll(): edge-detects every button,
  // times the long press, and pushes events whose mask is the CURRENTLY HELD
  // set. Called once per simulated ms from the simulator's Tick().
  void Poll();

  void Poke() { screensaver_ = false; }

  // PresetBusUI polls this directly to time its STORE and RECALL holds, so it
  // has to reflect held state, not the last button that changed.
  bool read_immediate(UiControl control) const {
    return (button_state_ & control) != 0;
  }

  uint16_t button_state() const { return button_state_; }

  void SetButtonIgnoreMask() { button_ignore_mask_ = button_state_; }
  void IgnoreButton(UiControl control) {
    button_ignore_mask_ = (uint16_t)(button_ignore_mask_ | control);
  }

  bool screensaver() const { return screensaver_; }
  void set_screensaver(bool v) { screensaver_ = v; }
  bool jump_to_menu() const { return jump_to_menu_; }
  void set_jump_to_menu(bool v) { jump_to_menu_ = v; }

  // --- simulator-only: the physical panel ---------------------------------
  // The front end holds and releases buttons; Poll() turns that into the same
  // event stream the hardware's debounced UI::Button would.
  void SetButton(uint16_t control, bool down);
  void ReleaseAll();
  uint16_t physical() const { return physical_; }

  bool available() const { return head_ != tail_; }
  UI::Event Pull();
  void PushEvent(UI::EventType t, uint16_t c, int16_t v, uint16_t m);

  // OC_ui.h's IgnoreEvent(), which DispatchEvents applies to every event.
  bool IgnoreEvent(const UI::Event &event);

  uint32_t ticks() const { return ticks_; }

private:
  uint32_t ticks_ = 0;
  uint16_t physical_ = 0;        // what is held down right now
  uint16_t button_state_ = 0;    // last polled state (what read_immediate sees)
  uint16_t button_ignore_mask_ = 0;
  uint32_t button_press_time_[CONTROL_BUTTON_LAST] = {};
  bool screensaver_ = false;
  bool jump_to_menu_ = false;

  // Stored field-by-field rather than as UI::Event: Event's default
  // constructor leaves its members indeterminate, and an array of those is
  // exactly the shape GCC's -Wmaybe-uninitialized (which is -Werror here)
  // objects to. Pull() reassembles the real Event.
  struct Queued {
    uint8_t type = 0;
    uint16_t control = 0;
    int16_t value = 0;
    uint16_t mask = 0;
  };
  Queued queue_[kEventQueueDepth];
  size_t head_ = 0, tail_ = 0;
};

extern Ui ui;

}  // namespace OC

#endif  // OC_UI_H_
