// Copyright (c) 2018, Jason Justian
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

// This app moves THIS module's own EEPROM to and from a USB SysEx host. It has
// nothing to do with the banks inside a 200e module, which the 200e Modules app
// reads and writes over the preset bus -- a user arriving from there reads
// "Backup: Data" as "back up the module bank I was just editing", which is why
// the screens now say "This module's EEPROM" out loud and label the EEPROM
// region rather than the direction.
//
// A restore overwrites that EEPROM with whatever the host sends and then
// re-inits every app; there is no undo. It used to start on a single encL
// press, and encL is back/cancel everywhere else in this firmware -- worse,
// "[RESTORE]" on the home screen and "[CANCEL]" on the listening screen were
// drawn at the identical pixel position, so cancelling and then pressing the
// same button again in the same place re-armed the overwrite. Now encL only
// ever asks or cancels, and the single irreversible commit in this app is a B
// press on a screen that exists only to state what B will do.
OC_APP_CLASS(AppBackup, TWOCCS("BU"), "Back It Up!", "Backup / Restore"),
  public SystemExclusiveHandler {
public:
  OC_APP_INTERFACE_DECLARE(AppBackup, 0);

  void Resume() {
    receiving = 0;
    packet = 0;
    restore_armed_ = 0;
  }

  void Controller() {
    if (receiving) ListenForSysEx();
  }

  void View() const {
    DrawInterface();
  }

  void ToggleReceiveMode() {
    receiving = 1 - receiving;
    packet = 0;
  }

  void ToggleCalibration() {
    if (!receiving) {
      calibration = 1 - calibration;
      packet = 0;
    }
  }

  // Step 1 of 2. Nothing is listening yet and no byte of EEPROM has moved.
  void ArmRestore() {
    restore_armed_ = 1;
    armed_ms_ = millis();
    // Only a B press that BEGINS on this screen can commit, so a B already
    // held when the question appeared is one gesture short, not one past.
    b_down_solo_ = 0;
  }
  void Disarm() { restore_armed_ = 0; }

  // The armed screen answers to exactly two things: B commits, encL says no.
  // encR -- which is Backup on the screen this one came from -- is inert here
  // on purpose, as are the other face buttons and both encoders: the control
  // that means "send" one screen earlier must never be the control that means
  // "receive over the top of everything" on this one.
  void HandleConfirmEvent(const UI::Event &event) {
    if (event.control == OC::CONTROL_BUTTON_L
        && event.type == UI::EVENT_BUTTON_PRESS) {
      Disarm();
      return;
    }
    if (event.control != OC::CONTROL_BUTTON_B) return;

    // The chord test belongs on the DOWN edge: event.mask is the raw pin
    // state when the event was queued, and a release is reported seven ticks
    // after the pin rises, so releasing another button a few milliseconds
    // earlier leaves B's press event looking solo when it was not.
    if (event.type == UI::EVENT_BUTTON_DOWN) {
      b_down_solo_ = (event.mask == OC::CONTROL_BUTTON_B);
      return;
    }
    if (event.type != UI::EVENT_BUTTON_PRESS) return;

    const bool solo = b_down_solo_;
    b_down_solo_ = 0;
    if (!solo || event.mask) return;   // ...and nothing else still held
    // The screen must have been readable for kConfirmDeadMs before it takes a
    // yes; a slip inside that window is silently ignored.
    if (millis() - armed_ms_ < kConfirmDeadMs) return;
    Disarm();
    ToggleReceiveMode();     // start listening
  }

  void HandleButtonPress(const UI::Event &event) {
    if (event.control == OC::CONTROL_BUTTON_L) {
      // encL is the harmless answer on every screen here: it cancels a listen
      // in progress, and otherwise it only opens the question.
      if (receiving) ToggleReceiveMode();
      else ArmRestore();
      return;
    }
    // Backup is not destructive -- it only reads EEPROM out -- so it stays a
    // single press on the button that means "do the thing" everywhere else.
    if (event.control == OC::CONTROL_BUTTON_R && !receiving) OnSendSysEx();
  }

  void OnSendSysEx() {
    if (!receiving) {
      packet = 0;
      uint8_t V[33];

      uint8_t start = calibration ? 0 : (EEPROM_CALIBRATIONDATA_END / 32);
      uint8_t end = calibration ? (EEPROM_CALIBRATIONDATA_END / 32)
                             : (EEPROMStorage::LENGTH / 32);
      for (uint8_t p = start; p < end; p++) {
        uint16_t address = p * 32;
        uint8_t ix = 0;
        V[ix++] = p; // Packet number
        packet = p;

        // Wrap into 32-byte packets
        for (int b = 0; b < 32; b++) V[ix++] = EEPROM.read(address++);

        UnpackedData unpacked;
        unpacked.set_data(ix, V);
        PackedData packed = unpacked.pack();
        SendSysEx(packed, 'B');
      }
    }
  }

  void OnReceiveSysEx() {
    uint8_t V[33];
    if (ExtractSysExData(V, 'B')) {
      uint8_t ix = 0;
      uint8_t p = V[ix++]; // Get packet number
      packet = p;
      uint16_t address = p * 32;
      for (int b = 0; b < 32; b++) EEPROM.write(address++, V[ix++]);

      // Reset on last packet
      if (p == ((EEPROM_CALIBRATIONDATA_END / 32) - 1)
          || p == ((EEPROMStorage::LENGTH / 32) - 1)) {
        receiving = 0;
        OC::app_switcher.Init(0);
      }
    }
  }

private:
  bool calibration = 0;
  bool receiving = 0;
  uint8_t packet = 0;
  bool restore_armed_ = 0;
  uint32_t armed_ms_ = 0;    // millis() the confirm screen appeared
  bool b_down_solo_ = 0;     // B went down alone, after that screen appeared

  // Same value and same reasoning as the 200e write confirm's kConfirmDeadMs:
  // long enough that no fumbled chord or double-tap crosses it, short enough
  // that a deliberate press never feels refused.
  static constexpr uint32_t kConfirmDeadMs = 350;

  // The region name is drawn on all three screens. It selects what a backup
  // SENDS and what a restore OVERWRITES, which the old "Backup: Data" label
  // hid: a stray encoder nudge could leave a restore pointed at calibration
  // while the screen still said "Backup".
  const char *RegionName() const {
    return calibration ? "Calibration" : "Data";
  }

  void DrawInterface() const {
    graphics.drawLine(0, 10, 127, 10);
    graphics.drawLine(0, 12, 127, 12);
    graphics.setPrintPos(0, 1);
    graphics.print("Backup / Restore");

    if (restore_armed_) {
      DrawRestoreConfirm();
      return;
    }

    graphics.setPrintPos(0, 15);
    if (receiving) {
      if (packet > 0) {
        graphics.print("Receiving...");

        // Progress bar
        graphics.drawRect(0, 33, (packet + 4) * 2, 8);
      } else graphics.print("Listening...");

      // Say what is being written over while it is being written over.
      graphics.setPrintPos(0, 25);
      graphics.print("Region: ");
      graphics.print(RegionName());

      graphics.setPrintPos(0, 55);
      graphics.print("encL:cancel");
      return;
    }

    if (packet > 0) graphics.print("Done!");
    else graphics.print("Restore or Backup?");

    graphics.setPrintPos(0, 25);
    graphics.print("This module's EEPROM");
    graphics.setPrintPos(0, 35);
    graphics.print("Region: ");
    graphics.print(RegionName());

    graphics.setPrintPos(0, 45);
    graphics.print("encR:Backup to USB");
    // The ellipsis is the promise that this one asks first.
    graphics.setPrintPos(0, 55);
    graphics.print("encL:Restore...");
  }

  // Nothing is listening and nothing has been overwritten when this is drawn.
  void DrawRestoreConfirm() const {
    // Below the header's divider lines, not over them: an inverted band that
    // swallowed the rule at y=12 read as a drawing glitch.
    graphics.setPrintPos(0, 15);
    graphics.print("OVERWRITE EEPROM");
    graphics.invertRect(0, 14, 128, 10);   // inversion is the only emphasis

    graphics.setPrintPos(0, 26);
    graphics.print("Region: ");
    graphics.print(RegionName());
    graphics.setPrintPos(0, 36);
    graphics.print("is replaced by what");
    graphics.setPrintPos(0, 46);
    graphics.print("the USB host sends.");

    // x=0 on the bottom row is the harmless answer on every screen in this
    // app -- the commit sits where no other screen puts anything, so no
    // remembered position can carry a "cancel" press onto an overwrite.
    graphics.setPrintPos(0, 56);
    graphics.print("encL:no");
    graphics.setPrintPos(72, 56);
    graphics.print("B:RESTORE");
  }
};

FLASHMEM
void AppBackup::Init() {
    Resume();
}
FLASHMEM
void AppBackup::DrawMenu() const {
    View();
}
void AppBackup::Process(OC::IOFrame* ioframe) {
    Controller();
}
void AppBackup::GetIOConfig(OC::IOConfig &ioconfig) const {
}
void AppBackup::DrawDebugInfo() const {
}

// Storage not used for this app
size_t AppBackup::SaveAppData(util::StreamBufferWriter &) const { return 0; }
size_t AppBackup::RestoreAppData(util::StreamBufferReader &) { return 0; }

FLASHMEM
void AppBackup::HandleAppEvent(OC::AppEvent event) {
  if (event == OC::APP_EVENT_RESUME) Resume();
  // Never leave a restore armed across a suspend or the screensaver: the
  // confirm screen would be gone but B would still start the overwrite.
  if (event == OC::APP_EVENT_SUSPEND || event == OC::APP_EVENT_SCREENSAVER_ON)
    Disarm();
}
void AppBackup::Loop() {} // Deprecated
FLASHMEM
void AppBackup::DrawScreensaver() const {
  View();
}
FLASHMEM
void AppBackup::HandleEncoderEvent(const UI::Event& event) {
  // Not while a restore is armed: the prompt names the region B is about to
  // overwrite, and a turn underneath it would make the prompt describe
  // something other than what B would do.
  if (restore_armed_) return;
  ToggleCalibration();
}
FLASHMEM
void AppBackup::HandleButtonEvent(const UI::Event& event) {
  // The armed screen wants the DOWN edges too -- that is where a chord is
  // still visible. Everywhere else only a completed press acts.
  if (restore_armed_) { HandleConfirmEvent(event); return; }
  if (event.type == UI::EVENT_BUTTON_PRESS) HandleButtonPress(event);
}
