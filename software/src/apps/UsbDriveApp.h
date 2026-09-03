#pragma once
// ---------------------------------------------------------------------------
// USB Drive -- plug the module into a computer and get at the SD card and
// the internal preset store like an ordinary USB drive, no reflash needed.
//
// There is no true USB Mass Storage personality in this Teensyduino core, only
// MTP (Media Transfer Protocol): native on Windows/most Linux desktops, needs
// a client on macOS (e.g. Android File Transfer). MTP+SD support already
// exists in this codebase -- PhzConfig::Init() calls MTP.begin() and
// MTP.addFilesystem() for both SD and internal flash under MTP_INTERFACE, and
// Main.cpp's loop() pumps MTP.loop() the same way -- it just was not reachable
// as a live app. It lives in its own MULTIBOOT flash slot (T41_MTP, slot 2:
// -DUSB_MTPDISK, -DNO_HEMISPHERE, a short stock-app list) because MTP wants
// its own USB personality and that build drops most apps to fit.
//
// So getting there is a MODE SWITCH, not a feature toggle: this screen arms
// OC::calibration_data.bootchoice() = 2 (the same field BootMenu()/Main.cpp's
// jump_to_alt() already read at boot -- see Main.cpp's MULTIBOOT block) and
// reboots. The next boot's slot0 (T41_audio) dispatcher sees bootchoice == 2
// and jumps straight into slot 2 instead of continuing normally. Nothing here
// touches the live jump -- that only ever runs at boot, before apps exist, in
// a build this app is never compiled into (T41_MTP has no MULTIBOOT code at
// all: see platformio.ini, only T41_audio and its derivatives define it).
//
// A user in slot 2 needs a way back that survives a power cycle -- bootchoice
// is sticky, so without one the module reboots straight back into MTP mode
// forever, and the only escape would be a host reflash. That return gesture
// (RETURN TO NORMAL MODE) lives in SETTINGS.h, gated on USB_MTPDISK so it only
// exists in the one build that needs it, in the one app (Setup/About) that
// build keeps regardless of app selection. See SETTINGS.h for its half.
//
// CONFIRM GESTURE: two deliberate steps, same shape as SETTINGS.h's REFLASH:
// BOOTLOADER gesture (Arm/Disarm/HandleConfirmEvent there) -- a LONG PRESS of
// B on the "USB Drive Mode" row arms it (nothing has happened yet), then only
// a SOLO short press of B on the confirm screen -- after kConfirmDeadMs, so a
// fumbled double-tap on the way to arming can't also commit -- actually
// reboots. encL cancels from the confirm screen, at any time.
// ---------------------------------------------------------------------------

#include "../HSUtils.h"
#include "../OC_calibration.h"
#include "../PresetEngine.h"

extern "C" void _reboot_Teensyduino_();

OC_APP_CLASS(AppUsbDrive, TWOCCS("UD"), "USB Drive", "USB Drive") {
public:
  OC_APP_INTERFACE_DECLARE(AppUsbDrive, 0);

private:
  enum Screen : uint8_t { SCR_MENU, SCR_CONFIRM_USBDRIVE, SCR_RECOVER_RESULT };
  enum Item : uint8_t { ITEM_USBDRIVE = 0, ITEM_RECOVER = 1, kNumItems = 2 };

  Screen screen_ = SCR_MENU;
  uint8_t cursor_ = ITEM_USBDRIVE;

  // Confirm-screen state, same fields/reasoning as SETTINGS.h's armed_ms_/
  // b_down_solo_: whether the screen is armed is screen_ itself (there is
  // only one confirm screen here, unlike SETTINGS.h's pending_ which picks
  // between several) -- a solo press is one that BEGAN on this screen (a
  // button already down when the screen appeared cannot commit), and
  // kConfirmDeadMs keeps the same value as SETTINGS.h/the 200e write confirm
  // for the same reason -- long enough no fumble crosses it, short enough a
  // deliberate press never feels refused.
  uint32_t armed_ms_ = 0;
  bool b_down_solo_ = false;
  static constexpr uint32_t kConfirmDeadMs = 350;

  char result_msg_[32] = {0};

  void Arm() {
    armed_ms_ = millis();
    b_down_solo_ = false;
    screen_ = SCR_CONFIRM_USBDRIVE;
  }
  void Disarm() {
    screen_ = SCR_MENU;
  }

  // Persist bootchoice the same way every other caller of it does --
  // OC::calibration_save(), which is also what BootMenu() calls after
  // set_bootchoice() (Main.cpp) -- then reboot. Reflash() in SETTINGS.h
  // shows a blocking message before _reboot_Teensyduino_() too; this mirrors
  // that shape, with calibration_save()'s own "saved" screen standing in for
  // the first half of it.
  void EnterUsbDriveMode() {
    OC::calibration_data.set_bootchoice(2);
    OC::calibration_save();   // blocks, draws "Calibration saved to EEPROM!"
    const uint32_t start = millis();
    while (millis() < start + SETTINGS_SAVE_TIMEOUT_MS) {
      GRAPHICS_BEGIN_FRAME(true);
      graphics.setPrintPos(5, 10);
      graphics.print("Entering USB Drive");
      graphics.setPrintPos(5, 19);
      graphics.print("Mode...");
      GRAPHICS_END_FRAME();
    }
    _reboot_Teensyduino_();
  }

  // Read-only from the running app's point of view: internal flash and the
  // SD card are both already reachable from T41_audio, so this needs no mode
  // switch and no confirm gesture -- it only ever WRITES into a slot that
  // SlotUsed() called empty (see PresetEngine.cpp's RecoverLegacyFromCard),
  // never touches anything already on the module, and never touches the card.
  void RunRecovery() {
    const int n = OC::PresetEngine::RecoverAllLegacyFromCard();
    if (n < 0)
      snprintf(result_msg_, sizeof(result_msg_), "No SD card present");
    else if (n == 0)
      snprintf(result_msg_, sizeof(result_msg_), "No legacy presets found");
    else
      snprintf(result_msg_, sizeof(result_msg_), "Recovered %d preset%s", n,
               n == 1 ? "" : "s");
    screen_ = SCR_RECOVER_RESULT;
  }

  // Same button-chord debounce as SETTINGS.h's HandleConfirmEvent: encL is
  // "no" everywhere in this app; B commits, but only a press that both BEGAN
  // alone on this screen (b_down_solo_, checked on the DOWN edge, where the
  // event's mask still shows what else is held) and lands with nothing else
  // held now (event.mask == 0) and after the screen has been up for
  // kConfirmDeadMs.
  void HandleConfirmEvent(const UI::Event &event) {
    if (event.control == OC::CONTROL_BUTTON_L &&
        event.type == UI::EVENT_BUTTON_PRESS) {
      Disarm();
      return;
    }
    if (event.control != OC::CONTROL_BUTTON_B) return;
    if (event.type == UI::EVENT_BUTTON_DOWN) {
      b_down_solo_ = (event.mask == OC::CONTROL_BUTTON_B);
      return;
    }
    if (event.type != UI::EVENT_BUTTON_PRESS) return;
    const bool solo = b_down_solo_;
    b_down_solo_ = false;
    if (!solo || event.mask) return;
    if (millis() - armed_ms_ < kConfirmDeadMs) return;
    EnterUsbDriveMode();
  }
};

FLASHMEM void AppUsbDrive::Init() {
  screen_ = SCR_MENU;
  cursor_ = ITEM_USBDRIVE;
  b_down_solo_ = false;
  result_msg_[0] = 0;
}

FLASHMEM size_t AppUsbDrive::SaveAppData(util::StreamBufferWriter &) const {
  return 0;   // nothing persisted: this app has no scene state of its own
}
FLASHMEM size_t AppUsbDrive::RestoreAppData(util::StreamBufferReader &) {
  return 0;
}

FLASHMEM void AppUsbDrive::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
    case OC::APP_EVENT_SUSPEND:
      // Never leave an armed confirm screen across a suspend, a screensaver
      // or a return to this app -- same reasoning as SETTINGS.h's Disarm()
      // call from both Resume() and Suspend(): the screen would be gone but
      // a stray B could still commit whatever it was armed to do.
      Disarm();
      break;
    default: break;
  }
}

void AppUsbDrive::Process(OC::IOFrame *) {}
FLASHMEM void AppUsbDrive::Loop() {}
FLASHMEM void AppUsbDrive::GetIOConfig(OC::IOConfig &) const {
  // No CV/gate I/O of any kind: this app only ever touches storage and
  // calibration_data's bootchoice.
}
FLASHMEM void AppUsbDrive::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("screen ");
  graphics.print((int)screen_);
}
FLASHMEM void AppUsbDrive::DrawScreensaver() const {}  // blank, like AppScaleEditor

FLASHMEM void AppUsbDrive::DrawMenu() const {
  switch (screen_) {
    default:
    case SCR_MENU: {
      graphics.setPrintPos(1, 1);
      graphics.print("USB DRIVE");
      graphics.drawHLine(0, 10, 128);

      graphics.setPrintPos(4, 16);
      graphics.print("USB Drive Mode");
      if (cursor_ == ITEM_USBDRIVE) graphics.invertRect(0, 15, 128, 9);

      graphics.setPrintPos(4, 28);
      graphics.print("Recover Legacy (SD)");
      if (cursor_ == ITEM_RECOVER) graphics.invertRect(0, 27, 128, 9);

      gfxFooter(cursor_ == ITEM_USBDRIVE ? "hold B: begin"
                                          : "B: recover now");
      break;
    }
    case SCR_CONFIRM_USBDRIVE: {
      graphics.setPrintPos(0, 13);
      graphics.print("USB DRIVE MODE");
      graphics.invertRect(0, 12, 128, 10);

      graphics.setPrintPos(0, 26);
      graphics.print("Reboots to MTP mode:");
      graphics.setPrintPos(0, 36);
      graphics.print("presets+SD as drives.");
      graphics.setPrintPos(0, 46);
      graphics.print("Return via Setup app.");

      graphics.setPrintPos(0, 56);
      graphics.print("encL:no");
      graphics.setPrintPos(90, 56);
      graphics.print("B:GO");
      break;
    }
    case SCR_RECOVER_RESULT: {
      graphics.setPrintPos(1, 1);
      graphics.print("RECOVER LEGACY (SD)");
      graphics.drawHLine(0, 10, 128);
      graphics.setPrintPos(4, 28);
      graphics.print(result_msg_);
      gfxFooter("any key: back");
      break;
    }
  }
}

FLASHMEM void AppUsbDrive::HandleButtonEvent(const UI::Event &event) {
  if (screen_ == SCR_CONFIRM_USBDRIVE) {
    HandleConfirmEvent(event);
    return;
  }
  if (screen_ == SCR_RECOVER_RESULT) {
    if (event.type == UI::EVENT_BUTTON_PRESS) screen_ = SCR_MENU;
    return;
  }
  // SCR_MENU
  if (event.control != OC::CONTROL_BUTTON_B) return;
  if (event.type == UI::EVENT_BUTTON_PRESS) {
    if (cursor_ == ITEM_RECOVER) RunRecovery();
    // ITEM_USBDRIVE: a short press does nothing -- the footer already says
    // to hold. This is what stops a single accidental B press from doing
    // anything at all, on top of the confirm screen behind it.
  } else if (event.type == UI::EVENT_BUTTON_LONG_PRESS) {
    if (cursor_ == ITEM_USBDRIVE) Arm();
  }
}

FLASHMEM void AppUsbDrive::HandleEncoderEvent(const UI::Event &event) {
  if (screen_ != SCR_MENU) return;
  if (event.control == OC::CONTROL_ENCODER_L && event.value != 0)
    cursor_ = (uint8_t)(cursor_ ^ 1);   // only 2 items: any turn flips it
}
