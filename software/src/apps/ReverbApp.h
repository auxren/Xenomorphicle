#pragma once
// ---------------------------------------------------------------------------
// Reverb -- the audio-applet Freeverb (audio_applets/FreeverbApplet.h) as a
// standalone full-screen app in the AUDIO folder, second in the family after
// Delay and on the same host and the same screen grammar.
//
// It is deliberately a SCREEN AND NOTHING ELSE. AudioAppletHost needed no
// change to take it, and AudioEffectScreen supplies every mark on the panel.
// What is in this file is only what is genuinely Reverb's: four parameters,
// what their bars and values mean, and what its buttons do. If a third effect
// needs more than that from either shared file, that is a finding about the
// abstraction and should be treated as one.
//
// WHY IT IS CALLED "REVERB" AND NOT "FREEVERB". The applet's applet_name()
// returns "Reverb" and has since it shipped, and applet_id() defaults to
// strhash(applet_name()) (HemisphereAudioApplet.h:45-47) -- so renaming it
// would silently invalidate every stored preset that references it. The
// screens spec calls this one Freeverb; the tile in Quadrants calls it Reverb;
// a player who has both open should not see two names for one effect. Matching
// what the instrument already says beats matching a document, so: Reverb.
// (This is the same hazard as Samverb/Bungverb, which is why that one is being
// left alone until somebody decides it deliberately.)
//
// TWO PAGES, not three. Delay has MODE because it has a time unit, a clock
// source and a modulation type to put somewhere; Reverb has none of those, and
// Audio-Apps-Screens.md is explicit that a page which would be empty does not
// exist. So encR cycles MAIN -> CV -> MAIN, and the footer names the
// destination of the press either way.
//
// B IS UNBOUND HERE, ON PURPOSE. Delay binds it to send mode because Delay has
// a send_mode bit; Reverb does not, and adding one purely so the footer looks
// symmetrical would be growing persisted state to tidy a legend. A footer that
// omits an unbound button is honest (Audio-Apps-Screens.md section 7 item 7).
//
// CONTROLS
//   encL turn   move the cursor
//   encL push   back to the MAIN page
//   encR turn   change this row's value
//   encR push   next page, cycling
//   A           BYPASS on/off
//   X held      fine adjust  (on the hold-Z chord card, not in a footer)
//   Y           CV page: cycle that row's source
//   B, Z        unbound; this app claims no Z chord
//
// INCLUDE-ORDER NOTE, because it is invisible and would be baffling to debug:
// FreeverbApplet's `applet_name()` only satisfies HemisphereApplet's pure
// virtual `applet_name() const` through the macro defined at
// audio_applets/_config.h:16. OC_apps.cpp includes that file (:65) before
// apps/_config.h (:81), so by the time this header is compiled the applet is
// already fully defined and the include below is a no-op. Including this app
// ahead of audio_applets/_config.h would make ReverbApplet abstract and the
// static instance below uninstantiable, with an error message that names none
// of this.
// ---------------------------------------------------------------------------

#include "../AudioAppletHost.h"
#include "../AudioEffectScreen.h"
#include "../HSApplication.h"
#include "../audio_applets/FreeverbApplet.h"

namespace ReverbAppNS {

enum Page : uint8_t { PAGE_MAIN, PAGE_CV, PAGE_COUNT };

// The row order is the applet's own cursor order (FreeverbApplet.h:162-171:
// SIZE, DAMP, CUTOFF, MIX), so nobody moving between the tile and the app has
// to relearn anything. Both pages use it.
enum { R_SIZE, R_DAMP, R_CUT, R_MIX, R_COUNT };

}  // namespace ReverbAppNS

// Zero-initialised storage, and load-bearing rather than tidy:
// HemisphereApplet::applet_started is declared with no initializer and the
// class has no constructor (HemisphereApplet.h:462-463), so an applet in
// garbage memory can boot with applet_started already true and never run
// Start() -- no DSP wired, no arena taken, and nothing on screen to say so.
// Quadrants only escapes this because its factory callocs
// (AppletRegistry.h:47-50). DMAMEM is zeroed by Main.cpp's
// startup_middle_hook() (:262-265), which the core calls before
// __libc_init_array. See AudioAppletHost.h's note 3 for the full ordering.
static DMAMEM ReverbApplet reverb_applet;

OC_APP_CLASS(AppReverb, TWOCCS("RV"), "Reverb", "Reverb"),
  public HSApplication {
public:
  // 3 x uint64 of applet config (ReverbApplet::OnDataRequest fills [0..2]).
  OC_APP_INTERFACE_DECLARE(AppReverb, 24);

  // --- HSApplication ---
  // Reverb reads no digital input and has no clock, but it still has four
  // CVInputMaps, and those read HS::frame internally -- which only
  // HSApplication::BaseController() ever loads (HSApplication.h:74-85). An app
  // that skipped it would show CV modulation frozen at whatever the previous
  // app left behind, silently.
  void Start() final {}
  void Resume() final {}
  void Controller() final {
    // No ClockSetup_instance.Controller() here, unlike Delay: nothing in this
    // applet reads a clock, so pumping one would be work with no reader.
    host_.Tick();
  }
  void View() const final { DrawMenu(); }

private:
  AudioAppletHost host_{ reverb_applet, 1 };  // MONO: one in, summed to both out

  uint8_t page_ = ReverbAppNS::PAGE_MAIN;
  uint8_t cursor_[ReverbAppNS::PAGE_COUNT] = { 0, 0 };
  bool bypassed_ = false;

  uint8_t Cursor() const { return cursor_[page_]; }

  void AdjustRow(int dir);
  void DrawMain() const;
  void DrawCv() const;
  static void FormatCutoff(char *buf, size_t n, int hz);
};

// ---------------------------------------------------------------------------
// FLASHMEM throughout: this app's only per-tick surface is Controller() above,
// which forwards and returns. Drawing is at redraw rate, input at human rate.
// ---------------------------------------------------------------------------

FLASHMEM void AppReverb::Init() {
  page_ = ReverbAppNS::PAGE_MAIN;
  for (auto &c : cursor_) c = 0;
  // Not persisted: bypass is a performance A/B, not a setting.
  bypassed_ = false;
  // Nothing here touches the applet. Init() runs for EVERY app in the container
  // at boot regardless of which is current, so an app nobody opens must not
  // take a ~50KB reverb arena. It starts on this app's first RESUME.
}

FLASHMEM size_t AppReverb::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  std::array<uint64_t, HemisphereAudioApplet::CONFIG_SIZE> data = {};
  reverb_applet.OnDataRequest(data);
  // [3] is never written by ReverbApplet::OnDataRequest, so it is not stored.
  for (int i = 0; i < 3; ++i) stream_buffer.Write<uint64_t>(data[i]);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppReverb::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  std::array<uint64_t, HemisphereAudioApplet::CONFIG_SIZE> data = {};
  for (int i = 0; i < 3; ++i) data[i] = stream_buffer.Read<uint64_t>();
  // A short read returns zeros, and zeros look like a valid config here: size
  // 0, damp 0, mix 0, cutoff 0. Applying them would replace good defaults with
  // a reverb that is off in every dimension, so don't.
  if (stream_buffer.underflow()) return 0;
  reverb_applet.OnDataReceive(data);
  return stream_buffer.read();
}

FLASHMEM void AppReverb::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
      // Claims AudioIO's shared effect slot from whichever effect app held it
      // last -- which today means opening Reverb disconnects Delay, and
      // opening Delay disconnects Reverb. Exactly one live effect, by design
      // (AudioIO.h's kOutputRouteEffectSlot comment).
      host_.Enter(!bypassed_);
      break;
    case OC::APP_EVENT_SUSPEND:
      // Deliberately nothing: the effect keeps sounding after you leave the
      // screen, and gives up the slot only when another effect claims it.
      break;
    default: break;
  }
}

FLASHMEM void AppReverb::Loop() {}

void AppReverb::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

// --- draw ------------------------------------------------------------------

// The applet prints "%5dHz" (FreeverbApplet.h:78), which is 7 characters. That
// fits a 64px tile; at full-screen geometry the value field starts at x>=91 and
// 7 characters need x>=85, colliding with the bar's right edge at x=87. So:
// kHz with one decimal above 1000 ("17.5k", 5 chars), plain Hz below ("950Hz",
// 5 chars). Audio-Apps-Screens.md finding M-6.
FLASHMEM void AppReverb::FormatCutoff(char *buf, size_t n, int hz) {
  if (hz >= 1000) snprintf(buf, n, "%d.%dk", hz / 1000, (hz % 1000) / 100);
  else snprintf(buf, n, "%dHz", hz);
}

FLASHMEM void AppReverb::DrawMain() const {
  using namespace ReverbAppNS;
  namespace S = AudioEffectScreen;
  char buf[16];

  snprintf(buf, sizeof(buf), "%d%%", reverb_applet.GetSize());
  S::ParamRow(R_SIZE, "Size:", (float)reverb_applet.GetSize() / 100.0f, buf);

  // Damp points the same way here as the engine documents: 1 = strong
  // high-frequency damping (effect_freeverb_F32.h:41-48), and the applet passes
  // damp*0.01f straight through (:37). So "Damp: 90%" means dark. Samverb
  // currently means the opposite with the same word -- finding M-1, not this
  // app's to fix.
  snprintf(buf, sizeof(buf), "%d%%", reverb_applet.GetDamp());
  S::ParamRow(R_DAMP, "Damp:", (float)reverb_applet.GetDamp() / 100.0f, buf);

  FormatCutoff(buf, sizeof(buf), reverb_applet.GetCutoff());
  S::ParamRow(R_CUT, "Cut :",
              (float)reverb_applet.GetCutoff() / (float)ReverbApplet::kMaxCutoff,
              buf);

  snprintf(buf, sizeof(buf), "%d%%", reverb_applet.GetMix());
  S::ParamRow(R_MIX, "Mix :", (float)reverb_applet.GetMix() / 100.0f, buf);
}

FLASHMEM void AppReverb::DrawCv() const {
  using namespace ReverbAppNS;

  static const char *const kLabels[R_COUNT] = { "Size", "Damp", "Cut ", "Mix " };
  CVInputMap *maps[R_COUNT] = { &reverb_applet.SizeCV(), &reverb_applet.DampCV(),
                                &reverb_applet.CutoffCV(), &reverb_applet.MixCV() };

  for (int row = 0; row < R_COUNT; ++row)
    AudioEffectScreen::CvRow(row, kLabels[row], *maps[row]);
}

FLASHMEM void AppReverb::DrawMenu() const {
  using namespace ReverbAppNS;

  AudioEffectScreen::Header("R E V E R B", host_.Live());

  if (!host_.Started()) {
    gfxFooter("starting");
    return;
  }

  if (!reverb_applet.ArenaReady()) {
    // Factory::get() found neither ~50KB of RAM2 nor its PSRAM fallback. The
    // applet already fails safe -- dry forced to unity, wet silent
    // (FreeverbApplet.h:38-41) -- so the honest words are the consequence plus
    // a remedy that is actually true here: unlike Delay's missing PSRAM chip,
    // a RAM2 arena IS freed by a reboot.
    AudioEffectScreen::RefusalBox("NO RAM: DRY ONLY");
    gfxFooter("no RAM - reboot frees");
    return;
  }

  if (page_ == PAGE_CV) DrawCv();
  else DrawMain();

  // One inversion, on the row encR will change.
  AudioEffectScreen::CursorBand(Cursor());

  if (bypassed_) {
    gfxFooter("BYPASSED - A:active");
  } else {
    // B is absent because B is unbound; Y appears only where every row has a
    // source to cycle. A footer that names a press the page does not have is
    // worse than a short one.
    gfxFooter(page_ == PAGE_CV ? "A:byp  Y:src  R:main" : "A:byp  R:cv");
  }
}

// Nothing moves on this screen on its own, so there is nothing to animate while
// the module is idle -- even though the reverb is still running and audible.
FLASHMEM void AppReverb::DrawScreensaver() const {}

// --- input -----------------------------------------------------------------

FLASHMEM void AppReverb::AdjustRow(int dir) {
  using namespace ReverbAppNS;
  namespace S = AudioEffectScreen;
  if (dir == 0) return;

  if (page_ == PAGE_CV) {
    // encR is attenuversion on every CV row of every app in this family, so
    // encR's meaning ("the number on this row") never changes. Same -127..127
    // clamp as HSApplication::EditSelectedInputMap (:248-251).
    CVInputMap *maps[R_COUNT] = { &reverb_applet.SizeCV(), &reverb_applet.DampCV(),
                                  &reverb_applet.CutoffCV(),
                                  &reverb_applet.MixCV() };
    int8_t &att = maps[Cursor()]->attenuversion;
    att = constrain(att + dir * S::Step(4, 1), -127, 127);
    return;
  }

  switch (Cursor()) {
    // Percent rows: 2 coarse puts a full sweep in 50 detents, 1 fine is the
    // finest step the parameter has. Same as Delay's percent rows.
    case R_SIZE: reverb_applet.NudgeSize(dir * S::Step(2, 1)); break;
    case R_DAMP: reverb_applet.NudgeDamp(dir * S::Step(2, 1)); break;
    // 100Hz coarse crosses 0..17500 in 175 detents; 10Hz fine is meaningful at
    // the bottom of the range where 100Hz is a large musical step. The applet's
    // own 50Hz step (FreeverbApplet.h:142) needed 350 detents for one sweep.
    case R_CUT:  reverb_applet.NudgeCutoff(dir * S::Step(100, 10)); break;
    case R_MIX:  reverb_applet.NudgeMix(dir * S::Step(2, 1)); break;
    default: break;
  }
}

FLASHMEM void AppReverb::HandleButtonEvent(const UI::Event &event) {
  using namespace ReverbAppNS;
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  switch (event.control) {
    case OC::CONTROL_BUTTON_UP:     // A: bypass
      bypassed_ = !bypassed_;
      // Output cables only. The applet keeps running, so the tail survives and
      // coming back is instant and lossless.
      host_.SetOutput(!bypassed_);
      break;
    case OC::CONTROL_BUTTON_R:      // encR: next page
      page_ = (uint8_t)((page_ + 1) % PAGE_COUNT);
      break;
    case OC::CONTROL_BUTTON_L:      // encL: back to MAIN
      page_ = PAGE_MAIN;
      break;
    case OC::CONTROL_BUTTON_Y:      // Y: cycle this row's source
      if (page_ == PAGE_CV) {
        CVInputMap *maps[R_COUNT] = { &reverb_applet.SizeCV(),
                                      &reverb_applet.DampCV(),
                                      &reverb_applet.CutoffCV(),
                                      &reverb_applet.MixCV() };
        maps[Cursor()]->ChangeSource(1);
      }
      break;
    default: break;                 // B unbound; X is a held modifier
  }
}

FLASHMEM void AppReverb::HandleEncoderEvent(const UI::Event &event) {
  if (event.control == OC::CONTROL_ENCODER_L) {
    int c = (int)cursor_[page_] + event.value;
    // Clamped, not wrapped: with four rows a wrap lands three rows from where
    // the eye was and reads as a jump.
    CONSTRAIN(c, 0, (int)ReverbAppNS::R_COUNT - 1);
    cursor_[page_] = (uint8_t)c;
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    AdjustRow(event.value);
  }
}

FLASHMEM void AppReverb::GetIOConfig(OC::IOConfig &ioconfig) const {
  using namespace OC;
  // The four CV sources are freely assignable from the CV page, so there is no
  // fixed jack-to-parameter map to declare. What is true and worth saying is
  // that this app drives no CV output at all.
  for (int ch = 0; ch < DAC_CHANNEL_COUNT; ++ch)
    ioconfig.outputs[ch].set("off", OC::OUTPUT_MODE_RAW);
}

FLASHMEM void AppReverb::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("slot ");
  graphics.print(host_.HoldsSlot() ? "held" : "-");
  graphics.setPrintPos(2, 22);
  graphics.print("arena ");
  graphics.print(host_.Started() && reverb_applet.ArenaReady() ? "ok" : "NONE");
}
