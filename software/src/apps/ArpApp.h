#pragma once
// Arpeggiator: take a held MIDI chord and play it out as a figure.
//
// Note ORDER lives in src/ArpEngine.h, which is pure and host-tested
// (test/test_arp_engine.cpp, 1043 checks). This file is the instrument around
// it: where the held notes come from, what advances a step, how long the gate
// stays up, and where the result goes.
//
// Three deliberate re-uses of machinery that already exists here, rather than
// new mechanisms:
//
//  * Held notes come from HSIOFrame's NoteBuffer (HSMIDITypes.h), which
//    already tracks every held note per channel for the whole instrument. An
//    arpeggiator that kept its own note list would be a second source of
//    truth for the same thing.
//  * The step clock is ONE DigitalInputMap. That single control already
//    spans the internal clock, any TR jack and MIDI, and carries clock
//    division/multiplication and Euclidean gating of its own -- which is the
//    whole "internal / external / trigger" requirement in one assignable row
//    instead of a mode switch and three code paths.
//  * Modulatable parameters are CVInputMaps, so transpose, octaves, skip
//    chance and gate length can be driven from a CV jack, a DAC output, a
//    MIDI CC (with auto-learn) or an internal LFO, each with an attenuverter.

#include "../HSApplication.h"
#include "../HSMIDI.h"
#include "../HSClockManager.h"
#include "../ArpEngine.h"

namespace ArpAppNS {

enum Page : uint8_t { PAGE_MAIN, PAGE_CHANCE, PAGE_IO, PAGE_CV, PAGE_COUNT };

enum MainRow : uint8_t {
  M_ORDER, M_OCTAVES, M_OCTMODE, M_GATE, M_TRANSPOSE, M_LATCH, M_COUNT
};
enum ChanceRow : uint8_t {
  C_SKIP, C_RATCHET_PCT, C_RATCHET_MAX, C_VELOCITY, C_COUNT
};
enum IoRow : uint8_t {
  I_CLOCK, I_RESET, I_MIDI_IN, I_MIDI_OUT, I_PITCH_OUT, I_GATE_OUT, I_COUNT
};
enum CvRow : uint8_t { V_TRANSPOSE, V_OCTAVES, V_SKIP, V_GATE, V_COUNT };

static const char* const order_names[arp::ORDER_COUNT] = {
  "Up", "Down", "Up-Down", "Down-Up", "Up-Down+", "Down-Up+",
  "Converge", "Diverge", "As Played", "Random"
};
static const char* const oct_names[arp::OCT_COUNT] = {
  "Oct Up", "Oct Down", "Oct U-D", "Per Note"
};

}  // namespace ArpAppNS

OC_APP_CLASS(AppArp, TWOCCS("AR"), "Arpeggiator", "Arp"),
  public HSApplication {
public:
  // order,oct,octmode,gate,transpose,latch,skip,rpct,rmax,vel,midi_in,
  // midi_out,pitch,gate_ch = 14 bytes, plus 4 CVInputMaps and 2
  // DigitalInputMaps packed by their own Size constants.
  OC_APP_INTERFACE_DECLARE(AppArp, 48);

  // BaseController() is the only place HS::frame.Load() happens, and every
  // CVInputMap/DigitalInputMap reads HS::frame. Skipping HSApplication would
  // freeze all modulation at whatever the previous app left behind, silently.
  void Start() final {}
  void Resume() final {}
  void Controller() final;
  void View() const final { DrawMenu(); }

private:
  arp::Engine engine_;
  arp::Params params_;

  // Assignable step clock and pattern reset.
  DigitalInputMap clock_in_;
  DigitalInputMap reset_in_;

  // Modulation. Each is summed with its knob value, not replacing it, so a
  // patched CV offsets the setting rather than overriding it.
  CVInputMap transpose_cv_, octaves_cv_, skip_cv_, gate_cv_;

  uint8_t midi_in_ch_ = 0;     // 0 = omni, else channel 1..16
  uint8_t midi_out_ch_ = 1;    // 0 = off
  uint8_t pitch_out_ = 0;      // DAC channel for pitch CV
  uint8_t gate_out_ = 1;       // DAC channel for the gate
  uint8_t gate_pct_ = 50;      // of the measured step interval
  bool    latch_ = false;

  // --- runtime ---
  arp::Note held_[arp::kMaxNotes];
  uint8_t  held_count_ = 0;
  arp::Note latched_[arp::kMaxNotes];
  uint8_t  latched_count_ = 0;

  int8_t   sounding_ = -1;     // MIDI note currently on, -1 when silent
  uint32_t step_tick_ = 0;     // CORE tick of the last step
  uint32_t interval_ = 0;      // measured ticks between steps
  uint32_t gate_off_tick_ = 0;
  uint8_t  repeats_left_ = 0;  // remaining ratchet sub-gates
  uint32_t sub_period_ = 0;
  arp::Step cur_{true, 0, 0, 1};

  uint8_t page_ = ArpAppNS::PAGE_MAIN;
  uint8_t cursor_[ArpAppNS::PAGE_COUNT] = {0, 0, 0, 0};
  bool    editing_ = false;

  uint8_t Cursor() const { return cursor_[page_]; }
  uint8_t RowCount() const;

  void ClaimMidiChannel();
  template <typename Dev> void PumpMidiIn(Dev &device);
  void GatherHeld();
  void DoStep();
  void NoteOff();
  void NoteOn(uint8_t pitch, uint8_t vel);
  arp::Params EffectiveParams() const;

  void AdjustRow(int dir);
  void DrawMain() const;
  void DrawChance() const;
  void DrawIo() const;
  void DrawCv() const;
};

// ---------------------------------------------------------------------------
// Only Controller() runs per tick; it is left out of FLASHMEM. Everything
// else is drawing or human-rate input and is cold enough for flash. Same
// discipline as DelayApp.h.
// ---------------------------------------------------------------------------

FLASHMEM void AppArp::Init() {
  params_ = arp::Params();
  engine_.Reset();
  clock_in_.SetClockSource(0);     // internal clock by default
  midi_in_ch_ = 0;
  midi_out_ch_ = 1;
  pitch_out_ = 0;
  gate_out_ = 1;
  gate_pct_ = 50;
  latch_ = false;
  page_ = ArpAppNS::PAGE_MAIN;
  for (auto& c : cursor_) c = 0;
  editing_ = false;
  held_count_ = latched_count_ = 0;
  sounding_ = -1;
}

// Sum knob and CV, then clamp. Additive rather than absolute so an unpatched
// input leaves the knob alone and a patched one offsets it.
FLASHMEM arp::Params AppArp::EffectiveParams() const {
  arp::Params p = params_;
  const int t = p.transpose + transpose_cv_.SemitoneIn(0);
  p.transpose = (int8_t)(t < -48 ? -48 : (t > 48 ? 48 : t));
  int o = p.octaves + (octaves_cv_.In(0) / (12 << 7));
  p.octaves = (uint8_t)(o < 1 ? 1 : (o > arp::kMaxOct ? arp::kMaxOct : o));
  int s = p.skip_pct + (int)(100.0f * skip_cv_.InF(0.0f));
  p.skip_pct = (uint8_t)(s < 0 ? 0 : (s > 100 ? 100 : s));
  return p;
}

// Ask the MIDI layer to actually keep the notes we intend to read.
//
// note_buffer only accumulates on channels something has registered interest
// in: MonoBufferPush() is gated on CheckMidiChannelFilter(), and that filter
// is rebuilt (UpdateMidiChannelFilter) purely from Captain's MIDI map. An app
// that merely READS the buffer therefore sees nothing at all -- which is
// exactly what the first hardware test showed, a chord held on the host and
// "0 nt" on screen.
//
// Asserted every tick rather than once on resume: it is two instructions, it
// is idempotent, and it survives UpdateMidiChannelFilter() being called from
// anywhere else while this app is in front.
void AppArp::ClaimMidiChannel() {
  if (midi_in_ch_ == 0) HS::frame.MIDIState.any_channel_omni = true;
  else HS::frame.MIDIState.midi_channel_filter |= (uint16_t)(1u << (midi_in_ch_ - 1));
}

void AppArp::GatherHeld() {
  held_count_ = 0;
  auto take = [&](uint8_t ch) {
    const auto& nb = HS::frame.MIDIState.note_buffer[ch];
    for (uint8_t i = 0; i < nb.size() && held_count_ < arp::kMaxNotes; ++i) {
      held_[held_count_].pitch = nb.data_[i].note;
      held_[held_count_].velocity = nb.data_[i].vel;
      ++held_count_;
    }
  };
  if (midi_in_ch_ == 0) { for (uint8_t c = 0; c < 16; ++c) take(c); }
  else take((uint8_t)(midi_in_ch_ - 1));

  if (held_count_ > 0) {
    // Remember the chord while it is held so LATCH has something to keep
    // playing once the keys are released.
    for (uint8_t i = 0; i < held_count_; ++i) latched_[i] = held_[i];
    latched_count_ = held_count_;
  }
}

void AppArp::NoteOff() {
  if (sounding_ < 0) return;
  if (midi_out_ch_) HS::frame.MIDIState.SendNoteOff((uint8_t)(midi_out_ch_ - 1),
                                                    (uint8_t)sounding_, 0);
  GateOut(gate_out_, false);
  sounding_ = -1;
}

void AppArp::NoteOn(uint8_t pitch, uint8_t vel) {
  NoteOff();
  if (midi_out_ch_) HS::frame.MIDIState.SendNoteOn((uint8_t)(midi_out_ch_ - 1), pitch, vel);
  Out(pitch_out_, MIDIQuantizer::CV(pitch));
  GateOut(gate_out_, true);
  sounding_ = (int8_t)pitch;
}

void AppArp::DoStep() {
  const uint32_t now = OC::CORE::ticks;
  // Measure the step interval rather than deriving it from tempo: the clock
  // can be a trigger jack at any rate, and gate length and ratchet spacing
  // both have to follow whatever is actually arriving.
  if (step_tick_) {
    const uint32_t d = now - step_tick_;
    interval_ = interval_ ? (interval_ * 3 + d) / 4 : d;   // light smoothing
  }
  step_tick_ = now;

  const arp::Params p = EffectiveParams();
  const arp::Note* src = held_count_ ? held_ : (latch_ ? latched_ : nullptr);
  const uint8_t n = held_count_ ? held_count_ : (latch_ ? latched_count_ : 0);
  if (n == 0) { NoteOff(); repeats_left_ = 0; return; }

  cur_ = engine_.Next(src, n, p);
  repeats_left_ = cur_.rest ? 0 : cur_.repeats;
  sub_period_ = (cur_.repeats > 1 && interval_) ? interval_ / cur_.repeats : interval_;

  if (cur_.rest) { NoteOff(); return; }
  NoteOn(cur_.pitch, cur_.velocity);
  --repeats_left_;

  int g = gate_pct_ + (int)(100.0f * gate_cv_.InF(0.0f));
  if (g < 5) g = 5; if (g > 100) g = 100;
  const uint32_t span = sub_period_ ? sub_period_ : 1;
  gate_off_tick_ = now + (span * (uint32_t)g) / 100u;
}

void AppArp::Controller() {
  // Nothing else pumps the internal clock unless an app does it, and this
  // app's default clock source IS the internal clock. Same reason Delay and
  // Calibr8or both call it.
  ClockSetup_instance.Controller();

  ClaimMidiChannel();
  GatherHeld();

  if (reset_in_.Clock()) { engine_.Reset(); step_tick_ = 0; }

  if (clock_in_.Clock()) {
    DoStep();
  } else if (repeats_left_ > 0 && interval_ &&
             OC::CORE::ticks - step_tick_ >=
                 (uint32_t)(cur_.repeats - repeats_left_) * sub_period_) {
    // A ratchet: re-trigger the same pitch inside the step.
    NoteOn(cur_.pitch, cur_.velocity);
    --repeats_left_;
    int g = gate_pct_ + (int)(100.0f * gate_cv_.InF(0.0f));
    if (g < 5) g = 5; if (g > 100) g = 100;
    gate_off_tick_ = OC::CORE::ticks + (sub_period_ * (uint32_t)g) / 100u;
  }

  if (sounding_ >= 0 && gate_off_tick_ && OC::CORE::ticks >= gate_off_tick_) NoteOff();

  // Releasing the keys with LATCH off stops the arpeggio; with it on the
  // remembered chord keeps playing.
  if (held_count_ == 0 && !latch_ && sounding_ >= 0) NoteOff();
}

// Poll the MIDI ports into HS::frame.
//
// There is no global MIDI-in pump in this firmware: ProcessMIDIMsg() is only
// ever called by Quadrants, Captain MIDI and the MidiLoop applet, each of
// which polls the devices itself. A new top-level app therefore receives no
// MIDI whatsoever until it does the same -- which is why the first hardware
// test showed a chord held on the host and "0 nt" on screen even after the
// channel filter was fixed.
//
// In Loop(), NOT Controller(): USBHost_t36 is not ISR-safe and Controller()
// runs in the 16.6 kHz CORE ISR.
//
// Thru is deliberately not duplicated here. Quadrants fans received messages
// back out to the other ports; doing that from this app as well would be a
// second, differently-configured thru path for the same traffic.
template <typename Dev>
void AppArp::PumpMidiIn(Dev &device) {
  HS::IOFrame &f = HS::frame;
  uint8_t mask = 0;
  if ((void*)&device == (void*)&usbMIDI) mask = mMaskUSBDev;
  else if ((void*)&device == (void*)&usbHostMIDI[0]) mask = mMaskUSBHost;
  else if ((void*)&device == (void*)&usbHostMIDI[1]) mask = mMaskUSBHost2;
  else if ((void*)&device == (void*)&MIDI1) mask = mMaskSerial;
  const bool clkrx = ~midi_clkrx_disable & mask;
  const bool msgrx = ~midi_msgrx_disable & mask;

  int budget = 60;   // same bound Quadrants uses: never spin on a flood
  while (budget-- > 0 && device.read()) {
    const MIDIMessage msg = { device.getChannel(), device.getType(),
                              device.getData1(), device.getData2() };
    if (msg.message == midi::SystemExclusive) continue;
    if (msg.message == midi::Clock || msg.message == midi::Start
        || msg.message == midi::Stop) {
      if (clkrx) f.MIDIState.ProcessMIDIMsg(msg);
    } else if (msgrx && (msg.message >> 4) != 0xF) {
      f.MIDIState.ProcessMIDIMsg(msg);
    }
  }
}

FLASHMEM void AppArp::Loop() {
  PumpMidiIn(usbMIDI);
  PumpMidiIn(usbHostMIDI[0]);
  PumpMidiIn(usbHostMIDI[1]);
  PumpMidiIn(MIDI1);
}
void AppArp::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

FLASHMEM void AppArp::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_SUSPEND:
    case OC::APP_EVENT_SCREENSAVER_ON:
      break;
    default: break;
  }
  // A hanging note outlives the app that made it, so make sure it cannot.
  // Same for the channel claim: put the filter back to whatever the MIDI maps
  // actually say, rather than leaving the instrument in omni because this app
  // was open once.
  if (event == OC::APP_EVENT_SUSPEND) {
    NoteOff();
    HS::frame.MIDIState.UpdateMidiChannelFilter();
  }
}

FLASHMEM size_t AppArp::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  stream_buffer.Write<uint8_t>(params_.order);
  stream_buffer.Write<uint8_t>(params_.octaves);
  stream_buffer.Write<uint8_t>(params_.oct_mode);
  stream_buffer.Write<int8_t>(params_.transpose);
  stream_buffer.Write<uint8_t>(params_.skip_pct);
  stream_buffer.Write<uint8_t>(params_.ratchet_max);
  stream_buffer.Write<uint8_t>(params_.ratchet_pct);
  stream_buffer.Write<uint8_t>(params_.vel_fixed);
  stream_buffer.Write<uint8_t>(gate_pct_);
  stream_buffer.Write<uint8_t>(latch_ ? 1 : 0);
  stream_buffer.Write<uint8_t>(midi_in_ch_);
  stream_buffer.Write<uint8_t>(midi_out_ch_);
  stream_buffer.Write<uint8_t>(pitch_out_);
  stream_buffer.Write<uint8_t>(gate_out_);
  return stream_buffer.overflow() ? 0 : stream_buffer.written();
}

FLASHMEM size_t AppArp::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  const uint8_t order = stream_buffer.Read<uint8_t>();
  const uint8_t oct = stream_buffer.Read<uint8_t>();
  const uint8_t omode = stream_buffer.Read<uint8_t>();
  const int8_t tr = stream_buffer.Read<int8_t>();
  const uint8_t skip = stream_buffer.Read<uint8_t>();
  const uint8_t rmax = stream_buffer.Read<uint8_t>();
  const uint8_t rpct = stream_buffer.Read<uint8_t>();
  const uint8_t vel = stream_buffer.Read<uint8_t>();
  const uint8_t gate = stream_buffer.Read<uint8_t>();
  const uint8_t latch = stream_buffer.Read<uint8_t>();
  const uint8_t min = stream_buffer.Read<uint8_t>();
  const uint8_t mout = stream_buffer.Read<uint8_t>();
  const uint8_t pout = stream_buffer.Read<uint8_t>();
  const uint8_t gout = stream_buffer.Read<uint8_t>();
  // A short read returns zeros, and zeros here look like a valid config
  // (octaves 0, gate 0) that would leave the app mute with nothing on screen
  // to explain it. Keep the defaults instead.
  if (stream_buffer.underflow()) return 0;
  params_.order = order < arp::ORDER_COUNT ? order : arp::ORDER_UP;
  params_.octaves = (oct >= 1 && oct <= arp::kMaxOct) ? oct : 1;
  params_.oct_mode = omode < arp::OCT_COUNT ? omode : arp::OCT_UP;
  params_.transpose = tr;
  params_.skip_pct = skip <= 100 ? skip : 0;
  params_.ratchet_max = (rmax >= 1 && rmax <= 4) ? rmax : 1;
  params_.ratchet_pct = rpct <= 100 ? rpct : 0;
  params_.vel_fixed = vel;
  gate_pct_ = (gate >= 5 && gate <= 100) ? gate : 50;
  latch_ = latch != 0;
  midi_in_ch_ = min <= 16 ? min : 0;
  midi_out_ch_ = mout <= 16 ? mout : 1;
  pitch_out_ = pout < DAC_CHANNEL_COUNT ? pout : 0;
  gate_out_ = gout < DAC_CHANNEL_COUNT ? gout : 1;
  return stream_buffer.read();
}

// --- draw ------------------------------------------------------------------

FLASHMEM uint8_t AppArp::RowCount() const {
  using namespace ArpAppNS;
  switch (page_) {
    case PAGE_MAIN: return M_COUNT;
    case PAGE_CHANCE: return C_COUNT;
    case PAGE_IO: return I_COUNT;
    default: return V_COUNT;
  }
}

FLASHMEM void AppArp::DrawMain() const {
  using namespace ArpAppNS;
  const char* rows[M_COUNT] = {"Order","Octaves","Oct mode","Gate","Transp","Latch"};
  for (uint8_t r = 0; r < M_COUNT; ++r) {
    const int y = 14 + r * 9;
    graphics.setPrintPos(2, y);
    graphics.print(rows[r]);
    graphics.setPrintPos(66, y);
    switch (r) {
      case M_ORDER:     graphics.print(order_names[params_.order]); break;
      case M_OCTAVES:   graphics.print(params_.octaves); break;
      case M_OCTMODE:   graphics.print(oct_names[params_.oct_mode]); break;
      case M_GATE:      graphics.printf("%d%%", gate_pct_); break;
      case M_TRANSPOSE: graphics.printf("%+d", params_.transpose); break;
      case M_LATCH:     graphics.print(latch_ ? "on" : "off"); break;
    }
    if (Cursor() == r) graphics.drawFrame(0, y - 2, 128, 10);
  }
}

FLASHMEM void AppArp::DrawChance() const {
  using namespace ArpAppNS;
  const char* rows[C_COUNT] = {"Skip","Ratchet","Rtc max","Velocity"};
  for (uint8_t r = 0; r < C_COUNT; ++r) {
    const int y = 14 + r * 9;
    graphics.setPrintPos(2, y);
    graphics.print(rows[r]);
    graphics.setPrintPos(66, y);
    switch (r) {
      case C_SKIP:        graphics.printf("%d%%", params_.skip_pct); break;
      case C_RATCHET_PCT: graphics.printf("%d%%", params_.ratchet_pct); break;
      case C_RATCHET_MAX: graphics.print(params_.ratchet_max); break;
      case C_VELOCITY:
        if (params_.vel_fixed) graphics.print(params_.vel_fixed);
        else graphics.print("played");
        break;
    }
    if (Cursor() == r) graphics.drawFrame(0, y - 2, 128, 10);
  }
}

FLASHMEM void AppArp::DrawIo() const {
  using namespace ArpAppNS;
  const char* rows[I_COUNT] = {"Clock","Reset","MIDI in","MIDI out","Pitch","Gate"};
  for (uint8_t r = 0; r < I_COUNT; ++r) {
    const int y = 14 + r * 8;
    graphics.setPrintPos(2, y);
    graphics.print(rows[r]);
    graphics.setPrintPos(60, y);
    switch (r) {
      case I_CLOCK: {
        const int st = clock_in_.div_mult.steps;
        graphics.printf("%s %c%d", clock_in_.InputName(),
                        st > 0 ? '/' : 'x', st > 0 ? st : -st);
        break;
      }
      case I_RESET: graphics.print(reset_in_.InputName()); break;
      case I_MIDI_IN:  if (midi_in_ch_) graphics.print(midi_in_ch_); else graphics.print("omni"); break;
      case I_MIDI_OUT: if (midi_out_ch_) graphics.print(midi_out_ch_); else graphics.print("off"); break;
      case I_PITCH_OUT: graphics.print((char)('A' + pitch_out_)); break;
      case I_GATE_OUT:  graphics.print((char)('A' + gate_out_)); break;
    }
    if (Cursor() == r) graphics.drawFrame(0, y - 2, 128, 9);
  }
}

FLASHMEM void AppArp::DrawCv() const {
  using namespace ArpAppNS;
  const char* rows[V_COUNT] = {"Transp","Octaves","Skip","Gate"};
  const CVInputMap* maps[V_COUNT] = {&transpose_cv_, &octaves_cv_, &skip_cv_, &gate_cv_};
  for (uint8_t r = 0; r < V_COUNT; ++r) {
    const int y = 14 + r * 10;
    graphics.setPrintPos(2, y);
    graphics.print(rows[r]);
    graphics.setPrintPos(60, y);
    graphics.print(maps[r]->InputName());
    if (maps[r]->enabled()) {
      graphics.setPrintPos(104, y);
      graphics.printf("%d", maps[r]->attenuversion * 100 / 60);
    }
    if (Cursor() == r) graphics.drawFrame(0, y - 2, 128, 11);
  }
}

FLASHMEM void AppArp::DrawMenu() const {
  using namespace ArpAppNS;
  const char* titles[PAGE_COUNT] = {"ARP", "CHANCE", "I/O", "CV"};
  graphics.setPrintPos(2, 2);
  graphics.print(titles[page_]);
  // Held-note count and the live step, so it is obvious at a glance whether
  // the app is hearing anything at all -- the first question when an
  // arpeggiator is silent.
  graphics.setPrintPos(78, 2);
  graphics.printf("%d nt", held_count_ ? held_count_ : latched_count_);
  if (sounding_ >= 0) graphics.drawRect(120, 1, 6, 6);
  graphics.drawLine(0, 10, 127, 10);

  switch (page_) {
    case PAGE_MAIN:   DrawMain(); break;
    case PAGE_CHANCE: DrawChance(); break;
    case PAGE_IO:     DrawIo(); break;
    default:          DrawCv(); break;
  }
}

FLASHMEM void AppArp::DrawScreensaver() const {
  graphics.setPrintPos(2, 2);
  graphics.print("A R P");
  graphics.setPrintPos(2, 24);
  graphics.print(ArpAppNS::order_names[params_.order]);
  if (sounding_ >= 0) graphics.drawRect(118, 1, 8, 8);
}

// --- input -----------------------------------------------------------------

FLASHMEM void AppArp::AdjustRow(int dir) {
  using namespace ArpAppNS;
  const uint8_t r = Cursor();
  auto bump = [&](int v, int lo, int hi) { v += dir; return v < lo ? lo : (v > hi ? hi : v); };
  switch (page_) {
    case PAGE_MAIN:
      switch (r) {
        case M_ORDER:     params_.order = (uint8_t)bump(params_.order, 0, arp::ORDER_COUNT - 1); break;
        case M_OCTAVES:   params_.octaves = (uint8_t)bump(params_.octaves, 1, arp::kMaxOct); break;
        case M_OCTMODE:   params_.oct_mode = (uint8_t)bump(params_.oct_mode, 0, arp::OCT_COUNT - 1); break;
        case M_GATE:      gate_pct_ = (uint8_t)bump(gate_pct_, 5, 100); break;
        case M_TRANSPOSE: params_.transpose = (int8_t)bump(params_.transpose, -24, 24); break;
        case M_LATCH:
          latch_ = !latch_;
          if (!latch_) latched_count_ = 0;   // dropping latch drops the memory
          break;
      }
      break;
    case PAGE_CHANCE:
      switch (r) {
        case C_SKIP:        params_.skip_pct = (uint8_t)bump(params_.skip_pct, 0, 100); break;
        case C_RATCHET_PCT: params_.ratchet_pct = (uint8_t)bump(params_.ratchet_pct, 0, 100); break;
        case C_RATCHET_MAX: params_.ratchet_max = (uint8_t)bump(params_.ratchet_max, 1, 4); break;
        case C_VELOCITY:    params_.vel_fixed = (uint8_t)bump(params_.vel_fixed, 0, 127); break;
      }
      break;
    case PAGE_IO:
      switch (r) {
        case I_CLOCK:     clock_in_.ChangeSource(dir); break;
        case I_RESET:     reset_in_.ChangeSource(dir); break;
        case I_MIDI_IN:   midi_in_ch_ = (uint8_t)bump(midi_in_ch_, 0, 16); break;
        case I_MIDI_OUT:  midi_out_ch_ = (uint8_t)bump(midi_out_ch_, 0, 16); break;
        case I_PITCH_OUT: pitch_out_ = (uint8_t)bump(pitch_out_, 0, DAC_CHANNEL_COUNT - 1); break;
        case I_GATE_OUT:  gate_out_ = (uint8_t)bump(gate_out_, 0, DAC_CHANNEL_COUNT - 1); break;
      }
      break;
    default: {
      CVInputMap* maps[V_COUNT] = {&transpose_cv_, &octaves_cv_, &skip_cv_, &gate_cv_};
      maps[r]->ChangeSource(dir);
      break;
    }
  }
}

FLASHMEM void AppArp::HandleButtonEvent(const UI::Event &event) {
  if (event.type != UI::EVENT_BUTTON_PRESS) return;
  switch (event.control) {
    case OC::CONTROL_BUTTON_L:
      page_ = (uint8_t)((page_ + ArpAppNS::PAGE_COUNT - 1) % ArpAppNS::PAGE_COUNT);
      break;
    case OC::CONTROL_BUTTON_R:
      page_ = (uint8_t)((page_ + 1) % ArpAppNS::PAGE_COUNT);
      break;
    case OC::CONTROL_BUTTON_UP:
      // Panic. A stuck note from a lost note-off is the failure an
      // arpeggiator gets blamed for, so make stopping it a single press.
      NoteOff();
      latched_count_ = 0;
      engine_.Reset();
      break;
    case OC::CONTROL_BUTTON_DOWN:
      editing_ = !editing_;
      break;
    default: break;
  }
}

FLASHMEM void AppArp::HandleEncoderEvent(const UI::Event &event) {
  const int dir = event.value > 0 ? 1 : -1;
  if (event.control == OC::CONTROL_ENCODER_L) {
    const uint8_t n = RowCount();
    int c = (int)Cursor() + dir;
    cursor_[page_] = (uint8_t)(c < 0 ? 0 : (c >= n ? n - 1 : c));
  } else {
    AdjustRow(dir);
  }
}

FLASHMEM void AppArp::GetIOConfig(OC::IOConfig &ioconfig) const {
  using namespace OC;
  for (int ch = 0; ch < DAC_CHANNEL_COUNT; ++ch)
    ioconfig.outputs[ch].set("off", OC::OUTPUT_MODE_RAW);
  ioconfig.outputs[pitch_out_].set("arp pitch", OC::OUTPUT_MODE_PITCH);
  ioconfig.outputs[gate_out_].set("arp gate", OC::OUTPUT_MODE_GATE);
}

FLASHMEM void AppArp::DrawDebugInfo() const {
  graphics.setPrintPos(2, 12);
  graphics.printf("held %d", held_count_);
  graphics.setPrintPos(2, 22);
  graphics.printf("pos %d", engine_.position());
  graphics.setPrintPos(2, 32);
  graphics.printf("ivl %lu", (unsigned long)interval_);
}
