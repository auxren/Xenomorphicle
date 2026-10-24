// Copyright (c) 2026, Oren Levy
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

// Cal Routine (design/cal-routine-design.md 6.2, 9.2): closed-loop
// calibration of the 8 CV outputs against a USB bench DMM on the host jack
// (Outputs), of the 8 CV inputs against the outputs patched A->1 ... H->8
// (Inputs, no meter), or both in turn (Self). The menu is the kMenu table
// below: later rows (bus modules) append there. The "259e pitch inputs" row
// runs Cal259eFlow (host-tested): the Xeno's output A and the USB meter
// calibrate a 259e's two pitch inputs over the 200e bus. Its screens are
// Draw259 (stage by stage), Draw259Confirm (the write and the slot reset, each
// a B press on a screen of its own) and Draw259Failed. The step machine is
// CalRoutineFlow (host-tested); this file is its screens and controls,
// CalRoutinePorts.cpp its hardware.
//
// Controls, on every screen: encL press = back / cancel (never destructive:
// a cancel releases every output and writes nothing; on Results, where it
// would throw away a finished run, it asks first and a second encL within
// 3 s discards), encR press = go on.
// A = go on without the empty input jacks after a failed patch check, or
// retry the output under the Results cursor. B on a failed patch check in
// Self = keep the outputs only (Results with the outputs part; nothing is
// written). The one write -- Save -- is a B press on a confirm screen of its
// own (Backup.h's two-step confirm: kConfirmDeadMs, B down solo, nothing
// else held), reached only from a Results screen that can save (every output
// passed; for Inputs, at least one input).
//
// Inversion marks what encR will act on (docs/ui-redesign/Established-Rules.md,
// L-06): the menu entry encR opens (either encoder turns the menu). The Results cursor, which encL moves and
// encR does not act on, is a leading '>'.
//
// Run time: the flow runs from Loop(); SUSPEND, SCREENSAVER_ON and the app
// switcher (which sends SUSPEND) cancel it, so no output stays held. The
// screensaver is held off while a run is open (only the Z+A chord starts it,
// and that cancels).
//
// Every member is defined out of class and FLASHMEM: none of it is ISR code,
// and every byte of ITCM comes out of DTCM in 32 KB steps.
#include "CalRoutinePorts.h"
#include "../CalRoutineFlow.h"

// The Cal Routine menu, one row per routine; a later routine (a bus module)
// is one more row and one more action.
enum CalRoutineMenuAction : uint8_t { CALR_MENU_OUTPUTS, CALR_MENU_INPUTS, CALR_MENU_SELF, CALR_MENU_259E };
struct CalRoutineMenuEntry {
  const char *name;  // the row, after one leading space (20 columns)
  const char *hint;  // the line under the list for the selected row (21 columns)
  uint8_t action;    // CalRoutineMenuAction
};
static const CalRoutineMenuEntry kCalRoutineMenu[] = {
  {"Outputs", "A-H with a USB meter", CALR_MENU_OUTPUTS},
  {"Inputs", "A>1 ... H>8, no meter", CALR_MENU_INPUTS},
  {"Self (both)", "outputs, then inputs", CALR_MENU_SELF},
  {"259e pitch inputs", "A>259e, USB meter", CALR_MENU_259E},
};
static constexpr int kCalRoutineMenuRows =
    int(sizeof(kCalRoutineMenu) / sizeof(kCalRoutineMenu[0]));

OC_APP_CLASS(AppCalRoutine, TWOCCS("CR"), "Cal Routine", "Calibration") {
public:
  OC_APP_INTERFACE_DECLARE(AppCalRoutine, 0);

protected:
  bool io_settings_allowed() const override;

private:
  using Stage = CalRoutineFlow::Stage;
  using Mode = CalRoutine::Mode;
  enum Screen : uint8_t { SCR_MENU, SCR_UNSUPPORTED, SCR_NOTICE, SCR_RUN, SCR_CONFIRM, SCR_SAVED };

  // Same value and reasoning as Backup.h / the 200e write confirm.
  static constexpr uint32_t kConfirmDeadMs = 350;
  static constexpr uint32_t kDiscardWindowMs = 3000;
  // Results rows: 0-7 outputs A-H, 8-15 inputs 1-8; a run shows the ones
  // its mode measured (RowFirst() .. RowLast()).
  static constexpr int kRows = 16;
  static constexpr int kListRows = 4;  // of them on screen at once

  Screen screen_ = SCR_MENU;
  Stage last_stage_ = Stage::Idle;
  Mode mode_ = Mode::Self;    // the routine chosen in the menu
  bool is259_ = false;        // the 259e row: Flow259e() runs, not Flow()
  Cal259eFlow::Stage last259_ = Cal259eFlow::Stage::Idle;
  int8_t menu_sel_ = 0;       // kCalRoutineMenu row
  int8_t menu_top_ = 0;       // first menu row on screen
  int8_t cursor_ = 0;         // Results row
  int8_t top_ = 0;            // first Results row on screen
  bool save_pending_ = false; // B committed; Loop() saves
  bool save_ok_ = false;
  uint32_t armed_ms_ = 0;     // millis() the confirm screen appeared
  // Results: encL armed the discard at this millis() (0 = not armed). A
  // second encL within kDiscardWindowMs (and past kConfirmDeadMs, so a
  // bounce or a double-tap is not a yes) discards the finished run.
  uint32_t discard_armed_ms_ = 0;
  bool b_down_solo_ = false;  // B went down alone, after that screen appeared

  void StartRun();
  void OpenMenuEntry();
  void EndRun(Screen next);
  int RowFirst() const;
  int RowLast() const;
  const char *ModeName() const;
  void ArmSave();
  void HandleConfirm(const UI::Event &event);
  void HandlePress(const UI::Event &event);
  void HandlePress259(const UI::Event &event);
  void Abort259();
  void End259();
  void MoveCursor(int delta);
  bool CursorOnFailedOutput() const;
  bool DiscardArmed() const;

  void Title(const char *text) const;
  void Row(int row, const char *text) const;
  void Footer(const char *text) const;
  void Bar(int row, int num, int den) const;
  void DrawMenuScreen() const;
  void DrawNotice() const;
  void DrawUnsupported() const;
  void DrawRun() const;
  void DrawInputStages(Stage s) const;
  void DrawPatchFailed() const;
  void DrawNoMeter() const;
  void DrawResults() const;
  void DrawFailed() const;
  void Draw259() const;
  void Draw259Results() const;
  void Draw259Failed() const;
  void Draw259Confirm() const;
  void DrawConfirm() const;
  void DrawSaved() const;
  void ResultRow(int r, char *buf, size_t n) const;
  void ResultDetail(int r, char *buf, size_t n) const;

  static void Centi(char *buf, size_t n, float v);    // 0.41 / 12.3 / 123
  static void Volts(char *buf, size_t n, int32_t mv); // 6.0
};

// ---- layout: title band, 5 rows, footer (21 x 8 at the 6x8 font) ------------

FLASHMEM bool AppCalRoutine::io_settings_allowed() const { return false; }

// Every line is cut at the 21 columns the panel has, never clipped mid-glyph.
FLASHMEM void AppCalRoutine::Title(const char *text) const {
  graphics.setPrintPos(0, 1);
  graphics.print(text, 21);
  graphics.drawLine(0, 10, 127, 10);
  graphics.drawLine(0, 12, 127, 12);
}

// Rows 0..4 at y = 15, 23, 31, 39, 47.
FLASHMEM void AppCalRoutine::Row(int row, const char *text) const {
  graphics.setPrintPos(0, 15 + 8 * row);
  graphics.print(text, 21);
}

FLASHMEM void AppCalRoutine::Footer(const char *text) const {
  graphics.setPrintPos(0, 56);
  graphics.print(text, 21);
}

FLASHMEM void AppCalRoutine::Bar(int row, int num, int den) const {
  const int y = 15 + 8 * row + 1;
  graphics.drawFrame(0, y, 128, 6);
  if (den > 0 && num > 0) {
    const int w = num >= den ? 124 : (124 * num) / den;
    if (w > 0) graphics.drawRect(2, y + 2, w, 2);
  }
}

FLASHMEM void AppCalRoutine::Centi(char *buf, size_t n, float v) {
  if (!(v >= 0.f)) v = 0.f;
  if (v < 9.995f) {
    const long c = long(v * 100.f + 0.5f);
    snprintf(buf, n, "%ld.%02ld", c / 100, c % 100);
  } else if (v < 99.95f) {
    const long d = long(v * 10.f + 0.5f);
    snprintf(buf, n, "%ld.%ld", d / 10, d % 10);
  } else {
    snprintf(buf, n, "%ld", long(v + 0.5f));
  }
}

FLASHMEM void AppCalRoutine::Volts(char *buf, size_t n, int32_t mv) {
  const long d = (long(mv) + (mv >= 0 ? 50 : -50)) / 100;  // tenths of a volt
  snprintf(buf, n, "%ld.%ld", d / 10, (d < 0 ? -d : d) % 10);
}

// ---- run ---------------------------------------------------------------------

FLASHMEM void AppCalRoutine::StartRun() {
  discard_armed_ms_ = 0;
  cursor_ = int8_t(mode_ == Mode::Inputs ? 8 : 0);
  top_ = cursor_;
  save_pending_ = false;
  CalRoutinePorts::Open();
  OC::ui.preempt_screensaver(true);
  if (is259_) {
    // The run needs Output A calibrated by Cal Routine (the marker), else the
    // flow stops at NoSelfCal.
    CalRoutinePorts::Flow259e().Begin(CalRoutinePorts::Meter(), CalRoutinePorts::Outputs(),
                                      CalRoutinePorts::Clock(), CalRoutinePorts::Bus(),
                                      CalRoutinePorts::Backup(), CalRoutinePorts::Entropy(),
                                      CalRoutinePorts::MarkerSet(), CalRoutinePorts::ConsoleLog);
    last259_ = CalRoutinePorts::Flow259e().stage();
    screen_ = SCR_RUN;
    return;
  }
  CalRoutinePorts::Flow().SetLog(CalRoutinePorts::ConsoleLog);
  // Inputs warns (and lets the owner go on) when the output tables it
  // measures against were not set by Cal Routine.
  CalRoutinePorts::Flow().Begin(CalRoutinePorts::Meter(), CalRoutinePorts::Outputs(), CalRoutinePorts::Inputs(),
              CalRoutinePorts::Clock(), mode_, CalRoutinePorts::MarkerSet());
  last_stage_ = CalRoutinePorts::Flow().stage();
  screen_ = SCR_RUN;
}

FLASHMEM void AppCalRoutine::OpenMenuEntry() {
  const uint8_t action = kCalRoutineMenu[menu_sel_].action;
  is259_ = action == CALR_MENU_259E;
  mode_ = action == CALR_MENU_OUTPUTS ? Mode::Outputs
        : action == CALR_MENU_INPUTS ? Mode::Inputs : Mode::Self;
  if (!CalRoutinePorts::HardwareSupported()) screen_ = SCR_UNSUPPORTED;
  else if (mode_ == Mode::Inputs) StartRun();  // no meter: the USB notice does not apply
  else screen_ = SCR_NOTICE;
}

FLASHMEM int AppCalRoutine::RowFirst() const { return mode_ == Mode::Inputs ? 8 : 0; }
FLASHMEM int AppCalRoutine::RowLast() const { return mode_ == Mode::Outputs ? 7 : kRows - 1; }

FLASHMEM const char *AppCalRoutine::ModeName() const {
  return is259_ ? "259e" : mode_ == Mode::Outputs ? "Outputs" : mode_ == Mode::Inputs ? "Inputs" : "Self";
}

// The flow is Idle (cancelled, discarded or saved) when this is called.
FLASHMEM void AppCalRoutine::EndRun(Screen next) {
  discard_armed_ms_ = 0;
  CalRoutinePorts::Close();
  OC::ui.preempt_screensaver(false);
  save_pending_ = false;
  screen_ = next;
}

// Cancel the 259e run from any screen. Never destructive: before the COMMIT
// nothing was written. A COMMIT already on the wire is waited for (the flow
// stays in Write and reports its status; the screen stays), and a cancel after
// a COMMIT verdict stays on the Failed screen, which says what the module may
// hold. Otherwise the run is dismissed and the menu returns.
FLASHMEM void AppCalRoutine::Abort259() {
  Cal259eFlow &f = CalRoutinePorts::Flow259e();
  f.Cancel();
  if (f.cancel_pending()) { screen_ = SCR_RUN; return; }
  if (f.stage() == Cal259eFlow::Stage::Failed && f.commit_result() != 0) { screen_ = SCR_RUN; return; }
  End259();
}

// Dismiss Done / Failed / NoSelfCal (and a cancel that wrote nothing) back to Idle.
FLASHMEM void AppCalRoutine::End259() {
  Cal259eFlow &f = CalRoutinePorts::Flow259e();
  if (f.stage() != Cal259eFlow::Stage::Idle) f.Cancel();
  if (f.stage() != Cal259eFlow::Stage::Idle) f.Cancel();
  EndRun(SCR_MENU);
}

FLASHMEM void AppCalRoutine::ArmSave() {
  screen_ = SCR_CONFIRM;
  armed_ms_ = millis();
  // Only a B press that BEGINS on this screen can commit.
  b_down_solo_ = false;
}

FLASHMEM bool AppCalRoutine::DiscardArmed() const {
  return discard_armed_ms_ && millis() - discard_armed_ms_ < kDiscardWindowMs;
}

FLASHMEM bool AppCalRoutine::CursorOnFailedOutput() const {
  return mode_ != Mode::Inputs && cursor_ < 8 && !CalRoutinePorts::Flow().results().out_pass[cursor_];
}

FLASHMEM void AppCalRoutine::MoveCursor(int delta) {
  int c = cursor_ + delta;
  if (c < RowFirst()) c = RowFirst();
  if (c > RowLast()) c = RowLast();
  cursor_ = int8_t(c);
  if (cursor_ < top_) top_ = cursor_;
  if (cursor_ > top_ + kListRows - 1) top_ = int8_t(cursor_ - (kListRows - 1));
}

// ---- interface -----------------------------------------------------------------

FLASHMEM void AppCalRoutine::Init() {
  screen_ = SCR_MENU;
}

FLASHMEM void AppCalRoutine::Loop() {
  if (is259_) {
    if (screen_ == SCR_RUN || screen_ == SCR_CONFIRM) {
      CalRoutinePorts::Service();
      CalRoutinePorts::Flow259e().Tick();
      const Cal259eFlow::Stage s = CalRoutinePorts::Flow259e().stage();
      if (s != last259_) {
        last259_ = s;
        discard_armed_ms_ = 0;
        OC::ui.Poke();
        // The two writes each get a confirm screen of their own.
        if (s == Cal259eFlow::Stage::Confirm || s == Cal259eFlow::Stage::ConfirmCompact) ArmSave();
        else if (screen_ == SCR_CONFIRM) screen_ = SCR_RUN;
      }
    }
    return;
  }
  if (screen_ == SCR_RUN || screen_ == SCR_CONFIRM) {
    CalRoutinePorts::Service();
    CalRoutinePorts::Flow().Tick();
    const Stage s = CalRoutinePorts::Flow().stage();
    if (s != last_stage_) {
      last_stage_ = s;
      discard_armed_ms_ = 0;
      OC::ui.Poke();  // a new prompt is activity: keep the panel lit for it
    }
  }
  if (save_pending_) {
    save_pending_ = false;
    // calibration_save() draws its own "saved" screen for a second.
    save_ok_ = CalRoutinePorts::Flow().Save(CalRoutinePorts::Save());
    if (save_ok_) EndRun(SCR_SAVED);
    else screen_ = SCR_SAVED;  // the flow stays on Results: try again, or discard
  }
}

FLASHMEM void AppCalRoutine::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
      if (screen_ != SCR_MENU) screen_ = SCR_MENU;
      break;
    case OC::APP_EVENT_SUSPEND:
    case OC::APP_EVENT_SCREENSAVER_ON:
      // Never leave an output held, a run half done or a save armed behind a
      // screen that is gone.
      if (is259_) {
        // A COMMIT already on the wire cannot be waited for with the app gone:
        // the module finishes it alone and its record is read by hand (=28s).
        CalRoutinePorts::Flow259e().Cancel();
        if (CalRoutinePorts::Flow259e().stage() != Cal259eFlow::Stage::Idle)
          CalRoutinePorts::Flow259e().Cancel();
      } else {
        CalRoutinePorts::Flow().Cancel();
      }
      EndRun(SCR_MENU);
      break;
    default:
      break;
  }
}

FLASHMEM void AppCalRoutine::HandleConfirm(const UI::Event &event) {
  if (event.control == OC::CONTROL_BUTTON_L && event.type == UI::EVENT_BUTTON_PRESS) {
    if (is259_) {
      // The measurements are minutes of work and cannot be got back to
      // Results: the first encL on the write confirm only asks (ConfirmCompact
      // is past the write: its encL cancels at once).
      if (CalRoutinePorts::Flow259e().stage() == Cal259eFlow::Stage::Confirm) {
        if (!DiscardArmed()) { discard_armed_ms_ = millis() | 1; return; }
        if (millis() - discard_armed_ms_ < kConfirmDeadMs) return;
      }
      Abort259();  // no: nothing more is written
    } else {
      screen_ = SCR_RUN;  // no: back to Results, nothing written
    }
    return;
  }
  if (event.control != OC::CONTROL_BUTTON_B) return;
  discard_armed_ms_ = 0;  // any B press is a no to the discard
  // The chord test belongs on the DOWN edge (Backup.h has the reasoning).
  if (event.type == UI::EVENT_BUTTON_DOWN) {
    b_down_solo_ = (event.mask == OC::CONTROL_BUTTON_B);
    return;
  }
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  const bool solo = b_down_solo_;
  b_down_solo_ = false;
  if (!solo || event.mask) return;
  if (millis() - armed_ms_ < kConfirmDeadMs) return;
  if (is259_) {
    Cal259eFlow &f = CalRoutinePorts::Flow259e();
    if (f.stage() == Cal259eFlow::Stage::Confirm) f.ConfirmWrite();
    else if (f.stage() == Cal259eFlow::Stage::ConfirmCompact) f.ConfirmCompact();
    screen_ = SCR_RUN;
    return;
  }
  if (!CalRoutinePorts::Flow().CanSave()) { screen_ = SCR_RUN; return; }
  save_pending_ = true;
}

FLASHMEM void AppCalRoutine::HandlePress(const UI::Event &event) {
  const bool l = event.control == OC::CONTROL_BUTTON_L;
  const bool r = event.control == OC::CONTROL_BUTTON_R;
  const bool a = event.control == OC::CONTROL_BUTTON_A;
  const bool b = event.control == OC::CONTROL_BUTTON_B;
  switch (screen_) {
    case SCR_MENU:
      if (r) OpenMenuEntry();
      break;
    case SCR_UNSUPPORTED:
      if (l || r) screen_ = SCR_MENU;
      break;
    case SCR_NOTICE:
      if (l) screen_ = SCR_MENU;
      else if (r) StartRun();
      break;
    case SCR_SAVED:
      if (!(l || r)) break;
      // After a failed save the results are still there to try again.
      screen_ = CalRoutinePorts::Flow().stage() == Stage::Results ? SCR_RUN : SCR_MENU;
      break;
    case SCR_RUN: {
      if (is259_) { HandlePress259(event); break; }
      const Stage s = CalRoutinePorts::Flow().stage();
      if (l) {
        if (s == Stage::Results) {
          // A finished run is minutes of measuring: the first encL only asks.
          if (!DiscardArmed()) {
            discard_armed_ms_ = millis() | 1;
            break;
          }
          if (millis() - discard_armed_ms_ < kConfirmDeadMs) break;
          CalRoutinePorts::Flow().Discard();
        } else if (s == Stage::Failed) {
          CalRoutinePorts::Flow().Discard();  // nothing measured is left to lose
        } else {
          CalRoutinePorts::Flow().Cancel();
        }
        EndRun(SCR_MENU);
        break;
      }
      discard_armed_ms_ = 0;  // any other press is a no
      if (s == Stage::PromptOutput || s == Stage::PromptPatch || s == Stage::WarnMarker ||
          s == Stage::PatchFailed) {
        if (r) CalRoutinePorts::Flow().Continue();  // PatchFailed: check again
        else if (a && s == Stage::PatchFailed) CalRoutinePorts::Flow().Skip();
        else if (b && s == Stage::PatchFailed && CalRoutinePorts::Flow().KeepOutputsOnly())
          mode_ = Mode::Outputs;  // the run is an Outputs run now: its rows, confirm, saved
      } else if (s == Stage::Results) {
        if (r && CalRoutinePorts::Flow().CanSave()) ArmSave();
        else if (a && CursorOnFailedOutput()) CalRoutinePorts::Flow().RetryOutput(cursor_);
      }
      break;
    }
    default:
      break;
  }
}

FLASHMEM void AppCalRoutine::HandlePress259(const UI::Event &event) {
  using S = Cal259eFlow::Stage;
  Cal259eFlow &f = CalRoutinePorts::Flow259e();
  const S s = f.stage();
  if (event.control == OC::CONTROL_BUTTON_L) {
    if (s == S::Results) {
      // A finished run is minutes of measuring: the first encL only asks.
      if (!DiscardArmed()) { discard_armed_ms_ = millis() | 1; return; }
      if (millis() - discard_armed_ms_ < kConfirmDeadMs) return;
      Abort259();
    } else if (s == S::Done || s == S::Failed || s == S::NoSelfCal) {
      End259();
    } else {
      Abort259();
    }
    return;
  }
  discard_armed_ms_ = 0;  // any other press is a no
  if (event.control != OC::CONTROL_BUTTON_R) return;
  if (s == S::Warmup || s == S::PromptPatch || s == S::Results) f.Continue();  // Results -> Confirm
  else if (s == S::Done) End259();
}

FLASHMEM void AppCalRoutine::HandleButtonEvent(const UI::Event &event) {
  if (screen_ == SCR_CONFIRM) { HandleConfirm(event); return; }
  if (event.type == UI::EVENT_BUTTON_PRESS) HandlePress(event);
}

FLASHMEM void AppCalRoutine::HandleEncoderEvent(const UI::Event &event) {
  if (screen_ == SCR_MENU) {
    int sel = menu_sel_ + event.value;
    if (sel < 0) sel = 0;
    if (sel > kCalRoutineMenuRows - 1) sel = kCalRoutineMenuRows - 1;
    menu_sel_ = int8_t(sel);
    if (menu_sel_ < menu_top_) menu_top_ = menu_sel_;
    if (menu_sel_ > menu_top_ + kListRows - 1) menu_top_ = int8_t(menu_sel_ - (kListRows - 1));
    return;
  }
  if (screen_ == SCR_RUN && CalRoutinePorts::Flow().stage() == Stage::Results &&
      event.control == OC::CONTROL_ENCODER_L)
    MoveCursor(event.value);
}

// ---- screens -----------------------------------------------------------------

FLASHMEM void AppCalRoutine::DrawMenuScreen() const {
  char b[24];
  Title("Cal Routine");
  for (int k = 0; k < kListRows; ++k) {
    const int i = menu_top_ + k;
    if (i >= kCalRoutineMenuRows) break;
    snprintf(b, sizeof(b), " %s", kCalRoutineMenu[i].name);
    Row(k, b);
    if (i == menu_sel_) graphics.invertRect(0, 14 + 8 * k, 128, 9);  // encR opens it
  }
  Row(4, kCalRoutineMenu[menu_sel_].hint);
  graphics.drawLine(0, 46, 127, 46);
  Footer("               R:open");
}

FLASHMEM void AppCalRoutine::DrawUnsupported() const {
  char b[24];
  snprintf(b, sizeof(b), "Cal Routine: %s", ModeName());
  Title(b);
  Row(0, "Needs a 1.2V/oct unit");
  Row(1, "with 0V at entry 0");
  Row(2, "(Xenomorpher). This");
  Row(3, "one is not: nothing");
  Row(4, "was changed.");
  Footer("L:back");
}

FLASHMEM void AppCalRoutine::DrawNotice() const {
  char b[24];
  snprintf(b, sizeof(b), "Cal Routine: %s", ModeName());
  Title(b);
  Row(0, "Meter alone on the");
  Row(1, "host jack: no hub, no");
  Row(2, "other USB devices.");
  Row(3, "Don't touch its USB");
  Row(4, "cable while it runs.");
  Footer("L:back           R:ok");
}

FLASHMEM void AppCalRoutine::DrawNoMeter() const {
  char b[24];
  if (CalRoutinePorts::Flow().retry_output() >= 0) snprintf(b, sizeof(b), "Retry: meter");
  else snprintf(b, sizeof(b), "%s: meter", ModeName());
  Title(b);
  if (CalRoutinePorts::Flow().meter_lost()) {
    const uint32_t gone = CalRoutinePorts::Flow().lost_for_ms();
    const uint32_t left = gone >= CalRoutine::kReconnectMs ? 0 : (CalRoutine::kReconnectMs - gone + 999) / 1000;
    Row(0, "Meter lost: plug it");
    Row(1, "back in.");
    snprintf(b, sizeof(b), "Stops in %lu s.", (unsigned long)left);
    Row(2, b);
    if (CalRoutinePorts::Flow().current_output() >= 0) {
      snprintf(b, sizeof(b), "Output %c restarts.", 'A' + CalRoutinePorts::Flow().current_output());
      Row(3, b);
    }
  } else {
    Row(0, "Plug the USB meter");
    Row(1, "into the host jack.");
    if (CalRoutinePorts::Status() == CalRoutinePorts::METER_ASKING)
      Row(3, "Found: asking *IDN?");
  }
  Footer("L:cancel");
}

FLASHMEM void AppCalRoutine::DrawRun() const {
  if (is259_) { Draw259(); return; }
  char b[24], v[12];
  const Stage s = CalRoutinePorts::Flow().stage();
  const int o = CalRoutinePorts::Flow().current_output();
  const bool retry = CalRoutinePorts::Flow().retry_output() >= 0;
  const char *cancel = retry ? "L:cancel all" : "L:cancel";
  switch (s) {
    case Stage::NoMeter:
      DrawNoMeter();
      return;
    case Stage::ConfigMeter:
      if (retry) Title("Retry: meter");
      else {
        snprintf(b, sizeof(b), "%s: meter", ModeName());
        Title(b);
      }
      snprintf(b, sizeof(b), "Meter: %.14s", CalRoutinePorts::MeterModel());
      Row(0, b);
      Row(1, "Setting it up...");
      Footer(cancel);
      return;
    case Stage::PromptOutput:
      if (retry) snprintf(b, sizeof(b), "Retry output %c", 'A' + o);
      else snprintf(b, sizeof(b), "Output %c  (%d/8)", 'A' + o, o + 1);
      Title(b);
      snprintf(b, sizeof(b), "Plug output %c into", 'A' + o);
      Row(0, b);
      Row(1, "the meter:");
      snprintf(b, sizeof(b), "+ to %c, COM to ground", 'A' + o);
      Row(2, b);
      snprintf(b, sizeof(b), "Nothing else on %c.", 'A' + o);
      Row(3, b);
      Footer(retry ? "L:cancel all R:start" : "L:cancel     R:start");
      return;
    case Stage::CalOutput:
    case Stage::SweepOutput: {
      if (retry) snprintf(b, sizeof(b), "Retry output %c", 'A' + o);
      else snprintf(b, sizeof(b), "Output %c  (%d/8)", 'A' + o, o + 1);
      Title(b);
      const int k = CalRoutinePorts::Flow().node_index();
      const int total = CalRoutineMath::kNodes + CalRoutine::kSweepPoints;
      int done = 0;
      if (s == Stage::CalOutput) {
        Volts(v, sizeof(v), k * CalRoutineMath::kNodeMv);
        snprintf(b, sizeof(b), "Setting %d/11: %sV", k + 1, v);
        done = k;
      } else {
        snprintf(b, sizeof(b), "Checking %d/%d", CalRoutinePorts::Flow().sweep_index() + 1, CalRoutinePorts::Flow().sweep_points());
        done = CalRoutineMath::kNodes + CalRoutinePorts::Flow().sweep_index();
      }
      Row(0, b);
      Bar(1, done, total);
      Row(3, "Leave the cables be.");
      Footer(cancel);
      return;
    }
    case Stage::WarnMarker:
    case Stage::PromptPatch:
    case Stage::CheckPatch:
    case Stage::CalInput:
      DrawInputStages(s);
      return;
    case Stage::PatchFailed:
      DrawPatchFailed();
      return;
    case Stage::Results:
      DrawResults();
      return;
    case Stage::Failed:
      DrawFailed();
      return;
    case Stage::Idle:
      Title("Cal Routine");
      return;
  }
}

// The input part: no meter, every output patched into its own input.
FLASHMEM void AppCalRoutine::DrawInputStages(Stage s) const {
  char b[24], v[12];
  const CalRoutineFlow &f = CalRoutinePorts::Flow();
  switch (s) {
    case Stage::WarnMarker:
      Title("Inputs: warning");
      Row(0, "The outputs were not");
      Row(1, "set by Cal Routine:");
      Row(2, "inputs will be only");
      Row(3, "as good as they are.");
      Row(4, "Outputs first is best");
      Footer("L:cancel    R:go on");
      return;
    case Stage::PromptPatch:
      Title("Inputs: patch");
      Row(0, "Patch outputs into");
      Row(1, "inputs: A>1 B>2 C>3");
      Row(2, "D>4 E>5 F>6 G>7 H>8");
      Row(3, "Nothing else on them");
      Row(4, "Meter not needed.");
      Footer("L:cancel      R:check");
      return;
    case Stage::CheckPatch: {
      Title("Inputs: patch check");
      const int o = f.patch_index();
      if (o < 0) snprintf(b, sizeof(b), "All outputs low");
      else snprintf(b, sizeof(b), "Raising output %c", 'A' + o);
      Row(0, b);
      Bar(1, o + 1, 9);
      Row(3, "Leave the cables be.");
      Footer("L:cancel");
      return;
    }
    default: {  // CalInput
      Title("Inputs 1-8");
      Volts(v, sizeof(v), f.input_mv());
      snprintf(b, sizeof(b), "All at %sV, pt %d", v, f.input_points() + 1);
      Row(0, b);
      Bar(1, f.input_mv(), 10800);
      Row(3, "Leave the cables be.");
      Footer("L:cancel");
      return;
    }
  }
}

// What the patch check found, one line per output that is wrong, by panel
// name: "C reaches 5, not 3" (a crossed or stacked cable) or "D: input 4
// silent" (nothing in the jack, or a dead cable).
FLASHMEM void AppCalRoutine::DrawPatchFailed() const {
  const CalRoutineFlow &f = CalRoutinePorts::Flow();
  char b[40], list[20];  // Row() cuts every line at 21 columns
  Title("Patch check FAILED");
  const bool self = f.mode() == Mode::Self;
  if (f.patch_no_reading()) {
    Row(0, "No input reading:");
    Row(1, "the inputs gave no");
    Row(2, "samples. Nothing was");
    Row(3, "judged; check again.");
    if (self) Row(4, "B: outputs only");
    Footer("L:cancel     R:again");
    return;
  }
  const uint8_t bad = uint8_t(f.patch_crossed() | f.patch_missing());
  int nbad = 0;
  for (int o = 0; o < 8; ++o) nbad += (bad >> o) & 1;
  const int shown = nbad <= 4 ? nbad : 3;  // else the 4th row says how many more
  int row = 0;
  for (int o = 0; o < 8 && row < shown; ++o) {
    if (!(bad & (1u << o))) continue;
    const uint8_t moved = f.patch_moved(o);
    if (!moved) {
      snprintf(b, sizeof(b), "%c: input %d silent", 'A' + o, o + 1);
    } else {
      int count = 0;
      for (int i = 0; i < 8; ++i) count += (moved >> i) & 1;
      if (count >= 3) {
        // A list would not fit: "C reaches 8 inputs" (18 columns at most).
        snprintf(b, sizeof(b), "%c reaches %d inputs", 'A' + o, count);
      } else {
        // At most "C reaches 1,2, not 3" (20 columns).
        int len = 0;
        list[0] = 0;
        for (int i = 0; i < 8; ++i)
          if (moved & (1u << i))
            len += snprintf(list + len, sizeof(list) - size_t(len), len ? ",%d" : "%d", i + 1);
        if (moved & (1u << o)) snprintf(b, sizeof(b), "%c reaches %s", 'A' + o, list);
        else snprintf(b, sizeof(b), "%c reaches %s, not %d", 'A' + o, list, o + 1);
      }
    }
    Row(row++, b);
  }
  if (nbad > shown) {
    snprintf(b, sizeof(b), "...and %d more", nbad - shown);
    Row(3, b);
  }
  // Row 4: in Self, B keeps the measured outputs; otherwise the way on.
  if (!f.patch_crossed()) {
    Row(4, self ? "B: outputs only" : "A: skip silent ones");
    Footer("L:stop A:skip R:again");
  } else {
    Row(4, self ? "B: outputs only" : "Fix, then R:again.");
    Footer("L:cancel     R:again");
  }
}

FLASHMEM void AppCalRoutine::DrawFailed() const {
  Title("Run stopped");
  const char *why = CalRoutinePorts::Flow().fail_reason();
  if (!strcmp(why, "METER_LOST")) {
    Row(0, "The meter was gone");
    Row(1, "for 20 s.");
  } else if (!strcmp(why, "METER_CONFIG")) {
    Row(0, "The meter refused a");
    Row(1, "setup command.");
  } else {
    Row(0, "The meter stopped");
    Row(1, "answering.");
  }
  Row(2, "All outputs released.");
  Row(3, "Nothing was saved.");
  Footer("L:back");
}

// One Results row, 20 columns after the cursor mark.
FLASHMEM void AppCalRoutine::ResultRow(int r, char *buf, size_t n) const {
  const CalRoutineFlow::Results &res = CalRoutinePorts::Flow().results();
  char m[12];
  if (r < 8) {
    bool rail = false;
    for (int k = 0; k < CalRoutineMath::kNodes; ++k) rail |= res.at_rail[r][k];
    Centi(m, sizeof(m), res.out_worst_mv[r]);
    if (res.out_pass[r]) {
      snprintf(buf, n, "%c ok   %smV%s", 'A' + r, m, rail ? " rail" : "");
    } else if (res.out_reason[r] == CalRoutineFlow::OUT_RESIDUAL) {
      snprintf(buf, n, "%c FAIL %smV", 'A' + r, m);
    } else {
      snprintf(buf, n, "%c FAIL %s", 'A' + r, CalRoutineFlow::OutReasonName(res.out_reason[r]));
    }
    return;
  }
  const int i = r - 8;
  if (res.in_skipped & (1u << i)) {
    snprintf(buf, n, "%d skipped", i + 1);
  } else if (res.in_pass[i]) {
    Centi(m, sizeof(m), res.in_holdout_mv[i]);
    snprintf(buf, n, "%d ok   %smV", i + 1, m);
  } else {
    snprintf(buf, n, "%d FAIL %s", i + 1, CvMeterMath::VerdictName(res.in_verdict[i]));
  }
}

// The line under the list: everything about the row under the cursor that
// its 20 columns could not hold.
FLASHMEM void AppCalRoutine::ResultDetail(int r, char *buf, size_t n) const {
  const CalRoutineFlow::Results &res = CalRoutinePorts::Flow().results();
  char c[12], v[12], lo[12], hi[12];
  if (r < 8) {
    Centi(c, sizeof(c), res.out_worst_mv[r]);
    Volts(v, sizeof(v), res.out_worst_at_mv[r]);
    if (res.out_reason[r] == CalRoutineFlow::OUT_NOISY) {
      snprintf(buf, n, "loose lead? A:retry");
      return;
    }
    if (res.out_reason[r] == CalRoutineFlow::OUT_NOT_MONOTONE ||
        res.out_reason[r] == CalRoutineFlow::OUT_SLOPE) {
      snprintf(buf, n, "bad table: A:retry");
      return;
    }
    // Entries the DAC cannot reach: kept at the rail, named by voltage. With
    // rails the line gives up the worst point's voltage to fit 21 columns:
    // "0.19c rail 10.8,12.0V" is the longest (the worst figure is <= 4 chars).
    int rails[CalRoutineMath::kNodes];
    int nr = 0;
    for (int k = 0; k < CalRoutineMath::kNodes; ++k)
      if (res.at_rail[r][k]) rails[nr++] = k;
    if (nr == 0) {
      snprintf(buf, n, "%sc@%sV", c, v);
    } else if (nr == 1) {
      Volts(lo, sizeof(lo), rails[0] * CalRoutineMath::kNodeMv);
      snprintf(buf, n, "%sc rail %sV", c, lo);
    } else if (nr == 2) {
      Volts(lo, sizeof(lo), rails[0] * CalRoutineMath::kNodeMv);
      Volts(hi, sizeof(hi), rails[1] * CalRoutineMath::kNodeMv);
      snprintf(buf, n, "%sc rail %s,%sV", c, lo, hi);
    } else {
      snprintf(buf, n, "%sc %d rail entries", c, nr);
    }
    return;
  }
  const int i = r - 8;
  if (res.in_skipped & (1u << i)) {
    snprintf(buf, n, "uncalibrated if saved");
    return;
  }
  Volts(lo, sizeof(lo), res.in_lo_mv[i]);
  Volts(hi, sizeof(hi), res.in_hi_mv[i]);
  if (res.in_pass[i]) {
    Centi(c, sizeof(c), res.in_holdout_mv[i]);
    snprintf(buf, n, "%sc %s-%sV", c, lo, hi);
  } else if (res.in_points[i]) {
    snprintf(buf, n, "%s-%sV: uncal.", lo, hi);
  } else {
    snprintf(buf, n, "no reading: uncal.");
  }
}

FLASHMEM void AppCalRoutine::DrawResults() const {
  const CalRoutineFlow::Results &res = CalRoutinePorts::Flow().results();
  char b[24], line[24];
  if (res.HasOutputs()) {
    int failed = 0;
    for (int o = 0; o < 8; ++o) failed += !res.out_pass[o];
    if (!failed) Title("Results: can save");
    else {
      snprintf(b, sizeof(b), "Results: %d out FAIL", failed);
      Title(b);
    }
  } else {
    int ok = 0;
    for (int i = 0; i < 8; ++i) ok += res.in_pass[i];
    if (ok) snprintf(b, sizeof(b), "Results: %d/8 in ok", ok);
    else snprintf(b, sizeof(b), "Results: no input ok");
    Title(b);
  }
  for (int k = 0; k < kListRows; ++k) {
    const int r = top_ + k;
    if (r > RowLast()) break;
    ResultRow(r, b, sizeof(b));
    snprintf(line, sizeof(line), "%c%s", r == cursor_ ? '>' : ' ', b);
    Row(k, line);
  }
  if (DiscardArmed()) {
    Row(4, "Press again: discard");
    graphics.invertRect(0, 46, 128, 9);  // the banner the next encL acts on
  } else {
    ResultDetail(cursor_, b, sizeof(b));
    Row(4, b);
  }
  graphics.drawLine(0, 46, 127, 46);
  if (CursorOnFailedOutput()) Footer("L:discard    A:retry");
  else if (CalRoutinePorts::Flow().CanSave()) Footer("L:discard   R:save...");
  else Footer("L:discard");
}

FLASHMEM void AppCalRoutine::DrawConfirm() const {
  if (is259_) { Draw259Confirm(); return; }
  const CalRoutineFlow::Results &res = CalRoutinePorts::Flow().results();
  char b[24];
  graphics.setPrintPos(0, 2);
  graphics.print("SAVE CALIBRATION");
  graphics.invertRect(0, 0, 128, 11);  // inversion is the only emphasis
  int ok = 0;
  uint8_t uncal = 0;
  for (int i = 0; i < 8; ++i) {
    if (res.in_pass[i]) ++ok;
    else uncal = uint8_t(uncal | (1u << i));
  }
  char u[24];
  u[0] = 0;
  if (uncal == 0xFF) {
    snprintf(u, sizeof(u), "Uncal: all");  // 8 numbers with spaces would be 22 columns
  } else if (uncal) {
    int len = snprintf(u, sizeof(u), "Uncal:");
    for (int i = 0; i < 8 && len > 0 && size_t(len) < sizeof(u); ++i)
      if (uncal & (1u << i)) len += snprintf(u + len, sizeof(u) - size_t(len), " %d", i + 1);
  }
  snprintf(b, sizeof(b), "Inputs: %d new curves", ok);
  int row = 0;
  if (!res.HasInputs()) {
    Row(row++, "Outputs A-H: new.");
    Row(row++, "Inputs: unchanged.");
  } else if (!res.HasOutputs()) {
    Row(row++, b);
    if (u[0]) Row(row++, u);
    Row(row++, "Outputs: unchanged.");
  } else {
    Row(row++, "Outputs A-H: new.");
    Row(row++, b);
    if (u[0]) Row(row++, u);
  }
  Row(row++, "Replaces the stored");
  Row(row, "calibration.");
  // The commit sits where no other screen of this app puts anything.
  graphics.setPrintPos(0, 56);
  graphics.print("encL:no");
  graphics.setPrintPos(84, 56);
  graphics.print("B:SAVE");
}

FLASHMEM void AppCalRoutine::DrawSaved() const {
  if (save_ok_) {
    Title("Saved");
    if (mode_ == Mode::Outputs) {
      Row(0, "Outputs A-H now use");
      Row(1, "this calibration in");
      Row(2, "every app.");
    } else if (mode_ == Mode::Inputs) {
      Row(0, "The passed inputs");
      Row(1, "now use this");
      Row(2, "calibration in every");
      Row(3, "app.");
    } else {
      Row(0, "Outputs A-H and the");
      Row(1, "passed inputs now");
      Row(2, "use this calibration");
      Row(3, "in every app.");
    }
    Footer("               R:done");
    return;
  }
  Title("SAVE FAILED");
  const char *at = CalRoutinePorts::SaveFailedAt();
  if (!strcmp(at, "CURVES") && mode_ == Mode::Self) {
    Row(0, "Output tables saved;");
    Row(1, "input curve file NOT");
    Row(2, "written: the inputs");
    Row(3, "keep their old cal.");
  } else {
    Row(0, "Nothing was written.");
  }
  Footer("R:back to results");
}


// ---- the 259e target's screens ---------------------------------------------------

FLASHMEM void AppCalRoutine::Draw259() const {
  using S = Cal259eFlow::Stage;
  const Cal259eFlow &f = CalRoutinePorts::Flow259e();
  char b[24], v[12];
  const int ch = f.channel() == 1 ? 1 : 0;
  char head[24];
  snprintf(head, sizeof(head), "259e %s (%d/2)", ch ? "MOD PITCH" : "PITCH", ch + 1);
  switch (f.stage()) {
    case S::Idle:
      Title("259e");
      return;
    case S::NoMeter:
      Title("259e: meter");
      Row(0, "Plug the USB meter");
      Row(1, "into the host jack.");
      if (CalRoutinePorts::Status() == CalRoutinePorts::METER_ASKING) Row(3, "Found: asking *IDN?");
      Footer("L:cancel");
      return;
    case S::NoSelfCal:
      Title("259e: not ready");
      Row(0, "Output A is not yet");
      Row(1, "calibrated by Cal");
      Row(2, "Routine. Run Outputs");
      Row(3, "(or Self) first.");
      Footer("L:back");
      return;
    case S::Scan:
      Title("259e: scan");
      Row(0, "Setting up the meter,");
      Row(1, "looking for a 259e");
      Row(2, "at 28-2B...");
      Footer("L:cancel");
      return;
    case S::Warmup:
      Title("259e: warm-up");
      Row(0, "Xeno + 259e on");
      Row(1, "30 min?");
      Row(3, "A cold module drifts");
      Row(4, "after the fit.");
      Footer("L:cancel      R:yes");
      return;
    case S::PromptPatch:
      Title(head);
      Row(0, ch ? "Out A>259e MOD PITCH" : "Out A>259e PITCH");
      Row(1, "Meter + at that jack,");
      Row(2, "COM on 259e ground.");
      Row(3, "Nothing else on A.");
      Footer("L:cancel   R:patched");
      return;
    case S::Identify:
      Title(head);
      Row(0, "Which 259e is it?");
      Row(1, "Raising output A...");
      Row(3, "Leave the cables be.");
      Footer("L:cancel");
      return;
    case S::Measure: {
      Title(head);
      Volts(v, sizeof(v), f.point_mv());
      const int pt = f.point_index();
      snprintf(b, sizeof(b), "%s %d/%d: %sV", pt < Cal259eFlow::kFitPoints ? "Fit" : "Verify",
               pt < Cal259eFlow::kFitPoints ? pt + 1 : pt - Cal259eFlow::kFitPoints + 1,
               pt < Cal259eFlow::kFitPoints ? Cal259eFlow::kFitPoints : Cal259eFlow::kVerifyPoints, v);
      Row(0, b);
      Bar(1, pt + 1, Cal259eFlow::kPoints);
      Row(3, "Leave the cables be.");
      Footer("L:cancel");
      return;
    }
    case S::Results:
      Draw259Results();
      return;
    case S::Confirm:
    case S::ConfirmCompact:
      // SCR_CONFIRM draws these; a frame between the stage change and the
      // screen change shows the results behind.
      Draw259Results();
      return;
    case S::Backup:
      Title("259e: backup");
      snprintf(b, sizeof(b), "Backing up 259e @%02X", f.target());
      Row(0, b);
      Row(1, "presets first...");
      Row(3, "Nothing written yet.");
      Footer("L:cancel");
      return;
    case S::Write:
      Title("259e: writing");
      snprintf(b, sizeof(b), "to 259e @%02X...", f.target());
      Row(0, "Writing the record");
      Row(1, b);
      Row(2, "Do not power off.");
      Footer("L:cancel");
      return;
    case S::Verify:
      Title("259e: verify");
      Row(0, "Reading the record");
      Row(1, "back...");
      Footer("L:cancel");
      return;
    case S::Done:
      Title("259e calibrated");
      snprintf(b, sizeof(b), "Record written to @%02X", f.target());
      Row(0, b);
      Row(1, "and read back");
      Row(2, "byte-exact.");
      Row(3, "Log: CALR,259REC");
      Footer("               R:done");
      return;
    case S::Failed:
      Draw259Failed();
      return;
  }
}

FLASHMEM void AppCalRoutine::Draw259Results() const {
  const Cal259eFlow &f = CalRoutinePorts::Flow259e();
  const Cal259eFlow::Result &res = f.result();
  char b[24], w[12], n[12];
  Title("259e results");
  for (int c = 0; c < 2; ++c) {
    Centi(w, sizeof(w), fabsf(res.worst_verify_cents[c]));
    snprintf(b, sizeof(b), "%-9s worst %sc", c ? "MOD PITCH" : "PITCH", w);
    Row(2 * c, b);
    Centi(n, sizeof(n), res.nonlin_cents[c]);
    const float pct = (float(res.ch[c].gain) / float(Cal259eFlow::kNominalGain) - 1.f) * 100.f;
    snprintf(b, sizeof(b), " nl %sc gain %+.1f%%", n, double(pct));
    Row(2 * c + 1, b);
  }
  if (DiscardArmed()) {
    Row(4, "Press again: discard");
    graphics.invertRect(0, 46, 128, 9);  // the banner the next encL acts on
  } else {
    snprintf(b, sizeof(b), "Target: 259e @%02X", f.target());
    Row(4, b);
  }
  graphics.drawLine(0, 46, 127, 46);
  Footer("L:discard   R:write...");
}

FLASHMEM void AppCalRoutine::Draw259Failed() const {
  using F = Cal259eFlow;
  const F &f = CalRoutinePorts::Flow259e();
  const F::FailCode code = f.fail();
  const char *l1 = "", *l2 = "";
  switch (code) {
    case F::F_NO_259E: l1 = "No 259e answered at"; l2 = "28-2B. On 31.2C4?"; break;
    case F::F_SEVERAL_MOVED: l1 = "More than one 259e"; l2 = "moved: patch one."; break;
    case F::F_IDENTITY: l1 = "No 259e saw A rise."; l2 = "Wrong jack patched?"; break;
    case F::F_PATCH: l1 = "Meter and held volts"; l2 = "differ by > 50 mV."; break;
    case F::F_NOISY: l1 = "Meter reading noisy"; l2 = "(loose lead?)"; break;
    case F::F_SCALE: l1 = "Gain is > 10% off"; l2 = "stock: not written."; break;
    case F::F_FIT: l1 = "The fit failed (an"; l2 = "offset out of range)."; break;
    case F::F_VERIFY: l1 = "Verify residual"; l2 = "is over 3 cents."; break;
    case F::F_BACKUP: l1 = "Preset backup failed"; l2 = "so nothing written."; break;
    case F::F_BUS: l1 = "200e bus error or"; l2 = "no reply from 259e."; break;
    case F::F_COMMIT_REFUSED: l1 = "The 259e refused"; l2 = "the record (=28s)."; break;
    case F::F_COMMIT_UNKNOWN: l1 = "COMMIT unknown: read"; l2 = "=28s and =28r."; break;
    case F::F_READBACK: l1 = "Read-back differs:"; l2 = "read =28r by hand."; break;
    case F::F_CANCELLED: l1 = "Cancelled."; break;
    case F::F_METER_LOST: l1 = "The meter was lost."; break;
    case F::F_METER_CONFIG: l1 = "The meter refused a"; l2 = "setup command."; break;
    case F::F_METER_TIMEOUT: l1 = "The meter stopped"; l2 = "answering."; break;
    case F::F_NO_METER: l1 = "No meter."; break;
    case F::F_NO_SELF_CAL: l1 = "Run Outputs first."; break;
    default: break;
  }
  Title(code == F::F_CANCELLED ? "Run cancelled" : "Run stopped");
  char b[24];
  snprintf(b, sizeof(b), "%.21s", F::FailName(code));
  Row(0, b);
  Row(1, l1);
  Row(2, l2);
  // commit_result(): 1 ok, E_NO_SLOT and E_COMMIT_REFUSED mean the module
  // did not take the record; any other non-zero code leaves it unknown.
  const int cr = f.commit_result();
  if (cr == 1) Row(3, "Record committed.");
  else if (cr == 0 || cr == E_NO_SLOT || cr == E_COMMIT_REFUSED) Row(3, "Nothing was written.");
  else Row(3, "Written: UNKNOWN.");
  Row(4, "Output A released.");
  Footer("L:back");
}

FLASHMEM void AppCalRoutine::Draw259Confirm() const {
  const Cal259eFlow &f = CalRoutinePorts::Flow259e();
  char b[24];
  graphics.setPrintPos(0, 2);
  if (f.stage() == Cal259eFlow::Stage::ConfirmCompact) {
    graphics.print("RESET 259e SLOTS?");
    graphics.invertRect(0, 0, 128, 11);
    Row(0, "All 8 slots used.");
    Row(1, "Reset slots? Audio");
    Row(2, "drops briefly. Power");
    Row(3, "loss now = presets");
    Row(4, "reinit. Backup taken.");
    graphics.setPrintPos(0, 56);
    graphics.print("encL:no");
    graphics.setPrintPos(78, 56);
    graphics.print("B:RESET");
    return;
  }
  snprintf(b, sizeof(b), "WRITE 259e @%02X?", f.target());
  graphics.print(b);
  graphics.invertRect(0, 0, 128, 11);  // inversion is the only emphasis
  Row(0, "Write calibration to");
  Row(1, "both pitch inputs.");
  Row(2, "Presets are backed");
  Row(3, "up first, then the");
  if (DiscardArmed()) {
    Row(4, "encL again: discard");
    graphics.invertRect(0, 46, 128, 9);
  } else {
    Row(4, "record is read back.");
  }
  graphics.setPrintPos(0, 56);
  graphics.print("encL:no");
  graphics.setPrintPos(84, 56);
  graphics.print("B:WRITE");
}

FLASHMEM void AppCalRoutine::DrawMenu() const {
  switch (screen_) {
    case SCR_MENU: DrawMenuScreen(); break;
    case SCR_UNSUPPORTED: DrawUnsupported(); break;
    case SCR_NOTICE: DrawNotice(); break;
    case SCR_RUN: DrawRun(); break;
    case SCR_CONFIRM: DrawConfirm(); break;
    case SCR_SAVED: DrawSaved(); break;
  }
}

FLASHMEM void AppCalRoutine::DrawScreensaver() const {
  // Only reachable by the Z+A chord, which cancels a run first: nothing to show.
}

FLASHMEM void AppCalRoutine::Process(OC::IOFrame *) {
}
FLASHMEM void AppCalRoutine::GetIOConfig(OC::IOConfig &) const {
}
FLASHMEM void AppCalRoutine::DrawDebugInfo() const {
}

// No app data: everything the routine stores is calibration (EEPROM pages,
// the curve files and the marker), written only by Save.
FLASHMEM size_t AppCalRoutine::SaveAppData(util::StreamBufferWriter &) const { return 0; }
FLASHMEM size_t AppCalRoutine::RestoreAppData(util::StreamBufferReader &) { return 0; }
