#pragma once
// ---------------------------------------------------------------------------
// Scope: a triggered, auto-ranging oscilloscope and spectrum view for the
// Xenomorpher's jacks. Design: docs/superpowers/specs/2026-10-24-scope-app-design.md.
//
// SOURCES (ScopeSources.h): audio in/out L/R (codec builds only), CV in 1-8,
// CV out 1-8, TR 1-4. A view is a set of up to 8 of them, picked from a
// quick list on the channel field or, on its last entry, a Custom picker.
//
// CAPTURE (ScopeCapture.h) runs in the hot paths at each source's own rate
// into decimating min/max rings: one bucket per screen column at the chosen
// timebase. Everything else happens here, at redraw rate, in loop context:
// converting to millivolts, finding the trigger (ScopeTrigger.h), damped
// auto-ranging (ScopeRange.h), the FFT (ScopeFft.h) and drawing.
//
// SCREEN: one 21-column line at the top, `channel time volts trigger`, the
// focused field inverted (the only use of inversion besides the jack-number
// chips in a stacked view); an auto time or volts field ends in "A". Below
// it is plot: one trace, overlaid traces on a shared scale, stacked lanes
// each on its own scale, or the spectrum.
//
// CONTROLS (encL = where you are, encR = what it is):
//   encL turn  focus along the top line      encR turn  change the field
//   encR push  focused field back to auto    encL push  back (closes Custom)
//   A freeze/run   B everything auto   X scope/FFT   Y overlaid/stacked
// The trigger field cycles auto (per shown trace), level, and external TR1-4.
// In level mode encR turns the level and encR push returns to auto.
//
// VOLTS: audio in is AC-coupled and full scale is about +-9.3 V at the jack
// (hardware/xenomorpher/README.md); CV in uses the calibrated reading,
// 1200 mV per 409.6 counts; CV out uses the DAC's own zero and octave
// calibration points, 1.2 V per octave.
// ---------------------------------------------------------------------------

#include "../HSUtils.h"
#include "../OC_ADC.h"
#include "../OC_DAC.h"
#include "../ScopeCapture.h"
#include "../ScopeFft.h"
#include "../ScopeRange.h"
#include "../ScopeSettings.h"
#include "../ScopeSources.h"
#include "../ScopeTrigger.h"
#include <new>  // placement new, std::nothrow
#include <stdlib.h>
#ifdef XENO_CODEC_AUDIO
#include "../Audio/AudioScopeCapture.h"
#include "../AudioIO.h"
#endif

namespace ScopeAppNS {

using namespace ScopeSources;

#ifdef XENO_CODEC_AUDIO
static constexpr bool kCodec = true;
#else
static constexpr bool kCodec = false;
#endif

constexpr int kTop = 11;                 // first plot row
constexpr int kPlotH = 64 - kTop;        // 53 rows
constexpr int kCols = 128;
constexpr int kMaxWindow = kCols;        // buckets on screen at most
constexpr int kAnalyse = 4 * kMaxWindow; // buckets the trigger looks at
constexpr float kAudioFsMv = 9300.f;
constexpr float kCvInMvPerCount = 1200.f / 409.6f;
constexpr float kCvFsMv = 12000.f;       // FFT normalisation for CV sources
constexpr float kMinPpMv = 5.f;          // flatter than this is no signal

inline int Wrap(int i, int n) {
  if (n <= 0) return 0;
  const int r = i % n;
  return r < 0 ? r + n : r;
}

enum Field { F_CH, F_TIME, F_VOLT, F_TRIG, F_COUNT };
enum TrigMode { TM_AUTO = 0, TM_LEVEL = 1, TM_EXT = 2 };  // TM_EXT + n = TRn+1

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

// One ring sample (bucket value) in millivolts for its source.
inline float ToMv(Source s, int16_t v) {
  switch (KindOf(s)) {
    case KIND_AUDIO_IN:
    case KIND_AUDIO_OUT: return v * (kAudioFsMv / 32768.f);
    case KIND_CV_IN: {
      const ADC_CHANNEL ch = AdcChannelForSub(SubIndex(s));
      return OC::ADC::calibrated_from_raw12(ch, (uint32_t)(v < 0 ? 0 : v)) * kCvInMvPerCount;
    }
    case KIND_CV_OUT: {
      const DAC_CHANNEL ch = (DAC_CHANNEL)SubIndex(s);
      const int32_t zero = (int32_t)OC::DAC::get_zero_offset(ch);
      const int32_t oct = (int32_t)OC::DAC::get_octave_offset(ch, 1) - zero;
      const int32_t code = (int32_t)v + 32768;
      return oct ? (code - zero) * 1200.f / oct : 0.f;
    }
    default: return v ? 1.f : 0.f;  // TR: 0/1
  }
}

// Capture latency, in CPU cycles, between a ring's commit_cyc and the real
// time of its newest sample (ScopeCapture.h).
inline uint32_t LatencyCyc(Source s) {
  switch (KindOf(s)) {
    case KIND_AUDIO_IN:
      return (uint32_t)((uint64_t)F_CPU_ACTUAL * ScopeCapture::kAudioInLatencySamples / 48000);
    case KIND_CV_IN:
      return (uint32_t)((uint64_t)F_CPU_ACTUAL * ScopeCapture::kCvInLatencyFrames / 37500);
    default: return 0;
  }
}

// Everything the Scope needs beyond its settings -- the capture rings and the
// loop-context work buffers -- in ONE block taken from the heap when the
// Scope is opened and returned when it closes. Static buffers would cost
// ~70 KB of RAM2 on every boot, Scope open or not; with an armed firmware
// card taking 229 KB at boot that starved a later unchecked allocation and
// boot-looped the unit. Null whenever the Scope is not open (or when the
// allocation failed: the Scope then says so instead of drawing).
struct Arena {
  ScopeRing rings[ScopeSources::kMaxTraces];
  float mv[kAnalyse], lo[kAnalyse], hi[kAnalyse];
  float fft_in[ScopeFft::kN], fft_db[ScopeFft::kN / 2 + 1];
  float acorr[kAnalyse / 2 + 4];  // EstimatePeriod scratch: lags up to n/2, plus 3
  ScopeFft::Work fft;
};
static Arena *g_arena = nullptr;
static bool g_open = false;  // between RESUME and SUSPEND

FLASHMEM static bool AllocArena() {
  if (g_arena) return true;
  void *p = malloc(sizeof(Arena));
  if (!p) return false;  // checked: no exceptions, a null here is the error state
  g_arena = new (p) Arena();
  ScopeCapture::Bind(g_arena->rings);
  return true;
}

// Loop context. Unmaps the capture slots first, so no ISR can write a ring
// that is about to be freed (Bind(nullptr) disarms).
FLASHMEM static void FreeArena() {
  ScopeCapture::Bind(nullptr);
  if (g_arena) {
    free(g_arena);
    g_arena = nullptr;
  }
}

}  // namespace ScopeAppNS

OC_APP_CLASS(AppScope, TWOCCS("SP"), "Scope", "Scope") {
public:
  OC_APP_INTERFACE_DECLARE(AppScope, kScopeSettingsBytes);

private:
  ScopeSettings set_;

  // --- live UI state ------------------------------------------------------
  uint8_t focus_ = ScopeAppNS::F_CH;
  bool frozen_ = false;
  bool picker_ = false;
  uint8_t picker_cursor_ = 0;
  bool level_follow_ = true;  // level tracks the signal until turned
  mutable int16_t live_level_mv_ = 0;  // the followed level while level_follow_

  // --- the armed view ---------------------------------------------------
  ScopeSources::Source list_[ScopeSources::kMaxTraces];
  int n_ = 0;
  int armed_time_idx_ = -1;

  // --- analysis state (mutated while drawing; see Render()) ---------------
  mutable ScopeRange::AutoTime atime_;
  mutable ScopeRange::AutoVolt avolt_[ScopeSources::kMaxTraces];
  mutable ScopeTrigger::AutoTrigger atrig_;
  mutable bool locked_ = false;
  mutable float peak_hz_ = 0.f;
  mutable float last_mv_[ScopeSources::kMaxTraces] = {};
  mutable int pending_time_idx_ = -1;  // auto timebase moved: re-arm in Loop()

#ifdef XENO_CODEC_AUDIO
  AudioScopeCapture scope_capture_;
  AudioConnection *conn_[4] = {nullptr, nullptr, nullptr, nullptr};
#endif
  bool audio_wired_ = false;

  ScopeSources::Mask ViewMask() const;
  int TimeIdx() const;
  int VoltIdx(int trace) const;
  int TrigSlot() const;
  int16_t LevelMv() const { return level_follow_ ? live_level_mv_ : set_.trig_level_mv; }
  bool TrigIsTr() const;
  void Rearm(bool keep_frozen = false);
  void WireAudio();
  void SetFrozen(bool f);
  void AllAuto();
  void TurnField(int v);
  void PushField();
  struct Snap {
    uint32_t c;           // committed buckets
    uint32_t newest_cyc;  // when its newest sample really arrived
    float bucket_s;
  };
  void Render() const;
  void RenderPicker() const;
  void RenderSpectrum() const;
  void DrawHeader(const char *trig_text, const char *time_text, const char *volt_text) const;
};

// ---------------------------------------------------------------------------
// Everything below runs in loop context except Process() (core ISR), which
// only forwards to ScopeCapture::OnCoreTick(). FLASHMEM throughout the rest:
// none of it is on an audio or ISR path.
// ---------------------------------------------------------------------------

FLASHMEM ScopeSources::Mask AppScope::ViewMask() const {
  using namespace ScopeSources;
  const Mask m = QuickIsCustom(set_.quick_idx) ? set_.custom_mask : QuickMask(set_.quick_idx);
  return m & AvailableMask(ScopeAppNS::kCodec);
}

FLASHMEM int AppScope::TimeIdx() const {
  return set_.time_auto ? atime_.Idx() : ScopeRange::ClampTime(set_.time_idx);
}

FLASHMEM int AppScope::VoltIdx(int trace) const {
  return set_.volt_auto ? avolt_[trace].Idx() : ScopeRange::ClampVolt(set_.volt_idx);
}

FLASHMEM int AppScope::TrigSlot() const {
  if (n_ <= 0) return -1;
  if (set_.trig_mode >= ScopeAppNS::TM_EXT) {
    for (int i = 0; i < n_; ++i)  // align ext sync to the first non-TR trace
      if (ScopeSources::KindOf(list_[i]) != ScopeSources::KIND_TR) return i;
    return 0;
  }
  return set_.trig_src < n_ ? set_.trig_src : 0;
}

FLASHMEM bool AppScope::TrigIsTr() const {
  const int s = TrigSlot();
  return s >= 0 && ScopeSources::KindOf(list_[s]) == ScopeSources::KIND_TR;
}

// A change to what is shown, made while frozen, unfreezes: the rings hold
// the old source list and timebase, and drawing them under the new ones
// would show one source's samples labelled as another.
FLASHMEM void AppScope::Rearm(bool keep_frozen) {
  using namespace ScopeSources;
  if (!keep_frozen) frozen_ = false;
  n_ = MaskToList(ViewMask(), list_);
  if (set_.trig_src >= n_) set_.trig_src = 0;
  const int ts = TrigSlot();
  const bool fft = set_.view == 1 && ts >= 0 && !TrigIsTr();
  const int ext = set_.trig_mode >= ScopeAppNS::TM_EXT ? set_.trig_mode - ScopeAppNS::TM_EXT : -1;
  armed_time_idx_ = TimeIdx();
  if (ScopeAppNS::g_open && !ScopeAppNS::AllocArena()) {
    // Retried on every re-arm, so closing something else and pressing a key recovers.
    ScopeCapture::Disarm();
  } else if (frozen_) {
    ScopeCapture::Disarm();
  } else {
    ScopeCapture::Arm(list_, n_, armed_time_idx_, fft ? ts : -1, ext);
  }
  atrig_.Reset();
  locked_ = false;
}

// std::nothrow on every connection allocated here, deliberately: with
// -fno-exceptions and no -fcheck-new, a failed plain `new` would run the
// AudioConnection constructor on nullptr. std::nothrow makes the compiler
// test the result, so the pointer simply stays null.
FLASHMEM void AppScope::WireAudio() {
  if (audio_wired_) return;
#ifdef XENO_CODEC_AUDIO
  // Wired permanently: an unshown tap costs a release() per block, so there
  // is no graph to rebuild on every resume.
  conn_[0] = new (std::nothrow) AudioConnection(OC::AudioIO::InputStream(0), 0, scope_capture_,
                                                AudioScopeCapture::TAP_IN_L);
  conn_[1] = new (std::nothrow) AudioConnection(OC::AudioIO::InputStream(0), 1, scope_capture_,
                                                AudioScopeCapture::TAP_IN_R);
  conn_[2] = new (std::nothrow) AudioConnection(OC::AudioIO::OutputStream(), 0, scope_capture_,
                                                AudioScopeCapture::TAP_OUT_L);
  conn_[3] = new (std::nothrow) AudioConnection(OC::AudioIO::OutputStream(), 1, scope_capture_,
                                                AudioScopeCapture::TAP_OUT_R);
#endif
  audio_wired_ = true;
}

FLASHMEM void AppScope::SetFrozen(bool f) {
  frozen_ = f;
  Rearm(true);  // frozen: capture disarmed, the rings keep their last frame
}

FLASHMEM void AppScope::AllAuto() {
  set_.time_auto = 1;
  set_.volt_auto = 1;
  set_.trig_mode = ScopeAppNS::TM_AUTO;
  level_follow_ = true;
  Rearm();
}

FLASHMEM void AppScope::Init() {
  ScopeSettingsDefaults(&set_);
  atime_.Reset(ScopeRange::kDefaultTimeIdx);
  for (auto &v : avolt_) v.Reset(ScopeRange::kDefaultVoltIdx);
  focus_ = ScopeAppNS::F_CH;
  frozen_ = false;
  picker_ = false;
  audio_wired_ = false;
  WireAudio();
  n_ = ScopeSources::MaskToList(ViewMask(), list_);  // not armed until resumed
}

FLASHMEM size_t AppScope::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  uint8_t buf[kScopeSettingsBytes];
  ScopeSettingsPack(set_, buf);
  for (uint8_t b : buf) stream_buffer.Write<uint8_t>(b);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppScope::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  uint8_t buf[kScopeSettingsBytes];
  size_t n = 0;
  while (n < sizeof(buf) && stream_buffer.available()) buf[n++] = stream_buffer.Read<uint8_t>();
  ScopeSettingsUnpack(buf, n, &set_);  // defaults on anything it can't read
  atime_.Reset(set_.time_idx);
  for (auto &v : avolt_) v.Reset(set_.volt_idx);
  n_ = ScopeSources::MaskToList(ViewMask(), list_);
  return stream_buffer.underflow() ? 0 : stream_buffer.read();
}

FLASHMEM void AppScope::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
      ScopeAppNS::g_open = true;
      Rearm();
      break;
    case OC::APP_EVENT_SUSPEND:
      ScopeAppNS::g_open = false;
      ScopeCapture::Disarm();
      ScopeAppNS::FreeArena();
      break;
    default: break;
  }
}

// Core ISR: the CV-out and TR lanes are sampled here (ScopeCapture.cpp).
void AppScope::Process(OC::IOFrame *) { ScopeCapture::OnCoreTick(); }

FLASHMEM void AppScope::Loop() {
  // Auto timebase moved while drawing: re-arm here, out of the const draw.
  if (pending_time_idx_ >= 0) {
    pending_time_idx_ = -1;
    if (!frozen_) Rearm();
  }
}

FLASHMEM void AppScope::TurnField(int v) {
  using namespace ScopeAppNS;
  switch (focus_) {
    case F_CH:
      set_.quick_idx = (uint8_t)ScopeAppNS::Wrap(set_.quick_idx + v, ScopeSources::QuickCount());
      set_.trig_src = 0;
      Rearm();
      break;
    case F_TIME:
      set_.time_idx = (uint8_t)ScopeRange::ClampTime(TimeIdx() + v);
      set_.time_auto = 0;
      Rearm();
      break;
    case F_VOLT:
      set_.volt_idx = (uint8_t)ScopeRange::ClampVolt(VoltIdx(0) + v);
      set_.volt_auto = 0;
      break;
    case F_TRIG:
      if (set_.trig_mode == TM_LEVEL) {  // turn the level
        const int step = ScopeRange::kVoltDivMv[VoltIdx(set_.trig_src < n_ ? set_.trig_src : 0)] / 4;
        set_.trig_level_mv = (int16_t)constrain(LevelMv() + v * (step ? step : 1), -15000, 15000);
        level_follow_ = false;
      } else {
        // auto on each shown non-TR trace, then level on trace 0, then TR1-4
        int pos = set_.trig_mode >= TM_EXT ? n_ + 1 + (set_.trig_mode - TM_EXT) : set_.trig_src;
        pos = ScopeAppNS::Wrap(pos + v, n_ + 1 + 4);
        if (pos < n_) {
          set_.trig_mode = TM_AUTO;
          set_.trig_src = (uint8_t)pos;
        } else if (pos == n_) {
          set_.trig_mode = TM_LEVEL;
          level_follow_ = true;
        } else {
          set_.trig_mode = (uint8_t)(TM_EXT + (pos - n_ - 1));
        }
        Rearm();
      }
      break;
    default: break;
  }
}

FLASHMEM void AppScope::PushField() {
  using namespace ScopeAppNS;
  switch (focus_) {
    case F_CH:
      if (ScopeSources::QuickIsCustom(set_.quick_idx)) {
        picker_ = true;
        picker_cursor_ = 0;
      }
      break;
    case F_TIME: set_.time_auto = 1; atime_.Reset(set_.time_idx); Rearm(); break;
    case F_VOLT: set_.volt_auto = 1; break;
    case F_TRIG: set_.trig_mode = TM_AUTO; level_follow_ = true; Rearm(); break;
    default: break;
  }
}

FLASHMEM void AppScope::HandleButtonEvent(const UI::Event &event) {
  using namespace ScopeSources;
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  if (picker_) {
    switch (event.control) {
      case OC::CONTROL_BUTTON_R: {
        const Mask bit = (Mask)1u << picker_cursor_;
        if (set_.custom_mask & bit) {
          set_.custom_mask &= ~bit;
        } else {
          Source tmp[kMaxTraces];
          if (MaskToList(set_.custom_mask, tmp) < kMaxTraces) set_.custom_mask |= bit;
        }
        break;
      }
      case OC::CONTROL_BUTTON_L: picker_ = false; Rearm(); break;
      default: break;
    }
    return;
  }
  switch (event.control) {
    case OC::CONTROL_BUTTON_A: SetFrozen(!frozen_); break;
    case OC::CONTROL_BUTTON_B: AllAuto(); break;
    case OC::CONTROL_BUTTON_X:
      if (!TrigIsTr()) { set_.view = set_.view ? 0 : 1; Rearm(); }
      break;
    case OC::CONTROL_BUTTON_Y:
      if (n_ > 1) set_.stacked = set_.stacked ? 0 : 1;
      break;
    case OC::CONTROL_BUTTON_R: PushField(); break;
    case OC::CONTROL_BUTTON_L: break;  // back: nothing open to close
    default: break;
  }
}

FLASHMEM void AppScope::HandleEncoderEvent(const UI::Event &event) {
  using namespace ScopeSources;
  if (picker_) {
    if (event.control == OC::CONTROL_ENCODER_R || event.control == OC::CONTROL_ENCODER_L) {
      const Mask avail = AvailableMask(ScopeAppNS::kCodec);
      int c = picker_cursor_;
      const int dir = event.value > 0 ? 1 : -1;
      for (int steps = event.value > 0 ? event.value : -event.value; steps > 0; --steps) {
        do { c = ScopeAppNS::Wrap(c + dir, SRC_COUNT); } while (!(avail & ((Mask)1u << c)));
      }
      picker_cursor_ = (uint8_t)c;
    }
    return;
  }
  if (event.control == OC::CONTROL_ENCODER_L) {
    focus_ = (uint8_t)ScopeAppNS::Wrap(focus_ + event.value, ScopeAppNS::F_COUNT);
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    TurnField(event.value);
  }
}

FLASHMEM void AppScope::GetIOConfig(OC::IOConfig &) const {
  // Reads the jacks directly through ScopeCapture; routes nothing.
}

FLASHMEM void AppScope::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.printf("n %d time %d", n_, TimeIdx());
  graphics.setPrintPos(2, 22);
  graphics.printf("lock %d per %d", locked_ ? 1 : 0, (int)atrig_.Period());
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// The top line. Fields sit left to right one space apart; the channel label
// gives way first if the line would pass 21 columns.
FLASHMEM void AppScope::DrawHeader(const char *trig_text, const char *time_text,
                                   const char *volt_text) const {
  using namespace ScopeAppNS;
  char ch[8];
  snprintf(ch, sizeof(ch), "%s", ScopeSources::QuickLabel(set_.quick_idx));
  const char *texts[F_COUNT] = {ch, time_text, volt_text, trig_text};
  int len[F_COUNT], total = F_COUNT - 1;
  for (int i = 0; i < F_COUNT; ++i) total += (len[i] = (int)strlen(texts[i]));
  if (total > 21) {
    const int cut = len[F_CH] - (total - 21);
    len[F_CH] = cut < 3 ? 3 : cut;
    ch[len[F_CH]] = 0;
  }
  int x = 1;
  for (int i = 0; i < F_COUNT; ++i) {
    graphics.setPrintPos(x, 1);
    graphics.print(texts[i]);
    if (focus_ == i && !picker_) graphics.invertRect(x - 1, 0, len[i] * 6 + 1, 9);
    x += (len[i] + 1) * 6;
  }
  if (frozen_) {  // a freeze is stated, not hinted
    graphics.setPrintPos(128 - 6 * 3, kTop + 1);
    graphics.print("FRZ");
  }
}

FLASHMEM void AppScope::RenderPicker() const {
  using namespace ScopeSources;
  Source tmp[kMaxTraces];
  graphics.setPrintPos(1, 1);
  graphics.printf("Custom %d/%d", MaskToList(set_.custom_mask, tmp), kMaxTraces);
  graphics.setPrintPos(127, 1);
  graphics.print_right("L:done");
  const Mask avail = AvailableMask(ScopeAppNS::kCodec);
  for (int s = 0; s < SRC_COUNT; ++s) {
    const int col = s % 4, row = s / 4;
    const int x = 1 + col * 32, y = 11 + row * 9;
    char lab[8];
    Label((Source)s, lab, sizeof(lab));
    graphics.setPrintPos(x, y);
    if (!(avail & ((Mask)1u << s))) { graphics.print(" -"); continue; }
    graphics.print((set_.custom_mask & ((Mask)1u << s)) ? "*" : " ");
    graphics.print(lab);
    if (s == picker_cursor_) graphics.invertRect(x - 1, y - 1, 31, 9);
  }
}

FLASHMEM void AppScope::RenderSpectrum() const {
  using namespace ScopeAppNS;
  Arena &ar = *g_arena;  // Render() checked it
  const int ts = TrigSlot();
  char pk[12] = "pk --";
  const char *none = "";
  if (ts >= 0) {
    const ScopeRing &r = ScopeCapture::g_rings[ts];
    const uint32_t c = r.Committed();
    const Source s = list_[ts];
    const float fs = (KindOf(s) == KIND_CV_IN || KindOf(s) == KIND_CV_OUT) ? kCvFsMv : kAudioFsMv;
    const float rate = RateHz(s);
    if (c >= (uint32_t)ScopeFft::kN && r.Decim() == 1) {
      for (int i = 0; i < ScopeFft::kN; ++i)
        ar.fft_in[i] = ToMv(s, r.At(c - ScopeFft::kN + i).lo) / fs;
      if (KindOf(s) == KIND_CV_IN || KindOf(s) == KIND_CV_OUT) {  // DC would swamp bin 0..1
        float m = 0;
        for (int i = 0; i < ScopeFft::kN; ++i) m += ar.fft_in[i];
        m /= ScopeFft::kN;
        for (int i = 0; i < ScopeFft::kN; ++i) ar.fft_in[i] -= m;
      }
      ScopeFft::MagnitudeDb(ar.fft_in, ar.fft_db, ar.fft);
      peak_hz_ = ScopeFft::PeakHz(ar.fft_db, rate);
      uint8_t rows[kCols];
      ScopeFft::Columns(ar.fft_db, rate, kPlotH, rows);
      for (int x = 0; x < kCols; ++x)
        if (rows[x]) graphics.drawVLine(x, 64 - rows[x], rows[x]);
      const float grid[3] = {100.f, 1000.f, 10000.f};
      for (float g : grid) {
        if (g >= rate / 2) continue;
        graphics.drawVLinePattern(ScopeFft::ColumnForHz(g, rate), kTop, kPlotH, 0x55);
      }
      if (peak_hz_ >= 1000.f) snprintf(pk, sizeof(pk), "pk %.1fk", peak_hz_ / 1000.f);
      else snprintf(pk, sizeof(pk), "pk %dHz", (int)lroundf(peak_hz_));
    }
  }
  // spectrum top line: channel, peak, and the analysed source
  DrawHeader(none, pk, "FFT");
}

FLASHMEM void AppScope::Render() const {
  using namespace ScopeAppNS;
  if (picker_) { RenderPicker(); return; }
  if (!g_arena) {
    if (g_open) {  // a failed allocation, not a closed Scope (the screensaver draws too)
      DrawHeader("", "", "");
      graphics.setPrintPos(1, 26);
      graphics.print("not enough memory");
      graphics.setPrintPos(1, 36);
      graphics.printf("need %d KB free", (int)((sizeof(Arena) + 1023) / 1024));
    }
    return;
  }
  Arena &ar = *g_arena;
  if (n_ == 0) {
    DrawHeader("", "", "");
    graphics.setPrintPos(1, 30);
    graphics.print("no sources: encR push");
    return;
  }
  if (set_.view == 1 && !TrigIsTr()) { RenderSpectrum(); return; }

  const int time_idx = armed_time_idx_ >= 0 ? armed_time_idx_ : TimeIdx();
  const uint32_t now_ms = millis();
  const float col_s = ScopeRange::ColSeconds(time_idx);

  // --- one snapshot of every ring, taken together ---------------------------
  // Each ring's newest bucket count and timestamp are read as a pair, all
  // before any analysis, so a block that lands mid-draw cannot shift one
  // trace against another.
  Snap snap[ScopeSources::kMaxTraces];
  for (int t = 0; t < n_; ++t) {
    const ScopeRing &r = ScopeCapture::g_rings[t];
    uint32_t cyc, c;
    do { cyc = r.commit_cyc; c = r.Committed(); } while (cyc != r.commit_cyc);
    snap[t].c = c;
    snap[t].newest_cyc = cyc - LatencyCyc(list_[t]);
    snap[t].bucket_s = r.Decim() / RateHz(list_[t]);
  }
  const uint32_t now_cyc = ARM_DWT_CYCCNT, now_us = micros();

  // --- the trigger source's recent buckets, in mV ---------------------------
  const int ts = TrigSlot();
  const Source tsrc = list_[ts];
  const ScopeRing &tr = ScopeCapture::g_rings[ts];
  const Snap &tsn = snap[ts];
  const int window = ScopeRange::WindowBuckets(col_s, tsn.bucket_s);
  const int m = (int)(tsn.c < (uint32_t)kAnalyse ? tsn.c : (uint32_t)kAnalyse);
  const uint32_t base = tsn.c - (uint32_t)m;
  float lo = 0, hi = 0;
  int idx = -1;
  locked_ = false;
  if (m > 0) {
    for (int i = 0; i < m; ++i) {
      const ScopeBucket b = tr.At(base + i);
      ar.lo[i] = ToMv(tsrc, b.lo);
      ar.hi[i] = ToMv(tsrc, b.hi);
      ar.mv[i] = 0.5f * (ar.lo[i] + ar.hi[i]);
    }
    lo = ar.lo[0];
    hi = ar.hi[0];
    for (int i = 0; i < m; ++i) { lo = fminf(lo, ar.lo[i]); hi = fmaxf(hi, ar.hi[i]); }
  }
  float period_s = 0.f;
  if (m >= window) {
    if (set_.trig_mode == TM_AUTO) {
      idx = atrig_.Find(ar.mv, m, window, ar.acorr, (int)(sizeof(ar.acorr) / sizeof(ar.acorr[0])), kMinPpMv);
      locked_ = atrig_.Locked();
      if (locked_) period_s = atrig_.Period() * tsn.bucket_s;
    } else if (set_.trig_mode == TM_LEVEL) {
      if (level_follow_) live_level_mv_ = (int16_t)(0.5f * (lo + hi));
      idx = ScopeTrigger::LevelTrigger(ar.mv, m, window, LevelMv(), 0.02f * (hi - lo), true);
      locked_ = idx >= 0;
      period_s = ScopeTrigger::EstimatePeriod(ar.mv, m, 2, m / 2, ar.acorr, (int)(sizeof(ar.acorr) / sizeof(ar.acorr[0])), kMinPpMv) * tsn.bucket_s;
    } else {  // external: the newest TR edge that leaves a full window
      const ScopeEdgeRing &e = ScopeCapture::g_edges[set_.trig_mode - TM_EXT];
      const uint32_t ne = e.Count();
      // the source's newest sample, and each edge, as ages back from now
      const float src_age_s = (float)(int32_t)(now_cyc - tsn.newest_cyc) / (float)F_CPU_ACTUAL;
      for (uint32_t k = ne, seen = 0; k > 0 && seen < (uint32_t)ScopeEdgeRing::kLen; --k, ++seen) {
        const float edge_age_s = (now_us - e.At(k - 1)) * 1e-6f;
        const float behind_s = edge_age_s - src_age_s;
        if (behind_s < 0) continue;
        const int i = m - 1 - (int)lroundf(behind_s / tsn.bucket_s);
        if (i < 0) break;
        if (i + window <= m) { idx = i; locked_ = true; break; }
      }
      uint32_t times[16];
      const uint32_t take = ne < 16 ? ne : 16;
      for (uint32_t k = 0; k < take; ++k) times[k] = e.At(ne - take + k);
      period_s = ScopeTrigger::EdgePeriod(times, (int)take) * 1e-6f;
    }
    if (idx < 0) idx = m - window;  // free-running
  }
  // Auto timebase, in every trigger mode: from the period when there is one,
  // otherwise towards whichever way the buckets say the signal lies.
  if (set_.time_auto && !frozen_ && m > 0) {
    const int dir = period_s > 0 ? 0 : ScopeRange::NoPeriodDir(ar.lo, ar.hi, m, kMinPpMv);
    if (atime_.Update(period_s, now_ms, dir) != time_idx) pending_time_idx_ = atime_.Idx();
  }
  // first column's bucket number on the source ring; before a full screen
  // has arrived, the newest data simply sits at the right
  const int start = idx >= 0 ? (int)(base + idx) : (int)tsn.c - window;
  const float back_s = ((float)tsn.c - 1.f - (float)start) * tsn.bucket_s;

  // --- header -----------------------------------------------------------
  char ttext[8], vtext[8], gtext[8];
  ScopeRange::FormatTime(time_idx, ttext, sizeof(ttext));
  if (set_.time_auto) strncat(ttext, "A", sizeof(ttext) - strlen(ttext) - 1);
  const bool stacked = set_.stacked && n_ > 1;
  if (stacked) snprintf(vtext, sizeof(vtext), "%s", set_.volt_auto ? "lanesA" : "lanes");
  else {
    ScopeRange::FormatVolt(VoltIdx(0), vtext, sizeof(vtext));
    if (set_.volt_auto) strncat(vtext, "A", sizeof(vtext) - strlen(vtext) - 1);
  }
  if (set_.trig_mode == TM_AUTO) {
    if (n_ == 1) snprintf(gtext, sizeof(gtext), "%s", locked_ ? "AUTO" : "FREE");
    else snprintf(gtext, sizeof(gtext), "%c%d", locked_ ? 'A' : 'F', ts + 1);
  } else if (set_.trig_mode == TM_LEVEL) {
    snprintf(gtext, sizeof(gtext), "%+.1fV", LevelMv() / 1000.f);
  } else {
    snprintf(gtext, sizeof(gtext), "TR%d", set_.trig_mode - TM_EXT + 1);
  }
  DrawHeader(gtext, ttext, vtext);
  if (tsn.c == 0) {
    graphics.setPrintPos(1, 30);
    graphics.print(frozen_ ? "frozen, no data" : "starting...");
    return;
  }

  // --- traces -----------------------------------------------------------
  // Every trace is drawn by time: column x is col_s * x after the first
  // column, and each trace finds that moment in its own ring. Overlaid
  // traces share trace 0's AutoVolt, fed with the union of their ranges; a
  // stacked lane uses its own.
  const int lanes = stacked ? n_ : 1;
  const int lane_h = kPlotH / lanes;
  const bool chips = stacked && lane_h >= 9;
  const int x0 = chips ? 8 : 0, x1 = chips ? 100 : kCols;
  int st[ScopeSources::kMaxTraces];
  for (int t = 0; t < n_; ++t) {
    if (t == ts) { st[t] = start; continue; }
    const float ahead_s = (float)(int32_t)(tsn.newest_cyc - snap[t].newest_cyc) / (float)F_CPU_ACTUAL;
    st[t] = ScopeRange::AlignBucket(snap[t].c, snap[t].bucket_s, back_s - ahead_s);
  }
  // A column's min and max over the buckets it covers on trace t.
  auto column = [&](int t, int x, float *clo, float *chi) -> bool {
    const ScopeRing &r = ScopeCapture::g_rings[t];
    const Snap &sn = snap[t];
    const int b0 = ScopeRange::BucketForColumn(x - x0, col_s, sn.bucket_s);
    int b1 = ScopeRange::BucketForColumn(x - x0 + 1, col_s, sn.bucket_s);
    if (b1 <= b0) b1 = b0 + 1;
    bool any = false;
    for (int b = b0; b < b1; ++b) {
      const int64_t bn = (int64_t)st[t] + b;
      if (bn < 0 || bn >= (int64_t)sn.c || bn < (int64_t)sn.c - (ScopeRing::kLen - 64)) continue;
      const ScopeBucket k = r.At((uint32_t)bn);
      const float l = ToMv(list_[t], k.lo), h = ToMv(list_[t], k.hi);
      if (!any) { *clo = l; *chi = h; any = true; }
      else { *clo = fminf(*clo, l); *chi = fmaxf(*chi, h); }
    }
    return any;
  };

  float u_lo = 1e9f, u_hi = -1e9f;
  for (int t = 0; t < n_; ++t) {
    const ScopeRing &r = ScopeCapture::g_rings[t];
    if (snap[t].c) last_mv_[t] = ToMv(list_[t], r.At(snap[t].c - 1).hi);
    if (KindOf(list_[t]) == KIND_TR) continue;
    float tlo = 1e9f, thi = -1e9f;
    for (int x = x0; x < x1; ++x) {
      float l, h;
      if (column(t, x, &l, &h)) { tlo = fminf(tlo, l); thi = fmaxf(thi, h); }
    }
    if (thi < tlo) continue;
    if (stacked) { if (!frozen_) avolt_[t].Update((int32_t)tlo, (int32_t)thi, now_ms); }
    else { u_lo = fminf(u_lo, tlo); u_hi = fmaxf(u_hi, thi); }
  }
  if (!stacked && u_hi >= u_lo && !frozen_) avolt_[0].Update((int32_t)u_lo, (int32_t)u_hi, now_ms);

  // grid: centre line and divisions (one-trace and overlaid views)
  if (!stacked) {
    graphics.drawHLinePattern(0, kTop + kPlotH / 2, kCols, 3);
    for (int x = ScopeRange::kColsPerDiv; x < kCols; x += ScopeRange::kColsPerDiv)
      graphics.drawVLinePattern(x, kTop, kPlotH, 0x11);
  }

  static const uint8_t kStyle[4] = {0xFF, 0x55, 0xCF, 0x11};  // solid, dotted, dashed, sparse
  for (int t = 0; t < n_; ++t) {
    const bool is_tr = KindOf(list_[t]) == KIND_TR;
    const int lane = stacked ? t : 0;
    const int ytop = kTop + lane * lane_h, yh = stacked ? lane_h - 1 : kPlotH;
    const float vdiv = ScopeRange::kVoltDivMv[stacked ? VoltIdx(t) : VoltIdx(0)];
    const float center = stacked ? avolt_[t].CenterMv() : avolt_[0].CenterMv();
    const float vscale = yh / (ScopeRange::kDivsY * vdiv);
    const uint8_t style = stacked ? 0xFF : kStyle[t & 3];
    auto row = [&](float mv) {
      int y;
      if (is_tr) {  // a 1-bit lane: low at the bottom, high a few rows up
        const int top = stacked ? ytop + 1 : 64 - 6;
        y = mv > 0.5f ? top : (stacked ? ytop + yh - 1 : 63);
      } else {
        y = ytop + yh / 2 - (int)lroundf((mv - center) * vscale);
      }
      return y < ytop ? ytop : y > ytop + yh - 1 ? ytop + yh - 1 : y;
    };
    int prev_y = -1, prev_x = -1;
    for (int x = x0; x < x1; ++x) {
      float l, h;
      if (!column(t, x, &l, &h)) { prev_y = -1; continue; }
      if (!(style & (1u << (x & 7)))) { prev_y = -1; continue; }
      const int ylo = row(l), yhi = row(h), ym = (ylo + yhi) / 2;
      if (ylo != yhi) graphics.drawVLine(x, yhi, ylo - yhi + 1);
      if (prev_y >= 0) graphics.drawLine(prev_x, prev_y, x, ym);
      else graphics.setPixel(x, ym);
      prev_x = x;
      prev_y = ym;
    }
    char lab[8];
    if (stacked) {
      if (t < n_ - 1) graphics.drawHLinePattern(0, ytop + lane_h - 1, kCols, 2);
      if (chips) {
        graphics.setPrintPos(1, ytop + (lane_h - 8) / 2);
        graphics.print((char)('1' + t));
        graphics.invertRect(0, ytop + (lane_h - 8) / 2 - 1, 7, 9);
        if (is_tr) snprintf(lab, sizeof(lab), "%s", last_mv_[t] > 0.5f ? "HI" : "LO");
        else snprintf(lab, sizeof(lab), "%.1fV", last_mv_[t] / 1000.f);
        graphics.setPrintPos(127, ytop + (lane_h - 8) / 2);
        graphics.print_right(lab);
      }
    } else if (n_ > 1 && prev_y >= 0) {  // overlaid: the trace number at its right end
      int y = prev_y - 4;
      y = y < kTop ? kTop : y > 56 ? 56 : y;
      graphics.setPrintPos(122, y);
      graphics.print((char)('1' + t));
    }
  }
  // trigger level tick at the left edge
  if (!stacked && (set_.trig_mode == TM_LEVEL || (set_.trig_mode == TM_AUTO && locked_))) {
    const float lvl = set_.trig_mode == TM_LEVEL ? LevelMv() : 0.5f * (lo + hi);
    const float vdiv = ScopeRange::kVoltDivMv[VoltIdx(0)];
    int y = kTop + kPlotH / 2 - (int)lroundf((lvl - avolt_[0].CenterMv()) * kPlotH / (ScopeRange::kDivsY * vdiv));
    y = y < kTop ? kTop : y > 63 ? 63 : y;
    graphics.drawHLine(0, y, 3);
  }
  if (ScopeSources::IsAc(tsrc) && !stacked) {
    graphics.setPrintPos(1, 56);
    graphics.print("AC");
  }
}

FLASHMEM void AppScope::DrawMenu() const { Render(); }

FLASHMEM void AppScope::DrawScreensaver() const {
  if (set_.screensaver_trace) Render();
}
