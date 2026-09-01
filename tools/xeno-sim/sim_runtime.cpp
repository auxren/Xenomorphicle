// The firmware's boot and main loop, on the host. See sim_runtime.h for why
// this file exists and what it is allowed to do.

#include "sim_runtime.h"

#include <stdio.h>

#include <Arduino.h>

#include "OC_ADC.h"
#include "OC_DAC.h"
#include "OC_apps.h"
#include "OC_app_switcher.h"
#include "OC_calibration.h"
#include "OC_config.h"
#include "OC_core.h"
#include "OC_digital_inputs.h"
#include "OC_gpio.h"
#include "OC_global_settings.h"
#include "OC_io.h"
#include "OC_menus.h"
#include "OC_scales.h"
#include "OC_ui.h"
#include "PhzConfig.h"
#include "PresetBus.h"
#include "PresetBusUI.h"
#include "PresetEngine.h"
#include "src/drivers/display.h"

#include "sim_bus.h"
#include "sim_host.h"
#include "sim_input.h"

// Main.cpp:107 -- the firmware's redraw flag.
uint_fast8_t MENU_REDRAW = true;

namespace {

// Main.cpp:109-110
OC::UiMode g_ui_mode = OC::UI_MODE_MENU;
OC::IOFrame g_io_frame;

uint32_t g_last_redraw_ticks = 0;
uint32_t g_isr_carry_us = 0;

// True once display::Init() has run, which is the point from which CoreIsr()
// is safe to call. Before it a delay() can only move the clock: driving the
// display and DAC through their own init sequence would be a fiction, and a
// worse one than a still clock.
bool g_background_ready = false;

// Re-entry guard. A delay() reached from inside CoreIsr() or ui.Poll() must
// not recurse into them -- on hardware an ISR cannot preempt itself either --
// so it degrades to moving the clock, which is what the caller asked for.
bool g_in_background = false;

// Main.cpp:139-156, CORE_timer_ISR(). On hardware this runs from an
// IntervalTimer at OC_CORE_TIMER_RATE (60 us, 16.6 kHz). Here it is called
// from SimRuntimeTickMs on the same schedule against the virtual clock: the
// rate and the ordering are right, but nothing can preempt anything, an
// overrun is impossible, and CPU load is not modelled.
void CoreIsr() {
  // DAC and display share SPI on the module, and the order matters there:
  // finalize the previous display transfer, push the DAC, start the next
  // display page. Kept because Flush() is what frees a frame buffer -- without
  // it every blocking firmware loop waits forever.
  display::Flush();
  OC::DAC::Update();
  display::Update();
  OC::ADC::Scan_DMA();
  OC::DigitalInputs::Scan();

  ++OC::CORE::ticks;
  if (OC::CORE::app_isr_enabled)
    OC::app_switcher.Process(&g_io_frame);
}

// Main.cpp:775-860, loop(), one pass. Transcribed in order; the parts left out
// are the ones with no host meaning (watchdog_feed, thisUSB.Task, MTP.loop,
// the serial console) plus the audio and USB bridge tasks the simulator does
// not carry. Each omission is a line in README.md.
void LoopPass() {
  using namespace OC;

  // Refresh display
  if (MENU_REDRAW && CORE::display_update_enabled) {
    GRAPHICS_BEGIN_FRAME(false);   // Don't busy wait

    if (UI_MODE_APP_SETTINGS == g_ui_mode) {
      ui.AppSettings(true);
    } else if (OC::PresetBusUI::Active()) {
      OC::PresetBusUI::Draw();
    } else {
      app_switcher.current_app()->Draw(g_ui_mode);
    }

    MENU_REDRAW = 0;
    g_last_redraw_ticks = ui.ticks();
    GRAPHICS_END_FRAME();
  }

  // Run current app
  if (CORE::app_loop_enabled)
    app_switcher.current_app()->DispatchLoop();

  // Take care of queued tasks
  OC::CORE::FlushTasks();
  OC::PresetEngine::Process();
  OC::PresetBus::Task();
  OC::PresetBusUI::Task();

  // UI events
  if (UI_MODE_APP_SETTINGS == g_ui_mode) {
    if (!ui.AppSettings(false))
      g_ui_mode = UI_MODE_MENU;
  } else {
    UiMode mode = ui.DispatchEvents(app_switcher.current_slot());

    if (mode != g_ui_mode) {
      if (UI_MODE_SCREENSAVER == mode)
        app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SCREENSAVER_ON);
      else if (UI_MODE_SCREENSAVER == g_ui_mode)
        app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SCREENSAVER_OFF);
      else if (UI_MODE_APP_SETTINGS == mode)
        app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SUSPEND);

      g_ui_mode = mode;
    }
  }

  if (ui.ticks() - g_last_redraw_ticks > REDRAW_TIMEOUT_MS)
    MENU_REDRAW = 1;
}

}  // namespace

void SimRuntimePoke() { MENU_REDRAW = 1; }

// Main.cpp:411-620, setup(), in order, minus everything with no host meaning.
void SimRuntimeBoot(bool reset_settings) {
  g_background_ready = false;
  OC::Pinout_Detect();          // sets the real pin map from the ID voltage
  SimInputInit();               // ...which is what the panel's pins are bound to

  SimLog("panel pins: A=%u B=%u X=%u Y=%u Z=%u encL=%u encR=%u (ID %.2fV)",
         but_top, but_bot, but_top2, but_bot2, but_mid, butL, butR,
         (double)SimIdVoltage());
  if (but_mid == 0xFF)
    SimLog("but_mid is unassigned on this ID voltage: Z will not poll.");

  OC::DEBUG::Init();
  OC::DigitalInputs::Init();

  OC::calibration_load();
  OC::SetFlipMode(OC::calibration_data.flipcontrols());

  OC::ADC::Init(&OC::calibration_data.adc, OC::calibration_data.flipcontrols());
  OC::ADC::Init_DMA();
  OC::DAC::Init(&OC::calibration_data.dac,
                &OC::global_settings.autotune_calibration_data,
                OC::calibration_data.flipcontrols());

  display::AdjustOffset(OC::calibration_data.display_offset);
  display::SetFlipMode(OC::calibration_data.flipscreen());
  display::Init();
  // From here on a firmware delay() runs the background rather than only
  // moving the clock -- CoreIsr() has something to drive. See SimBackgroundUs.
  g_background_ready = true;

  GRAPHICS_BEGIN_FRAME(true);
  GRAPHICS_END_FRAME();

  OC::ui.Init();
  OC::ui.configure_encoders(OC::calibration_data.encoder_config());

  g_io_frame.Reset();

  // SDcard_Ready stays false: the simulator has no card, which is the same
  // path a module with an empty slot takes. LittleFS is RAM-backed (FS.h).
  PhzConfig::Init();

  // Main.cpp:534. Real code, and it blocks for SPLASHSCREEN_DELAY_MS while
  // drawing every pass; display::SimPump() (shim/src/src/drivers/display.h)
  // is what keeps the clock and the display moving underneath it, so the
  // boot gestures -- hold encL for Calibrate, encR for the app menu -- work
  // here exactly as they do on the module.
  // The EEPROM-reset gesture is A and B held through the splash, and
  // Splashscreen() decides it by reading those two pins itself -- it takes
  // `reset_settings` by reference and OVERWRITES it. So --reset-settings has
  // to hold the buttons, not set the flag.
  if (reset_settings) {
    SimInputSetButton(OC::CONTROL_BUTTON_UP, true);
    SimInputSetButton(OC::CONTROL_BUTTON_DOWN, true);
    SimLog("--reset-settings: holding A+B through the splash, as the gesture is");
  }
  g_ui_mode = OC::ui.Splashscreen(reset_settings, 0);
  SimInputReleaseAll();
  bool start_cal = false;
  if (g_ui_mode == OC::UI_MODE_CALIBRATE) {
    start_cal = true;
    g_ui_mode = OC::UI_MODE_MENU;
  }

  OC::ui.set_screensaver_timeout(OC::calibration_data.screensaver_timeout);

  bool firstrun = !PhzConfig::load_config();

  // AppSwitcher::Init opens Ui::ConfirmReset() -- a blocking screen -- when
  // the stored settings are missing or invalid, which on a RAM-backed file
  // system is every boot. It is real firmware and it waits for a real button,
  // so the simulator schedules one: encL for CANCEL (keep defaults), encR for
  // OK (erase and re-save) under --reset-settings. Nothing reaches into the
  // firmware to skip the screen; --keys "" with a breakpoint will show it.
  if (reset_settings || firstrun) {
    const uint16_t answer = reset_settings ? OC::CONTROL_BUTTON_R : OC::CONTROL_BUTTON_L;
    SimInputScheduleTap(answer, 120, 60);
    SimLog("boot: settings absent -> ConfirmReset answered %s automatically",
           reset_settings ? "OK (erase)" : "CANCEL (keep defaults)");
  }
  firstrun |= !OC::app_switcher.Init(reset_settings || firstrun);

  OC::PresetEngine::Init();
  OC::PresetBus::Init();
  OC::PresetBusUI::Init();

  OC::ui.Splashscreen(firstrun, 1);      // the welcome frame
  if (start_cal) OC::start_calibration();

  OC::app_switcher.current_app()->DispatchAppEvent(OC::APP_EVENT_RESUME);

  // Main.cpp:776-778
  OC::CORE::app_isr_enabled = true;
  OC::CORE::display_update_enabled = true;
  OC::CORE::app_loop_enabled = true;
  MENU_REDRAW = 1;
}

namespace {

// Everything that runs off a timer: the core ISR at OC_CORE_TIMER_RATE and the
// UI poll at OC_UI_TIMER_RATE, both on the virtual clock. Called from
// SimRuntimeTickMs and, while a firmware loop is blocked on a frame, from
// display::SimPump().
uint32_t g_ui_carry_us = 0;

void BackgroundUs(uint32_t us) {
  g_isr_carry_us += us;
  while (g_isr_carry_us >= OC_CORE_TIMER_RATE) {
    g_isr_carry_us -= OC_CORE_TIMER_RATE;
    CoreIsr();
  }
  g_ui_carry_us += us;
  while (g_ui_carry_us >= OC_UI_TIMER_RATE) {
    g_ui_carry_us -= OC_UI_TIMER_RATE;
    SimInputTick();    // pin levels for this UI tick
    OC::ui.Poll();     // the real 1 kHz UI ISR
  }
  SimAdvanceUs(us);
}

}  // namespace

// See shim/src/src/drivers/display.h.
void display::SimPump() { BackgroundUs(OC_CORE_TIMER_RATE); }

// The firmware's delay()/delayMicroseconds(), which are not pauses: see the
// note in shim/arduino/Arduino.h. Advanced one core-ISR period at a time so
// that everything the background runs sees a clock that is moving, rather than
// N periods' worth of work at one frozen instant.
void SimBackgroundUs(uint32_t us) {
  if (!us) return;
  if (!g_background_ready || g_in_background) { SimAdvanceUs(us); return; }
  g_in_background = true;
  while (us) {
    const uint32_t chunk = us > OC_CORE_TIMER_RATE ? OC_CORE_TIMER_RATE : us;
    BackgroundUs(chunk);
    us -= chunk;
  }
  g_in_background = false;
}

void SimRuntimeTickMs() {
  const uint32_t until = SimNowUs() + 1000;
  // The timer contexts first, then one pass of loop() -- the priority order
  // the firmware runs them in.
  while ((int32_t)(until - SimNowUs()) > 0)
    BackgroundUs(OC_CORE_TIMER_RATE);
  SimBusTask();        // the fake modules on the other end of the wire
  LoopPass();
}

void SimRuntimeAdvanceMs(uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) SimRuntimeTickMs();
}

const char *SimRuntimeScreen() {
  if (OC::PresetBusUI::Active()) return "preset";
  switch (g_ui_mode) {
    case OC::UI_MODE_SCREENSAVER: return "saver";
    case OC::UI_MODE_APP_SETTINGS: return "menu";
    case OC::UI_MODE_CALIBRATE: return "cal";
    default: return "app";
  }
}

const char *SimRuntimeAppName() {
  OC::AppBase *app = OC::app_switcher.current_app();
  return app ? app->name() : "?";
}

std::string SimRuntimeStatusLine() {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s  app=%s  held=[%s]  t=%ums  ticks=%lu",
           SimRuntimeScreen(), SimRuntimeAppName(),
           SimInputHeldTokens().c_str(), SimNowMs(),
           (unsigned long)OC::ui.ticks());
  return buf;
}
