#pragma once
// ---------------------------------------------------------------------------
// Bungverb -- the Schroeder/Moorer reverb applet (audio_applets/SamverbApplet.h)
// as a standalone full-screen app in the AUDIO folder. Third in the family,
// after Delay and Reverb, on the same host and the same screen grammar.
//
// WHY "BUNGVERB" AND NOT "SAMVERB". This effect has three names in the tree:
// the file is SamverbApplet.h, the class is BungverbApplet, and applet_name()
// returns "Bungverb". Exactly one of those three is visible to a player -- the
// applet_name(), which is what the tile prints in Quadrants -- so that is the
// one this app matches. A player with the tile and the app both open must not
// meet two names for one effect.
//
// AND IT IS NOT SAFE TO JUST RENAME IT. applet_id() defaults to
// strhash(applet_name()) (HemisphereAudioApplet.h:45-47), and that id is what
// save/load matches on, so changing the string silently invalidates every
// stored preset referencing this applet. Unifying the three names is a
// deliberate act that must override applet_id() to return strhash("Bungverb")
// explicitly, and must say so in its commit. Not done here, and not to be done
// by accident. Same call as Reverb, for the same reason.
//
// IT IS A SCREEN AND NOTHING ELSE. AudioAppletHost needed no change to take it
// (a third applet, second mono one), and AudioEffectScreen supplies every mark.
// What is in this file is four parameters, what their bars and values mean, and
// what the buttons do.
//
// TWO PAGES, MAIN -> CV, like Reverb: no time unit, no clock source, no
// modulation type, so no MODE page. B is unbound because this applet has no
// send_mode bit, and adding persisted state to make a footer symmetrical would
// be the wrong trade (Audio-Apps-Screens.md section 7 item 7).
//
// DAMP READS THE SAME WAY HERE AS IN REVERB: larger = darker. See the long note
// on SamverbApplet's accessor block -- the applet passes 1.0f - damp*0.01f and
// that is CORRECT, because AudioEffectReverbSchroederF32 multiplies the input
// by its coefficient where AudioEffectFreeverbF32 multiplies the state. Finding
// M-1 claims these two disagree; they do not, and making them "agree" would
// invert this one.
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
// INCLUDE-ORDER NOTE: SamverbApplet's `applet_name()` only satisfies
// HemisphereApplet's pure virtual through the macro at
// audio_applets/_config.h:16. OC_apps.cpp includes that file (:65) before
// apps/_config.h (:81), so by the time this header compiles the applet is
// already fully defined and the include below is a no-op. Including this app
// ahead of audio_applets/_config.h would make BungverbApplet abstract and the
// static instance below uninstantiable, with an error naming none of this.
// ---------------------------------------------------------------------------

#include "../AudioAppletHost.h"
#include "../AudioEffectScreen.h"
#include "../HSApplication.h"
#include "../audio_applets/SamverbApplet.h"

namespace BungverbAppNS {

enum Page : uint8_t { PAGE_MAIN, PAGE_CV, PAGE_COUNT };

// The applet's own cursor order (SamverbApplet.h: DECAY_TIME, DAMP, CUTOFF,
// MIX), so nobody moving between the tile and the app relearns anything. Both
// pages use it.
enum { B_TIME, B_DAMP, B_CUT, B_MIX, B_COUNT };

}  // namespace BungverbAppNS

// Zero-initialised storage, and load-bearing rather than tidy:
// HemisphereApplet::applet_started is declared with no initializer and the class
// has no constructor (HemisphereApplet.h:462-463), so an applet in garbage
// memory can boot with applet_started already true and never run Start().
// DMAMEM is zeroed by Main.cpp's startup_middle_hook() (:262-265) before
// __libc_init_array. See AudioAppletHost.h's note 3.
static DMAMEM BungverbApplet bungverb_applet;

OC_APP_CLASS(AppBungverb, TWOCCS("BV"), "Bungverb", "Bungverb"),
  public HSApplication {
public:
  // 4 x uint64: unlike Delay and Reverb, this applet's OnDataRequest fills all
  // four slots -- decay_time gets data[3] to itself (SamverbApplet.h:97-101).
  OC_APP_INTERFACE_DECLARE(AppBungverb, 32);

  // --- HSApplication ---
  // No clock and no digital input, but four CVInputMaps, and those read
  // HS::frame internally -- which only HSApplication::BaseController() loads
  // (HSApplication.h:74-85). Skipping it would freeze CV modulation at whatever
  // the previous app left behind, silently.
  void Start() final {}
  void Resume() final {}
  void Controller() final {
    // No ClockSetup_instance.Controller(): nothing here reads a clock.
    host_.Tick();
  }
  void View() const final { DrawMenu(); }

private:
  AudioAppletHost host_{ bungverb_applet, 1 };  // MONO: one in, summed to both

  uint8_t page_ = BungverbAppNS::PAGE_MAIN;
  uint8_t cursor_[BungverbAppNS::PAGE_COUNT] = { 0, 0 };
  bool bypassed_ = false;

  uint8_t Cursor() const { return cursor_[page_]; }
  CVInputMap *MapForRow(int row) const;

  void AdjustRow(int dir);
  void DrawMain() const;
  void DrawCv() const;
};

// ---------------------------------------------------------------------------
// FLASHMEM throughout: the only per-tick surface is Controller() above, which
// forwards and returns.
// ---------------------------------------------------------------------------

// One place that knows the row -> CV map correspondence, so the draw, the
// encoder and the Y button cannot drift apart.
FLASHMEM CVInputMap *AppBungverb::MapForRow(int row) const {
  using namespace BungverbAppNS;
  switch (row) {
    case B_DAMP: return &bungverb_applet.DampCV();
    case B_CUT:  return &bungverb_applet.CutoffCV();
    case B_MIX:  return &bungverb_applet.MixCV();
    default:     return &bungverb_applet.DecayCV();
  }
}

FLASHMEM void AppBungverb::Init() {
  page_ = BungverbAppNS::PAGE_MAIN;
  for (auto &c : cursor_) c = 0;
  // Not persisted: bypass is a performance A/B, not a setting.
  bypassed_ = false;
  // Nothing here touches the applet. Init() runs for EVERY app at boot, so an
  // app nobody opens must not take an ~85KB reverb arena. It starts on this
  // app's first RESUME.
}

FLASHMEM size_t AppBungverb::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  std::array<uint64_t, HemisphereAudioApplet::CONFIG_SIZE> data = {};
  bungverb_applet.OnDataRequest(data);
  for (size_t i = 0; i < HemisphereAudioApplet::CONFIG_SIZE; ++i)
    stream_buffer.Write<uint64_t>(data[i]);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppBungverb::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  std::array<uint64_t, HemisphereAudioApplet::CONFIG_SIZE> data = {};
  for (size_t i = 0; i < HemisphereAudioApplet::CONFIG_SIZE; ++i)
    data[i] = stream_buffer.Read<uint64_t>();
  // A short read returns zeros, and zeros look like a valid config: decay 0,
  // damp 0, mix 0, cutoff 0. Applying them would replace good defaults with a
  // reverb that is off in every dimension.
  if (stream_buffer.underflow()) return 0;
  bungverb_applet.OnDataReceive(data);
  return stream_buffer.read();
}

FLASHMEM void AppBungverb::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
      // Claims AudioIO's shared effect slot from whichever effect app held it
      // last. Exactly one live effect, by design.
      host_.Enter(!bypassed_);
      break;
    case OC::APP_EVENT_SUSPEND:
      // Deliberately nothing: the effect keeps sounding after you leave.
      break;
    default: break;
  }
}

FLASHMEM void AppBungverb::Loop() {}

void AppBungverb::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

// --- draw ------------------------------------------------------------------

FLASHMEM void AppBungverb::DrawMain() const {
  using namespace BungverbAppNS;
  namespace S = AudioEffectScreen;
  char buf[16];

  // Tenths throughout, so the display can never show a value the parameter is
  // not actually on -- see NudgeDecayTenths()'s comment for the float-drift it
  // replaced. Max "20.0s" is 5 characters, starting at x=97.
  const int tenths = bungverb_applet.GetDecayTenths();
  snprintf(buf, sizeof(buf), "%d.%ds", tenths / 10, tenths % 10);
  S::ParamRow(B_TIME, "Time:",
              (float)tenths / (float)BungverbApplet::kMaxDecayTenths, buf);

  // Larger = darker, same as Reverb. The applet's 1.0f - damp inversion is
  // compensating for its engine, not disagreeing with Reverb's label.
  snprintf(buf, sizeof(buf), "%d%%", bungverb_applet.GetDamp());
  S::ParamRow(B_DAMP, "Damp:", (float)bungverb_applet.GetDamp() / 100.0f, buf);

  // kHz above 1000, plain Hz below -- the applet's own "%5dHz" (:80) is 7
  // characters and would collide with the bar's right edge at x=87 at
  // full-screen geometry. Finding M-6, same as Reverb.
  const int hz = bungverb_applet.GetCutoff();
  if (hz >= 1000) snprintf(buf, sizeof(buf), "%d.%dk", hz / 1000, (hz % 1000) / 100);
  else snprintf(buf, sizeof(buf), "%dHz", hz);
  S::ParamRow(B_CUT, "Cut :",
              (float)hz / (float)BungverbApplet::kMaxCutoff, buf);

  snprintf(buf, sizeof(buf), "%d%%", bungverb_applet.GetMix());
  S::ParamRow(B_MIX, "Mix :", (float)bungverb_applet.GetMix() / 100.0f, buf);
}

FLASHMEM void AppBungverb::DrawCv() const {
  using namespace BungverbAppNS;
  static const char *const kLabels[B_COUNT] = { "Time", "Damp", "Cut ", "Mix " };
  for (int row = 0; row < B_COUNT; ++row)
    AudioEffectScreen::CvRow(row, kLabels[row], *MapForRow(row));
}

FLASHMEM void AppBungverb::DrawMenu() const {
  using namespace BungverbAppNS;

  AudioEffectScreen::Header("B U N G V E R B", host_.Live());

  if (!host_.Started()) {
    gfxFooter("starting");
    return;
  }

  if (!bungverb_applet.ArenaReady()) {
    // Factory::get() found neither ~85KB of RAM2 nor its PSRAM fallback. The
    // applet already fails safe -- dry forced to unity, wet silent -- so the
    // honest words are the consequence plus a remedy that is true here: a RAM2
    // arena IS freed by a reboot, unlike Delay's missing PSRAM chip.
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
    gfxFooter(page_ == PAGE_CV ? "A:byp  Y:src  R:main" : "A:byp  R:cv");
  }
}

// Nothing moves on this screen on its own, even though the reverb is still
// running and audible while the module is idle.
FLASHMEM void AppBungverb::DrawScreensaver() const {}

// --- input -----------------------------------------------------------------

FLASHMEM void AppBungverb::AdjustRow(int dir) {
  using namespace BungverbAppNS;
  namespace S = AudioEffectScreen;
  if (dir == 0) return;

  if (page_ == PAGE_CV) {
    // encR is attenuversion on every CV row of every app in this family. Same
    // -127..127 clamp as HSApplication::EditSelectedInputMap (:248-251).
    int8_t &att = MapForRow(Cursor())->attenuversion;
    att = constrain(att + dir * S::Step(4, 1), -127, 127);
    return;
  }

  switch (Cursor()) {
    // 0.5s coarse crosses 0.1..20s in 40 detents; 0.1s fine is the finest step
    // the parameter has, and the engine's own floor
    // (effect_reverb_schroeder_F32.h:28). X therefore always does something on
    // this row, which a blanket "divide by ten" would not have managed.
    case B_TIME: bungverb_applet.NudgeDecayTenths(dir * S::Step(5, 1)); break;
    case B_DAMP: bungverb_applet.NudgeDamp(dir * S::Step(2, 1)); break;
    // 100Hz coarse crosses 0..17500 in 175 detents; 10Hz fine is meaningful at
    // the bottom of the range. Same as Reverb.
    case B_CUT:  bungverb_applet.NudgeCutoff(dir * S::Step(100, 10)); break;
    case B_MIX:  bungverb_applet.NudgeMix(dir * S::Step(2, 1)); break;
    default: break;
  }
}

FLASHMEM void AppBungverb::HandleButtonEvent(const UI::Event &event) {
  using namespace BungverbAppNS;
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
      if (page_ == PAGE_CV) MapForRow(Cursor())->ChangeSource(1);
      break;
    default: break;                 // B unbound; X is a held modifier
  }
}

FLASHMEM void AppBungverb::HandleEncoderEvent(const UI::Event &event) {
  if (event.control == OC::CONTROL_ENCODER_L) {
    int c = (int)cursor_[page_] + event.value;
    // Clamped, not wrapped: with four rows a wrap lands three rows from where
    // the eye was and reads as a jump.
    CONSTRAIN(c, 0, (int)BungverbAppNS::B_COUNT - 1);
    cursor_[page_] = (uint8_t)c;
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    AdjustRow(event.value);
  }
}

FLASHMEM void AppBungverb::GetIOConfig(OC::IOConfig &ioconfig) const {
  using namespace OC;
  // The four CV sources are freely assignable from the CV page, so there is no
  // fixed jack-to-parameter map to declare. This app drives no CV output.
  for (int ch = 0; ch < DAC_CHANNEL_COUNT; ++ch)
    ioconfig.outputs[ch].set("off", OC::OUTPUT_MODE_RAW);
}

FLASHMEM void AppBungverb::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("slot ");
  graphics.print(host_.HoldsSlot() ? "held" : "-");
  graphics.setPrintPos(2, 22);
  graphics.print("arena ");
  graphics.print(host_.Started() && bungverb_applet.ArenaReady() ? "ok" : "NONE");
}
