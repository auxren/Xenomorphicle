#pragma once
// ---------------------------------------------------------------------------
// AudioAppletHost -- run ONE HemisphereAudioApplet as a standalone full-screen
// app, instead of as a quarter-screen tile inside Quadrants.
//
// This is the LIFECYCLE half only: start the applet exactly once, wire it into
// the audio graph, claim the shared output slot, pump its Controller(). It
// draws nothing and reads no parameter, because every applet's parameters are
// its own and its screen is its own. Each hosted app (apps/DelayApp.h, and
// Reverb/Samverb behind it) owns its layout and its input handling and talks
// to its applet through that applet's own accessors. Keeping the host ignorant
// of parameters is what lets one host serve three unrelated effects.
//
// WHAT THE HOST HAS TO GET RIGHT, and why each one is not optional:
//
// 1. HS::frame must be pumped. `HSApplication::BaseController()` is the ONLY
//    place `HS::frame.Load()` happens (HSApplication.h:74-85), and every one
//    of these applets reads its CV through CVInputMap/DigitalInputMap, which
//    read HS::frame internally (CVInputMap.h:73-111, 340-360). An app that
//    does not derive from HSApplication and call BaseController() from
//    Process() gets modulation frozen at whatever the last app left behind --
//    not broken, not blank, just silently stuck. So the APP derives from
//    HSApplication; this host is called from inside its Controller().
//
// 2. `HemisphereApplet::ProcessCursors()` has to run every tick or no cursor
//    anywhere blinks (Quadrants.h:437 calls it for exactly this reason).
//    Done here, so no hosted app can forget it.
//
// 3. `HemisphereApplet::applet_started` is UNINITIALIZED --
//    HemisphereApplet.h:462-463 declares it with no initializer and the class
//    has no constructor. Quadrants only escapes it because its applet factory
//    callocs (AppletRegistry.h:47-50). A standalone host must hold its applet
//    in zero-initialized storage or `Start()` may never run and the applet
//    silently never wires its own DSP. Every hosted app therefore declares its
//    applet `static DMAMEM`, which Main.cpp's startup_middle_hook() memsets to
//    zero (Main.cpp:262-265) BEFORE __libc_init_array runs the constructors --
//    order confirmed in the core's startup.c (configure_external_ram at :187,
//    startup_middle_hook at :192, __libc_init_array at :210), which also means
//    external_psram_size is already valid by the time an applet constructor
//    sizes a PSRAM buffer from it.
//
// 4. `SetDisplaySide()` is called ONCE and never changed. Everything the
//    applet's cursor state touches is keyed on `hemisphere`
//    (enc_edit[hemisphere], cursor_countdown[hemisphere]), so a host that
//    moved the side around would scramble it. AUDIO_SLOT_L, matching what
//    AudioAppletSubapp already uses for the left audio slot.
//
// THE OUTPUT SLOT is AudioIO's kOutputRouteEffectSlot, shared by every
// standalone effect app and CLAIMED on entry rather than owned -- see
// AudioIO.h for the three-part argument (shared not per-app, claimed not
// released on suspend, and never sharing Quadrants' slot 0). Eviction
// disconnects the previous holder on BOTH sides, input included: a dethroned
// effect that kept its input connected would go on burning CPU and holding
// F32 blocks to produce audio going nowhere.
//
// BYPASS is a disconnect of the output cables only. The applet keeps running,
// so a delay keeps its tail and a reverb keeps its tank, and coming back out
// of bypass is instant and lossless -- which is the entire point of giving a
// full-screen effect an A/B button (Audio-Apps-Screens.md section 3.2).
// ---------------------------------------------------------------------------

#ifdef ARDUINO_TEENSY41

#include "AudioIO.h"
#include "HemisphereAudioApplet.h"

class AudioAppletHost {
public:
  // `channels` is 1 for a MONO applet, 2 for STEREO. A mono applet takes the
  // left input and is summed into BOTH output channels -- a mono effect that
  // came out of one side of the panel would read as a broken cable, not as a
  // mono effect.
  AudioAppletHost(HemisphereAudioApplet &applet, uint8_t channels)
    : applet_(applet), channels_(channels) {}

  bool Started() const { return started_; }
  bool HoldsSlot() const { return slot_owner_ == this; }
  // "Live" = this host owns the shared slot AND is not bypassed.
  bool Live() const { return HoldsSlot() && output_on_; }

  // From APP_EVENT_RESUME. Idempotent: safe on every resume, does the
  // expensive half only once.
  void Enter(bool output_on);

  // Bypass toggle. No-op unless this host currently owns the slot.
  void SetOutput(bool on);

  // Per tick, from the app's Controller() (i.e. inside BaseController(), so
  // HS::frame is already loaded). NOT FLASHMEM: this runs at core tick rate.
  void Tick() {
    if (started_) applet_.Controller();
    HemisphereApplet::ProcessCursors();
  }

private:
  void StartOnce();
  void BuildCables();
  void ConnectAll();
  void DisconnectAll();

  HemisphereAudioApplet &applet_;
  const uint8_t channels_;
  bool started_ = false;
  bool cables_built_ = false;
  bool output_on_ = true;
  AudioConnection *in_[2] = { nullptr, nullptr };
  AudioConnection *out_[2] = { nullptr, nullptr };

  // Whoever currently holds AudioIO::kOutputRouteEffectSlot, or null. One
  // slot, so one pointer.
  inline static AudioAppletHost *slot_owner_ = nullptr;
};

// Everything below runs on an app event or a button press -- cold enough for
// flash. Only Tick() above is on the per-tick path, and it stays in RAM.

// Deliberately lazy: AppBase::Init() runs for EVERY app in the container at
// boot regardless of which one is current, so building the graph there would
// make an app nobody opens cost PSRAM, F32 cables and CPU. It also cannot
// happen at boot even if we wanted it to -- OC::AudioIO::OutputStream()
// creates the codec output on first call and documents that calling it before
// the rest of the graph exists costs 3ms of latency (AudioIO.cpp:116-124).
FLASHMEM void AudioAppletHost::StartOnce() {
  if (started_) return;
  // AudioNoInterrupts around Start(): the graph is live and Start() both
  // allocates (the PSRAM delay buffer, the F32 cable pool) and connects. Same
  // reasoning as TweightyApp.h:394-402.
  AudioNoInterrupts();
  applet_.BaseStart(HS::AUDIO_SLOT_L);
  AudioInterrupts();
  started_ = true;
}

FLASHMEM void AudioAppletHost::BuildCables() {
  if (cables_built_) return;
  // Built disconnected. `new` rather than members so a host that is never
  // entered costs four pointers, and so the cables outlive any single
  // claim/evict cycle -- AudioConnection's destructor unlinks it from the
  // graph, which is not something to do from a button press.
  //
  // Bracketed because AudioConnection's 4-arg constructor CONNECTS: without
  // this there is a window, however short, where the ISR can route a block
  // through a cable that exists only to be immediately disconnected.
  AudioNoInterrupts();
  for (uint8_t ch = 0; ch < channels_; ch++) {
    in_[ch] = new AudioConnection(
      OC::AudioIO::InputStream(0), ch, *applet_.InputStream(), ch
    );
    if (in_[ch]) in_[ch]->disconnect();
  }
  for (uint8_t ch = 0; ch < OC::AudioIO::kOutputRouteChannels; ch++) {
    // Source-major layout: source `s`, channel `c` is at index
    // s * kOutputRouteChannels + c (Audio/AudioMixer.h:67-72).
    const uint8_t dest
      = OC::AudioIO::kOutputRouteEffectSlot * OC::AudioIO::kOutputRouteChannels
      + ch;
    // A mono applet has one output; it feeds both route channels.
    const uint8_t src_ch = (channels_ > 1) ? ch : 0;
    out_[ch] = new AudioConnection(
      *applet_.OutputStream(), src_ch, OC::AudioIO::OutputStream(), dest
    );
    if (out_[ch]) out_[ch]->disconnect();
  }
  AudioInterrupts();
  cables_built_ = true;
}

FLASHMEM void AudioAppletHost::ConnectAll() {
  for (auto *c : in_) if (c) c->connect();
  for (auto *c : out_) if (c) { if (output_on_) c->connect(); else c->disconnect(); }
}

FLASHMEM void AudioAppletHost::DisconnectAll() {
  for (auto *c : in_) if (c) c->disconnect();
  for (auto *c : out_) if (c) c->disconnect();
}

FLASHMEM void AudioAppletHost::Enter(bool output_on) {
  output_on_ = output_on;
  StartOnce();
  BuildCables();
  if (slot_owner_ == this) {
    // Already ours; just make the output match the app's bypass state, in
    // case it changed while we were away (it cannot today, but a host that
    // only works when nothing else touched it is a host with a trap in it).
    AudioNoInterrupts();
    for (auto *c : out_) if (c) { if (output_on_) c->connect(); else c->disconnect(); }
    AudioInterrupts();
    return;
  }
  AudioNoInterrupts();
  if (slot_owner_) slot_owner_->DisconnectAll();
  ConnectAll();
  slot_owner_ = this;
  AudioInterrupts();
}

FLASHMEM void AudioAppletHost::SetOutput(bool on) {
  output_on_ = on;
  if (slot_owner_ != this) return;
  AudioNoInterrupts();
  for (auto *c : out_) if (c) { if (on) c->connect(); else c->disconnect(); }
  AudioInterrupts();
}

#endif  // ARDUINO_TEENSY41
