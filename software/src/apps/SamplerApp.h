#pragma once
// ---------------------------------------------------------------------------
// Sampler -- an 8-voice, 8-CV-input sample player, a hardware-appropriate
// reinterpretation of the 1010music Bitbox Micro idiom for this module's
// 128x64 1-bit OLED + two encoders + A/B/X/Y panel (no touchscreen, no
// directory browser -- see SamplerMath.h's header comment for why file
// selection is a plain 3-digit number, the same convention
// audio_applets/WAVPlayerApplet.h already established).
//
// ENGINE: wires teensy-variable-playback's AudioPlaySdResmp directly (the
// same library WAVPlayerApplet.h already uses -- see that file's header
// comment for the API this borrows), the way apps/TweightyApp.h and
// apps/ScopeApp.h wire directly into OC::AudioIO rather than going through
// Quadrants' applet system. This is a standalone app, not a
// Hemisphere/Quadrants audio applet.
//
// 8 SLOTS, one per CV/gate input (ADC_CHANNEL_1..8) -- the same 8-channel
// idiom Tweighty's 8 taps and Scope's 8 CV channels already use on this
// hardware. Each slot independently owns a 3-digit SD file number, a base
// playback rate (%), loop on/off, and a trigger mode. See SamplerMath.h for
// the pure clamping/edge-detection/filename logic this class calls into, and
// for why one CV jack serves as both the gate/trigger AND the pitch-mod
// source for its own slot (this hardware has 8 CV ins but only 4 dedicated
// TR jacks, already spoken for elsewhere).
//
// TRIGGER MODES (SamplerMath::TriggerMode):
//   ONE_SHOT: a rising edge (crossing SamplerMath::kGateThresholdRaw) starts
//     playback (retriggering if already playing); the gate is not consulted
//     again until it falls and rises once more. Plays to the end, or loops
//     forever if loop_on is set -- there is no "stop" from the gate in this
//     mode, matching a classic drum-machine one-shot voice.
//   GATE_SUSTAIN: a rising edge starts playback the same way; a falling edge
//     stops it immediately (AudioPlaySdResmp::stop()), regardless of
//     loop_on or how far through the sample playback had gotten -- the
//     "held note" behavior a keyboard/gate player expects.
//   Both modes run through the SAME edge detector (SamplerMath::UpdateGate())
//   every Loop() pass; only what happens on FALLING differs.
//
// AUDIO GRAPH: each slot's AudioPlaySdResmp (stereo out, channels 0/1 --
// mono files are mirrored onto both by the library itself) feeds a local
// AudioSummingRoute<2, kSlotCount> (Audio/AudioMixer.h -- the same class
// AudioIO.cpp's own output_route uses), source-major: slot i's L/R land at
// input index i*2+0/i*2+1. That stereo sum then feeds OC::AudioIO's own
// output_route at kOutputRouteSamplerSlot (AudioIO.h) -- a slot claimed for
// this app alongside Quadrants' slot 0 and Tweighty's slot 1, the same
// "shared destination needs a dedicated slot" pattern TweightyApp.h's
// WireAudio() comment documents. Wired unconditionally at Init() (not
// lazily like Tweighty) because nothing here costs a large buffer merely by
// existing -- AudioPlaySdResmp only actually allocates its SD read-ahead
// buffers when a file is playing (ResamplingReader/IndexableFile), so an
// idle, never-triggered Sampler costs no more than 8 idle AudioStream
// objects.
//
// CPU / SD-BANDWIDTH RISK -- READ THIS BEFORE ASSUMING 8 VOICES WORKS ON
// HARDWARE: each AudioPlaySdResmp's own SD reader (ResamplingSdReader.h)
// wants up to BUFFER_COUNT_SD(7) * BUFFER_SIZE_SD(2048 samples) = 14336
// int16 samples, ~28KB, of read-ahead buffer PER ACTIVE VOICE (preferring
// RAM2/DMAMEM, falling back to PSRAM only if that malloc() fails --
// IndexableFile.h's indexedbuffer). All 8 slots playing at once could
// therefore want on the order of ~224KB of buffer memory concurrently, on
// top of AudioIO.h's own AUDIO_MEMORY(252)/F32_AUDIO_MEMORY(80) block pools
// and whatever else this build's other always-on consumers (Quadrants'
// chain, Tweighty if ever opened) are already holding -- this codebase has
// ALREADY hit a hard DTCM ceiling once from a smaller combination of
// features (see platformio.ini's T41_console comment on the T41_audio_dbg
// DACCVIOL boot fault). Beyond memory, 8 concurrent voices means the SD
// driver is servicing 8 independently-offset, small (~4KB) interleaved
// reads every audio block instead of one long sequential stream -- SD cards
// are tuned for sequential throughput, and WAVPlayerApplet.h (this
// library's only other consumer in this codebase) has only ever been
// exercised as a SINGLE simultaneous voice. Nothing in this codebase
// establishes that 8-way interleaved SD access meets its deadline before
// audible dropout. This file implements the full 8-slot design as
// specified -- the 8-slot audio graph is genuinely wired end to end -- but
// whether 8 concurrent voices is actually safe on real hardware is an open
// bench question a build cannot answer; the SD card has to be on the bench.
// If it turns out not to be, 2-4 concurrent voices (still 8 *assignable*
// slots, just not all playing back at once) is the likely safe fallback;
// nothing here artificially enforces that cap, by design.
//
// CONTROL-RATE ONLY: this app defines no AudioStream::update() of its own --
// AudioPlaySdResmp and AudioSummingRoute are both already complete,
// existing AudioStream implementations. Everything below (CV/gate polling,
// trigger edges, rate CV, file (re)load, UI) runs from Loop() (non-ISR, same
// context as Bus200eApp.h's bus polling) or UI event handlers -- there is no
// Controller()/audio-ISR-hot surface in this app at all, so unlike
// TweightyApp.h nothing here needs to stay off FLASHMEM; every out-of-class
// function below is FLASHMEM, matching ScopeApp.h (which has the same
// Controller()-less shape).
//
// CONTROLS: encL selects the active slot (0..7, wraps). B cycles which
// field is focused for that slot (FILE -> RATE -> LOOP -> MODE -> wraps).
// encR adjusts the focused field. A manually previews/triggers the selected
// slot (a one-shot StartSlot(), regardless of that slot's own trigger mode
// -- useful for auditioning a sample without patching a gate cable).
// ---------------------------------------------------------------------------

#include "../HSUtils.h"
#include "../OC_ADC.h"
#include "../SamplerMath.h"
#ifdef AUDIO_INTERFACE
#include <TeensyVariablePlayback.h>
#include "../Audio/AudioMixer.h"
#include "../AudioIO.h"
#endif

namespace SamplerAppNS {

// Same ADC_CHANNEL_n indirection as ScopeApp.h's AdcChannelForSub -- these
// globals are not guaranteed to be plain 0..7 (OC_ADC.cpp can remap them),
// so always read the live global for the requested slot rather than caching.
inline ADC_CHANNEL AdcChannelForSlot(int slot) {
  switch (slot) {
    default:
    case 0: return ADC_CHANNEL_1;
    case 1: return ADC_CHANNEL_2;
    case 2: return ADC_CHANNEL_3;
    case 3: return ADC_CHANNEL_4;
#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
    case 4: return ADC_CHANNEL_5;
    case 5: return ADC_CHANNEL_6;
    case 6: return ADC_CHANNEL_7;
    case 7: return ADC_CHANNEL_8;
#endif
  }
}

enum FocusParam : uint8_t {
  FOCUS_FILE = 0,
  FOCUS_RATE,
  FOCUS_LOOP,
  FOCUS_TRIGMODE,
  FOCUS_COUNT,
};

}  // namespace SamplerAppNS

OC_APP_CLASS(AppSampler, TWOCCS("SM"), "Sampler", "Sample Player") {
public:
  // Per slot: file_num_ u16 + rate_pct_ i16 + flags_ u8 (bit0 loop_on, bit1
  // trigger mode) = 5 bytes * 8 slots = 40, + slot_ u8 = 41 bytes total.
  OC_APP_INTERFACE_DECLARE(AppSampler, 41);

private:
  // --- persisted (41 bytes) -----------------------------------------------
  uint16_t file_num_[SamplerMath::kSlotCount];
  int16_t rate_pct_[SamplerMath::kSlotCount];
  uint8_t flags_[SamplerMath::kSlotCount];
  uint8_t slot_ = 0;  // 0..7, last-selected slot (encL)

  // --- live UI state (ephemeral, not persisted) ---------------------------
  uint8_t focus_ = SamplerAppNS::FOCUS_FILE;

  // --- per-slot live state (ephemeral) -------------------------------------
  bool prev_gate_high_[SamplerMath::kSlotCount] = {};
  bool need_reload_[SamplerMath::kSlotCount] = {};
  bool file_loaded_[SamplerMath::kSlotCount] = {};

#ifdef AUDIO_INTERFACE
  AudioPlaySdResmp players_[SamplerMath::kSlotCount];
  AudioSummingRoute<OC::AudioIO::kOutputRouteChannels, (uint8_t)SamplerMath::kSlotCount> slot_mix_;
  AudioConnection *conn_player_l_[SamplerMath::kSlotCount] = {};
  AudioConnection *conn_player_r_[SamplerMath::kSlotCount] = {};
  AudioConnection *conn_out_l_ = nullptr;
  AudioConnection *conn_out_r_ = nullptr;
#endif
  bool audio_wired_ = false;

  bool LoopOn(int i) const { return (flags_[i] & 0x1) != 0; }
  void SetLoopOn(int i, bool v) {
    if (v) flags_[i] = (uint8_t)(flags_[i] | 0x1);
    else flags_[i] = (uint8_t)(flags_[i] & ~0x1);
  }
  uint8_t TrigMode(int i) const { return (uint8_t)((flags_[i] >> 1) & 0x1); }
  void SetTrigMode(int i, uint8_t m) {
    flags_[i] = (uint8_t)((flags_[i] & ~0x2) | ((m & 0x1) << 1));
  }

  void WireAudio();
  void LoadSlotFile(int i);
  void StartSlot(int i);
  void StopSlot(int i);
  void PollSlots();
  void AdjustFocused(int delta);
};

// ---------------------------------------------------------------------------
// Out-of-class and FLASHMEM throughout -- see the class comment's
// CONTROL-RATE ONLY paragraph for why: there is no Controller() in this
// app, so (unlike TweightyApp.h) nothing here is audio-ISR-hot or runs
// unconditionally every tick regardless of the current app. Loop() runs
// from the main non-ISR loop only while Sampler is the current app, exactly
// like Bus200eApp::Loop().
// ---------------------------------------------------------------------------

FLASHMEM void AppSampler::WireAudio() {
  if (audio_wired_) return;
#ifdef AUDIO_INTERFACE
  for (int i = 0; i < SamplerMath::kSlotCount; ++i) {
    players_[i].enableInterpolation(true);
    players_[i].setBufferInPSRAM(false);  // prefer RAM2; see class comment's risk note
    conn_player_l_[i] = new AudioConnection(
        players_[i], 0, slot_mix_, i * OC::AudioIO::kOutputRouteChannels + 0);
    conn_player_r_[i] = new AudioConnection(
        players_[i], 1, slot_mix_, i * OC::AudioIO::kOutputRouteChannels + 1);
  }
  conn_out_l_ = new AudioConnection(
      slot_mix_, 0, OC::AudioIO::OutputStream(),
      OC::AudioIO::kOutputRouteSamplerSlot * OC::AudioIO::kOutputRouteChannels + 0);
  conn_out_r_ = new AudioConnection(
      slot_mix_, 1, OC::AudioIO::OutputStream(),
      OC::AudioIO::kOutputRouteSamplerSlot * OC::AudioIO::kOutputRouteChannels + 1);
#endif
  audio_wired_ = true;
}

FLASHMEM void AppSampler::Init() {
  using namespace SamplerMath;
  for (int i = 0; i < kSlotCount; ++i) {
    file_num_[i] = 0;
    rate_pct_[i] = (int16_t)kDefaultRatePercent;
    flags_[i] = 0;  // loop off, TRIG_ONE_SHOT
    prev_gate_high_[i] = false;
    need_reload_[i] = true;
    file_loaded_[i] = false;
  }
  slot_ = 0;
  focus_ = SamplerAppNS::FOCUS_FILE;
  audio_wired_ = false;
  WireAudio();
}

FLASHMEM size_t AppSampler::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  for (int i = 0; i < SamplerMath::kSlotCount; ++i)
    stream_buffer.Write<uint16_t>(file_num_[i]);
  for (int i = 0; i < SamplerMath::kSlotCount; ++i)
    stream_buffer.Write<int16_t>(rate_pct_[i]);
  for (int i = 0; i < SamplerMath::kSlotCount; ++i)
    stream_buffer.Write<uint8_t>(flags_[i]);
  stream_buffer.Write<uint8_t>(slot_);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppSampler::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  using namespace SamplerMath;
  uint16_t fn[kSlotCount];
  int16_t rp[kSlotCount];
  uint8_t fl[kSlotCount];
  for (int i = 0; i < kSlotCount; ++i) fn[i] = stream_buffer.Read<uint16_t>();
  for (int i = 0; i < kSlotCount; ++i) rp[i] = stream_buffer.Read<int16_t>();
  for (int i = 0; i < kSlotCount; ++i) fl[i] = stream_buffer.Read<uint8_t>();
  const uint8_t sel = stream_buffer.Read<uint8_t>();

  for (int i = 0; i < kSlotCount; ++i) {
    file_num_[i] = (uint16_t)ClampFileNum((int)fn[i]);
    rate_pct_[i] = (int16_t)ClampRatePercent((int)rp[i]);
    // bit0 loop_on passes through as-is; bit1 trigger mode is re-validated.
    flags_[i] = (uint8_t)((fl[i] & 0x1) | (ClampTriggerMode((fl[i] >> 1) & 0x1) << 1));
    need_reload_[i] = true;
    file_loaded_[i] = false;
    prev_gate_high_[i] = false;
  }
  slot_ = (uint8_t)WrapSlotIndex((int)sel);
  focus_ = SamplerAppNS::FOCUS_FILE;

  return stream_buffer.underflow() ? 0 : stream_buffer.read();
}

FLASHMEM void AppSampler::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
      // Audio graph is wired unconditionally at Init() (see class comment),
      // so RESUME has nothing to (re)connect -- but a stale file selection
      // made while backgrounded (bus preset recall rewriting file_num_ via
      // RestoreAppData) is picked up here rather than waiting for the user
      // to touch encR on that field.
      for (int i = 0; i < SamplerMath::kSlotCount; ++i)
        if (need_reload_[i]) LoadSlotFile(i);
      break;
    default:
      break;
  }
}

FLASHMEM void AppSampler::LoadSlotFile(int i) {
#ifdef AUDIO_INTERFACE
  if (!SDcard_Ready) {
    file_loaded_[i] = false;
    need_reload_[i] = false;
    return;
  }
  char filename[SamplerMath::kFilenameBufLen];
  SamplerMath::BuildFilename(file_num_[i], filename, sizeof(filename));
  // playWav() stops any current playback on this voice first (see
  // playresmp.h), so switching a slot's file out from under an in-progress
  // one-shot cuts it -- documented behavior, matching WAVPlayerApplet's own
  // ChangeToFile().
  file_loaded_[i] = players_[i].playWav(filename);
  players_[i].setLoopType(LoopOn(i) ? looptype_repeat : looptype_none);
#endif
  need_reload_[i] = false;
}

FLASHMEM void AppSampler::StartSlot(int i) {
#ifdef AUDIO_INTERFACE
  if (need_reload_[i]) LoadSlotFile(i);
  if (!file_loaded_[i]) return;
  players_[i].setLoopType(LoopOn(i) ? looptype_repeat : looptype_none);
  if (players_[i].isPlaying()) {
    if (players_[i].available()) players_[i].retrigger();
  } else {
    players_[i].play();
  }
#endif
}

FLASHMEM void AppSampler::StopSlot(int i) {
#ifdef AUDIO_INTERFACE
  players_[i].stop();
#endif
}

// Runs every Loop() pass (main non-ISR loop, only while Sampler is current):
// polls all 8 CV/gate inputs, edge-detects each against its own previous
// sample, drives that slot's trigger mode, and pushes each slot's live
// rate (base % combined with its own CV, SamplerMath::ComputeRateMultiplier)
// into the engine. See the class comment for why one CV input is both the
// gate source and the pitch-mod source for its own slot.
FLASHMEM void AppSampler::PollSlots() {
  using namespace SamplerMath;
  for (int i = 0; i < kSlotCount; ++i) {
    const int32_t raw = OC::ADC::value(SamplerAppNS::AdcChannelForSlot(i));
    const GateEdge edge = UpdateGate(raw, prev_gate_high_[i]);

    if (edge == EDGE_RISING) {
      StartSlot(i);
    } else if (edge == EDGE_FALLING && TrigMode(i) == TRIG_GATE_SUSTAIN) {
      StopSlot(i);
    }

#ifdef AUDIO_INTERFACE
    if (file_loaded_[i]) {
      const float rate = ComputeRateMultiplier((int)rate_pct_[i], raw);
      players_[i].setPlaybackRate(rate);
    }
    if (need_reload_[i]) LoadSlotFile(i);
#endif
  }
}

void AppSampler::Process(OC::IOFrame *) {}

FLASHMEM void AppSampler::Loop() {
  PollSlots();
}

FLASHMEM void AppSampler::AdjustFocused(int delta) {
  using namespace SamplerMath;
  const int i = slot_;
  switch (focus_) {
    case SamplerAppNS::FOCUS_FILE:
      file_num_[i] = (uint16_t)ClampFileNum((int)file_num_[i] + delta);
      need_reload_[i] = true;
      break;
    case SamplerAppNS::FOCUS_RATE:
      rate_pct_[i] = (int16_t)ClampRatePercent((int)rate_pct_[i] + delta);
      break;
    case SamplerAppNS::FOCUS_LOOP:
      if (delta != 0) {
        SetLoopOn(i, !LoopOn(i));
#ifdef AUDIO_INTERFACE
        players_[i].setLoopType(LoopOn(i) ? looptype_repeat : looptype_none);
#endif
      }
      break;
    case SamplerAppNS::FOCUS_TRIGMODE:
      if (delta != 0) SetTrigMode(i, (uint8_t)(1 - TrigMode(i)));
      break;
    default:
      break;
  }
}

FLASHMEM void AppSampler::HandleButtonEvent(const UI::Event &event) {
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  switch (event.control) {
    case OC::CONTROL_BUTTON_UP:      // A: manual preview/trigger of selected slot
      StartSlot(slot_);
      break;
    case OC::CONTROL_BUTTON_DOWN:    // B: cycle focused field
      focus_ = (uint8_t)((focus_ + 1) % SamplerAppNS::FOCUS_COUNT);
      break;
    default:
      break;  // X/Y/encoder-pushes unbound in v1
  }
}

FLASHMEM void AppSampler::HandleEncoderEvent(const UI::Event &event) {
  if (event.control == OC::CONTROL_ENCODER_L) {
    slot_ = (uint8_t)SamplerMath::WrapSlotIndex((int)slot_ + event.value);
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    AdjustFocused(event.value);
  }
}

// One detail row per field for the SELECTED slot (file/rate/loop/mode, the
// focused one inverted -- same field-list-with-cursor grammar as
// TweightyApp::DrawEdit()), plus an 8-box slot ring along the bottom
// (same shape language as TweightyApp::DrawHome()'s tap ring): filled =
// has a file loaded, framed = no file, and the selected slot is additionally
// outlined. A slot currently playing gets its box inverted instead, so
// "what's live right now" is a glance, not a readout -- the same reason
// Scope's/Tweighty's own per-channel/per-tap views lean on shape over text.
FLASHMEM void AppSampler::DrawMenu() const {
  using namespace SamplerMath;
  gfxHeader("S A M P L E R");

  const int i = slot_;
  char buf[16];

  graphics.setPrintPos(1, 12);
  graphics.print("Slot ");
  graphics.print(i + 1);
  graphics.print("/8");
#ifdef AUDIO_INTERFACE
  if (file_loaded_[i] && players_[i].isPlaying()) {
    graphics.setPrintPos(100, 12);
    graphics.print("PLAY");
  }
#endif

  static const char *const kLabels[SamplerAppNS::FOCUS_COUNT] = {
    "File", "Rate", "Loop", "Mode"
  };
  for (int f = 0; f < SamplerAppNS::FOCUS_COUNT; ++f) {
    const int y = 22 + f * 9;
    graphics.setPrintPos(4, y);
    graphics.print(kLabels[f]);
    graphics.setPrintPos(127, y);
    switch (f) {
      case SamplerAppNS::FOCUS_FILE:
        snprintf(buf, sizeof(buf), "%03u", file_num_[i]);
        graphics.print_right(buf);
        break;
      case SamplerAppNS::FOCUS_RATE:
        snprintf(buf, sizeof(buf), "%d%%", rate_pct_[i]);
        graphics.print_right(buf);
        break;
      case SamplerAppNS::FOCUS_LOOP:
        graphics.print_right(LoopOn(i) ? "ON" : "OFF");
        break;
      case SamplerAppNS::FOCUS_TRIGMODE:
        graphics.print_right(TrigMode(i) == TRIG_GATE_SUSTAIN ? "GATE" : "1SHOT");
        break;
      default:
        break;
    }
    if (f == focus_) graphics.invertRect(0, 21 + f * 9, 128, 9);
  }

  // 8-box slot ring, 16px each across the full 128px width, mirroring
  // TweightyApp::DrawMixer()'s per-tap column geometry.
  static constexpr int kRingY = 58;
  for (int s = 0; s < SamplerMath::kSlotCount; ++s) {
    const int x = s * 16 + 4;
#ifdef AUDIO_INTERFACE
    const bool playing = file_loaded_[s] && players_[s].isPlaying();
    if (playing) {
      // Solid box with a hollow core. NOT drawRect-then-invertRect over the
      // same rect, which is what this used to be: invertRect is XOR
      // (src/drivers/weegfx.cpp:204-209, draw_rect<PIXEL_OP_XOR>), so filling
      // every pixel and then inverting every pixel turned them all back off.
      // A PLAYING slot rendered as a blank hole -- less visible than an idle
      // loaded one, and indistinguishable from the gap between boxes. The
      // state the header comment calls "what's live right now ... a glance"
      // was the one state you could not see.
      //
      // Inverting a filled box cannot work on a black panel; there is no
      // surround for it to invert against. So live reads as the loaded box
      // with its centre punched out, which is distinct from both a solid box
      // (loaded, idle) and an outline (no file) at a glance.
      graphics.drawRect(x, kRingY, 8, 6);
      graphics.invertRect(x + 2, kRingY + 2, 4, 2);
    } else if (file_loaded_[s]) {
      graphics.drawRect(x, kRingY, 8, 6);
    } else {
      graphics.drawFrame(x, kRingY, 8, 6);
    }
#else
    graphics.drawFrame(x, kRingY, 8, 6);
#endif
    if (s == slot_) graphics.drawFrame(x - 2, kRingY - 2, 12, 10);
  }

  // 21 columns is the whole row (gfxFooter prints at x=1, and 1 + 21*6 = 127).
  // This used to read "A:play B:field L/R:sel/adj" -- 26 characters, so five
  // of them were never drawn and the panel showed "...L/R:se", which reads as
  // a typo rather than as a truncation.
  //
  // edgecheck.py does NOT catch this. It looks for a PARTIALLY drawn glyph in
  // columns 126-127; characters clipped away entirely leave no partial glyph
  // behind, so an over-long string passes it silently. Count footer strings.
  //
  // "R:adj" is the right thing to drop rather than abbreviating everything:
  // the inverted row already means "the right encoder changes this" (L-06), so
  // labelling encR's TURN restates the grammar. Delay and Reverb label encR's
  // PRESS only, for the same reason.
  gfxFooter("A:play B:field L:slot");
}

FLASHMEM void AppSampler::DrawScreensaver() const {
  DrawMenu();
}

FLASHMEM void AppSampler::GetIOConfig(OC::IOConfig &ioconfig) const {
  using namespace OC;
  char label[24];
  for (int i = 0; i < SamplerMath::kSlotCount && i < ADC_CHANNEL_COUNT; ++i) {
    snprintf(label, sizeof(label), "Slot%d Trig/Rate", i + 1);
    ioconfig.cv[i].set(label);
  }
  // No CV/gate/audio outputs driven directly by this app -- playback reaches
  // the codec via OC::AudioIO's output_route (see the class comment's AUDIO
  // GRAPH paragraph), not the panel DAC outputs.
}

FLASHMEM void AppSampler::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("slot ");
  graphics.print((int)slot_);
#ifdef AUDIO_INTERFACE
  graphics.setPrintPos(2, 22);
  graphics.print("loaded ");
  int n = 0;
  for (int i = 0; i < SamplerMath::kSlotCount; ++i) if (file_loaded_[i]) ++n;
  graphics.print(n);
#endif
}
