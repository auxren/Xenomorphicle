// The Cal Routine app's ports: see CalRoutinePorts.h.
//
// Every function here is FLASHMEM with external linkage (named namespace or
// member functions), so the section attribute holds under LTO; the virtual
// overrides are defined out of class for the same reason.
#include <Arduino.h>

#include "CalRoutinePorts.h"

#include <new>
#include <string.h>

#include "../AdcSlotMap.h"
#include "../CalRoutineMarker.h"
#include "../CvMeterMath.h"
#include "../InputCal.h"
#include "../InputCurveFiles.h"
#include "../OC_ADC.h"
#include "../OC_DAC.h"
#include "../OC_calibration.h"
#include "../OC_config.h"
#include "../OC_global_settings.h"
#include "../OC_gpio.h"
#include "../src/drivers/EEPROMStorage.h"
#include "../CvMeter.h"
#if defined(ARDUINO_TEENSY41) && !defined(XENO_SIM)
#include "../src/drivers/USBTMCDevice.h"
#define CALR_HAVE_METER 1
#endif

static_assert(OCTAVES + 1 == CalRoutineMath::kNodes,
              "the Cal Routine writes 11 DAC table entries per output");
static_assert(EEPROM_CALROUTINE_MARKER_START == CalRoutineMarkerStart &&
              EEPROM_CALROUTINE_MARKER_END - EEPROM_CALROUTINE_MARKER_START == CalRoutineMarkerSize,
              "OC_config.h and CalRoutineMarker.h disagree about the marker");

namespace CalRoutinePorts {

using CvMeterMath::PointStats;

FLASHMEM int PanelInPhys(int label) {
  switch (label) {
    case 0: return ADC_CHANNEL_1;
    case 1: return ADC_CHANNEL_2;
    case 2: return ADC_CHANNEL_3;
    case 3: return ADC_CHANNEL_4;
    case 4: return ADC_CHANNEL_5;
    case 5: return ADC_CHANNEL_6;
    case 6: return ADC_CHANNEL_7;
    default: return ADC_CHANNEL_8;
  }
}

FLASHMEM int PanelOutPhys(int label) {
  switch (label) {
    case 0: return DAC_CHANNEL_A;
    case 1: return DAC_CHANNEL_B;
    case 2: return DAC_CHANNEL_C;
    case 3: return DAC_CHANNEL_D;
    case 4: return DAC_CHANNEL_E;
    case 5: return DAC_CHANNEL_F;
    case 6: return DAC_CHANNEL_G;
    default: return DAC_CHANNEL_H;
  }
}

FLASHMEM bool HardwareSupported() {
  return NorthernLightModular && OC::DAC::kOctaveZero == 0 && !DAC_20Vpp;
}

// ---- meter ---------------------------------------------------------------------

static bool owned = false;

struct Meter_ : CalRoutine::MeterPort {
  enum Idn : uint8_t { IDN_NONE, IDN_WAIT, IDN_PAUSE, IDN_OK };
  Idn idn = IDN_NONE;
  bool mine = false;          // the driver's current query is one this port began
  uint32_t pause_ms = 0;
  char identity[96] = {};
  char model[24] = {};

  bool Begin(const char *scpi, uint32_t timeout_ms) override;
  int Poll(const char **reply) override;
  bool Connected() const override;
  const char *Identity() const override;
  void Service();
  void Forget();
  void TakeIdentity(const char *reply);
};

static Meter_ meter;

FLASHMEM void Meter_::Forget() {
  idn = IDN_NONE;
  mine = false;
  identity[0] = 0;
  model[0] = 0;
}

// "Siglent Technologies,SDM3065X,SDM36GBX...,1.01" -> identity as sent
// (printable bytes), model = the second field.
FLASHMEM void Meter_::TakeIdentity(const char *reply) {
  size_t k = 0;
  for (const char *p = reply; *p && k < sizeof(identity) - 1; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c >= 0x20 && c < 0x7F) identity[k++] = char(c);
  }
  identity[k] = 0;
  const char *f = strchr(identity, ',');
  size_t m = 0;
  if (f) {
    for (++f; *f && *f != ',' && m < sizeof(model) - 1; ++f) model[m++] = *f;
  }
  model[m] = 0;
  if (!m) strcpy(model, "meter");  // answered, but not in the usual shape
}

#ifdef CALR_HAVE_METER

FLASHMEM bool Meter_::Connected() const { return usbMeter.connected() && idn == IDN_OK; }

FLASHMEM bool Meter_::Begin(const char *scpi, uint32_t timeout_ms) {
  if (!Connected()) return false;
  mine = usbMeter.BeginQuery(scpi, timeout_ms);
  return mine;
}

FLASHMEM int Meter_::Poll(const char **reply) {
  if (!mine) return -1;
  using QS = USBTMCDevice::QueryState;
  const QS st = usbMeter.Poll();
  if (st == QS::Busy) return 0;
  mine = false;
  if (st == QS::Done) {
    *reply = usbMeter.Reply();
    return 1;
  }
  return -1;  // Timeout, Error (DISCONNECT included), anything unexpected
}

// The identity is asked here, not by the flow: until the meter has answered
// *IDN?, Connected() is false and the flow waits on its NoMeter screen. A
// replug forgets it, so a different meter is named, not the old one.
FLASHMEM void Meter_::Service() {
  if (!usbMeter.connected()) {
    if (idn != IDN_NONE || mine) Forget();
    return;
  }
  const uint32_t now = millis();
  switch (idn) {
    case IDN_NONE:
      // Refused while the driver is busy (a console query in flight) or
      // resynchronising: try again shortly.
      if (usbMeter.BeginQuery("*IDN?", 2000)) {
        idn = IDN_WAIT;
      } else {
        idn = IDN_PAUSE;
        pause_ms = now;
      }
      break;
    case IDN_WAIT: {
      using QS = USBTMCDevice::QueryState;
      const QS st = usbMeter.Poll();
      if (st == QS::Busy) break;
      if (st == QS::Done && usbMeter.Reply()[0]) {
        TakeIdentity(usbMeter.Reply());
        idn = IDN_OK;
      } else {
        idn = IDN_PAUSE;
        pause_ms = now;
      }
      break;
    }
    case IDN_PAUSE:
      if (now - pause_ms >= 500) idn = IDN_NONE;
      break;
    case IDN_OK:
      break;
  }
}

#else  // no USB host meter in this build (the simulator)

FLASHMEM bool Meter_::Connected() const { return false; }
FLASHMEM bool Meter_::Begin(const char *, uint32_t) { return false; }
FLASHMEM int Meter_::Poll(const char **) { return -1; }
FLASHMEM void Meter_::Service() {}

#endif

FLASHMEM const char *Meter_::Identity() const { return identity; }

FLASHMEM MeterStatus Status() {
#ifdef CALR_HAVE_METER
  if (!usbMeter.connected()) return METER_ABSENT;
  return meter.idn == Meter_::IDN_OK ? METER_READY : METER_ASKING;
#else
  return METER_ABSENT;
#endif
}

FLASHMEM const char *MeterModel() { return meter.model; }

// ---- outputs ---------------------------------------------------------------------

struct Outputs_ : CalRoutine::OutputPort {
  uint8_t held = 0;  // physical channels this port holds
  void Hold(int out, uint16_t code) override;
  void ReleaseAll() override;
  uint16_t TableCode(int out, int k) const override;
  uint16_t CodeForMv(int out, const uint16_t table[CalRoutineMath::kNodes], int32_t mv) const override;
};

static Outputs_ outputs;

FLASHMEM void Outputs_::Hold(int out, uint16_t code) {
  const int phys = PanelOutPhys(out);
  OC::DAC::Hold(phys, code);
  held = uint8_t(held | (1u << phys));
}

FLASHMEM void Outputs_::ReleaseAll() {
  for (int c = 0; c < 8; ++c)
    if (held & (1u << c)) OC::DAC::Release(c);
  held = 0;
}

FLASHMEM uint16_t Outputs_::TableCode(int out, int k) const {
  return OC::calibration_data.dac.calibrated_octaves[PanelOutPhys(out)][k];
}

// Entry k at k x 1200 mV from 0 V: the Xenomorpher's table (HardwareSupported).
FLASHMEM uint16_t Outputs_::CodeForMv(int, const uint16_t table[CalRoutineMath::kNodes],
                                       int32_t mv) const {
  uint16_t code = 0;
  CvMeterMath::MillivoltsToDacCode(table, CalRoutineMath::kNodes, 0, CalRoutineMath::kNodeMv, mv,
                                   &code);
  return code;
}

// ---- inputs ----------------------------------------------------------------------

// The ADC ISR refreshes its smoothed reading every ~180 us; sampling faster
// only repeats it (CvMeter.cpp, the same window).
static constexpr uint32_t kSampleIntervalUs = 180;

// All 8 inputs in one window: the ADC converts them together, so a window
// costs 8 smoothed reads per sample instead of 1.
struct Inputs_ : CalRoutine::InputPort {
  bool open = false;
  uint64_t sum[8] = {};
  uint32_t lo[8] = {}, hi[8] = {};
  uint32_t n = 0;
  uint32_t last_us = 0;
  void StartWindow() override;
  bool WindowDone(PointStats s[8]) override;
  void Sample();
};

static Inputs_ inputs;

FLASHMEM void Inputs_::StartWindow() {
  for (int i = 0; i < 8; ++i) {
    sum[i] = 0;
    lo[i] = 0xFFFFFFFFu;
    hi[i] = 0;
  }
  n = 0;
  last_us = micros() - kSampleIntervalUs;
  open = true;
}

// s[] in panel order (input 1..8); the sums are kept that way too.
FLASHMEM bool Inputs_::WindowDone(PointStats s[8]) {
  open = false;
  for (int i = 0; i < 8; ++i) {
    s[i].samples = n;
    s[i].mean_raw = n ? float(sum[i]) / (256.f * float(n)) : 0.f;
    s[i].min_raw = n ? float(lo[i]) * (1.f / 256.f) : 0.f;
    s[i].max_raw = n ? float(hi[i]) * (1.f / 256.f) : 0.f;
  }
  return true;
}

FLASHMEM void Inputs_::Sample() {
  if (!open) return;
  const uint32_t now = micros();
  if (now - last_us < kSampleIntervalUs) return;
  last_us = now;
  for (int i = 0; i < 8; ++i) {
    const uint32_t q = OC::ADC::smoothed_raw_q8(PanelInPhys(i));
    sum[i] += q;
    if (q < lo[i]) lo[i] = q;
    if (q > hi[i]) hi[i] = q;
  }
  ++n;
}

// ---- clock -----------------------------------------------------------------------

struct Clock_ : CalRoutine::Clock {
  uint32_t Ms() const override;
};
static Clock_ clock_;
FLASHMEM uint32_t Clock_::Ms() const { return millis(); }

// ---- save ------------------------------------------------------------------------

struct Save_ : CalRoutine::SavePort {
  const char *failed = "";
  bool SaveDacTable(const uint16_t table[8][CalRoutineMath::kNodes]) override;
  bool SaveCurves(const int16_t curves[8][CalRoutineMath::kCurvePoints], uint8_t valid_mask,
                  const uint8_t source[8]) override;
  bool SaveLines(const CvMeterMath::Fit lines[8], uint8_t valid_mask, const uint8_t source[8],
                 const float verify_mv[8]) override;
  void WriteMarker() override;
  void ResetAutotune() override;
  void ReloadInputs() override;
};

static Save_ save;

// Panel mask -> physical mask.
FLASHMEM uint8_t PhysMask(uint8_t panel_mask) {
  uint8_t m = 0;
  for (int i = 0; i < 8; ++i)
    if (panel_mask & (1u << i)) m = uint8_t(m | (1u << PanelInPhys(i)));
  return m;
}

// calibration_save() writes the whole calibration page (only the bytes that
// changed) and shows its own "Calibration saved" screen for a second.
FLASHMEM bool Save_::SaveDacTable(const uint16_t table[8][CalRoutineMath::kNodes]) {
  failed = "";
  for (int o = 0; o < 8; ++o) {
    uint16_t *dst = OC::calibration_data.dac.calibrated_octaves[PanelOutPhys(o)];
    for (int k = 0; k < CalRoutineMath::kNodes; ++k) dst[k] = table[o][k];
  }
  OC::calibration_save();
  return true;
}

// A new record, not an edit of the old one: an input that failed or was
// skipped is saved as "no curve" (it falls back to its ICA1 line, which
// SaveLines clears too, i.e. to the shared scale), as design 6.2 says.
FLASHMEM bool Save_::SaveCurves(const int16_t curves[8][CalRoutineMath::kCurvePoints],
                                 uint8_t valid_mask, const uint8_t source[8]) {
  DMAMEM static InputCurveRecord rec;  // 300 bytes, out of DTCM
  memset(&rec, 0, sizeof(rec));
  for (int i = 0; i < 8; ++i) {
    const int phys = PanelInPhys(i);
    for (int b = 0; b < kInputCurvePoints; ++b) rec.mv_x2[phys][b] = curves[i][b];
    rec.source_output[phys] = source[i];
  }
  rec.valid_mask = PhysMask(valid_mask);
  // Measured with the deterministic ADC start (AdcSlotMap.h, Task 5a).
  MarkMeasuredWithFix(rec, rec.valid_mask);
  if (!InputCurveSaveToFlash(rec)) {
    failed = "CURVES";
    return false;
  }
  return true;
}

FLASHMEM bool Save_::SaveLines(const CvMeterMath::Fit lines[8], uint8_t valid_mask,
                                const uint8_t source[8], const float verify_mv[8]) {
  CvMeterMath::InputCalibration &cal = OC::input_calibration;
  for (int i = 0; i < 8; ++i) {
    const int phys = PanelInPhys(i);
    const uint8_t bit = uint8_t(1u << phys);
    if (!(valid_mask & (1u << i))) {
      cal.valid_mask = uint8_t(cal.valid_mask & ~bit);
      continue;
    }
    cal.gain[phys] = lines[i].gain;
    cal.offset_mv[phys] = lines[i].offset_mv;
    int64_t r = CvMeterMath::ScaleRound(verify_mv[i], 1);
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    cal.verify_mv10[phys] = int16_t(r);
    cal.source_output[phys] = source[i];
    cal.valid_mask = uint8_t(cal.valid_mask | bit);
  }
  MarkMeasuredWithFix(cal, PhysMask(valid_mask));  // AdcSlotMap.h
  OC::input_calibration_save();
  return true;
}

FLASHMEM void Save_::WriteMarker() { MarkerWrite(kCalRoutineMarkerStored); }

// autotune_calibration_data.Reset(), written out: LTO kept the inline
// Reset() out of line, and an out-of-line copy lands in ITCM.
FLASHMEM void Save_::ResetAutotune() {
  for (auto &ch : OC::global_settings.autotune_calibration_data.channels) {
    ch.valid = false;
    for (auto &v : ch.calibrated_octaves) v = 0;
  }
}

FLASHMEM void Save_::ReloadInputs() { InputCal::Reload(); }

FLASHMEM const char *SaveFailedAt() { return save.failed; }

// ---- marker ----------------------------------------------------------------------

FLASHMEM bool MarkerSet() {
  uint8_t b[CalRoutineMarkerSize];
  EEPROMStorage::read(EEPROM_CALROUTINE_MARKER_START, b, sizeof(b));
  return CalRoutineMarkerStored(b);
}

FLASHMEM uint32_t DacTableSum() {
  uint32_t h = 2166136261u;
  for (int ch = 0; ch < DAC_CHANNEL_COUNT; ++ch)
    for (int i = 0; i < OCTAVES + 1; ++i) {
      const uint16_t v = OC::calibration_data.dac.calibrated_octaves[ch][i];
      h = (h ^ (v & 0xFFu)) * 16777619u;
      h = (h ^ (v >> 8)) * 16777619u;
    }
  return h;
}

FLASHMEM void MarkerWrite(uint8_t flags) {
  uint8_t b[CalRoutineMarkerSize];
  CalRoutineMarkerEncode(flags, b);
  EEPROMStorage::update(EEPROM_CALROUTINE_MARKER_START, b, sizeof(b));  // EEPROM.update per byte
}

// ---- console ---------------------------------------------------------------------

#ifdef PRINT_DEBUG
static uint32_t console_dropped = 0;
#endif

FLASHMEM void ConsoleLog(const char *line) {
#ifdef PRINT_DEBUG
  // Serial.write() waits up to 120 ms for a host that is not draining the
  // port; the flow runs from loop(), so a line that does not fit is dropped
  // (and the count printed with the next one that does).
  const int len = int(strlen(line));
  if (!Serial || Serial.availableForWrite() < len + 24) {
    ++console_dropped;
    return;
  }
  if (console_dropped) {
    Serial.printf("CALR,DROPPED,%lu\r\n", (unsigned long)console_dropped);
    console_dropped = 0;
  }
  Serial.write((const uint8_t *)line, len);
  Serial.write("\r\n", 2);
#else
  (void)line;
#endif
}

// ---- lifecycle -------------------------------------------------------------------

// Zeroed at startup (the .bss.dma hook); CPU-only access, no DMA.
alignas(CalRoutineFlow) DMAMEM static uint8_t flow_store[sizeof(CalRoutineFlow)];
static CalRoutineFlow *flow = nullptr;

FLASHMEM CalRoutineFlow &Flow() {
  if (!flow) flow = new (flow_store) CalRoutineFlow;
  return *flow;
}

FLASHMEM CalRoutine::MeterPort &Meter() { return meter; }
FLASHMEM CalRoutine::OutputPort &Outputs() { return outputs; }
FLASHMEM CalRoutine::InputPort &Inputs() { return inputs; }
FLASHMEM CalRoutine::Clock &Clock() { return clock_; }
FLASHMEM CalRoutine::SavePort &Save() { return save; }

FLASHMEM void Open() {
  owned = true;
#ifdef CVMETER_ENABLED
  OC::CvMeter::CalRoutineTakeover();  // a console Q in progress lets go of its output
#endif
  meter.Forget();  // name whatever is plugged in now
}

FLASHMEM void Close() {
  outputs.ReleaseAll();
  inputs.open = false;
  owned = false;
  // A query of ours still in the driver finishes on its own (loop() keeps
  // polling it); nothing reads its reply.
  meter.mine = false;
}

FLASHMEM bool MeterOwned() { return owned; }

FLASHMEM void Service() {
  if (!owned) return;
  meter.Service();
  inputs.Sample();
}

}  // namespace CalRoutinePorts
