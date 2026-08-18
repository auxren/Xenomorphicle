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

// ---- drawing ---------------------------------------------------------------
// Layout per the Orin_Fun design system review (module border, letterspaced
// legend + rule, 225e-style 7-segment LCD window, banana-jack presence dots,
// inversion = focus and nothing else).

// 7-segment digit: cell 12x22, segment thickness 2. Bits: A B C D E F G.
static const uint8_t kSeg[10] = {
  0b1111110,  // 0: ABCDEF
  0b0110000,  // 1: BC
  0b1101101,  // 2: ABGED
  0b1111001,  // 3: ABGCD
  0b0110011,  // 4: FBGC
  0b1011011,  // 5: AFGCD
  0b1011111,  // 6: AFGEDC
  0b1110000,  // 7: ABC
  0b1111111,  // 8
  0b1111011,  // 9: ABCDFG
};

FLASHMEM static void draw_7seg(int dx, int dy, uint8_t digit) {
  const uint8_t m = kSeg[digit % 10];
  if (m & 0b1000000) graphics.drawRect(dx + 2, dy,      8, 2);  // A
  if (m & 0b0100000) graphics.drawRect(dx + 10, dy + 2, 2, 8);  // B
  if (m & 0b0010000) graphics.drawRect(dx + 10, dy + 12, 2, 8); // C
  if (m & 0b0001000) graphics.drawRect(dx + 2, dy + 20, 8, 2);  // D
  if (m & 0b0000100) graphics.drawRect(dx,     dy + 12, 2, 8);  // E
  if (m & 0b0000010) graphics.drawRect(dx,     dy + 2,  2, 8);  // F
  if (m & 0b0000001) graphics.drawRect(dx + 2, dy + 10, 8, 2);  // G
}

FLASHMEM static void draw_jack(int cx, int cy, bool active) {
  graphics.drawCircle(cx, cy, 3);
  if (active) graphics.drawRect(cx - 1, cy - 1, 3, 3);
}

FLASHMEM void Draw() {
  graphics.drawFrame(0, 0, 128, 64);            // module border
  graphics.setPrintPos(4, 2);
  graphics.print("P R E S E T  B U S");         // letterspaced legend
  graphics.drawHLine(1, 11, 126);               // rule

  // LCD window, 225e-style
  graphics.drawFrame(43, 13, 42, 38);
  if (sel_stored < 0) sel_stored = PresetEngine::SlotUsed(sel) ? 1 : 0;
  const uint8_t shown = sel + 1;                // 01-30, zero-padded
  draw_7seg(50, 16, shown / 10);
  draw_7seg(66, 16, shown % 10);
  graphics.setPrintPos(sel_stored ? 46 : 49, 40);
  graphics.print(sel_stored ? "STORED" : "EMPTY");

  // edge legends, spatially mapped to the encoders (225e panel order)
  graphics.setPrintPos(4, 21);
  graphics.print("STORE");
  graphics.setPrintPos(7, 31);
  graphics.print("hold");
  graphics.setPrintPos(88, 21);
  graphics.print("RECALL");
  graphics.setPrintPos(91, 31);
  graphics.print("press");

  // WPM presence: banana-jack dot, filled = present
  graphics.drawCircle(92, 43, 2);
  if (PresetBus::WpmPresent()) graphics.drawRect(91, 42, 3, 3);
  graphics.setPrintPos(97, 40);
  graphics.print(PresetBus::WpmPresent() ? "WPM" : "wpm");

  // 225e last/next pulse jacks
  draw_jack(8, 57, next_trig != 0);
  graphics.setPrintPos(14, 54);
  graphics.print("NEXT");
  graphics.setPrintPos(40, 54);
  if (next_trig) graphics.printf("TR%d", next_trig);
  else graphics.print("off");

  draw_jack(72, 57, last_trig != 0);
  graphics.setPrintPos(78, 54);
  graphics.print("LAST");
  graphics.setPrintPos(104, 54);
  if (last_trig) graphics.printf("TR%d", last_trig);
  else graphics.print("off");

  // one focus grammar: invert what the right encoder will change
  switch (cursor) {
    case 0: graphics.invertRect(48, 14, 32, 26); break;
    case 1: graphics.invertRect(39, 53, 20, 10); break;
    case 2: graphics.invertRect(103, 53, 20, 10); break;
  }
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
    // Throttled redraw: forcing MENU_REDRAW every pass makes the renderer
    // race the display DMA and the last-scanned corner visibly tears.
    static uint32_t last_kick = 0;
    if (millis() - last_kick >= 66) {   // ~15Hz; events redraw immediately
      last_kick = millis();
      ::MENU_REDRAW = 1;
    }
    ui.Poke();  // keep the screensaver away
  }
}

}  // namespace PresetBusUI
}  // namespace OC

#endif  // ARDUINO_TEENSY41 && PRESET_BUS
