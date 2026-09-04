#pragma once
// ---------------------------------------------------------------------------
// Delay -- the audio-applet Delay (audio_applets/DelayApplet.h), lifted out of
// Quadrants' quarter-screen tile and given the whole 128x64 panel as an app in
// its own right, in the AUDIO folder.
//
// SAME DSP, SAME STATE, DIFFERENT SCREEN. There is exactly one DelayApplet
// behind this app and it owns every parameter; this file holds no copy of any
// of them and reaches them through the applet's own accessors. Two writers of
// the same state kept in sync by hand is the Tweighty background-pump bug that
// UI-Redesign-Constraints.md section 1 records as a real shipped defect, and a
// screen is not a reason to repeat it.
//
// WHY THE HOST DRAWS AND THE APPLET DOES NOT.
// `BaseView(true, true)` is not a full-screen mode. It calls DrawFullScreen(),
// which is View() plus one icon at x=96 (HemisphereApplet.h:114-117), and
// View() is still clipped to 64px -- the 64px assumption is baked past
// gfx_offset into the gfx layer itself: gfxHeader hard-codes `62 - strlen*6`
// (HemisphereApplet.cpp:363-379), gfxCursor/gfxSpicyCursor clamp with
// min(x, 63 - box_w) (:162,:186), gfxEndCursor clamps to (0, 63 - w) (:288),
// and gfxDisplayInputMapEditor hard-codes gfxClear(0,0,63,11)
// (HemisphereApplet.h:347,368). So this app does NOT call View() or BaseView()
// at all. It draws its own screen with raw graphics:: calls at the full 128px,
// which is what makes a genuine full-screen layout possible without touching a
// single one of those shared clamps.
//
// THE LAYOUT IS TWEIGHTY'S SCR_EDIT, PIXEL FOR PIXEL (TweightyApp.h:616-647):
// label at x=4, bar at x=34 w=54 h=8, value right-aligned to x=127. Only the
// row pitch changes, 9px -> 10px, which buys a fourth row that clears the
// footer rule -- Tweighty's 9px pitch makes its last cursor band span y=47..55
// and therefore forces gfxFooter() to be drawn FIRST or the band erases it
// (its own comment at :598-601). At 10px the last band ends at y=50 and draw
// order is free. Open Tweighty, open Delay: the same four things are in the
// same four places.
//
// INVERSION, and L-06. Exactly one thing on this screen ever inverts: the
// cursor band, invertRect(0, y-1, 128, 10). encL turn moves the cursor and
// encR turn changes that row's value -- there is no edit mode, because a
// two-encoder app does not need the one a one-encoder applet tile did. So "the
// row encL points at" and "the value encR will change" are the same object,
// and the band is literally `invert exactly what the right encoder will
// change`: the rule stated at PresetBusUI.cpp:433 and adopted as finding L-06.
// This screen adds no seventh meaning of inversion. It removes one -- the
// applet's blinking-underline-vs-inverted-field edit mode
// (HemisphereApplet.cpp:153-171) is simply not used here.
//
// The other states are drawn, never inverted: bypass is a filled/hollow 8x8
// box at (118,1) -- the same presence idiom as Tweighty's transport box
// (TweightyApp.h:556-557) and the rig's banana-jack glyphs -- the clock gate is
// the same filled/hollow box in the bar gutter, and the no-PSRAM refusal is a
// drawn frame with words in it.
//
// CONTROLS
//   encL turn   move the cursor          (same as Tweighty :497-500)
//   encL push   back to the MAIN page    (same as Tweighty :487-488)
//   encR turn   change this row's value  (same as Tweighty :501-502)
//   encR push   next page, cycling       (same as Tweighty :483-485)
//   A           BYPASS on/off
//   B           SEND mode on/off
//   X held      fine adjust
//   Y           cycle this row's source (the rows that have one)
//   Z, chords   untouched; this app claims no Z chord
//
// X-held-fine is on the hold-Z chord card rather than in a footer because the
// footers are already 18-20 of their 21 characters -- see OC_app_base.cpp's
// kChordGloss entry for "DL". Y is advertised only on the CV page, where every
// row has a source; on rows with no source it does nothing and promises
// nothing, which is a different thing from the AuxButton defect it replaces
// (that one DID act, on one row, invisibly).
// ---------------------------------------------------------------------------

#include "../AudioAppletHost.h"
#include "../AudioEffectScreen.h"
#include "../HSApplication.h"
#include "../audio_applets/DelayApplet.h"
#include "../applets/ClockSetupT4.h"

namespace DelayAppNS {

enum Page : uint8_t { PAGE_MAIN, PAGE_MODE, PAGE_CV, PAGE_COUNT };

// MAIN rows.  MODE rows.        CV rows.
enum { M_TIME, M_TAPS, M_FDBK, M_WET, M_COUNT };
enum { D_UNIT, D_CLK, D_MOD, D_COUNT };
enum { C_TIME, C_FDBK, C_WET, C_COUNT };

static constexpr uint8_t kRowCount[PAGE_COUNT] = { M_COUNT, D_COUNT, C_COUNT };

}  // namespace DelayAppNS

// Zero-initialised storage, and it is load-bearing rather than tidy:
// HemisphereApplet::applet_started is declared with no initializer and the
// class has no constructor (HemisphereApplet.h:462-463), so an applet in
// garbage memory can boot with applet_started already true and never run
// Start() -- no DSP wired, no buffer acquired, and nothing on screen to say
// so. Quadrants only escapes this because its factory callocs
// (AppletRegistry.h:47-50). DMAMEM here is zeroed by Main.cpp's
// startup_middle_hook() (:262-265), which the core calls BEFORE
// __libc_init_array, and after configure_external_ram() -- so the constructor
// below also sees a valid external_psram_size when it sizes the PSRAM buffer.
//
// DMAMEM rather than plain .bss because this object is several KB and RAM1 is
// where the stack lives: the T41_console build documents a real DACCVIOL stack
// overflow from spending DTCM carelessly (platformio.ini's T41_console
// comment).
static DMAMEM DelayApplet<STEREO> delay_applet;

OC_APP_CLASS(AppDelay, TWOCCS("DL"), "Delay", "Delay"),
  public HSApplication {
public:
  // 3 x uint64 of applet config (DelayApplet::OnDataRequest fills [0..2]).
  OC_APP_INTERFACE_DECLARE(AppDelay, 24);

  // --- HSApplication ---
  // Deriving from HSApplication is not decoration. BaseController() is the
  // ONLY place HS::frame.Load() happens (HSApplication.h:74-85), and every CV
  // the applet reads goes through CVInputMap/DigitalInputMap, which read
  // HS::frame internally. An app that skipped it would show CV modulation
  // frozen at whatever the previous app left in the frame -- silently, with
  // nothing on screen wrong.
  void Start() final {}
  void Resume() final {}
  void Controller() final {
    // Delay's CLOCK time unit reads DigitalInputMap sources that include the
    // internal clock, and nothing advances the internal clock unless somebody
    // pumps ClockSetup -- Quadrants does it at Quadrants.h:412, Calibr8or at
    // Calibr8or.h:424. Without this, choosing CLOCK on the MODE page would
    // give a delay time of zero and no way to tell why.
    ClockSetup_instance.Controller();
    host_.Tick();
  }
  void View() const final { DrawMenu(); }

private:
  AudioAppletHost host_{ delay_applet, 2 };

  uint8_t page_ = DelayAppNS::PAGE_MAIN;
  uint8_t cursor_[DelayAppNS::PAGE_COUNT] = { 0, 0, 0 };
  bool bypassed_ = false;

  uint8_t Cursor() const { return cursor_[page_]; }

  void AdjustRow(int dir);
  void CycleSource();
  void DrawMain() const;
  void DrawMode() const;
  void DrawCv() const;
};

// ---------------------------------------------------------------------------
// Everything below is FLASHMEM: this app's only per-tick surface is
// Controller() above, which does nothing but forward. Drawing happens at
// redraw rate and input at human rate, both plenty cold for flash. Same
// discipline as ScopeApp.h:117-125.
// ---------------------------------------------------------------------------

FLASHMEM void AppDelay::Init() {
  page_ = DelayAppNS::PAGE_MAIN;
  for (auto &c : cursor_) c = 0;
  // Not persisted, and that is the decision: bypass is a performance A/B, not
  // a setting. Booting into a silent effect because of a comparison somebody
  // made last week is a bug report, not a restored preference.
  bypassed_ = false;
  // Nothing here touches the applet. Init() runs for EVERY app in the
  // container at boot regardless of which one is current (see AudioIO.cpp's
  // output_route comment), so an app nobody opens must not cost PSRAM, F32
  // cables or CPU. The applet starts on this app's first RESUME instead.
}

FLASHMEM size_t AppDelay::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  std::array<uint64_t, HemisphereAudioApplet::CONFIG_SIZE> data = {};
  delay_applet.OnDataRequest(data);
  // [3] is never written by DelayApplet::OnDataRequest, so it is not stored.
  for (int i = 0; i < 3; ++i) stream_buffer.Write<uint64_t>(data[i]);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppDelay::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  std::array<uint64_t, HemisphereAudioApplet::CONFIG_SIZE> data = {};
  for (int i = 0; i < 3; ++i) data[i] = stream_buffer.Read<uint64_t>();
  // A short read returns zeros, and zeros are a VALID-looking config here:
  // delay_time 0, taps 0, wet 0. Applying them would replace the applet's
  // perfectly good defaults with a silent delay of no length, so don't.
  if (stream_buffer.underflow()) return 0;
  delay_applet.OnDataReceive(data);
  return stream_buffer.read();
}

FLASHMEM void AppDelay::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
      // Claims AudioIO's shared effect slot from whichever effect app held it
      // last, and starts the applet if this is the first time.
      host_.Enter(!bypassed_);
      break;
    case OC::APP_EVENT_SUSPEND:
      // Deliberately nothing. The instrument's idiom is that audio keeps
      // sounding when you leave the screen -- Quadrants and Tweighty both stay
      // live when backgrounded (TweightyApp.h:423-429). The slot is given up
      // when another effect app CLAIMS it, not when this one is walked away
      // from.
      break;
    default: break;
  }
}

FLASHMEM void AppDelay::Loop() {}

void AppDelay::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

// --- draw ------------------------------------------------------------------

FLASHMEM void AppDelay::DrawMain() const {
  using namespace DelayAppNS;
  namespace S = AudioEffectScreen;
  char buf[16];

  // --- Time, whose units and therefore whose whole row change with MODE. It is
  // the one row in this family whose bar TYPE changes, so it is the one row
  // that cannot use a single ParamRow call.
  switch (delay_applet.GetTimeUnits()) {
    case DelayApplet<STEREO>::CLOCK: {
      const int r = delay_applet.GetRatio();
      snprintf(buf, sizeof(buf), r < 0 ? "x%d" : "/%d", r < 0 ? -r + 1 : r + 1);
      S::BipolarParamRow(M_TIME, "Time:", (float)r / 127.0f, buf);
      break;
    }
    case DelayApplet<STEREO>::HZ: {
      const int lo = delay_applet.MinPitch(), hi = delay_applet.MaxPitch();
      const float span = (float)(hi - lo);
      snprintf(buf, sizeof(buf), "%d.%01d", SPLIT_INT_DEC(delay_applet.GetHz(), 10));
      S::ParamRow(M_TIME, "Time:",
                  span > 0.0f ? (float)(delay_applet.GetPitch() - lo) / span : 0.0f,
                  buf);
      break;
    }
    default: {
      const int lo = delay_applet.MinDelayMs(), hi = delay_applet.MaxDelayMs();
      const int ms = delay_applet.GetDelayMs();
      const float span = (float)(hi - lo);
      // Same %d.%02ds Tweighty uses for its own Time field (:626), so the two
      // screens read a delay time the same way.
      snprintf(buf, sizeof(buf), "%d.%02ds", ms / 1000, (ms % 1000) / 10);
      S::ParamRow(M_TIME, "Time:", span > 0.0f ? (float)(ms - lo) / span : 0.0f,
                  buf);
      break;
    }
  }

  // --- Taps. Same (n-1)/(max-1) fraction Tweighty uses (:630).
  snprintf(buf, sizeof(buf), "%d", delay_applet.GetTaps());
  S::ParamRow(M_TAPS, "Taps:", (float)(delay_applet.GetTaps() - 1) / 7.0f, buf);

  // --- Feedback. Bipolar on the stereo instance, where negative feedback is a
  // real and different sound, so the bar has to show a sign.
  const int fb = delay_applet.GetFeedback();
  snprintf(buf, sizeof(buf), "%d%%", fb);
  S::BipolarParamRow(M_FDBK, "Fdbk:", (float)fb / 100.0f, buf);

  // --- Wet, or Send. The label flip IS the feedback for the B button.
  snprintf(buf, sizeof(buf), "%d%%", delay_applet.GetWet());
  S::ParamRow(M_WET, delay_applet.SendMode() ? "Snd :" : "Wet :",
              (float)delay_applet.GetWet() / 100.0f, buf);
}

// The MODE page exists to AVOID a bug, not to hold spare parameters. In the
// quarter-screen applet the clock-source row only exists while the time unit
// is CLOCK, which is what forces the double MoveCursor plus ++cursor at
// DelayApplet.h whose own comment reads "smh my head" and which produces an
// unpredictable cursor jump. Here the row is always present and always
// editable, so there is no vanishing row and no cursor to teleport: a row
// whose value is inert until you switch units is honest; a row that disappears
// under the cursor is not.
//
// Note that this does NOT fix the applet tile. Quadrants still draws the old
// View() with the hidden row and still runs that hack -- fixing it there is a
// change to the quarter-screen layout, which is not what this app is.
FLASHMEM void AppDelay::DrawMode() const {
  using namespace DelayAppNS;
  namespace S = AudioEffectScreen;
  char buf[16];

  // No bars on this page: every row is an enumerated setting, and a bar would
  // imply a quantity that is not there.
  switch (delay_applet.GetTimeUnits()) {
    case DelayApplet<STEREO>::CLOCK: S::EnumRow(D_UNIT, "Unit:", "clk"); break;
    case DelayApplet<STEREO>::HZ:    S::EnumRow(D_UNIT, "Unit:", "Hz");  break;
    default:                         S::EnumRow(D_UNIT, "Unit:", "ms");  break;
  }

  S::Label(D_CLK, "Clk :");
  auto &clk = delay_applet.ClockSource();
  // Filled while the gate is high, hollow otherwise, in the bar gutter. NOT
  // the shipped gfxPrint(DigitalInputMap&) behaviour, which uses gfxInvert to
  // mean "gate high" (HemisphereApplet.cpp:235-238) -- that is a seventh
  // meaning of inversion on top of the six the panel audit counted, and this
  // screen is not adding one. Same filled/hollow presence idiom as the bypass
  // box above it.
  if (clk.Gate()) graphics.drawRect(S::kBarX, S::kRowY[D_CLK], 8, 8);
  else graphics.drawFrame(S::kBarX, S::kRowY[D_CLK], 8, 8);
  const int steps = clk.div_mult.steps;
  snprintf(buf, sizeof(buf), "%s %c%d", clk.InputName(),
           steps > 0 ? '/' : 'x', steps > 0 ? steps : -steps);
  S::Value(D_CLK, buf);

  // The applet's own strings are "Crossfade"/"Stretch" (:239), 9 characters,
  // which does not fit a value field that starts at x>=91. Abbreviated, and
  // the abbreviation is the same length either way so the column does not
  // jitter as you turn.
  S::EnumRow(D_MOD, "Mod :",
             delay_applet.GetModType() == DelayApplet<STEREO>::CROSSFADE
               ? "Xfade" : "Strch");
}

// One row per CV destination: where it comes from, how much is arriving right
// now, and how much of it is being applied. This replaces BOTH the inline CV
// columns in the applet's View() and the gfxDisplayInputMapEditor() overlay --
// which is worth stating because that overlay ends in gfxInvert(0,0,63,11), an
// inverted banner and an L-06 violation in its own right
// (HemisphereApplet.h:344-370). Not used here.
//
// The live bar is InRescaled(12) into 24px of WIDTH, not the shipped
// gfxPrint(CVInputMap&) (HemisphereApplet.cpp:239-246), which draws a line up
// to 24 pixels tall UPWARD from the row baseline. At this 10px row pitch that
// line would bleed through the two rows above it.
FLASHMEM void AppDelay::DrawCv() const {
  using namespace DelayAppNS;

  static const char *const kLabels[C_COUNT] = { "Time", "Fdbk", "Wet " };
  CVInputMap *maps[C_COUNT] = { &delay_applet.TimeCV(), &delay_applet.FeedbackCV(),
                                &delay_applet.WetCV() };

  for (int row = 0; row < C_COUNT; ++row) {
    // Row 2 tracks the B button the same way MAIN's Wet row does, so send mode
    // is visible on this page too and B does not need a footer slot here.
    AudioEffectScreen::CvRow(
      row, (row == C_WET && delay_applet.SendMode()) ? "Snd " : kLabels[row],
      *maps[row]);
  }
}

FLASHMEM void AppDelay::DrawMenu() const {
  using namespace DelayAppNS;

  AudioEffectScreen::Header("D E L A Y", host_.Live());

  // Before the first RESUME there is no buffer, no MIN/MAX_DELAY_SECS and
  // nothing true to draw. It should not be reachable -- RESUME precedes the
  // first redraw -- but a screen that would divide by an unset range is not
  // something to leave to sequencing.
  if (!host_.Started()) {
    gfxFooter("starting");
    return;
  }

  if (!delay_applet.BufferReady()) {
    // No PSRAM: the wet path produces nothing at all and only the dry leg
    // survives, so at Wet=100% this app is silent. Say so, with the
    // consequence, rather than letting it be discovered by ear. There is no
    // remedy from the panel -- a fact stated plainly beats an invented one.
    AudioEffectScreen::RefusalBox("NO RAM: DRY ONLY");
    // Worded for THIS app's actual cause. Delay's buffer comes only from
    // extmem_calloc (Audio/AudioBuffer.h:145-150), so the failure is a missing
    // PSRAM chip and a reboot frees nothing -- unlike the reverbs, whose arena
    // comes off the RAM2 heap where a reboot genuinely does help.
    gfxFooter("no PSRAM fitted - dry");
    return;
  }

  switch (page_) {
    case PAGE_MODE: DrawMode(); break;
    case PAGE_CV:   DrawCv();   break;
    default:        DrawMain(); break;
  }

  // One inversion, on the row encR will change. See this file's header.
  AudioEffectScreen::CursorBand(Cursor());

  if (bypassed_) {
    // While bypassed the legend is replaced ENTIRELY: fact plus remedy in one
    // line, the `busy: scan (L stops)` shape the panel audit calls the best
    // example in the tree. A hollow box on its own would be an unworded glyph
    // for a state, which the grammar forbids.
    gfxFooter("BYPASSED - A:active");
  } else {
    switch (page_) {
      // A gives up its footer slot on the CV page to Y, which is the press
      // that is only discoverable from a label: A's state is already carried
      // by the header box, and in the state where A matters -- bypassed --
      // the footer says so above. 21 characters is the budget and four
      // presses want 24.
      case PAGE_MODE: gfxFooter("A:byp  B:snd  R:cv");   break;
      case PAGE_CV:   gfxFooter("A:byp  Y:src  R:main"); break;
      default:        gfxFooter("A:byp  B:snd  R:mode"); break;
    }
  }
}

// Nothing. The effect is still running and still audible while the screensaver
// is up -- that is the point of not releasing the slot on suspend -- but
// nothing on this screen moves on its own, so there is nothing to show.
FLASHMEM void AppDelay::DrawScreensaver() const {}

// --- input -----------------------------------------------------------------

// Coarse steps are sized so a full sweep is a wrist's worth of turning rather
// than a minute of it; X held gives the finest step the parameter actually
// has. Deliberately NOT a blanket "divide by ten": on a row whose value is a
// whole percent, a tenth of a step does not exist, and a modifier that
// silently does nothing is the defect this app exists to stop repeating.
FLASHMEM void AppDelay::AdjustRow(int dir) {
  using namespace DelayAppNS;
  if (dir == 0) return;

  if (page_ == PAGE_MAIN) {
    switch (Cursor()) {
      case M_TIME:
        switch (delay_applet.GetTimeUnits()) {
          case DelayApplet<STEREO>::CLOCK:
            delay_applet.NudgeRatio(dir);
            break;
          case DelayApplet<STEREO>::HZ:
            // Pitch steps are eighths of a semitone; coarse is a whole
            // semitone, fine is the eighth. Both land on the grid.
            delay_applet.NudgePitch(dir * AudioEffectScreen::Step(8, 1));
            break;
          default:
            // 50ms coarse puts the ~10.9s range in ~218 detents; 5ms fine is
            // finer than anyone dials a delay by ear. The applet's own 1ms
            // step needed 10,900 detents to cross the range, which is about
            // two minutes of turning and is why it had an acceleration hack.
            delay_applet.NudgeDelayMs(dir * AudioEffectScreen::Step(50, 5));
            break;
        }
        break;
      case M_TAPS: delay_applet.NudgeTaps(dir); break;
      case M_FDBK: delay_applet.NudgeFeedback(dir * AudioEffectScreen::Step(2, 1)); break;
      case M_WET:  delay_applet.NudgeWet(dir * AudioEffectScreen::Step(2, 1)); break;
      default: break;
    }
    return;
  }

  if (page_ == PAGE_MODE) {
    switch (Cursor()) {
      case D_UNIT: delay_applet.SetTimeUnits(delay_applet.GetTimeUnits() + dir); break;
      // Y cycles the source on this row; encR is the number on it, which for a
      // clock source is its divide/multiply. Same split as the CV page, so
      // encR means the same thing on every row of every page.
      case D_CLK:  delay_applet.ClockSource().div_mult.Adjust(dir); break;
      case D_MOD:  delay_applet.NudgeModType(dir); break;
      default: break;
    }
    return;
  }

  // CV page: encR is attenuversion, which is what keeps encR's meaning ("the
  // number on this row") identical everywhere. Same -127..127 clamp as
  // HSApplication::EditSelectedInputMap (:248-251).
  CVInputMap *maps[C_COUNT] = { &delay_applet.TimeCV(), &delay_applet.FeedbackCV(),
                                &delay_applet.WetCV() };
  int8_t &att = maps[Cursor()]->attenuversion;
  att = constrain(att + dir * AudioEffectScreen::Step(4, 1), -127, 127);
}

FLASHMEM void AppDelay::CycleSource() {
  using namespace DelayAppNS;
  if (page_ == PAGE_CV) {
    CVInputMap *maps[C_COUNT] = { &delay_applet.TimeCV(), &delay_applet.FeedbackCV(),
                                  &delay_applet.WetCV() };
    maps[Cursor()]->ChangeSource(1);
  } else if (page_ == PAGE_MODE && Cursor() == D_CLK) {
    delay_applet.ClockSource().ChangeSource(1);
  }
  // Any other row has no source to cycle. Y is not offered in those footers,
  // so nothing is promised and nothing is broken.
}

FLASHMEM void AppDelay::HandleButtonEvent(const UI::Event &event) {
  using namespace DelayAppNS;
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  switch (event.control) {
    case OC::CONTROL_BUTTON_UP:     // A: bypass
      bypassed_ = !bypassed_;
      // Only the output cables move. The applet keeps running, so the delay
      // keeps its tail and coming back is instant and lossless -- which is the
      // one thing a full-screen effect can give a player that a quarter-screen
      // tile sharing one encoder cannot.
      host_.SetOutput(!bypassed_);
      break;
    case OC::CONTROL_BUTTON_DOWN:   // B: send mode
      delay_applet.ToggleSendMode();
      break;
    case OC::CONTROL_BUTTON_R:      // encR: next page
      page_ = (uint8_t)((page_ + 1) % PAGE_COUNT);
      break;
    case OC::CONTROL_BUTTON_L:      // encL: back to MAIN
      page_ = PAGE_MAIN;
      break;
    case OC::CONTROL_BUTTON_Y:      // Y: cycle this row's source
      CycleSource();
      break;
    default: break;                 // X is a held modifier, not a press
  }
}

FLASHMEM void AppDelay::HandleEncoderEvent(const UI::Event &event) {
  if (event.control == OC::CONTROL_ENCODER_L) {
    int c = (int)cursor_[page_] + event.value;
    // Clamped, not wrapped: with 3 or 4 rows, wrapping past the end lands two
    // rows from where the eye was and is read as a jump, not a wrap.
    CONSTRAIN(c, 0, (int)DelayAppNS::kRowCount[page_] - 1);
    cursor_[page_] = (uint8_t)c;
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    AdjustRow(event.value);
  }
}

FLASHMEM void AppDelay::GetIOConfig(OC::IOConfig &ioconfig) const {
  using namespace OC;
  // The applet's CV and clock sources are freely assignable through the CV and
  // MODE pages, so there is no fixed jack-to-parameter map to declare. What is
  // true and worth saying is that this app drives no CV output at all.
  for (int ch = 0; ch < DAC_CHANNEL_COUNT; ++ch)
    ioconfig.outputs[ch].set("off", OC::OUTPUT_MODE_RAW);
}

FLASHMEM void AppDelay::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("slot ");
  graphics.print(host_.HoldsSlot() ? "held" : "-");
  graphics.setPrintPos(2, 22);
  graphics.print("buf ");
  graphics.print(host_.Started() && delay_applet.BufferReady() ? "ok" : "NONE");
}
