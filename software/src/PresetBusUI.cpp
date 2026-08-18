// 225e-style front-panel preset manager for the 200e bus. See PresetBusUI.h.
#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)

#include <Arduino.h>

#include "PresetBusUI.h"
#include "PresetBus.h"
#include "PresetEngine.h"
#include "OC_ui.h"
#include "OC_menus.h"
#include "OC_digital_inputs.h"
#include "PhzConfig.h"
#include "HSUtils.h"

extern uint_fast8_t MENU_REDRAW;

namespace OC {
namespace PresetBusUI {

// persistence (PRESETBUS_KEY namespace in GLOBALS.CFG)
static constexpr uint16_t kNextTrigKey = (8 << 8) | 0x11;
static constexpr uint16_t kLastTrigKey = (8 << 8) | 0x12;

static bool active = false;
static uint8_t sel = 0;          // selected preset, 0-29 (shown 1-30)
static int8_t cursor = 0;        // 0 = preset, 1 = next-trig, 2 = last-trig
static uint8_t next_trig = 0;    // 0 = off, 1-4 = TR1-4
static uint8_t last_trig = 0;
static bool assign_dirty = false;

// stored/empty indicator cache (checked on selection change only)
static int8_t sel_stored = -1;

// trigger edge detection (runs whether or not the overlay is open)
static bool trig_prev[4] = {false, false, false, false};

bool Active() { return active; }

FLASHMEM void Init() {
  uint64_t v = 0;
  if (PhzConfig::getValue(kNextTrigKey, v) && v <= 4) next_trig = (uint8_t)v;
  v = 0;
  if (PhzConfig::getValue(kLastTrigKey, v) && v <= 4) last_trig = (uint8_t)v;
  const int last = PresetEngine::LastSlot();
  if (last >= 0) sel = (uint8_t)last;
}

FLASHMEM static void persist_assignments() {
  if (!assign_dirty) return;
  assign_dirty = false;
  // own the globals map for the write; any later writer reloads its own file
  PhzConfig::load_config();
  PhzConfig::setValue(kNextTrigKey, next_trig);
  PhzConfig::setValue(kLastTrigKey, last_trig);
  PhzConfig::save_config();
}

FLASHMEM void Enter() {
  const int last = PresetEngine::LastSlot();
  if (last >= 0) sel = (uint8_t)last;
  cursor = 0;
  sel_stored = -1;
  active = true;
}

FLASHMEM void Exit() {
  active = false;
  persist_assignments();
}

FLASHMEM static void recall_selected() {
  PresetBus::BroadcastRecall(sel);
  HS::PokePopup(HS::MESSAGE_POPUP, "Bus recall...");
}

FLASHMEM bool HandleEvent(const UI::Event &event) {
  if (!active) return false;

  if (event.control == CONTROL_BUTTON_UP || event.control == CONTROL_BUTTON_DOWN) {
    if (event.type == UI::EVENT_BUTTON_PRESS) Exit();
    return true;
  }

  if (event.control == CONTROL_ENCODER_L) {
    cursor = constrain(cursor + (event.value > 0 ? 1 : -1), 0, 2);
    return true;
  }
  if (event.control == CONTROL_ENCODER_R) {
    switch (cursor) {
      case 0:
        sel = (uint8_t)constrain((int)sel + event.value, 0, 29);
        sel_stored = -1;
        break;
      case 1:
        next_trig = (uint8_t)constrain((int)next_trig + event.value, 0, 4);
        assign_dirty = true;
        break;
      case 2:
        last_trig = (uint8_t)constrain((int)last_trig + event.value, 0, 4);
        assign_dirty = true;
        break;
    }
    return true;
  }
  if (event.control == CONTROL_BUTTON_R && event.type == UI::EVENT_BUTTON_PRESS) {
    recall_selected();
    return true;
  }
  if (event.control == CONTROL_BUTTON_L && event.type == UI::EVENT_BUTTON_LONG_PRESS) {
    PresetBus::BroadcastSave(sel);
    sel_stored = -1;  // re-check after the save lands
    HS::PokePopup(HS::MESSAGE_POPUP, "Bus store...");
    return true;
  }
  // swallow everything else (incl. the L short-press release) while active
  return true;
}

FLASHMEM void Draw() {
  graphics.setPrintPos(1, 2);
  graphics.print("Preset Bus");
  graphics.setPrintPos(92, 2);
  graphics.printf("wpm:%s", PresetBus::WpmPresent() ? "Y" : "n");
  graphics.drawLine(0, 10, 127, 10);

  if (sel_stored < 0) sel_stored = PresetEngine::SlotUsed(sel) ? 1 : 0;

  // big current preset number (1-30), 225e-style
  graphics.setPrintPos(46, 20);
  graphics.printf("%2d", sel + 1);
  // chunky: overprint with 1px offsets for a bold look
  graphics.setPrintPos(47, 20);
  graphics.printf("%2d", sel + 1);
  graphics.setPrintPos(70, 20);
  graphics.print(sel_stored ? "" : "*");

  graphics.setPrintPos(24, 30);
  graphics.print(sel_stored ? "stored" : "not stored");

  graphics.setPrintPos(8, 42);
  if (next_trig) graphics.printf("next:TR%d", next_trig);
  else graphics.print("next:off");
  graphics.setPrintPos(72, 42);
  if (last_trig) graphics.printf("last:TR%d", last_trig);
  else graphics.print("last:off");

  // cursor
  switch (cursor) {
    case 0: graphics.drawFrame(42, 16, 24, 12); break;
    case 1: graphics.drawFrame(6, 40, 58, 11); break;
    case 2: graphics.drawFrame(70, 40, 56, 11); break;
  }

  graphics.setPrintPos(1, 55);
  graphics.print("R:recall hold-L:store");
}

void Task() {
  // 225e last/next pulse inputs: rising edge cycles + recalls bus-wide.
  // Level-sampled here in loop context so the app's own edge consumption
  // is untouched; preset changes are slow relative to the loop rate.
  if (next_trig || last_trig) {
    for (uint8_t t = 0; t < 4; ++t) {
      const bool level = DigitalInputs::read_immediate((DigitalInput)t);
      const bool rising = level && !trig_prev[t];
      trig_prev[t] = level;
      if (!rising) continue;
      if (next_trig == t + 1) {
        sel = (sel + 1) % 30;
        sel_stored = -1;
        recall_selected();
      } else if (last_trig == t + 1) {
        sel = (sel + 29) % 30;
        sel_stored = -1;
        recall_selected();
      }
    }
  }

  if (active) {
    ::MENU_REDRAW = 1;   // keep the overlay live
    ui.Poke();           // and the screensaver away
  }
}

}  // namespace PresetBusUI
}  // namespace OC

#endif  // ARDUINO_TEENSY41 && PRESET_BUS
