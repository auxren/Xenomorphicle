#pragma once
// ---------------------------------------------------------------------------
// Tweighty -- an 8-tap looping delay whose transport is WRITE/RECIRC, not
// a fixed feedback loop: RECIRC freezes a captured window and lets it
// recirculate, with an equal-power crossfade smoothing the WRITE<->RECIRC
// edge (and every RECIRC loop-wrap) so the freeze/thaw never clicks however
// deep the feedback is set. Design lifted from the Buchla-format 288r Time
// Domain Processor's TIME-mode transport, ported as an original
// implementation rather than a literal port -- this app makes no claim to
// 288r hardware fidelity, hence the plain name rather than a model number.
//
// PITCH mode (dual-head windowed reader) is NOT implemented -- 288r's own
// docs mark it as having an unresolved buffer-wrap glitch. TIME mode only
// for v1.
//
// TWO-STATE, not three: WRITE records the panel input into the loop buffer
// continuously; RECIRC gates fresh input off so only the captured window
// keeps recirculating through feedback. "Loop" is what RECIRC's playback
// does once a window exists, not a state of its own -- see
// TweightyTransport.h for why, and AudioTweightyF32.h for how the two
// modes share one read path with no seam.
//
// CONTROLS (same panel grammar as every other full-screen app here -- A, B,
// X, Y, both encoder pushes; CONTROL_BUTTON_M/"Z" is not wired on this
// hardware, see Bus200eApp.h's header for why):
//   A = WRITE<->RECIRC toggle (same edge Digital-In-1 drives)
//   B = envelope output on/off (DAC ch 1)
//   encR push = enter/leave the edit screen   encL push = back to home
//   SCR_EDIT: encL turn = field select   encR turn = adjust, direct-commit
//             (no confirm gate -- unlike a 200e bank write, every edit here
//             is instantly reversible, so there is nothing to arm)
//
// AUDIO WIRING: the engine is F32-native: OC::AudioIO's int16 in/out streams
// bridge to it through AudioConvertI16toF32Multi<2>/AudioConvertF32toI16Multi
// <2> (the same edge adapters HemisphereAudioAppletF32 uses for every F32
// audio applet). Connections are built once, lazily, and only ever
// connect()ed while this app is on screen -- mirrors TunerApp.h's WireAudio/
// SetActive pattern, widened here to a full duplex path since this app
// writes audio out, not just analyzes it.
// ---------------------------------------------------------------------------

#include "../Audio/AudioTweightyF32.h"
#include "../TweightyTapPhase.h"
#include "../TweightyTransport.h"
#include "../HemisphereAudioAppletF32.h"
#include "../AudioIO.h"
#include "../util/util_macros.h"

namespace TweightyAppNS {

enum Screen : uint8_t {
  SCR_HOME = 0,
  SCR_EDIT,
};

// SCR_EDIT's field cursor.
enum EditParam : uint8_t {
  EDIT_TIME = 0,
  EDIT_TAPS,
  EDIT_FEEDBACK,
  EDIT_WETDRY,
  EDIT_COUNT,
};

// Time range in centiseconds -- the persisted unit (u16). Floor is a UI
// choice (audibly a very short slap, well above the engine's own
// kChunkSize-derived floor); ceiling matches AudioTweightyF32::
// MAX_LOOP_SECONDS exactly, which is itself borrowed from DelayApplet.h's
// bench-proven float32/one-pole slew range -- see that class's header
// comment for why this number is not re-derived here.
static constexpr uint16_t kMinTimeCentis = 5;      // 0.05s
static constexpr uint16_t kMaxTimeCentis = 1200;   // 12.00s
static constexpr uint16_t kDefaultTimeCentis = 100; // 1.00s

}  // namespace TweightyAppNS

OC_APP_CLASS(AppTweighty, TWOCCS("TW"), "Tweighty", "Delay Loop"),
  public HSApplication {
public:
  // transport_state_ u8, tap_count_ u8, time_centis_ u16, feedback_ u8,
  // wet_dry_ u8, env_out_ u8 = 7 bytes. The tap phase ring is NOT persisted
  // here -- it is re-derived from TweightyTapPhaseDefault() on restore, so
  // there is no eighth field carrying it.
  OC_APP_INTERFACE_DECLARE(AppTweighty, 7);

  void Start() final {}
  void Resume() final {}

  // ISR-hot (core timer): push every live control into the engine each pass
  // and drive the WRITE<->RECIRC toggle from Digital-In-1's rising edge.
  // Deliberately in-class (NOT FLASHMEM) -- see the file-tail comment on why
  // everything below this class is out-of-class instead.
  void Controller() final {
    using namespace TweightyAppNS;

    // Digital-In-1 and button A both drive the same toggle (see the header
    // comment). Clock(ch) is already an edge flag in this framework
    // (HSIOFrame's trigmap), but routing it through the same
    // transport_rising_edge() helper TweightyTransport's own host tests
    // exercise keeps this call site identical in shape to what a raw level
    // input would need, and costs nothing extra.
    if (transport_rising_edge(Clock(0), prev_digital_toggle_))
      engine_.RequestTransportToggle();

    engine_.SetTapCount(tap_count_);
    engine_.SetFeedback((float)feedback_byte_ / 255.0f);
    engine_.SetWetDry((float)wetdry_byte_ / 255.0f);

    // TIME: CV0 is a live multiplier on the persisted base length, not a
    // rewrite of it. SetTargetTimeSeconds is what drives RECIRC's wrap
    // detector, so it only ever sees the panel/knob value -- patching CV0
    // cannot silently retune the loop window out from under a captured
    // RECIRC window. Each tap's own crossfade-smoothed target is where the
    // modulation actually lands instead.
    const float base_secs = (float)time_centis_ * 0.01f;
    float time_mult = 1.0f + (float)In(0) / (float)HSAPPLICATION_5V;
    CONSTRAIN(time_mult, 0.1f, 2.0f);

    engine_.SetTargetTimeSeconds(base_secs);
    for (int i = 0; i < kTweightyTapCount; ++i) {
      const float secs =
          TweightyTapTargetSecs(base_secs, phase_table_.phase[i], time_mult);
      engine_.SetTapTargetSecs(i, secs);
    }

    if (env_out_)
      Out(0, (int)(engine_.meter_level_ * (float)HSAPPLICATION_5V));
  }

  void View() const final { DrawMenu(); }

private:
  // --- audio graph -----------------------------------------------------
  AudioTweightyF32 engine_;
  AudioConvertI16toF32Multi<2> in_adapter_;
  AudioConvertF32toI16Multi<2> out_adapter_;
  AudioConnection *conn_in_l_ = nullptr;
  AudioConnection *conn_in_r_ = nullptr;
  AudioConnection_F32 *conn_f32_in_l_ = nullptr;
  AudioConnection_F32 *conn_f32_in_r_ = nullptr;
  AudioConnection_F32 *conn_f32_out_l_ = nullptr;
  AudioConnection_F32 *conn_f32_out_r_ = nullptr;
  AudioConnection *conn_out_l_ = nullptr;
  AudioConnection *conn_out_r_ = nullptr;
  bool audio_wired_ = false;      // connections exist (built once, lazily)
  bool engine_acquired_ = false;  // PSRAM buffer currently held

  // --- UI state ----------------------------------------------------------
  uint8_t screen_ = TweightyAppNS::SCR_HOME;
  uint8_t edit_cursor_ = TweightyAppNS::EDIT_TIME;
  bool prev_digital_toggle_ = false;

  // Owned here, not by the engine -- it lives with the settings it is
  // reasoned about alongside, and Controller() pushes it through the
  // engine's per-tap setter every pass. Not persisted; see the class
  // comment on OC_APP_INTERFACE_DECLARE above.
  TweightyTapPhaseTable phase_table_;

  // --- persisted (7 bytes total) ------------------------------------------
  uint16_t time_centis_ = TweightyAppNS::kDefaultTimeCentis;
  uint8_t tap_count_ = kTweightyTapCount;
  uint8_t feedback_byte_ = 77;    // ~30%
  uint8_t wetdry_byte_ = 128;     // 50%
  bool env_out_ = false;

  void WireAudio();
  void SetActive(bool on);
  void DrawHome() const;
  void DrawEdit() const;
  void AdjustEditParam(int delta);
  static void DrawBar(int x, int y, int w, float frac);
};

// ---------------------------------------------------------------------------
// Everything below is out-of-class and FLASHMEM. LTO silently drops FLASHMEM
// from in-class (implicitly-inline) definitions, which is how this repo has
// repeatedly tipped a 32KB ITCM bank boundary -- see Bus200eApp.h's tail
// comment. Controller() above and AudioTweightyF32's own methods are the
// only exceptions, both audio/ISR-hot by design.
// ---------------------------------------------------------------------------

FLASHMEM void AppTweighty::Init() {
  using namespace TweightyAppNS;
  screen_ = SCR_HOME;
  edit_cursor_ = EDIT_TIME;
  prev_digital_toggle_ = false;
  time_centis_ = kDefaultTimeCentis;
  tap_count_ = kTweightyTapCount;
  feedback_byte_ = 77;
  wetdry_byte_ = 128;
  env_out_ = false;
  TweightyTapPhaseDefault(phase_table_);
}

FLASHMEM size_t AppTweighty::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  // volatile (audio-ISR-hot mirror) -> plain local: Write<T>() binds a
  // const T&, which cannot bind directly to a volatile member.
  const uint8_t transport_snapshot = engine_.transport_state_;
  stream_buffer.Write<uint8_t>(transport_snapshot);
  stream_buffer.Write<uint8_t>(tap_count_);
  stream_buffer.Write<uint16_t>(time_centis_);
  stream_buffer.Write<uint8_t>(feedback_byte_);
  stream_buffer.Write<uint8_t>(wetdry_byte_);
  stream_buffer.Write<uint8_t>(env_out_ ? 1 : 0);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppTweighty::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  using namespace TweightyAppNS;
  const uint8_t transport_byte = stream_buffer.Read<uint8_t>();
  const uint8_t taps = stream_buffer.Read<uint8_t>();
  const uint16_t centis = stream_buffer.Read<uint16_t>();
  const uint8_t fb = stream_buffer.Read<uint8_t>();
  const uint8_t wd = stream_buffer.Read<uint8_t>();
  const uint8_t env = stream_buffer.Read<uint8_t>();

  tap_count_ = (taps >= 1 && taps <= kTweightyTapCount)
                   ? taps : (uint8_t)kTweightyTapCount;
  time_centis_ = (centis >= kMinTimeCentis && centis <= kMaxTimeCentis)
                     ? centis : kDefaultTimeCentis;
  feedback_byte_ = fb;
  wetdry_byte_ = wd;
  env_out_ = env != 0;
  TweightyTapPhaseDefault(phase_table_);

  engine_.SetTapCount(tap_count_);
  engine_.SetFeedback((float)feedback_byte_ / 255.0f);
  engine_.SetWetDry((float)wetdry_byte_ / 255.0f);

  // The engine boots into WRITE (its own default); only flip it if the saved
  // state disagrees. RequestTransportToggle() only mutates the plain
  // TweightyTransportState struct -- no buffer access -- so this is safe
  // to call here, before the engine has ever been Acquire()d.
  if (transport_byte == XP_RECIRC && engine_.transport_state_ == XP_WRITE)
    engine_.RequestTransportToggle();

  return stream_buffer.underflow() ? 0 : stream_buffer.read();
}

// The audio graph is global and always running, so the taps into it are
// built once and connected only while this app is on screen -- an idle
// engine would otherwise cost a PSRAM buffer and CPU in every other app.
// Mirrors TunerApp.h's WireAudio(), widened to the full duplex path this app
// needs (Tuner only ever taps the input for analysis).
FLASHMEM void AppTweighty::WireAudio() {
  if (audio_wired_) return;
#ifdef AUDIO_INTERFACE
  conn_in_l_ = new AudioConnection(OC::AudioIO::InputStream(0), 0, in_adapter_, 0);
  conn_in_r_ = new AudioConnection(OC::AudioIO::InputStream(0), 1, in_adapter_, 1);
  conn_f32_in_l_ = new AudioConnection_F32(in_adapter_, 0, engine_, 0);
  conn_f32_in_r_ = new AudioConnection_F32(in_adapter_, 1, engine_, 1);
  conn_f32_out_l_ = new AudioConnection_F32(engine_, 0, out_adapter_, 0);
  conn_f32_out_r_ = new AudioConnection_F32(engine_, 1, out_adapter_, 1);
  conn_out_l_ = new AudioConnection(out_adapter_, 0, OC::AudioIO::OutputStream(), 0);
  conn_out_r_ = new AudioConnection(out_adapter_, 1, OC::AudioIO::OutputStream(), 1);
  if (conn_in_l_) conn_in_l_->disconnect();
  if (conn_in_r_) conn_in_r_->disconnect();
  if (conn_f32_in_l_) conn_f32_in_l_->disconnect();
  if (conn_f32_in_r_) conn_f32_in_r_->disconnect();
  if (conn_f32_out_l_) conn_f32_out_l_->disconnect();
  if (conn_f32_out_r_) conn_f32_out_r_->disconnect();
  if (conn_out_l_) conn_out_l_->disconnect();
  if (conn_out_r_) conn_out_r_->disconnect();
#endif
  audio_wired_ = true;
}

// Resume/suspend: connect the graph and take the PSRAM buffer, or give both
// back. Acquire()/Release() bracket every activation (not just the first),
// so an idle Tweighty app holds no PSRAM for other apps to contend with.
//
// AudioNoInterrupts()/AudioInterrupts() bracket every connect/disconnect and
// Acquire()/Release() here (matches AudioAppletSubapp.h's own convention for
// exactly this hazard) because AudioConnection_F32::disconnect() -- unlike
// the stock int16 AudioConnection this codebase widened -- never resets the
// stream's `active` flag, so update() keeps being called from the audio ISR
// on this engine even after the disconnect calls below return. Without the
// bracket, the ISR could run engine_.update() concurrently with
// engine_.Release() freeing the crossfade tables out from under it -- a
// reachable use-after-free, not a theoretical one: any ordinary app-switch
// away from Tweighty while a crossfade is mid-flight (up to ~46ms after any
// tap edit or transport toggle) would hit it. engine_'s own ready_ flag
// (see AudioTweightyF32::Acquire/Release) is the second half of the fix --
// it protects update() calls that land outside this bracket too, since nothing
// in this codebase can currently fix the disconnect()-never-resets-active
// gap without touching the shared F32 library (tracked in TODO.md).
FLASHMEM void AppTweighty::SetActive(bool on) {
  WireAudio();
  AudioNoInterrupts();
  if (on) {
    engine_.Acquire();
    engine_acquired_ = true;
#ifdef AUDIO_INTERFACE
    if (conn_in_l_) conn_in_l_->connect();
    if (conn_in_r_) conn_in_r_->connect();
    // AudioConnection_F32's no-arg connect() is protected (unlike the int16
    // AudioConnection this codebase widened for exactly this pooled-cable
    // use) -- only the 4-arg form is public, so reconnect names the same
    // endpoints WireAudio() built the pointer with.
    if (conn_f32_in_l_) conn_f32_in_l_->connect(in_adapter_, 0, engine_, 0);
    if (conn_f32_in_r_) conn_f32_in_r_->connect(in_adapter_, 1, engine_, 1);
    if (conn_f32_out_l_) conn_f32_out_l_->connect(engine_, 0, out_adapter_, 0);
    if (conn_f32_out_r_) conn_f32_out_r_->connect(engine_, 1, out_adapter_, 1);
    if (conn_out_l_) conn_out_l_->connect();
    if (conn_out_r_) conn_out_r_->connect();
#endif
  } else {
#ifdef AUDIO_INTERFACE
    if (conn_in_l_) conn_in_l_->disconnect();
    if (conn_in_r_) conn_in_r_->disconnect();
    if (conn_f32_in_l_) conn_f32_in_l_->disconnect();
    if (conn_f32_in_r_) conn_f32_in_r_->disconnect();
    if (conn_f32_out_l_) conn_f32_out_l_->disconnect();
    if (conn_f32_out_r_) conn_f32_out_r_->disconnect();
    if (conn_out_l_) conn_out_l_->disconnect();
    if (conn_out_r_) conn_out_r_->disconnect();
#endif
    if (engine_acquired_) {
      engine_.Release();
      engine_acquired_ = false;
    }
  }
  AudioInterrupts();
}

FLASHMEM void AppTweighty::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME: SetActive(true); break;
    case OC::APP_EVENT_SUSPEND:
    case OC::APP_EVENT_SCREENSAVER_ON: SetActive(false); break;
    default: break;
  }
}

void AppTweighty::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

FLASHMEM void AppTweighty::Loop() {}

FLASHMEM void AppTweighty::AdjustEditParam(int delta) {
  using namespace TweightyAppNS;
  switch (edit_cursor_) {
    case EDIT_TIME: {
      int t = (int)time_centis_ + delta * 5;   // 0.05s per detent
      CONSTRAIN(t, (int)kMinTimeCentis, (int)kMaxTimeCentis);
      time_centis_ = (uint16_t)t;
      break;
    }
    case EDIT_TAPS: {
      int n = (int)tap_count_ + delta;
      CONSTRAIN(n, 1, kTweightyTapCount);
      tap_count_ = (uint8_t)n;
      break;
    }
    case EDIT_FEEDBACK: {
      int f = (int)feedback_byte_ + delta * 4;
      CONSTRAIN(f, 0, 255);
      feedback_byte_ = (uint8_t)f;
      break;
    }
    case EDIT_WETDRY: {
      int w = (int)wetdry_byte_ + delta * 4;
      CONSTRAIN(w, 0, 255);
      wetdry_byte_ = (uint8_t)w;
      break;
    }
    default: break;
  }
}

FLASHMEM void AppTweighty::HandleButtonEvent(const UI::Event &event) {
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  using namespace TweightyAppNS;
  switch (event.control) {
    case OC::CONTROL_BUTTON_UP:     // A = transport toggle, from either screen
      engine_.RequestTransportToggle();
      break;
    case OC::CONTROL_BUTTON_DOWN:   // B = envelope output on/off
      env_out_ = !env_out_;
      break;
    case OC::CONTROL_BUTTON_R:      // encR = enter the edit screen / leave it
      screen_ = (screen_ == SCR_HOME) ? SCR_EDIT : SCR_HOME;
      break;
    case OC::CONTROL_BUTTON_L:      // encL = back to home
      screen_ = SCR_HOME;
      break;
    default: break;   // X/Y unbound in v1
  }
}

FLASHMEM void AppTweighty::HandleEncoderEvent(const UI::Event &event) {
  using namespace TweightyAppNS;
  if (screen_ != SCR_EDIT) return;
  if (event.control == OC::CONTROL_ENCODER_L) {
    int c = (int)edit_cursor_ + event.value;
    CONSTRAIN(c, 0, EDIT_COUNT - 1);
    edit_cursor_ = (uint8_t)c;
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    AdjustEditParam(event.value);
  }
}

// Shared bar widget -- track (drawFrame) + fill (drawRect, 1px inset),
// used for every meter on both screens (Home's Time bar, Edit's four field
// bars and its live output meter) so the eye learns one meter grammar
// instead of five. frac is clamped here so every caller can pass a raw
// ratio without its own bounds check.
FLASHMEM void AppTweighty::DrawBar(int x, int y, int w, float frac) {
  graphics.drawFrame(x, y, w, 8);
  if (frac < 0.0f) frac = 0.0f;
  else if (frac > 1.0f) frac = 1.0f;
  const int fillw = (int)((w - 2) * frac + 0.5f);
  if (fillw > 0) graphics.drawRect(x + 1, y + 1, fillw, 6);
}

// SCR_HOME is the glance screen: what state is the module in, is the loop
// about to overflow its buffer (Time, the one panel value CV0 can make
// diverge from what's shown -- see Controller()), which taps are live.
// Feedback/Mix have no CV input (see GetIOConfig()) and are dialed in once,
// not re-read reflexively, so they live on SCR_EDIT only -- see that
// function's header comment for the full split rationale.
FLASHMEM void AppTweighty::DrawHome() const {
  const char *word = engine_.transport_state_ == XP_WRITE ? "WRITE" : "RECIRC";
  const int word_len = (int)strlen(word);
  graphics.setPrintPos((128 - word_len * 6) / 2, 12);
  graphics.print(word);

  // Filled = WRITE (the actively-capturing, "live" state) so this band
  // agrees with the tap ring below it, where filled has always meant
  // active/present -- one shape language for "the live thing" everywhere
  // in this app, not two shapes that mean opposite things on one screen.
  if (engine_.transport_state_ == XP_WRITE) graphics.drawRect(4, 21, 120, 9);
  else graphics.drawFrame(4, 21, 120, 9);

  graphics.drawHLine(0, 31, 128);

  graphics.setPrintPos(4, 34);
  graphics.print("Time");
  DrawBar(34, 34, 54,
          (float)(time_centis_ - TweightyAppNS::kMinTimeCentis) /
              (float)(TweightyAppNS::kMaxTimeCentis - TweightyAppNS::kMinTimeCentis));
  char buf[8];
  snprintf(buf, sizeof(buf), "%d.%02ds", time_centis_ / 100, time_centis_ % 100);
  graphics.setPrintPos(127, 34);
  graphics.print_right(buf);

  graphics.setPrintPos(4, 43);
  graphics.print(env_out_ ? "ENV ON" : "ENV OFF");

  // Tap ring: derived straight from tap_count_ (active_tap_mask_ is always
  // the low tap_count_ bits), so this is a density readout, not independent
  // state -- shape, not a number, because "how thick is this patch" is a
  // one-glance question a count would make you do arithmetic to answer.
  const uint8_t mask = engine_.active_tap_mask_;
  for (int i = 0; i < kTweightyTapCount; ++i) {
    const int x = 62 + i * 8;
    if (mask & (1 << i)) graphics.drawRect(x, 44, 6, 6);
    else graphics.drawFrame(x, 44, 6, 6);
  }

  gfxFooter("A:toggle B:env R:edit");
}

// SCR_EDIT is the complete programming surface: all four fields visible and
// legible at once (not just the selected one), each with a bar so "where am
// I in the range" is a glance instead of arithmetic against a spec you have
// to remember. A/B (transport toggle, envelope-out) already work from this
// screen -- HandleButtonEvent's CONTROL_BUTTON_UP/DOWN cases aren't gated on
// screen_ -- the baseline just never told you so; the status row and footer
// below fix that. The live output meter answers "watching the effect of
// each change" without leaving this screen. XFADE is cut from both screens
// (see DrawHome's absence of it too): the crossfade resolves in ~46ms, well
// under a glance's fixation time, so it can never be caught meaningfully.
//
// gfxFooter() is called FIRST, before the field rows: it clears+redraws
// y=54-63, which would erase the last cursor band's invertRect (it spans
// y=47-55) if drawn after.
FLASHMEM void AppTweighty::DrawEdit() const {
  using namespace TweightyAppNS;
  gfxFooter("A:toggle B:env L:back");

  graphics.setPrintPos(4, 12);
  graphics.print(engine_.transport_state_ == XP_WRITE ? "WRITE" : "RECIRC");
  graphics.setPrintPos(46, 12);
  graphics.print(env_out_ ? "ENV ON" : "ENV OFF");
  DrawBar(94, 12, 32, engine_.meter_level_);

  static const char *const kLabels[EDIT_COUNT] = {
    "Time", "Taps", "Fdbk", "Mix"
  };
  char buf[8];
  for (int i = 0; i < EDIT_COUNT; ++i) {
    const int y = 21 + i * 9;
    graphics.setPrintPos(4, y);
    graphics.print(kLabels[i]);
    graphics.setPrintPos(127, y);
    switch (i) {
      case EDIT_TIME:
        DrawBar(34, y, 54,
                (float)(time_centis_ - kMinTimeCentis) /
                    (float)(kMaxTimeCentis - kMinTimeCentis));
        snprintf(buf, sizeof(buf), "%d.%02ds", time_centis_ / 100, time_centis_ % 100);
        graphics.print_right(buf);
        break;
      case EDIT_TAPS:
        DrawBar(34, y, 54, (float)(tap_count_ - 1) / (float)(kTweightyTapCount - 1));
        snprintf(buf, sizeof(buf), "%d", (int)tap_count_);
        graphics.print_right(buf);
        break;
      case EDIT_FEEDBACK:
        DrawBar(34, y, 54, (float)feedback_byte_ / 255.0f);
        snprintf(buf, sizeof(buf), "%d%%", (feedback_byte_ * 100) / 255);
        graphics.print_right(buf);
        break;
      case EDIT_WETDRY:
        DrawBar(34, y, 54, (float)wetdry_byte_ / 255.0f);
        snprintf(buf, sizeof(buf), "%d%%", (wetdry_byte_ * 100) / 255);
        graphics.print_right(buf);
        break;
      default: break;
    }
    if (i == edit_cursor_) graphics.invertRect(0, 20 + i * 9, 128, 9);
  }
}

FLASHMEM void AppTweighty::DrawMenu() const {
  gfxHeader("T W E I G H T Y");
  using namespace TweightyAppNS;
  switch (screen_) {
    case SCR_EDIT: DrawEdit(); return;
    default: DrawHome(); return;
  }
}

FLASHMEM void AppTweighty::DrawScreensaver() const { DrawMenu(); }

FLASHMEM void AppTweighty::GetIOConfig(OC::IOConfig &ioconfig) const {
  using namespace OC;
  ioconfig.digital_inputs[DIGITAL_INPUT_1].set("Toggle");
  ioconfig.cv[0].set("Time");
  ioconfig.outputs[0].set(env_out_ ? "Envelope" : "off",
                          env_out_ ? OC::OUTPUT_MODE_UNI : OC::OUTPUT_MODE_RAW);
  for (int i = 1; i < DAC_CHANNEL_COUNT; ++i)
    ioconfig.outputs[i].set("off", OC::OUTPUT_MODE_RAW);
}

FLASHMEM void AppTweighty::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("meter ");
  graphics.print((int)(engine_.meter_level_ * 1000.0f));
  graphics.setPrintPos(2, 22);
  graphics.print("taps ");
  graphics.print((int)engine_.active_tap_mask_);
}
