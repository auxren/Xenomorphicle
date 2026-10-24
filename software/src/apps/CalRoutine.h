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

// Cal Routine (design/cal-routine-design.md 6.2): closed-loop calibration of
// the 8 CV outputs and the 8 CV inputs with a USB bench DMM on the host jack.
// The step machine is CalRoutineFlow (host-tested); this file is its screens
// and controls, CalRoutinePorts.cpp its hardware.
//
// Controls, on every screen: encL press = back / cancel (never destructive:
// a cancel releases every output and writes nothing; on Results, where it
// would throw away a finished run, it asks first and a second encL within
// 3 s discards), encR press = go on.
// A = skip an input at its prompt, or retry the output under the Results
// cursor. The one write -- Save -- is a B press on a confirm screen of its
// own (Backup.h's two-step confirm: kConfirmDeadMs, B down solo, nothing
// else held), reached only from a Results screen whose outputs all passed.
//
// Inversion marks what encR will act on (docs/ui-redesign/Established-Rules.md,
// L-06): the menu entry encR opens. The Results cursor, which encL moves and
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

OC_APP_CLASS(AppCalRoutine, TWOCCS("CR"), "Cal Routine", "Calibration") {
public:
  OC_APP_INTERFACE_DECLARE(AppCalRoutine, 0);

protected:
  bool io_settings_allowed() const override;

private:
  using Stage = CalRoutineFlow::Stage;
  enum Screen : uint8_t { SCR_MENU, SCR_UNSUPPORTED, SCR_NOTICE, SCR_RUN, SCR_CONFIRM, SCR_SAVED };

  // Same value and reasoning as Backup.h / the 200e write confirm.
  static constexpr uint32_t kConfirmDeadMs = 350;
  static constexpr uint32_t kDiscardWindowMs = 3000;
  static constexpr int kRows = 16;     // Results: outputs A-H, then inputs 1-8
  static constexpr int kListRows = 4;  // of them on screen at once

  Screen screen_ = SCR_MENU;
  Stage last_stage_ = Stage::Idle;
  uint8_t skipped_ = 0;       // inputs skipped at their prompt (panel mask)
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
  void EndRun(Screen next);
  void ArmSave();
  void HandleConfirm(const UI::Event &event);
  void HandlePress(const UI::Event &event);
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
  void DrawNoMeter() const;
  void DrawResults() const;
  void DrawFailed() const;
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
  skipped_ = 0;
  discard_armed_ms_ = 0;
  cursor_ = 0;
  top_ = 0;
  save_pending_ = false;
  CalRoutinePorts::Open();
  OC::ui.preempt_screensaver(true);
  CalRoutinePorts::Flow().SetLog(CalRoutinePorts::ConsoleLog);
  CalRoutinePorts::Flow().Begin(CalRoutinePorts::Meter(), CalRoutinePorts::Outputs(), CalRoutinePorts::Inputs(),
              CalRoutinePorts::Clock());
  last_stage_ = CalRoutinePorts::Flow().stage();
  screen_ = SCR_RUN;
}

// The flow is Idle (cancelled, discarded or saved) when this is called.
FLASHMEM void AppCalRoutine::EndRun(Screen next) {
  discard_armed_ms_ = 0;
  CalRoutinePorts::Close();
  OC::ui.preempt_screensaver(false);
  save_pending_ = false;
  screen_ = next;
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
  return cursor_ < 8 && !CalRoutinePorts::Flow().results().out_pass[cursor_];
}

FLASHMEM void AppCalRoutine::MoveCursor(int delta) {
  int c = cursor_ + delta;
  if (c < 0) c = 0;
  if (c > kRows - 1) c = kRows - 1;
  cursor_ = int8_t(c);
  if (cursor_ < top_) top_ = cursor_;
  if (cursor_ > top_ + kListRows - 1) top_ = int8_t(cursor_ - (kListRows - 1));
}

// ---- interface -----------------------------------------------------------------

FLASHMEM void AppCalRoutine::Init() {
  screen_ = SCR_MENU;
}

FLASHMEM void AppCalRoutine::Loop() {
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
      CalRoutinePorts::Flow().Cancel();
      EndRun(SCR_MENU);
      break;
    default:
      break;
  }
}

FLASHMEM void AppCalRoutine::HandleConfirm(const UI::Event &event) {
  if (event.control == OC::CONTROL_BUTTON_L && event.type == UI::EVENT_BUTTON_PRESS) {
    screen_ = SCR_RUN;  // no: back to Results, nothing written
    return;
  }
  if (event.control != OC::CONTROL_BUTTON_B) return;
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
  if (!CalRoutinePorts::Flow().CanSave()) { screen_ = SCR_RUN; return; }
  save_pending_ = true;
}

FLASHMEM void AppCalRoutine::HandlePress(const UI::Event &event) {
  const bool l = event.control == OC::CONTROL_BUTTON_L;
  const bool r = event.control == OC::CONTROL_BUTTON_R;
  const bool a = event.control == OC::CONTROL_BUTTON_A;
  switch (screen_) {
    case SCR_MENU:
      if (r) screen_ = CalRoutinePorts::HardwareSupported() ? SCR_NOTICE : SCR_UNSUPPORTED;
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
      if (s == Stage::PromptOutput || s == Stage::PromptInput) {
        if (r) CalRoutinePorts::Flow().Continue();
        else if (a && s == Stage::PromptInput) {
          skipped_ = uint8_t(skipped_ | (1u << CalRoutinePorts::Flow().current_input()));
          CalRoutinePorts::Flow().Skip();
        }
      } else if (s == Stage::Results) {
        if (r && CalRoutinePorts::Flow().CanSave()) ArmSave();
        else if (a && CursorOnFailedOutput()) {
          if (cursor_ == CalRoutine::kSourceOutput) skipped_ = 0;  // inputs run again
          CalRoutinePorts::Flow().RetryOutput(cursor_);
        }
      }
      break;
    }
    default:
      break;
  }
}

FLASHMEM void AppCalRoutine::HandleButtonEvent(const UI::Event &event) {
  if (screen_ == SCR_CONFIRM) { HandleConfirm(event); return; }
  if (event.type == UI::EVENT_BUTTON_PRESS) HandlePress(event);
}

FLASHMEM void AppCalRoutine::HandleEncoderEvent(const UI::Event &event) {
  if (screen_ == SCR_RUN && CalRoutinePorts::Flow().stage() == Stage::Results &&
      event.control == OC::CONTROL_ENCODER_L)
    MoveCursor(event.value);
}

// ---- screens -----------------------------------------------------------------

FLASHMEM void AppCalRoutine::DrawMenuScreen() const {
  Title("Cal Routine");
  Row(0, " Self");
  graphics.invertRect(0, 14, 128, 9);  // encR opens it
  Row(1, " outputs A-H, then");
  Row(2, " inputs 1-8, with a");
  Row(3, " USB meter");
  Footer("               R:open");
}

FLASHMEM void AppCalRoutine::DrawUnsupported() const {
  Title("Cal Routine: Self");
  Row(0, "Needs a 1.2V/oct unit");
  Row(1, "with 0V at entry 0");
  Row(2, "(Xenomorpher). This");
  Row(3, "one is not: nothing");
  Row(4, "was changed.");
  Footer("L:back");
}

FLASHMEM void AppCalRoutine::DrawNotice() const {
  Title("Cal Routine: Self");
  Row(0, "Meter alone on the");
  Row(1, "host jack: no hub, no");
  Row(2, "other USB devices.");
  Row(3, "Don't touch its USB");
  Row(4, "cable while it runs.");
  Footer("L:back           R:ok");
}

FLASHMEM void AppCalRoutine::DrawNoMeter() const {
  char b[24];
  Title(CalRoutinePorts::Flow().retry_output() >= 0 ? "Retry: meter" : "Self: meter");
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
  char b[24], v[12];
  const Stage s = CalRoutinePorts::Flow().stage();
  const int o = CalRoutinePorts::Flow().current_output();
  const int in = CalRoutinePorts::Flow().current_input();
  const bool retry = CalRoutinePorts::Flow().retry_output() >= 0;
  const char *cancel = retry ? "L:cancel all" : "L:cancel";
  switch (s) {
    case Stage::NoMeter:
      DrawNoMeter();
      return;
    case Stage::ConfigMeter:
      Title(retry ? "Retry: meter" : "Self: meter");
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
    case Stage::PromptInput:
      snprintf(b, sizeof(b), "Input %d  (%d/8)", in + 1, in + 1);
      Title(b);
      Row(0, "Patch output A into");
      snprintf(b, sizeof(b), "input %d. Nothing", in + 1);
      Row(1, b);
      snprintf(b, sizeof(b), "else on input %d.", in + 1);
      Row(2, b);
      Row(3, "The meter may stay.");
      Footer("L:cancel A:skip R:go");
      return;
    case Stage::CalInput:
      snprintf(b, sizeof(b), "Input %d  (%d/8)", in + 1, in + 1);
      Title(b);
      Volts(v, sizeof(v), CalRoutinePorts::Flow().input_mv());
      snprintf(b, sizeof(b), "A at %sV, point %d", v, CalRoutinePorts::Flow().input_points() + 1);
      Row(0, b);
      Bar(1, CalRoutinePorts::Flow().input_mv(), 12000);
      Row(3, "Leave the cables be.");
      Footer(cancel);
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
  if (skipped_ & (1u << i)) {
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
  if (skipped_ & (1u << i)) {
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
  int failed = 0;
  for (int o = 0; o < 8; ++o) failed += !res.out_pass[o];
  if (!failed) Title("Results: can save");
  else {
    snprintf(b, sizeof(b), "Results: %d out FAIL", failed);
    Title(b);
  }
  for (int k = 0; k < kListRows; ++k) {
    const int r = top_ + k;
    if (r >= kRows) break;
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
  Row(0, "Outputs A-H: new.");
  snprintf(b, sizeof(b), "Inputs: %d new curves", ok);
  Row(1, b);
  if (uncal == 0xFF) {
    Row(2, "Uncal: all");  // 8 numbers with spaces would be 22 columns
  } else if (uncal) {
    int len = snprintf(b, sizeof(b), "Uncal:");
    for (int i = 0; i < 8 && len > 0 && size_t(len) < sizeof(b); ++i)
      if (uncal & (1u << i)) len += snprintf(b + len, sizeof(b) - size_t(len), " %d", i + 1);
    Row(2, b);
  }
  Row(3, "Replaces the stored");
  Row(4, "calibration.");
  // The commit sits where no other screen of this app puts anything.
  graphics.setPrintPos(0, 56);
  graphics.print("encL:no");
  graphics.setPrintPos(84, 56);
  graphics.print("B:SAVE");
}

FLASHMEM void AppCalRoutine::DrawSaved() const {
  if (save_ok_) {
    Title("Saved");
    Row(0, "Outputs A-H and the");
    Row(1, "passed inputs now");
    Row(2, "use this calibration");
    Row(3, "in every app.");
    Footer("               R:done");
    return;
  }
  Title("SAVE FAILED");
  const char *at = CalRoutinePorts::SaveFailedAt();
  if (!strcmp(at, "CURVES")) {
    Row(0, "Output tables saved;");
    Row(1, "input curve file NOT");
    Row(2, "written: the inputs");
    Row(3, "keep their old cal.");
  } else {
    Row(0, "Nothing was written.");
  }
  Footer("R:back to results");
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
