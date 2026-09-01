// The panel, as pins. See sim_input.h.

#include "sim_input.h"

#include <Arduino.h>

#include "OC_gpio.h"
#include "OC_ui.h"

#include "sim_host.h"

#include <vector>

namespace {

// Control mask -> pin, in the order OC_ui.cpp's button_pins[] uses, because
// that array's index IS the control bit (control_mask(i) == 1 << i).
struct ButtonBinding {
  uint16_t control;
  uint8_t *pin;
  const char *token;
};

ButtonBinding g_buttons[7];
int g_button_count = 0;

uint16_t g_held = 0;

// Encoder phases. A detent is four (A, B) pairs applied one per UI tick; see
// the header for what that costs and why.
struct Phase { uint8_t a, b; };
constexpr Phase kCw[4]  = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};
constexpr Phase kCcw[4] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};

struct EncoderQueue {
  uint8_t *pin_a = nullptr;
  uint8_t *pin_b = nullptr;
  int pending = 0;        // detents still to emit, signed
  int phase = -1;         // -1 = idle, else 0..3 within the current detent
  bool dir_cw = true;
};

EncoderQueue g_enc[2];    // [0] = left, [1] = right

// Scheduled presses (see SimInputScheduleTap).
struct ScheduledTap { uint16_t control; uint32_t down_ms, up_ms; bool started; };
std::vector<ScheduledTap> g_taps;

// Scheduled encoder turns (see SimInputScheduleEncoder).
struct ScheduledEnc { bool right; int delta; uint32_t at_ms; };
std::vector<ScheduledEnc> g_sched_enc;

void SetPin(uint8_t pin, uint8_t level) {
  if (pin < kSimPinCount) SimPinLevels()[pin] = level;
}

}  // namespace

void SimInputInit() {
  using namespace OC;
  g_button_count = 0;
  auto bind = [](uint16_t control, uint8_t *pin, const char *token) {
    if (!pin || *pin == 0xFF) return;   // unassigned on this hardware ID
    g_buttons[g_button_count++] = {control, pin, token};
  };
  // The order here is OC_ui.cpp's button_pins[]; the comment there is the
  // authority on which control each pin is.
  bind(CONTROL_BUTTON_UP,    &but_top,  "a");
  bind(CONTROL_BUTTON_DOWN,  &but_bot,  "b");
  bind(CONTROL_BUTTON_L,     &butL,     "l");
  bind(CONTROL_BUTTON_R,     &butR,     "r");
  bind(CONTROL_BUTTON_M,     &but_mid,  "z");
  bind(CONTROL_BUTTON_UP2,   &but_top2, "x");
  bind(CONTROL_BUTTON_DOWN2, &but_bot2, "y");

  g_enc[0].pin_a = &encL1;
  g_enc[0].pin_b = &encL2;
  g_enc[1].pin_a = &encR1;
  g_enc[1].pin_b = &encR2;

  // Buttons and encoders are INPUT_PULLUP and active low; idle is high.
  for (int i = 0; i < g_button_count; ++i) SetPin(*g_buttons[i].pin, HIGH);
  for (auto &e : g_enc) {
    SetPin(*e.pin_a, HIGH);
    SetPin(*e.pin_b, HIGH);
    e.pending = 0;
    e.phase = -1;
  }
  // Trigger inputs idle inactive. Which level that is depends on the board:
  // OC_digital_inputs.h reads them as active-high on ADC33131D hardware.
  const uint8_t idle = ADC33131D_Uses_FlexIO ? LOW : HIGH;
  SetPin(TR1, idle);
  SetPin(TR2, idle);
  SetPin(TR3, idle);
  SetPin(TR4, idle);
  g_held = 0;
}

void SimInputTick() {
  const uint32_t now = SimNowMs();
  for (size_t i = 0; i < g_sched_enc.size();) {
    if (now >= g_sched_enc[i].at_ms) {
      const ScheduledEnc e = g_sched_enc[i];
      g_sched_enc.erase(g_sched_enc.begin() + i);
      SimInputEncoder(e.right, e.delta);
      continue;
    }
    ++i;
  }
  for (size_t i = 0; i < g_taps.size();) {
    ScheduledTap &t = g_taps[i];
    if (!t.started && now >= t.down_ms) { SimInputSetButton(t.control, true); t.started = true; }
    if (t.started && now >= t.up_ms) {
      SimInputSetButton(t.control, false);
      g_taps.erase(g_taps.begin() + i);
      continue;
    }
    ++i;
  }
  for (auto &e : g_enc) {
    if (e.phase < 0) {
      if (!e.pending) continue;
      e.dir_cw = e.pending > 0;
      e.phase = 0;
    }
    const Phase &p = e.dir_cw ? kCw[e.phase] : kCcw[e.phase];
    SetPin(*e.pin_a, p.a);
    SetPin(*e.pin_b, p.b);
    if (++e.phase >= 4) {
      e.phase = -1;
      e.pending += e.dir_cw ? -1 : 1;
    }
  }
}

void SimInputSetButton(uint16_t control, bool down) {
  for (int i = 0; i < g_button_count; ++i) {
    if (g_buttons[i].control != control) continue;
    SetPin(*g_buttons[i].pin, down ? LOW : HIGH);
    if (down) g_held |= control; else g_held &= (uint16_t)~control;
    return;
  }
}

void SimInputReleaseAll() {
  for (int i = 0; i < g_button_count; ++i) SetPin(*g_buttons[i].pin, HIGH);
  g_held = 0;
}

void SimInputEncoder(bool right, int delta) {
  if (delta > 64) delta = 64;
  if (delta < -64) delta = -64;
  EncoderQueue &e = g_enc[right ? 1 : 0];
  // A reversal mid-queue would be a physical impossibility; drop what is left
  // and start the other way, which is what a hand doing it would produce.
  if ((e.pending > 0) != (delta > 0) && e.pending) e.pending = 0;
  e.pending += delta;
}

bool SimInputEncoderBusy() {
  for (const auto &e : g_enc)
    if (e.pending || e.phase >= 0) return true;
  return false;
}

uint16_t SimInputControlForToken(const std::string &tok) {
  for (int i = 0; i < g_button_count; ++i)
    if (tok == g_buttons[i].token) return g_buttons[i].control;
  return 0;
}

const char *SimInputTokenForControl(uint16_t control) {
  for (int i = 0; i < g_button_count; ++i)
    if (g_buttons[i].control == control) return g_buttons[i].token;
  return "?";
}

uint16_t SimInputHeld() { return g_held; }

std::string SimInputHeldTokens() {
  std::string out;
  for (int i = 0; i < g_button_count; ++i) {
    if (!(g_held & g_buttons[i].control)) continue;
    if (!out.empty()) out += "+";
    out += g_buttons[i].token;
  }
  return out;
}

void SimInputScheduleEncoder(bool right, int delta, uint32_t at_ms) {
  g_sched_enc.push_back({right, delta, SimNowMs() + at_ms});
}

void SimInputScheduleTap(uint16_t control, uint32_t at_ms, uint32_t dur_ms) {
  g_taps.push_back({control, SimNowMs() + at_ms, SimNowMs() + at_ms + dur_ms, false});
}

void SimInputSetTrigger(int index, bool high) {
  const uint8_t pins[4] = {TR1, TR2, TR3, TR4};
  if (index < 0 || index > 3) return;
  SetPin(pins[index], high ? HIGH : LOW);
}
