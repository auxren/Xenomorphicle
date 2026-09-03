#pragma once
// ---------------------------------------------------------------------------
// Scope -- a simple oscilloscope for the O.R.N.8 hardware's 20 monitorable
// signals: CV IN 1-8, CV OUT 1-8, and (on an audio build) AUDIO IN L/R,
// AUDIO OUT L/R. One channel at a time, a scrolling 128px-wide waveform
// trace, a discrete vertical gain control, freeze, and a numeric readout of
// the latest sample.
//
// CHANNEL MODEL: a flat 0..19 index (see ScopeMath.h for the exact layout
// and why it is a fixed table rather than something computed from
// AUDIO_INTERFACE). encL cycles it; the cursor is kept off the 4 audio
// entries entirely on a build without AUDIO_INTERFACE (they would have
// nothing behind them).
//
// SAMPLING: CV channels are sampled once per redraw, in Render() below --
// non-ISR, no timer needed, since a screen redraw is already the natural
// "how often does a human need a new sample" rate. Audio channels are
// sampled at audio-block rate by AudioScopeCapture (Audio/AudioScopeCapture.h),
// wired permanently at Init() the same way TweightyApp::WireAudio() taps
// OC::AudioIO's streams -- being an extra downstream reader of those
// existing streams needs no special handling (ordinary Teensy Audio Library
// fan-out), unlike output_route's own summing-destination story.
//
// MEMORY: only the CURRENTLY SELECTED channel keeps a live ring buffer --
// switching channels (encL) resets whichever ring is now selected, rather
// than reasoning about 20 buffers' worth of state. CV channels share one
// int16_t ring[ScopeMath::kRingSize] owned here; audio channels read
// straight out of AudioScopeCapture's own 4 ring buffers (it only bothers
// writing the one currently selected -- see that class's header comment).
//
// CONTROLS: A = freeze/pause (stop appending new samples; press again to
// resume). B = reset gain to 1.0x. encL = channel select (wraps). encR =
// gain, 0.25x..4x in discrete steps. encL PUSH (CONTROL_BUTTON_L) toggles
// whether DrawScreensaver() shows a live, still-updating trace instead of
// the framework's default blank screensaver -- the one button/encoder-push
// this app doesn't otherwise use, and a single press (not a hold) since
// nothing else is competing for it. Z/CONTROL_BUTTON_M is not wired on this
// hardware (see Bus200eApp.h's header comment) so it was never an option.
// ---------------------------------------------------------------------------

#include "../HSUtils.h"
#include "../OC_ADC.h"
#include "../OC_DAC.h"
#include "../ScopeMath.h"
#ifdef AUDIO_INTERFACE
#include "../Audio/AudioScopeCapture.h"
#include "../AudioIO.h"
#endif

namespace ScopeAppNS {

#ifdef AUDIO_INTERFACE
static constexpr int kUsableChannels = ScopeMath::kChannelCount;       // 20
#else
static constexpr int kUsableChannels = ScopeMath::kCvChannelCount;     // 16
#endif

// OC::ADC::value()'s ADC_CHANNEL_n globals are not guaranteed to be plain
// 0..7 -- OC_ADC.cpp can remap them (flip180) -- so this always reads the
// live global for the requested sub-index rather than caching one.
inline ADC_CHANNEL AdcChannelForSub(int sub) {
  switch (sub) {
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

}  // namespace ScopeAppNS

OC_APP_CLASS(AppScope, TWOCCS("SP"), "Scope", "Scope") {
public:
  // channel_ u8 + gain_index_ u8 + screensaver_trace_ u8 = 3 bytes.
  OC_APP_INTERFACE_DECLARE(AppScope, 3);

private:
  // --- persisted (3 bytes) -------------------------------------------------
  uint8_t channel_ = 0;                    // 0..19, see ScopeMath.h
  uint8_t gain_index_ = ScopeMath::kDefaultGainIndex;
  bool screensaver_trace_ = false;         // encL-push toggle

  // --- live UI state ---------------------------------------------------
  bool frozen_ = false;

  // --- CV ring (audio channels use AudioScopeCapture's own instead) -------
  // mutable: appended to from Render(), which DrawMenu()/DrawScreensaver()
  // (both const, per OC_APP_INTERFACE_DECLARE) call -- sampling on redraw
  // is this app's design (see the class comment), not a side channel that
  // would be surprising to find behind a const method.
  mutable int16_t cv_ring_[ScopeMath::kRingSize] = {};
  mutable uint8_t cv_head_ = 0;
  mutable int32_t last_raw_ = 0;

#ifdef AUDIO_INTERFACE
  AudioScopeCapture scope_capture_;
  AudioConnection *conn_in_l_ = nullptr;
  AudioConnection *conn_in_r_ = nullptr;
  AudioConnection *conn_out_l_ = nullptr;
  AudioConnection *conn_out_r_ = nullptr;
#endif
  bool audio_wired_ = false;

  void WireAudio();
  void SetChannel(int new_channel);
  void SetFrozen(bool frozen);
  void Render() const;
};

// ---------------------------------------------------------------------------
// Out-of-class and FLASHMEM: this app has no audio-ISR-hot or unconditional
// per-tick surface of its own (unlike TweightyApp's Controller()/
// BackgroundPump()) -- CV sampling happens at redraw rate inside Render(),
// which is plenty cold for flash. AudioScopeCapture::update() is the one
// piece of real ISR-hot code this feature adds, and it lives in its own
// header (Audio/AudioScopeCapture.h), never FLASHMEM, same discipline as
// AudioAnalyzeStrobe::update() and AudioTweightyF32::update().
// ---------------------------------------------------------------------------

FLASHMEM void AppScope::WireAudio() {
  if (audio_wired_) return;
#ifdef AUDIO_INTERFACE
  // Wired permanently at Init(), per the class comment -- unlike
  // TunerApp/TweightyApp's own taps, there is no connect()/disconnect()
  // dance here: AudioScopeCapture's active_channel_ (default -1, "nothing
  // selected") is the actual cost gate, so an unselected or backgrounded
  // Scope costs a bare release() per block on all 4 taps, not a torn-down
  // graph to rebuild on every RESUME.
  conn_in_l_ = new AudioConnection(OC::AudioIO::InputStream(0), 0,
                                    scope_capture_, AudioScopeCapture::TAP_IN_L);
  conn_in_r_ = new AudioConnection(OC::AudioIO::InputStream(0), 1,
                                    scope_capture_, AudioScopeCapture::TAP_IN_R);
  conn_out_l_ = new AudioConnection(OC::AudioIO::OutputStream(), 0,
                                     scope_capture_, AudioScopeCapture::TAP_OUT_L);
  conn_out_r_ = new AudioConnection(OC::AudioIO::OutputStream(), 1,
                                     scope_capture_, AudioScopeCapture::TAP_OUT_R);
#endif
  audio_wired_ = true;
}

// Resets whichever ring the new channel needs (its own CV ring, or the
// matching AudioScopeCapture tap) and arms/disarms the audio tap's active
// channel to match -- see AudioScopeCapture's header comment for why only
// the selected tap ever gets scanned.
FLASHMEM void AppScope::SetChannel(int new_channel) {
  channel_ = (uint8_t)ScopeMath::WrapIndex(new_channel, ScopeAppNS::kUsableChannels);
  cv_head_ = 0;
  last_raw_ = 0;
  for (auto &v : cv_ring_) v = 0;
#ifdef AUDIO_INTERFACE
  if (ScopeMath::ChannelKindOf(channel_) == ScopeMath::KIND_AUDIO) {
    const int sub = ScopeMath::ChannelSubIndex(channel_);
    scope_capture_.ResetRing(sub);
    scope_capture_.SetActiveChannel(frozen_ ? -1 : sub);
  } else {
    scope_capture_.SetActiveChannel(-1);
  }
#endif
}

FLASHMEM void AppScope::SetFrozen(bool frozen) {
  frozen_ = frozen;
#ifdef AUDIO_INTERFACE
  if (ScopeMath::ChannelKindOf(channel_) == ScopeMath::KIND_AUDIO) {
    const int sub = ScopeMath::ChannelSubIndex(channel_);
    scope_capture_.SetActiveChannel(frozen_ ? -1 : sub);
  }
#endif
}

FLASHMEM void AppScope::Init() {
  channel_ = 0;
  gain_index_ = ScopeMath::kDefaultGainIndex;
  screensaver_trace_ = false;
  frozen_ = false;
  audio_wired_ = false;
  WireAudio();
  SetChannel(0);
}

FLASHMEM size_t AppScope::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  stream_buffer.Write<uint8_t>(channel_);
  stream_buffer.Write<uint8_t>(gain_index_);
  stream_buffer.Write<uint8_t>(screensaver_trace_ ? 1 : 0);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppScope::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  const uint8_t ch = stream_buffer.Read<uint8_t>();
  const uint8_t gain = stream_buffer.Read<uint8_t>();
  const uint8_t scr = stream_buffer.Read<uint8_t>();
  gain_index_ = (uint8_t)ScopeMath::ClampGainIndex((int)gain);
  screensaver_trace_ = scr != 0;
  SetChannel((int)ch);   // wraps/validates internally
  return stream_buffer.underflow() ? 0 : stream_buffer.read();
}

FLASHMEM void AppScope::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
#ifdef AUDIO_INTERFACE
      if (!frozen_ && ScopeMath::ChannelKindOf(channel_) == ScopeMath::KIND_AUDIO)
        scope_capture_.SetActiveChannel(ScopeMath::ChannelSubIndex(channel_));
#endif
      break;
    case OC::APP_EVENT_SUSPEND:
      // Switching to a different app entirely: neither DrawMenu() nor
      // DrawScreensaver() will run again until RESUME, so there is nothing
      // left to animate -- stop the audio tap from doing pointless work
      // meanwhile (mirrors TunerApp::SetActive(false)'s reasoning, just
      // gating one flag here instead of a connect/disconnect pair).
#ifdef AUDIO_INTERFACE
      scope_capture_.SetActiveChannel(-1);
#endif
      break;
    case OC::APP_EVENT_SCREENSAVER_ON:
    case OC::APP_EVENT_SCREENSAVER_OFF:
      // Deliberately no-op: Render() (called from both DrawMenu() and
      // DrawScreensaver()) already decides whether to sample/draw, so
      // there is nothing extra to arm or disarm on either edge.
      break;
    default: break;
  }
}

void AppScope::Process(OC::IOFrame *) {}
FLASHMEM void AppScope::Loop() {}

// Shared by DrawMenu() and DrawScreensaver() so the screensaver's live
// trace is the SAME per-frame cost as the normal view, not extra work on
// top of it -- see the class comment's SAMPLING paragraph.
FLASHMEM void AppScope::Render() const {
  using namespace ScopeMath;

  const ChannelKind kind = ChannelKindOf(channel_);
  const int sub = ChannelSubIndex(channel_);
  const float gain = kGainSteps[gain_index_];
  const int32_t full_scale = ChannelFullScale(channel_);

  // --- sample (skipped entirely while frozen) -----------------------------
  if (!frozen_) {
    switch (kind) {
      case KIND_CV_IN:
        last_raw_ = OC::ADC::value(ScopeAppNS::AdcChannelForSub(sub));
        cv_ring_[cv_head_] = (int16_t)last_raw_;
        cv_head_ = (uint8_t)((cv_head_ + 1) % kRingSize);
        break;
      case KIND_CV_OUT:
        last_raw_ = (int32_t)OC::DAC::value((size_t)sub) - kCvOutCenter;
        cv_ring_[cv_head_] = (int16_t)last_raw_;
        cv_head_ = (uint8_t)((cv_head_ + 1) % kRingSize);
        break;
      case KIND_AUDIO:
#ifdef AUDIO_INTERFACE
        {
          const uint8_t h = scope_capture_.Head(sub);
          last_raw_ = scope_capture_.RingValue(sub, (h + kRingSize - 1) % kRingSize);
        }
#endif
        break;
    }
  }

  // --- header: title, gain, freeze flag -----------------------------------
  graphics.setPrintPos(1, 1);
  graphics.print("SCOPE");
  if (frozen_) {
    graphics.setPrintPos(40, 1);
    graphics.print("FRZ");
  }
  char gbuf[8];
  snprintf(gbuf, sizeof(gbuf), "x%.2f", gain);
  graphics.setPrintPos(127, 1);
  graphics.print_right(gbuf);
  graphics.drawHLine(0, 10, 128);

  // --- channel label + numeric readout ------------------------------------
  char label[16];
  ChannelLabel(channel_, label, sizeof(label));
  graphics.setPrintPos(1, 13);
  graphics.print(label);
  char vbuf[12];
  snprintf(vbuf, sizeof(vbuf), "%ld", (long)last_raw_);
  graphics.setPrintPos(127, 13);
  graphics.print_right(vbuf);

  // --- trace -----------------------------------------------------------
  static constexpr int kPlotTop = 23;
  static constexpr int kPlotH = 29;   // rows 23..51
  graphics.drawHLinePattern(0, kPlotTop + kPlotH / 2, 128, 3);  // zero line

  int prev_y = kPlotTop;
  for (int x = 0; x < kRingSize; ++x) {
    int32_t raw;
    if (kind == KIND_AUDIO) {
#ifdef AUDIO_INTERFACE
      const int idx = RingReadIndex(scope_capture_.Head(sub), x);
      raw = scope_capture_.RingValue(sub, idx);
#else
      raw = 0;
#endif
    } else {
      const int idx = RingReadIndex(cv_head_, x);
      raw = cv_ring_[idx];
    }
    const int y = kPlotTop + ValueToRow(raw, gain, full_scale, kPlotH);
    if (x == 0) graphics.drawLine(x, y, x, y);
    else graphics.drawLine(x - 1, prev_y, x, y);
    prev_y = y;
  }

  gfxFooter("A:frz B:gain L:scrsvr");
}

FLASHMEM void AppScope::DrawMenu() const { Render(); }

// Off by default (screensaver_trace_ == false): falls back to a blank
// screen like a simple app normally would (e.g. AppScaleEditor::
// DrawScreensaver()). Toggled on (encL push), it reuses Render() verbatim
// -- same sampling, same draw -- so the "scope stays watching" idle screen
// costs exactly what the on-screen view already costs, no more.
FLASHMEM void AppScope::DrawScreensaver() const {
  if (!screensaver_trace_) return;
  Render();
}

FLASHMEM void AppScope::HandleButtonEvent(const UI::Event &event) {
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  switch (event.control) {
    case OC::CONTROL_BUTTON_UP:      // A: freeze/pause toggle
      SetFrozen(!frozen_);
      break;
    case OC::CONTROL_BUTTON_DOWN:    // B: reset gain to 1.0x
      gain_index_ = (uint8_t)ScopeMath::kDefaultGainIndex;
      break;
    case OC::CONTROL_BUTTON_L:       // encL push: live trace as screensaver, on/off
      screensaver_trace_ = !screensaver_trace_;
      break;
    default: break;   // R/X/Y unbound in v1
  }
}

FLASHMEM void AppScope::HandleEncoderEvent(const UI::Event &event) {
  if (event.control == OC::CONTROL_ENCODER_L) {
    SetChannel((int)channel_ + event.value);
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    gain_index_ = (uint8_t)ScopeMath::ClampGainIndex((int)gain_index_ + event.value);
  }
}

FLASHMEM void AppScope::GetIOConfig(OC::IOConfig &) const {
  // Scope only ever reads OC::ADC::value()/OC::DAC::value() directly; it
  // does not route or drive anything through the panel I/O config.
}

FLASHMEM void AppScope::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("ch ");
  graphics.print((int)channel_);
  graphics.setPrintPos(2, 22);
  graphics.print("raw ");
  graphics.print((int)last_raw_);
}
