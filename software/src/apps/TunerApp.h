#pragma once
// ---------------------------------------------------------------------------
// Strobe tuner.
//
// Reads the panel audio input, identifies the note, and shows the tuning error
// the way a mechanical strobe does: as phase, not as a number. Three bands
// drift at 1x, 2x and 4x the error, so the eye integrates for as long as you
// let it and the resolution keeps improving - a still band is a still band.
//
// The numeric cents readout is derived from the same phase (its drift RATE is
// exactly the frequency error), averaged over a moving window; it exists to
// disambiguate the bands, which alias past about half the frame rate exactly
// as a real strobe aliases under a fixed-rate lamp.
//
// Accuracy floor is the Teensy's crystal (tens of ppm; a cent is 578ppm), so
// the instrument being tuned drifts further than this reads.
// ---------------------------------------------------------------------------

#include "../Audio/AudioAnalyzeStrobe.h"
#include "../AudioIO.h"
#include <analyze_notefreq.h>

namespace TunerAppNS {

static const char *const kNoteNames[12] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// strobe band geometry
static constexpr int kBandPeriod = 16;   // px per light/dark pair
static constexpr int kRows = 3;
static constexpr int kRowMul[kRows] = { 1, 2, 4 };
static constexpr int kRowY[kRows] = { 30, 39, 48 };
static constexpr int kRowH[kRows] = { 7, 7, 7 };

}  // namespace TunerAppNS

OC_APP_CLASS(AppTuner, TWOCCS("TU"), "Tuner", "Strobe tuner") {
public:
  OC_APP_INTERFACE_DECLARE(AppTuner, 4);

private:
  AudioAnalyzeStrobe strobe_;
  AudioAnalyzeNoteFrequency notefreq_;
  AudioConnection *conn_strobe_ = nullptr;
  AudioConnection *conn_notefreq_ = nullptr;
  bool audio_wired_ = false;

  // settings (persisted)
  uint16_t a4_hz_ = 440;
  bool manual_ = false;          // lock to a chosen note instead of auto
  int8_t manual_note_ = 69;      // MIDI note when locked

  // live state
  float detected_hz_ = 0.0f;     // from the YIN coarse detector
  int8_t target_note_ = 69;      // the note the strobe is referenced to
  float target_hz_ = 440.0f;
  float strobe_phase_ = 0.0f;    // radians, for the display
  float strobe_mag_ = 0.0f;
  float cents_ = 0.0f;           // from phase drift rate
  bool have_signal_ = false;

  // phase-drift integrator
  float last_phase_ = 0.0f;
  float drift_accum_ = 0.0f;     // radians accumulated
  uint32_t drift_us_ = 0;        // over this much time
  uint32_t last_us_ = 0;
  uint32_t last_signal_ms_ = 0;

  float NoteToHz(int note) const {
    return (float)a4_hz_ * powf(2.0f, (float)(note - 69) / 12.0f);
  }

  void SetTarget(int note) {
    if (note < 12) note = 12;
    if (note > 120) note = 120;
    target_note_ = (int8_t)note;
    target_hz_ = NoteToHz(note);
    strobe_.setReference(target_hz_);
    // a fresh reference means the old phase history is meaningless
    drift_accum_ = 0.0f;
    drift_us_ = 0;
    cents_ = 0.0f;
    last_phase_ = 0.0f;
  }

  void WireAudio();
  void SetActive(bool on);
  void DrawStrobe() const;
};

// The audio graph is global and always running, so the tuner's taps are
// created once and then connected only while the app is on screen - an idle
// analyzer would cost CPU in every other app.
FLASHMEM void AppTuner::WireAudio() {
  if (audio_wired_) return;
#ifdef AUDIO_INTERFACE
  conn_strobe_ = new AudioConnection(OC::AudioIO::InputStream(0), 0, strobe_, 0);
  conn_notefreq_ = new AudioConnection(OC::AudioIO::InputStream(0), 0, notefreq_, 0);
  if (conn_strobe_) conn_strobe_->disconnect();
  if (conn_notefreq_) conn_notefreq_->disconnect();
#endif
  audio_wired_ = true;
}

FLASHMEM void AppTuner::SetActive(bool on) {
  WireAudio();
  if (on) {
    if (conn_strobe_) conn_strobe_->connect();
    if (conn_notefreq_) conn_notefreq_->connect();
    notefreq_.begin(0.15f);
  } else {
    if (conn_strobe_) conn_strobe_->disconnect();
    if (conn_notefreq_) conn_notefreq_->disconnect();
  }
  strobe_.setActive(on);
}

FLASHMEM void AppTuner::Init() {
  SetTarget(69);
  last_us_ = micros();
}

FLASHMEM size_t AppTuner::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  stream_buffer.Write<uint16_t>(a4_hz_);
  stream_buffer.Write<uint8_t>(manual_ ? 1 : 0);
  stream_buffer.Write<uint8_t>((uint8_t)manual_note_);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppTuner::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  const uint16_t a4 = stream_buffer.Read<uint16_t>();
  a4_hz_ = (a4 >= 400 && a4 <= 480) ? a4 : 440;
  manual_ = stream_buffer.Read<uint8_t>() != 0;
  const uint8_t mn = stream_buffer.Read<uint8_t>();
  manual_note_ = (mn >= 12 && mn <= 120) ? (int8_t)mn : 69;
  SetTarget(manual_ ? manual_note_ : 69);
  return stream_buffer.underflow() ? 0 : stream_buffer.read();
}

FLASHMEM void AppTuner::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME: SetActive(true); break;
    case OC::APP_EVENT_SUSPEND:
    case OC::APP_EVENT_SCREENSAVER_ON: SetActive(false); break;
    default: break;
  }
}

void AppTuner::Process(OC::IOFrame *ioframe) { (void)ioframe; }

FLASHMEM void AppTuner::Loop() {
  // --- coarse: what note is this? -----------------------------------------
  if (notefreq_.available()) {
    const float f = notefreq_.read();
    if (f > 20.0f && f < 5000.0f) {
      detected_hz_ = f;
      last_signal_ms_ = millis();
      if (!manual_) {
        const int n = (int)lroundf(69.0f + 12.0f * log2f(f / (float)a4_hz_));
        if (n != target_note_) SetTarget(n);
      }
    }
  }

  // --- fine: phase drift IS the tuning error ------------------------------
  float phase = 0.0f, mag = 0.0f;
  strobe_.read(phase, mag);
  strobe_phase_ = phase;
  strobe_mag_ = mag;

  const uint32_t now = micros();
  const uint32_t dt = now - last_us_;
  last_us_ = now;

  // a strobe with nothing on its input just shows noise; say so instead
  have_signal_ = (mag > 0.0015f) && (millis() - last_signal_ms_ < 1500);
  if (!have_signal_) {
    drift_accum_ = 0.0f;
    drift_us_ = 0;
    cents_ = 0.0f;
    last_phase_ = phase;
    return;
  }

  float dp = phase - last_phase_;
  last_phase_ = phase;
  while (dp > (float)M_PI) dp -= 2.0f * (float)M_PI;
  while (dp < -(float)M_PI) dp += 2.0f * (float)M_PI;

  drift_accum_ += dp;
  drift_us_ += dt;

  // resolve over ~400ms: long enough to be steady, short enough to follow a
  // hand on the knob. The window is what buys the resolution - halve it and
  // you halve the precision.
  if (drift_us_ >= 400000u) {
    const float secs = (float)drift_us_ * 1e-6f;
    const float df = drift_accum_ / (2.0f * (float)M_PI * secs);   // Hz error
    const float ratio = (target_hz_ + df) / target_hz_;
    cents_ = (ratio > 0.0f) ? 1200.0f * log2f(ratio) : 0.0f;
    drift_accum_ = 0.0f;
    drift_us_ = 0;
  }
}

FLASHMEM void AppTuner::DrawStrobe() const {
  using namespace TunerAppNS;
  // Each row is the same phase multiplied by its harmonic number, so row 3
  // creeps four times as fast as row 1 for the same error: coarse row to get
  // close, fine row to finish.
  const float turns = strobe_phase_ / (2.0f * (float)M_PI);
  for (int r = 0; r < kRows; ++r) {
    float t = turns * (float)kRowMul[r];
    t -= floorf(t);                                  // wrap to 0..1
    const int off = (int)(t * (float)kBandPeriod);
    for (int x = -kBandPeriod + off; x < 128; x += kBandPeriod) {
      const int x0 = x < 0 ? 0 : x;
      int w = kBandPeriod / 2 - (x0 - x);
      if (w <= 0) continue;
      if (x0 + w > 128) w = 128 - x0;
      if (w > 0) graphics.drawRect(x0, kRowY[r], w, kRowH[r]);
    }
  }
}

FLASHMEM void AppTuner::DrawMenu() const {
  using namespace TunerAppNS;
  menu::DefaultTitleBar::Draw();
  graphics.print("Tuner");
  graphics.setPrintPos(72, 2);
  graphics.print("A4=");
  graphics.print((int)a4_hz_);

  // note name, big-ish, plus the measured pitch
  graphics.setPrintPos(2, 16);
  if (have_signal_ || manual_) {
    const int n = target_note_;
    graphics.print(kNoteNames[((n % 12) + 12) % 12]);
    graphics.print(n / 12 - 1);
  } else {
    graphics.print("--");
  }
  if (manual_) {
    graphics.setPrintPos(30, 16);
    graphics.print("LOCK");
  }

  graphics.setPrintPos(62, 16);
  if (have_signal_) {
    graphics.print((int)(detected_hz_ + 0.5f));
    graphics.print("Hz");
  } else {
    graphics.print("no sig");
  }

  DrawStrobe();

  // cents readout and a centre-null bar
  graphics.setPrintPos(2, 56);
  if (have_signal_) {
    const int c10 = (int)lroundf(cents_ * 10.0f);
    if (c10 > 0) graphics.print("+");
    graphics.print(c10 / 10);
    graphics.print(".");
    graphics.print(abs(c10 % 10));
    graphics.print("c");
  } else {
    graphics.print("---");
  }

  // bar: +/-50 cents across the right half, with a centre tick
  const int bx = 56, bw = 70, by = 58;
  graphics.drawHLine(bx, by + 2, bw);
  graphics.drawVLine(bx + bw / 2, by, 6);
  if (have_signal_) {
    float c = cents_;
    if (c > 50.0f) c = 50.0f;
    if (c < -50.0f) c = -50.0f;
    const int px = bx + bw / 2 + (int)(c * (float)(bw / 2) / 50.0f);
    graphics.drawRect(px - 1, by, 3, 6);
  }
}

FLASHMEM void AppTuner::DrawScreensaver() const { DrawMenu(); }

FLASHMEM void AppTuner::HandleButtonEvent(const UI::Event &event) {
  if (UI::EVENT_BUTTON_PRESS == event.type) {
    if (OC::CONTROL_BUTTON_R == event.control) {
      // lock the strobe to whatever it is showing now (or release it)
      manual_ = !manual_;
      if (manual_) manual_note_ = target_note_;
      else SetTarget(target_note_);
    } else if (OC::CONTROL_BUTTON_L == event.control) {
      a4_hz_ = 440;                    // snap the reference back to standard
      SetTarget(target_note_);
    }
  }
}

FLASHMEM void AppTuner::HandleEncoderEvent(const UI::Event &event) {
  if (OC::CONTROL_ENCODER_R == event.control) {
    int a4 = (int)a4_hz_ + event.value;
    if (a4 < 400) a4 = 400;
    if (a4 > 480) a4 = 480;
    a4_hz_ = (uint16_t)a4;
    SetTarget(target_note_);
  } else if (OC::CONTROL_ENCODER_L == event.control) {
    // left encoder picks the note, and picking one implies locking to it
    manual_ = true;
    manual_note_ = (int8_t)(target_note_ + event.value);
    SetTarget(manual_note_);
    manual_note_ = target_note_;
  }
}

FLASHMEM void AppTuner::GetIOConfig(OC::IOConfig &ioconfig) const {
  // the tuner listens on the audio input; it drives no CV outputs
  for (int i = 0; i < DAC_CHANNEL_COUNT; ++i)
    ioconfig.outputs[i].set("off", OC::OUTPUT_MODE_RAW);
}

FLASHMEM void AppTuner::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.print("mag ");
  graphics.print((int)(strobe_mag_ * 10000.0f));
  graphics.setPrintPos(2, 22);
  graphics.print("ref ");
  graphics.print((int)(target_hz_ * 100.0f));
}
