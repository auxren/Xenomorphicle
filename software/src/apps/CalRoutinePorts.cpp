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
#include "../CalRec.h"
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
#include "../PresetBus.h"
#include "../PresetEngine.h"
#include "../Buchla200eWriteGuard.h"
#include "../Buchla259eSlotCodec.h"
#include "../Bus200eBridge.h"
#include "../Cal259eBackup.h"
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

#ifdef XENO_SIM
static SimPlant *sim_plant = nullptr;
static double sim_out_mv = 0.0;  // what the held output puts out (ideal), mV
FLASHMEM void SimSetPlant(SimPlant *plant) {
  sim_plant = plant;
  sim_out_mv = 0.0;
}
#endif

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
#ifdef XENO_SIM
  if (sim_plant) return true;  // --sim-259e: an ideal 1.2 V/oct output stage
#endif
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

#elif defined(XENO_SIM)

// The simulator has no USB host: with a plant (--sim-259e) the meter is an
// ideal SDM3065X reading the held output, 60 ms per READ?, else absent.
static char sim_cur[32];
static char sim_reply[48];
static bool sim_busy = false;
static uint32_t sim_ready = 0;

FLASHMEM bool Meter_::Connected() const { return sim_plant != nullptr; }
FLASHMEM bool Meter_::Begin(const char *scpi, uint32_t) {
  if (!sim_plant || sim_busy) return false;
  strncpy(sim_cur, scpi, sizeof(sim_cur) - 1);
  sim_cur[sizeof(sim_cur) - 1] = 0;
  sim_busy = true;
  sim_ready = millis() + (strcmp(scpi, "READ?") == 0 ? 60 : 5);
  return true;
}
FLASHMEM int Meter_::Poll(const char **reply) {
  if (!sim_busy) return -1;
  if (int32_t(millis() - sim_ready) < 0) return 0;
  sim_busy = false;
  sim_reply[0] = 0;
  if (strcmp(sim_cur, "READ?") == 0)
    snprintf(sim_reply, sizeof(sim_reply), "%+.9E\n", sim_out_mv / 1000.0);
  else if (strcmp(sim_cur, "SYST:ERR?") == 0)
    snprintf(sim_reply, sizeof(sim_reply), "0,\"No error\"\n");
  *reply = sim_reply;
  return 1;
}
FLASHMEM void Meter_::Service() {
  if (sim_plant && !model[0]) TakeIdentity("Siglent Technologies,SDM3065X,SIM0001,1.01");
}

#else  // no USB host meter in this build

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
#elif defined(XENO_SIM)
  return sim_plant ? METER_READY : METER_ABSENT;
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

#ifdef XENO_SIM
// The ideal output stage: the mV whose code through the stored table is `code`
// (the table is monotone; bisection on CodeForMv).
static double SimMvOfCode(const Outputs_ &o, int out, uint16_t code) {
  uint16_t table[CalRoutineMath::kNodes];
  for (int k = 0; k < CalRoutineMath::kNodes; ++k) table[k] = o.TableCode(out, k);
  int32_t lo = 0, hi = CalRoutineMath::kNodes * CalRoutineMath::kNodeMv - CalRoutineMath::kNodeMv;
  while (hi - lo > 1) {
    const int32_t mid = (lo + hi) / 2;
    if (o.CodeForMv(out, table, mid) < code) lo = mid; else hi = mid;
  }
  return o.CodeForMv(out, table, lo) >= code ? lo : hi;
}
#endif

FLASHMEM void Outputs_::Hold(int out, uint16_t code) {
  const int phys = PanelOutPhys(out);
  OC::DAC::Hold(phys, code);
  held = uint8_t(held | (1u << phys));
#ifdef XENO_SIM
  if (sim_plant) {
    sim_out_mv = SimMvOfCode(*this, out, code);
    sim_plant->OutputMv(sim_out_mv);
  }
#endif
}

FLASHMEM void Outputs_::ReleaseAll() {
  for (int c = 0; c < 8; ++c)
    if (held & (1u << c)) OC::DAC::Release(c);
  held = 0;
#ifdef XENO_SIM
  sim_out_mv = 0.0;
  if (sim_plant) sim_plant->OutputMv(0.0);
#endif
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
  failed = "";  // an Inputs save starts here, not at SaveDacTable
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

// Zeroed at startup like flow_store; constructed on first use.
alignas(Cal259eFlow) DMAMEM static uint8_t flow259_store[sizeof(Cal259eFlow)];
static Cal259eFlow *flow259 = nullptr;

FLASHMEM Cal259eFlow &Flow259e() {
  if (!flow259) flow259 = new (flow259_store) Cal259eFlow;
  return *flow259;
}

// A cancel (or an app switch) after the COMMIT frame went out leaves the flow
// waiting for the module's verdict with the app gone: that wait still counts
// as a run, and CalTask() below finishes it.
FLASHMEM bool CalRunActive() {
  return owned || (flow259 && flow259->cancel_pending());
}

FLASHMEM int Cancelled259Verdict() { return flow259 ? flow259->cancelled_verdict() : 0; }

// Once per loop pass, whatever app is showing and whatever the build: finish a
// cancelled run's COMMIT verdict (it logs CALR,259END,CANCELLED_<result>).
// Bounded work per call. Main.cpp calls it outside PRINT_DEBUG too: in the
// T41 / T41_audio builds nothing else would ever collect the verdict, and
// CalRunActive() would stay true for good. The app's own Loop() ticks the flow
// as well while it is open; a second Tick() in a pass is harmless.
FLASHMEM void CalTask() {
  if (flow259 && flow259->cancel_pending()) flow259->Tick();
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

// ---- the 200e bus for the 259e client, and its read-only console ---------------

#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS) && !defined(XENO_SIM)
struct Bus_ : BusPort {
  int Send(const uint8_t *f, uint8_t n) override { return OC::PresetBus::SendRawFrame(f, n); }
  bool PopReply(uint8_t *f, uint8_t *n) override { return OC::PresetBus::PopCalReply(f, n); }
};
#else
struct Bus_ : BusPort {
#ifdef XENO_SIM
  int Send(const uint8_t *f, uint8_t n) override {
    BusPort *b = sim_plant ? sim_plant->Bus259e() : nullptr;
    return b ? b->Send(f, n) : -2;
  }
  bool PopReply(uint8_t *f, uint8_t *n) override {
    BusPort *b = sim_plant ? sim_plant->Bus259e() : nullptr;
    return b && b->PopReply(f, n);
  }
#else
  int Send(const uint8_t *, uint8_t) override { return -2; }
  bool PopReply(uint8_t *, uint8_t *) override { return false; }
#endif
};
#endif
static Bus_ bus_port;
FLASHMEM BusPort &Bus() { return bus_port; }

// ---- the 259e flow's backup and entropy ports -------------------------------------

#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS) && !defined(XENO_SIM)
// The master BACKUP of a 259e (Cal259eBackup.h holds the rules, host-tested): the
// bank lands in MasterCardImage() (console 'm' leaves it there); the 990-byte bank
// is stored in CAL259BK.BIN and read back.
struct RealMaster : BackupMaster {
  bool FwCardMode() override;
  bool BridgeIdle() override;
  Bus200eMasterState State() override;
  void Reset() override;
  int Backup(uint8_t a) override;
  uint8_t ModAddr() override;
  bool IsRestore() override;
  uint32_t Bytes() override;
  const uint8_t *Image() override;
  uint32_t Crc32(const uint8_t *d, uint32_t n) override;
  bool Save(uint8_t a, const uint8_t *d, uint32_t n, uint32_t crc) override;
  bool Verify(uint8_t a, uint32_t n, uint32_t crc) override;
};
FLASHMEM bool RealMaster::FwCardMode() { return OC::PresetBus::FwCardMode(); }
FLASHMEM bool RealMaster::BridgeIdle() { return Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE; }
FLASHMEM Bus200eMasterState RealMaster::State() { return OC::PresetBus::MasterState(); }
FLASHMEM void RealMaster::Reset() { OC::PresetBus::MasterReset(); }
FLASHMEM int RealMaster::Backup(uint8_t a) { return OC::PresetBus::MasterBackupForCal(a); }
FLASHMEM uint8_t RealMaster::ModAddr() { return Bus200eMasterModAddr(); }
FLASHMEM bool RealMaster::IsRestore() { return Bus200eMasterIsRestore() != 0; }
FLASHMEM uint32_t RealMaster::Bytes() { return Bus200eMasterBytesTransferred(); }
FLASHMEM const uint8_t *RealMaster::Image() { return OC::PresetBus::MasterCardImage(); }
FLASHMEM uint32_t RealMaster::Crc32(const uint8_t *d, uint32_t n) { return Buchla200eCrc32(d, n); }
FLASHMEM bool RealMaster::Save(uint8_t a, const uint8_t *d, uint32_t n, uint32_t crc) {
  return OC::PresetEngine::SaveCalBackup(a, d, n, crc);
}
FLASHMEM bool RealMaster::Verify(uint8_t a, uint32_t n, uint32_t crc) {
  return OC::PresetEngine::VerifyCalBackup(a, n, crc);
}
static RealMaster real_master;
static Cal259eBackup backup_port(real_master);
#else
struct Backup_ : BackupPort {
#ifdef XENO_SIM
  uint32_t t0 = 0;
  bool on = false;
  bool Begin(uint8_t) override { on = true; t0 = millis(); return sim_plant != nullptr; }
  int Poll() override { return !on ? -1 : millis() - t0 >= 300 ? 1 : 0; }
  uint32_t Bytes() const override { return Cal259eFlow::kBackupBankBytes; }  // the sim's bank is complete
#else
  bool Begin(uint8_t) override { return false; }
  int Poll() override { return -1; }
#endif
};
static Backup_ backup_port;
#endif
FLASHMEM BackupPort &Backup() { return backup_port; }

struct Entropy_ : EntropyPort {
  uint32_t calls = 0;
  void Fill(uint8_t *d, int n) override;
  uint32_t UnixTime() override;
};

#if defined(__IMXRT1062__) && !defined(XENO_SIM)
static uint32_t Mix32(uint32_t h, uint32_t v) {  // FNV-1a over the 4 bytes, then a finaliser
  for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xFF; h *= 16777619u; }
  h ^= h >> 15; h *= 0x2c1b3c6dU; h ^= h >> 12;
  return h;
}
FLASHMEM void Entropy_::Fill(uint8_t *d, int n) {
  uint32_t h = 2166136261u;
  h = Mix32(h, HW_OCOTP_CFG0);
  h = Mix32(h, HW_OCOTP_CFG1);
  h = Mix32(h, ARM_DWT_CYCCNT);
  h = Mix32(h, micros());
  h = Mix32(h, uint32_t(rtc_get()));
  h = Mix32(h, ++calls);
  for (int i = 0; i < n; ++i) {
    if ((i & 3) == 0) h = Mix32(h, ARM_DWT_CYCCNT + uint32_t(i));
    d[i] = uint8_t(h >> (8 * (i & 3)));
  }
}
FLASHMEM uint32_t Entropy_::UnixTime() {
  const uint32_t t = uint32_t(rtc_get());
  return t < 1577836800u ? 0u : t;  // before 2020-01-01: the RTC was never set
}
#else
// Host and simulator: deterministic.
FLASHMEM void Entropy_::Fill(uint8_t *d, int n) {
  uint32_t h = 0x9E3779B9u + ++calls;
  for (int i = 0; i < n; ++i) { h = h * 1664525u + 1013904223u; d[i] = uint8_t(h >> 24); }
}
FLASHMEM uint32_t Entropy_::UnixTime() { return 0; }
#endif
static Entropy_ entropy_port;
FLASHMEM EntropyPort &Entropy() { return entropy_port; }

#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS) && defined(PRINT_DEBUG) && !defined(XENO_SIM)

namespace {

enum CState : uint8_t { CS_IDLE, CS_ADDR_HI, CS_ADDR_LO, CS_CMD, CS_CH };
enum CJob : uint8_t { J_NONE, J_STATUS, J_RECORD, J_ADC };
constexpr int kAdcWords = 8;

CState cstate = CS_IDLE;
CJob cjob = J_NONE;
uint8_t caddr = 0;
uint8_t cch = 0;
int cword = 0;
int cprev_n = -1;
Cal259eBus cbus;

// One owner, one place: the console refuses while a Cal Routine run is open.
bool CalBusy() { return CalRunActive(); }

int HexVal(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

const char *ErrName(int e) {
  switch (e) {
    case E_BUS_BUSY: return "BUS_BUSY";
    case E_NO_REPLY: return "NO_REPLY";
    case E_BAD_REPLY: return "BAD_REPLY";
    case E_COMMIT_REFUSED: return "COMMIT_REFUSED";
    case E_NO_SLOT: return "NO_SLOT";
    case E_COMMIT_UNKNOWN: return "COMMIT_UNKNOWN";
    case E_BUS_ERROR: return "BUS_ERROR";
    case E_INTERNAL: return "INTERNAL";
    case E_INCONSISTENT: return "INCONSISTENT";
    default: return "ERROR";
  }
}

bool AddrOk(uint8_t a) { return (a >= 0x28 && a <= 0x2B) || a == 0x5C; }

void StartJob() {
  cbus.Begin(Bus(), Clock(), caddr);
  bool ok = false;
  switch (cjob) {
    case J_STATUS: ok = cbus.ReadStatus(); break;
    case J_RECORD: ok = cbus.ReadRecord(); break;
    case J_ADC: cword = 0; cprev_n = -1; ok = cbus.ReadAdc(cch); break;
    default: break;
  }
  if (!ok) {
    Serial.printf("CALR,259E,%02X,ERR,START\r\n", caddr);
    cjob = J_NONE;
  }
}

}  // namespace

FLASHMEM bool Cal259Consume(int cmd) {
  if (cstate == CS_IDLE) return false;
  const int v = HexVal(cmd);
  switch (cstate) {
    case CS_ADDR_HI:
      if (v < 0) break;
      caddr = uint8_t(v << 4);
      cstate = CS_ADDR_LO;
      return true;
    case CS_ADDR_LO:
      if (v < 0) break;
      caddr = uint8_t(caddr | v);
      if (!AddrOk(caddr)) {
        Serial.printf("CALR,259E,%02X,ERR,ADDRESS (259e 28-2B, 251e 5C)\r\n", caddr);
        cstate = CS_IDLE;
        return true;
      }
      cstate = CS_CMD;
      return true;
    case CS_CMD:
      cstate = CS_IDLE;
      if (cmd == 's' || cmd == 'r') {
        cjob = cmd == 's' ? J_STATUS : J_RECORD;
        StartJob();
        return true;
      }
      if (cmd == 'a') { cstate = CS_CH; return true; }
      break;
    case CS_CH:
      cstate = CS_IDLE;
      if (cmd == '0' || cmd == '1') {
        cch = uint8_t(cmd - '0');
        cjob = J_ADC;
        StartJob();
        return true;
      }
      break;
    default:
      break;
  }
  cstate = CS_IDLE;
  Serial.println("CALR,259E,ERR,cancelled (=<aa>s  =<aa>r  =<aa>a0  =<aa>a1; aa = bus address in hex)");
  return true;
}

FLASHMEM void Cal259Command() {
  if (CalBusy()) { Serial.println("CALR,259E,ERR,BUSY_CAL"); return; }
  if (cjob != J_NONE) { Serial.println("CALR,259E,ERR,BUSY"); return; }
  cstate = CS_ADDR_HI;
}

FLASHMEM void Cal259Task() {
  if (cjob == J_NONE) return;
  if (CalBusy()) {  // the Cal Routine started under us: let go
    Serial.printf("CALR,259E,%02X,ERR,BUSY_CAL\r\n", caddr);
    cjob = J_NONE;
    cstate = CS_IDLE;
    return;
  }
  const int r = cbus.Tick();
  if (r == 0) return;
  if (r < 0) {
    Serial.printf("CALR,259E,%02X,ERR,%s\r\n", caddr, ErrName(r));
    cjob = J_NONE;
    return;
  }
  switch (cjob) {
    case J_STATUS: {
      const ::Status s = cbus.status();
      Serial.printf("CALR,259E,%02X,STATUS,%02X,%u,%u\r\n", caddr, s.flags, s.fmt, s.seq);
      cjob = J_NONE;
      break;
    }
    case J_RECORD:
      if (!cbus.has_record()) {
        Serial.printf("CALR,259E,%02X,REC,NONE\r\n", caddr);
      } else {
        // Bytes 0..28 were read from the module; byte 29 (the CRC low byte) cannot be
        // read from a 31.2C4 module and is inferred from the CRC of the first 28, so it
        // is a separate field and never part of the read hex.
        char hex[2 * CalRec::kRecLen259e + 1];
        for (int i = 0; i < CalRec::kRecLen259e - 1; ++i)
          snprintf(hex + 2 * i, 3, "%02X", cbus.record()[i]);
        Serial.printf("CALR,259E,%02X,REC,%s,inferred_crc_lo=%02X\r\n", caddr, hex,
                      cbus.record()[CalRec::kRecLen259e - 1]);
      }
      cjob = J_NONE;
      break;
    case J_ADC: {
      // fresh: the module's sample counter moved since the previous word
      const int n = cbus.adc_n();
      Serial.printf("CALR,259E,%02X,ADC,%u,%d,%u,%d,%s\r\n", caddr, cch, cword, cbus.adc(), n,
                    n != cprev_n ? "FRESH" : "SAME");
      cprev_n = n;
      if (++cword >= kAdcWords) { cjob = J_NONE; break; }
      if (!cbus.ReadAdc(cch)) cjob = J_NONE;
      break;
    }
    default:
      cjob = J_NONE;
      break;
  }
}

#else

FLASHMEM bool Cal259Consume(int) { return false; }
FLASHMEM void Cal259Command() {}
FLASHMEM void Cal259Task() {}

#endif

}  // namespace CalRoutinePorts
