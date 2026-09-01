// Copyright 2019 Patrick Dowling
//
// Author: Patrick Dowling (pld@gurkenkiste.com)
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
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//

#include <string.h>

#include "OC_apps.h"
#include "OC_global_settings.h"
#include "OC_io_settings_menu.h"
#include "OC_calibration.h"
#include "OC_gpio.h"  // but_mid: whether this pin map has the grey Z button at all
#include "VBiasManager.h"

namespace OC {

// This isn't necessarily ideal place for this to live, but it's simultaneously
// global (there can be only one) and app-specific. Really we kind of want some
// kind of popup/overlay/menu stack.
static IOSettingsMenu io_settings_menu;

// ===========================================================================
// The chord card
// ===========================================================================
//
// Every global gesture on this panel is an unlabelled chord. A (or Z) plus a
// push of the RIGHT encoder opens the app switcher -- the only route from one
// app to any other -- A/Z plus the LEFT encoder opens I/O settings, both
// encoder pushes open the preset-bus overlay, and A+B means a different thing
// in each app that binds it. Not one of those was printed on any screen, so
// there was no in-instrument route to the rest of the firmware at all.
//
// Worse, holding A -- the button that IS the modifier -- drew nothing
// whatsoever: `--keys "step300,a-down,step2000"` left the frame buffer
// byte-identical. Press-and-hold-the-first-button is the universal discovery
// move on unlabelled hardware, and here it was the one gesture with no
// feedback of any kind, which is what left A/X/Y reading as dead buttons.
//
// So: hold a modifier on its own and it lists what it modifies, drawn over the
// running app and dismissed by letting go. Releasing it does nothing else --
// see kChordHintDelayTicks for why that is guaranteed and not merely hoped.

// Why 700 ticks. Ui::Poll() runs off a 1 kHz IntervalTimer (OC_UI_TIMER_RATE
// is 1000us), so a UI tick is a millisecond and this is 700ms.
//
// The FLOOR is Ui::kLongPressTicks (500), and it is a hard floor rather than a
// preference. Below it, letting go after reading the card emits
// EVENT_BUTTON_PRESS for A -- and a plain A press is a live control in most
// apps: Captain MIDI changes setup (CaptainMIDI.h:2336), Hemisphere moves the
// applet selection (Hemisphere.h:1753), Setup/About inverts the whole display
// (SETTINGS.h:568). Past 500 ticks the release is an EVENT_BUTTON_LONG_RELEASE
// instead, and nothing in the tree binds A on long press or on long release:
// the only two A-long-press handlers, AppDualSequencer::HandleUpButtonLong()
// (Sequins.h:2398) and AppChordQuantizer::HandleUpButtonLong() (Chords.h:1339),
// are empty stubs. Waiting out the long-press boundary is therefore what makes
// "reveal, then release, and nothing happened" actually true.
//
// The CEILING is patience: long-press affordances elsewhere land at 0.5-1.0s,
// and somebody poking an unlabelled button gives up after a second or two.
//
// 700 sits 200ms above the floor. That margin buys two things. A chord
// performed at any speed the firmware itself still classes as a *press* can
// never flash the card on its way through; and the card does not appear on the
// same tick as the firmware's own long-press event, so it can never be read as
// one app's reaction to a long press.
static constexpr uint32_t kChordHintDelayTicks = 700;

// Which modifier is being held alone, 0 for none. Armed on a solo button-down
// and disarmed by any other control, so a chord never leaves the card behind.
static UiControl chord_hint_modifier = static_cast<UiControl>(0);
static uint32_t chord_hint_ticks = 0;

// The grey Z button only exists on some pin maps. CONTROL_BUTTON_M is bit 4,
// and Ui::Poll() only scans CONTROL_BUTTON_LAST buttons -- which is 4 on the
// Teensy 3.2 (antihem) and Teensy 4.0 builds, so bit 4 can never appear in an
// event mask there and every Z row on this card would be a lie. On VOR the
// middle button is the bias control (see DispatchEvent below), not Z.
static inline bool z_button_present()
{
#if defined(VOR) || !defined(ARDUINO_TEENSY41)
  return false;
#else
  return but_mid != 0xFF;
#endif
}

namespace {

// A row is (label, gloss) rather than one composed string so the app-specific
// half can be substituted without a scratch buffer to overflow, and so the
// card's text column can be centred on the width the strings actually need.
struct HintRow {
  const char *label;
  const char *gloss;  // may be null
};

// 21 columns is the whole 128px row at the 6x8 font, and is the hard limit: a
// 22nd character starts at x=126, gets two of its six pixel columns, and reads
// as a shorter plausible string rather than as damage. Every row here is
// budgeted at 18 or under, so the centred text block sits inset from both
// edges. Nothing enforces that on its own: selfcheck.sh's edgecheck.py sweep
// does not cover these screens, so a new row has to be checked by hand with
//   xeno-sim --app NAME --keys "step300,a-down,step1200" --dump-fb | edgecheck.py -
static constexpr int kChordHintMaxChars = 21;

static constexpr int kChordHintRowH = 9;   // 8px glyph + 1px leading

// What A+B and a bare Z do in the apps that bind them. An app missing from
// this table gets no such row at all: A+B genuinely means three different
// things, and a generic label would be wrong in most apps that have one.
struct ChordGloss {
  uint16_t app_id;
  const char *a_plus_b;  // null: this app does not bind A+B
  const char *z_alone;   // null: this app does not bind Z
};

static const ChordGloss kChordGloss[] = {
  // Quadrants (Quadrants.h:615 / :1873), Hemisphere (Hemisphere.h:980 / :1714)
  // and Calibr8or (Calibr8or.h:568 / :919) all open the clock-setup overlay on
  // A+B and toggle the internal clock on Z.
  { TWOCCS("QS"), "Clock Setup",  "Clock Run" },
  { TWOCCS("HS"), "Clock Setup",  "Clock Run" },
  { TWOCCS("C8"), "Clock Setup",  "Clock Run" },
  // CaptainMIDI.h:2318 -- A+B opens the clock router. Captain binds no Z.
  { TWOCCS("MI"), "Clock Router", nullptr },
  // SETTINGS.h:548 -- A+B flips the screen 180 degrees. Setup/About binds no Z.
  { TWOCCS("SE"), "Flip Screen",  nullptr },
  // Scenery.h:713 -- Z jumps to a random scene. Scenery binds no A+B.
  { TWOCCS("SX"), nullptr,        "Random Scene" },
};

static const ChordGloss *find_gloss(uint16_t app_id)
{
  for (const auto &g : kChordGloss)
    if (g.app_id == app_id) return &g;
  return nullptr;
}

static int row_chars(const HintRow &row)
{
  return static_cast<int>(strlen(row.label)) +
         (row.gloss ? static_cast<int>(strlen(row.gloss)) : 0);
}

} // anonymous namespace

// Draws the card. `title` labels it, `rows`/`row_count` are its contents.
FLASHMEM static void DrawChordCard(const char *title, const HintRow *rows, int row_count)
{
  // Tint the running app to 50% so the card reads as an overlay on top of it
  // rather than as a screen the module has navigated to.
  //
  // A checkerboard is the only dither that reads as a flat grey here. The
  // previous attempt, gfxDottedLine(10, y, 118, y, 3 + (y % 4)), walked strides
  // 3,4,5,6 down the box: four interleaved periods whose beat bands diagonally
  // at about the stroke pitch of the 6x8 font, which is exactly why it read as
  // noise laid over the text instead of as dimming behind it. (x+y)&1 is a flat
  // 50% at the highest spatial frequency the panel can express, so it has no
  // beat to alias against anything. drawHLinePattern walks the frame buffer
  // directly -- this repaints at the loop's ~1kHz redraw for as long as the
  // button is held, so the per-pixel modulo in gfxDottedLine is worth avoiding.
  for (weegfx::coord_t y = 0; y < 64; ++y)
    graphics.drawHLinePattern(y & 1, y, 128 - (y & 1), 2);

  int text_chars = 0;
  for (int i = 0; i < row_count; ++i) {
    const int n = row_chars(rows[i]);
    if (n > text_chars) text_chars = n;
  }
  if (text_chars > kChordHintMaxChars) text_chars = kChordHintMaxChars;

  // Full width, and that is load-bearing rather than cosmetic. edgecheck.py
  // inspects columns 126 and 127 of every row it can read text on, and calls a
  // row clipped unless both are uniform. A card inset from the edges leaves
  // those two columns showing whatever the app underneath happened to draw
  // there, so every card would have been reported as clipped and the check
  // would have become useless on exactly the screens carrying new text. Running
  // the card edge to edge puts the cleared interior under 126 and the frame's
  // right rule under 127, so both columns are uniform by construction. The text
  // block is then centred inside it on the width the rows actually need.
  const int card_h = (row_count + 1) * kChordHintRowH + 4;
  const int card_y = (64 - card_h) / 2;
  const int text_x = (128 - text_chars * 6) / 2;

  // Opaque, so the list is read against black and not against the tint.
  graphics.clearRect(0, card_y, 128, card_h);
  graphics.drawFrame(0, card_y, 128, card_h);

  int row_y = card_y + 1;
  gfxPrint((128 - 6 * static_cast<int>(strlen(title))) / 2, row_y, title);
  graphics.invertRect(1, row_y, 126, kChordHintRowH);

  for (int i = 0; i < row_count; ++i) {
    row_y += kChordHintRowH;
    gfxPrint(text_x, row_y, rows[i].label);
    if (rows[i].gloss) gfxPrint(rows[i].gloss);
  }
}

// The chord card, if a modifier has been held on its own for long enough.
//
// The visibility test is ui.read_immediate() against the LIVE pin state, not a
// latch fed by release events: OC_ui.cpp's global hotkey block deliberately
// swallows the releases of every button a chord consumed, so a latch cleared on
// release would strand the card on screen after each gesture it exists to
// teach. The pin cannot lie about whether a finger is still down.
FLASHMEM static void DrawChordHint(uint16_t app_id, bool io_settings_allowed)
{
  if (!chord_hint_modifier) return;
  if (!ui.read_immediate(chord_hint_modifier)) return;
  if (ui.ticks() - chord_hint_ticks < kChordHintDelayTicks) return;

  const ChordGloss *gloss = find_gloss(app_id);

  HintRow rows[5];
  int n = 0;

  if (CONTROL_BUTTON_A == chord_hint_modifier) {
    rows[n++] = { "A+encR: Switch App", nullptr };
    // Offered only where EditIOSettings() will actually open something. It
    // refuses in silence for any app that overrides io_settings_allowed(), and
    // a row promising a screen that never arrives is how a working gesture gets
    // learned as a broken one. No app in this tree overrides it today (the base
    // returns true and nothing else defines it), so as things stand this row
    // always appears -- the test is here so the card stays honest the moment
    // one does, not because it is filtering anything now.
    if (io_settings_allowed) rows[n++] = { "A+encL: I/O Cfg", nullptr };
    if (gloss && gloss->a_plus_b) rows[n++] = { "A+B: ", gloss->a_plus_b };
    rows[n++] = { "encL+encR: Presets", nullptr };
    if (z_button_present()) rows[n++] = { "Z+A: Screensaver", nullptr };
    DrawChordCard("HOLD A", rows, n);
  } else {
    rows[n++] = { "Z+encR: Switch App", nullptr };
    if (io_settings_allowed) rows[n++] = { "Z+encL: I/O Cfg", nullptr };
    rows[n++] = { "Z+A: Screensaver", nullptr };
    // Z is the one row here that can fire on RELEASE (Quadrants.h:1873 toggles
    // the clock on either release), so naming what it does is not decoration:
    // it is the difference between letting go and being surprised.
    if (gloss && gloss->z_alone) rows[n++] = { "Z: ", gloss->z_alone };
    rows[n++] = { "encL+encR: Presets", nullptr };
    DrawChordCard("HOLD Z", rows, n);
  }
}

// Arms the card on a modifier pressed ALONE, disarms it the moment anything
// else is touched.
//
// "Alone" has to be decided on the DOWN edge, for the reason SETTINGS.h:559
// already writes down from the other side: event.mask is the raw pin state as
// the event was queued, and a release is only reported seven ticks after the
// pin rises, so by the time a release event exists its mask says nothing useful
// about what was held with it.
//
// Disarming on any other control is what keeps the card out of the way of the
// chords themselves. A+B reaches HandleButtonEvent through here, so B's own
// down event tears the card down on the same tick the app acts; A+encR and
// A+encL never reach this function at all, because OC_ui.cpp's hotkey block
// consumes them before dispatch -- they are cleared instead by the
// DispatchAppEvent and EditIOSettings each of them ends in.
static void ArmChordHint(const UI::Event &event)
{
  if (UI::EVENT_BUTTON_DOWN == event.type &&
      (CONTROL_BUTTON_A == event.control ||
       (CONTROL_BUTTON_Z == event.control && z_button_present())) &&
      event.control == event.mask) {
    chord_hint_modifier = static_cast<UiControl>(event.control);
    chord_hint_ticks = ui.ticks();
  } else if (event.control != chord_hint_modifier) {
    chord_hint_modifier = static_cast<UiControl>(0);
  }
}

void AppBase::InitDefaults()
{
  io_settings_.InitDefaults();
  Init();
}

size_t AppBase::Save(util::StreamBufferWriter &stream_buffer) const
{
  io_settings_.Save(stream_buffer);
  SaveAppData(stream_buffer);
  return stream_buffer.written();
}

size_t AppBase::Restore(util::StreamBufferReader &stream_buffer)
{
  io_settings_.Restore(stream_buffer);
  RestoreAppData(stream_buffer);
  return stream_buffer.read();
}

FLASHMEM void AppBase::Draw(UiMode ui_mode) const
{
  if (UI_MODE_MENU == ui_mode) {
    if (!io_settings_menu.active())
      DrawMenu();
    else
      io_settings_menu.Draw();
  } else {
    DrawScreensaver();
  }

  // Passed in rather than read here because io_settings_allowed() is a
  // protected member: this file owns the card, but AppBase's declaration lives
  // in OC_apps.h and is not ours to extend.
  DrawChordHint(id(), io_settings_allowed());
}

UiMode AppBase::DispatchEvent(const UI::Event &event)
{
  UiMode mode = UI_MODE_MENU;

  // Before anything can act on the event, and on both branches below: the card
  // has to disarm on the very event that starts a chord, including the ones the
  // I/O settings menu consumes.
  ArmChordHint(event);

  if (!io_settings_menu.active()) {
    switch (event.type) {
      case UI::EVENT_ENCODER:
        HandleEncoderEvent(event);
        break;

      case UI::EVENT_BUTTON_PRESS:
#ifdef VOR
        if (OC::CONTROL_BUTTON_M == event.control) {
            VBiasManager *vbias_m = vbias_m->get();
            vbias_m->AdvanceBias();
        } else
#endif
        HandleButtonEvent(event);
        break;

      case UI::EVENT_BUTTON_DOWN:
#ifdef VOR
        // dual encoder press
        if ( ((OC::CONTROL_BUTTON_L | OC::CONTROL_BUTTON_R) == event.mask) )
        {
            VBiasManager *vbias_m = vbias_m->get();
            vbias_m->AdvanceBias();
            ui.SetButtonIgnoreMask(); // ignore release and long-press
            break;
        }
#endif
        HandleButtonEvent(event);
        break;

      case UI::EVENT_BUTTON_LONG_PRESS:
      default:
        HandleButtonEvent(event);
        break;
    }
  } else {
    mode = io_settings_menu.DispatchEvent(event);
  }
  return mode;
}

void AppBase::DispatchAppEvent(AppEvent app_event)
{
  switch(app_event) {
    case APP_EVENT_RESUME:
    io_settings_menu.Close();
    default:
    break;
  }

  // NOTE it might make sense/simplify things further to split this into
  // dedicated functions for the derived classes (OnSuspend/OnResume etc) since
  // a lot of the apps have very similar, mostly empty implementations.
  HandleAppEvent(app_event);
  // Suspend/resume is the tail of the A+encR chord: the modifier is still held
  // while the app switcher runs, and OC_ui.cpp swallows its release, so the
  // card has to be torn down from here too or it would be waiting on the app
  // the user just came back to.
  chord_hint_modifier = static_cast<UiControl>(0);
}

void AppBase::EditIOSettings()
{
  if (io_settings_allowed()) {
    APPS_SERIAL_PRINTLN("EditIOSettings(%s)", name());
    io_settings_menu.Edit(this);
  }
  // ...and this is the tail of A+encL, which never reaches DispatchEvent at
  // all: OC_ui.cpp's hotkey block calls straight in here and continues. Cleared
  // whether or not the app allowed the menu, since either way the chord
  // happened and the card must not outlive it.
  chord_hint_modifier = static_cast<UiControl>(0);
}

void AppBase::DispatchLoop()
{
  if (io_settings_menu.active())
    io_settings_menu.Update();

  Loop();
}

uint32_t AppBase::io_settings_status_mask() const
{
  return io_settings_.status_mask() &
         global_settings.autotune_calibration_data.valid_mask();
}

} // OC
