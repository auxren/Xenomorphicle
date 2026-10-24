// Bench CV meter: console CV stream, DAC hold, per-input CV calibration and
// frequency counter. See CvMeter.h for the key map, CvMeterMath.h for the
// arithmetic and the line formats, docs/CV-Meter.md for the procedure.
//
// Everything here runs from loop() (Task) or the console reader (Consume,
// Command), both loop context, and lives in flash: the console build has
// about 2.5 KB of ITCM padding left before the next 32 KB block, and none of
// this is time-critical. The ADC ISR is untouched -- it only gained an
// accessor (OC::ADC::smoothed_raw_q8) -- and the DAC ISR path gained one
// load and branch for the hold (OC::DAC::Update).

#include <Arduino.h>
#include <math.h>
#include "CvMeter.h"

#ifdef CVMETER_ENABLED

#include "CvMeterMath.h"
#include "InputCal.h"
#include "OC_ADC.h"
#include "OC_DAC.h"
#include "OC_calibration.h"
#include "OC_digital_inputs.h"
#include "OC_gpio.h"
#include "src/drivers/FreqMeasure/OC_FreqMeasure.h"

namespace OC {
namespace CvMeter {

using namespace CvMeterMath;

// ---- panel numbering -> physical channel -----------------------------------
// Console numbers are panel labels. The ADC_/DAC_CHANNEL_n globals are the
// physical index behind each label (flip mode and some hardware IDs remap
// them at boot), and every calibration table is indexed physically.

FLASHMEM static int InPhys(int label) {
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

FLASHMEM static int OutPhys(int label) {
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

// '1'..'8' or 'a'..'h' / 'A'..'H' -> 0..7, else -1
FLASHMEM static int ParseOutput(int c) {
  if (c >= '1' && c <= '8') return c - '1';
  if (c >= 'a' && c <= 'h') return c - 'a';
  if (c >= 'A' && c <= 'H') return c - 'A';
  return -1;
}

FLASHMEM static int ParseInput(int c) {
  if (c >= '1' && c <= '8') return c - '1';
  return -1;
}

FLASHMEM static int32_t OctMv() { return OctaveMv(NorthernLightModular); }
FLASHMEM static int32_t MvPerStep() { return DacStepMv(NorthernLightModular, DAC_20Vpp); }

FLASHMEM static bool OutputCode(int out_label, int32_t mv, uint16_t *code) {
  const int phys = OutPhys(out_label);
  return MillivoltsToDacCode(OC::calibration_data.dac.calibrated_octaves[phys],
                             OCTAVES + 1, DAC::kOctaveZero, MvPerStep(), mv, code);
}

// The same calibration the apps get (InputCal.h): the input's curve or ICA1
// line where it has one -- interpolated in float on the very table the ISR
// applies, so the meter keeps its sub-count resolution -- else the shared
// path. An ICA1 fit that InputCal refused is shown as shared, as the apps see it.
FLASHMEM static float InputMv(int phys, float raw, bool *per_input) {
  if (InputCal::Calibrated(phys)) {
    if (per_input) *per_input = true;
    return InputCal::Millivolts(phys, raw);
  }
  if (per_input) *per_input = false;
  return SharedMillivolts(OC::calibration_data.adc.offset[phys], raw,
                          OC::calibration_data.adc.pitch_cv_scale, OctMv());
}

// What one input can read, by the calibration that is in effect for it.
FLASHMEM static void InputRange(int phys, int32_t *lo, int32_t *hi) {
  if (InputCal::Calibrated(phys)) {
    const float a = InputMv(phys, 0.f, nullptr);
    const float b = InputMv(phys, float(kRawMax), nullptr);
    *lo = int32_t(lroundf(a < b ? a : b));
    *hi = int32_t(lroundf(a < b ? b : a));
  } else {
    SharedReadableRange(OC::calibration_data.adc.offset[phys],
                        OC::calibration_data.adc.pitch_cv_scale, OctMv(), lo, hi);
  }
}

FLASHMEM static void PrintFixed(int64_t v, int decimals) {
  char b[32];
  FormatFixed(b, sizeof(b), v, decimals);
  Serial.print(b);
}

// ---- sampling ---------------------------------------------------------------
// The ADC ISR refreshes its smoothed readings every ~180 us; sampling faster
// than that only repeats them. Physical order, 12.8 fixed point.

struct Window {
  uint64_t sum[kChannels];
  uint32_t lo[kChannels];
  uint32_t hi[kChannels];
  uint32_t n;

  FLASHMEM void Reset() {
    for (int i = 0; i < kChannels; ++i) { sum[i] = 0; lo[i] = 0xFFFFFFFFu; hi[i] = 0; }
    n = 0;
  }
  FLASHMEM void Add(const uint32_t q[kChannels]) {
    for (int i = 0; i < kChannels; ++i) {
      sum[i] += q[i];
      if (q[i] < lo[i]) lo[i] = q[i];
      if (q[i] > hi[i]) hi[i] = q[i];
    }
    ++n;
  }
  FLASHMEM uint32_t mean_q8(int i) const { return n ? uint32_t((sum[i] + n / 2) / n) : 0; }
};

static constexpr uint32_t kSampleIntervalUs = 180;

static uint32_t sample_last_us = 0;

FLASHMEM static void ReadAll(uint32_t q[kChannels]) {
  for (int i = 0; i < kChannels; ++i) q[i] = ADC::smoothed_raw_q8(i);
}

// ---- line output that never stalls the loop ----------------------------------
// Teensy's Serial.write() waits up to 120 ms per call for a host that is not
// draining the port. A stream must drop a line instead, and count it.

FLASHMEM static bool TryWriteLine(const char *line, int len) {
  if (len <= 0) return false;
  if (!Serial) return false;
  if (Serial.availableForWrite() < len + 2) return false;
  Serial.write((const uint8_t *)line, len);
  Serial.write((const uint8_t *)"\r\n", 2);
  return true;
}

// =============================================================================
// CV stream ('v')

static constexpr uint32_t kStreamPeriodMs = 20;  // 50 Hz

static bool stream_on = false;
static uint32_t stream_next_ms = 0;
static uint32_t stream_seq = 0;
static uint32_t stream_dropped = 0;
static uint32_t stream_lines = 0;
static Window stream_win;

FLASHMEM static void PrintRanges(const char *tag) {
  for (int l = 0; l < kChannels; ++l) {
    const int phys = InPhys(l);
    int32_t lo, hi;
    InputRange(phys, &lo, &hi);
    Serial.printf("%s,%d,%ld,%ld,%c\r\n", tag, l + 1, (long)lo, (long)hi,
                  InputCal::Calibrated(phys) ? 'C' : 'S');
  }
}

FLASHMEM static void StreamStart() {
  Serial.println("CV#,ms,seq,dropped,raw1,raw2,raw3,raw4,raw5,raw6,raw7,raw8,"
                 "mv1,mv2,mv3,mv4,mv5,mv6,mv7,mv8,cal,clip");
  // What each input can read before it clips, by its calibration in effect.
  PrintRanges("CVRANGE");
  stream_win.Reset();
  stream_seq = stream_dropped = stream_lines = 0;
  stream_next_ms = millis() + kStreamPeriodMs;
  stream_on = true;
}

FLASHMEM static void StreamStop() {
  stream_on = false;
  Serial.printf("CV#,stopped,lines=%lu,dropped=%lu\r\n",
                (unsigned long)stream_lines, (unsigned long)stream_dropped);
}

FLASHMEM static void StreamEmit(uint32_t now) {
  if (!stream_win.n) {
    uint32_t q[kChannels];
    ReadAll(q);
    stream_win.Add(q);
  }
  uint32_t raw_q8[kChannels];
  int32_t mv10[kChannels];
  char cal[kChannels], clip[kChannels];
  for (int l = 0; l < kChannels; ++l) {
    const int phys = InPhys(l);
    raw_q8[l] = stream_win.mean_q8(phys);
    bool per_input = false;
    const float mv = InputMv(phys, float(raw_q8[l]) * (1.f / 256.f), &per_input);
    mv10[l] = int32_t(ScaleRound(mv, 1));
    cal[l] = per_input ? 'C' : 'S';
    clip[l] = ClipFlag(stream_win.lo[phys], stream_win.hi[phys]);
  }
  char line[200];
  const int len = FormatCvLine(line, sizeof(line), now, stream_seq++, stream_dropped,
                               raw_q8, mv10, cal, clip);
  if (TryWriteLine(line, len)) ++stream_lines;
  else ++stream_dropped;
  stream_win.Reset();
}

// =============================================================================
// DAC hold ('d')

enum DState : uint8_t { D_IDLE, D_WAIT_CH, D_WAIT_ARG };
static DState d_state = D_IDLE;
static int d_out = 0;
static MvArg d_arg;

FLASHMEM static void HoldOutput(int out_label, int32_t mv) {
  uint16_t code;
  if (!OutputCode(out_label, mv, &code)) {
    int32_t lo, hi;
    DacRange(OCTAVES + 1, DAC::kOctaveZero, MvPerStep(), &lo, &hi);
    Serial.printf("DAC,ERR,%d,%ld mV is outside this output's calibrated range %ld..%ld mV\r\n",
                  out_label + 1, (long)mv, (long)lo, (long)hi);
    return;
  }
  DAC::Hold(OutPhys(out_label), code);
  Serial.printf("DAC,%d,HOLD,%ld,%u\r\n", out_label + 1, (long)mv, (unsigned)code);
}

FLASHMEM static void ReleaseOutput(int out_label) {
  DAC::Release(OutPhys(out_label));
  Serial.printf("DAC,%d,RELEASED\r\n", out_label + 1);
}

// =============================================================================
// Per-input calibration ('Q')

enum QState : uint8_t { Q_IDLE, Q_WAIT_SUB, Q_WAIT_OUT };
static QState q_state = Q_IDLE;
static int q_in = 0;
static bool q_all = false;
static uint32_t q_clear_arm_ms = 0;

enum FlowState : uint8_t {
  FLOW_IDLE,
  FLOW_WAIT_PATCH,   // operator patches, presses y
  FLOW_SETTLE,       // DAC set, waiting for it and the input filter
  FLOW_MEASURE,      // averaging
  FLOW_WAIT_COMMIT,  // result shown, operator presses y to keep it
};

static constexpr uint32_t kSettleMs = 300;
static constexpr uint32_t kMeasureMs = 600;

struct Flow {
  FlowState state;
  bool all;
  int in;      // panel label 0..7
  int out;     // panel label 0..7
  int point;   // 0, 1: fit; 2: verify
  int32_t mv[3];
  uint16_t code[3];
  PointStats stats[3];
  Fit fit;
  float residual_mv;
  uint32_t t0;
  Window win;
};
static Flow flow;

FLASHMEM static void FlowEnd() {
  DAC::Release(OutPhys(flow.out));
  flow.state = FLOW_IDLE;
}

FLASHMEM static void FlowAbort(const char *why) {
  Serial.printf("INCAL,ABORT,in=%d,%s\r\n", flow.in + 1, why);
  FlowEnd();
}

// Sets up input flow.in; false (and a message) if it cannot be done.
FLASHMEM static bool FlowPrepare() {
  const int phys = InPhys(flow.in);
  int32_t dlo, dhi, alo, ahi;
  // Only the span the DAC calibration actually measured with a meter
  // (entries 0..8); above that its table is extrapolated.
  DacRange(kDacMeteredTopIndex + 1, DAC::kOctaveZero, MvPerStep(), &dlo, &dhi);
  InputRange(phys, &alo, &ahi);
  const int32_t lo = dlo > alo ? dlo : alo;
  const int32_t hi = dhi < ahi ? dhi : ahi;
  if (!ChooseCalPoints(lo, hi, MvPerStep(), flow.mv)) {
    Serial.printf("INCAL,ERR,in=%d,usable range %ld..%ld mV is under 2 V\r\n",
                  flow.in + 1, (long)lo, (long)hi);
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!OutputCode(flow.out, flow.mv[i], &flow.code[i])) {
      Serial.printf("INCAL,ERR,in=%d,%ld mV not reachable on output %d\r\n",
                    flow.in + 1, (long)flow.mv[i], flow.out + 1);
      return false;
    }
  }
  flow.point = 0;
  flow.state = FLOW_WAIT_PATCH;
  Serial.printf("INCAL,PATCH,in=%d,out=%d,points=%ld,%ld,%ld\r\n", flow.in + 1, flow.out + 1,
                (long)flow.mv[0], (long)flow.mv[1], (long)flow.mv[2]);
  Serial.printf("  Patch OUTPUT %d to INPUT %d (nothing else on either jack). Output %d will be\r\n"
                "  driven to %ld, %ld and %ld mV. Press y when patched; any other key aborts.\r\n",
                flow.out + 1, flow.in + 1, flow.out + 1,
                (long)flow.mv[0], (long)flow.mv[1], (long)flow.mv[2]);
  return true;
}

FLASHMEM static void FlowStart(int in, int out, bool all) {
  if (flow.state != FLOW_IDLE) FlowEnd();
  flow.all = all;
  flow.in = in;
  flow.out = out;
  if (DAC::held_mask() & (1u << OutPhys(out)))
    Serial.printf("  note: output %d was held; calibration takes it over and releases it\r\n", out + 1);
  if (!FlowPrepare()) flow.state = FLOW_IDLE;
}

// Next input of an "all" run, or done.
FLASHMEM static void FlowNext() {
  DAC::Release(OutPhys(flow.out));
  if (flow.all && flow.in < kChannels - 1) {
    ++flow.in;
    if (FlowPrepare()) return;
  }
  if (flow.all) Serial.println("INCAL,DONE");
  flow.state = FLOW_IDLE;
}

FLASHMEM static void FlowSetPoint() {
  DAC::Hold(OutPhys(flow.out), flow.code[flow.point]);
  flow.t0 = millis();
  flow.state = FLOW_SETTLE;
}

FLASHMEM static void FlowFinish() {
  DAC::Release(OutPhys(flow.out));
  const Verdict v = CheckCalibration(flow.stats, flow.mv,
                                     OC::calibration_data.adc.pitch_cv_scale, OctMv(),
                                     &flow.fit, &flow.residual_mv);
  float noise = 0.f;
  for (int i = 0; i < 3; ++i) {
    const float pp = flow.stats[i].max_raw - flow.stats[i].min_raw;
    if (pp > noise) noise = pp;
  }
  Serial.printf("INCAL,RESULT,in=%d,out=%d,gain_uv=", flow.in + 1, flow.out + 1);
  PrintFixed(ScaleRound(flow.fit.gain * 1000.f, 1), 1);
  Serial.print(",offset_mv=");
  PrintFixed(ScaleRound(flow.fit.offset_mv, 2), 2);
  Serial.print(",resid_mv=");
  PrintFixed(ScaleRound(flow.residual_mv, 2), 2);
  Serial.print(",noise_pp=");
  PrintFixed(ScaleRound(noise, 2), 2);
  Serial.printf(",verdict=%s\r\n", VerdictName(v));
  if (v != VERDICT_OK) {
    Serial.printf("  NOT committed (%s). Limits: residual <= %d mV, gain within %d%% of the shared "
                  "scale, noise <= %d counts p-p, no point within %d counts of a rail.\r\n",
                  VerdictName(v), int(kMaxVerifyResidualMv), int(kGainTolerance * 100.f + 0.5f),
                  int(kMaxNoisePpCounts), int(kRailMarginCounts));
    FlowNext();
    return;
  }
  Serial.println("  Press y to store this calibration; any other key discards it.");
  flow.state = FLOW_WAIT_COMMIT;
}

FLASHMEM static void FlowCommit() {
  const int phys = InPhys(flow.in);
  CvMeterMath::InputCalibration &cal = OC::input_calibration;
  cal.gain[phys] = flow.fit.gain;
  cal.offset_mv[phys] = flow.fit.offset_mv;
  int64_t r = ScaleRound(flow.residual_mv, 1);
  if (r > 32767) r = 32767;
  if (r < -32768) r = -32768;
  cal.verify_mv10[phys] = int16_t(r);
  cal.source_output[phys] = uint8_t(flow.out + 1);
  cal.valid_mask |= uint8_t(1u << phys);
  input_calibration_save();
  Serial.printf("INCAL,SAVED,in=%d\r\n", flow.in + 1);
}

// Keys while the dialog is open. Every byte belongs to it: a stray byte
// during a measurement aborts rather than being read as a new command.
FLASHMEM static void FlowKey(int cmd) {
  switch (flow.state) {
    case FLOW_WAIT_PATCH:
      if (cmd == 'y' || cmd == 'Y') FlowSetPoint();
      else FlowAbort("cancelled at the patch prompt");
      break;
    case FLOW_WAIT_COMMIT:
      if (cmd == 'y' || cmd == 'Y') FlowCommit();
      else Serial.printf("INCAL,DISCARDED,in=%d\r\n", flow.in + 1);
      FlowNext();
      break;
    case FLOW_SETTLE:
    case FLOW_MEASURE:
      FlowAbort("key pressed during a measurement");
      break;
    default:
      break;
  }
}

FLASHMEM static void FlowTask(uint32_t now, const uint32_t *q) {
  if (flow.state == FLOW_SETTLE) {
    if (now - flow.t0 >= kSettleMs) {
      flow.win.Reset();
      flow.t0 = now;
      flow.state = FLOW_MEASURE;
    }
    return;
  }
  if (flow.state != FLOW_MEASURE) return;
  const int phys = InPhys(flow.in);
  if (q) flow.win.Add(q);
  if (now - flow.t0 < kMeasureMs) return;

  PointStats &s = flow.stats[flow.point];
  s.samples = flow.win.n;
  s.mean_raw = flow.win.n ? float(flow.win.sum[phys]) / (256.f * float(flow.win.n)) : 0.f;
  s.min_raw = flow.win.n ? float(flow.win.lo[phys]) * (1.f / 256.f) : 0.f;
  s.max_raw = flow.win.n ? float(flow.win.hi[phys]) * (1.f / 256.f) : 0.f;
  Serial.printf("INCAL,POINT,in=%d,%d,%ld,", flow.in + 1, flow.point + 1, (long)flow.mv[flow.point]);
  PrintFixed(ScaleRound(s.mean_raw, 2), 2);
  Serial.print(",");
  PrintFixed(ScaleRound(s.max_raw - s.min_raw, 2), 2);
  Serial.printf(",%lu\r\n", (unsigned long)s.samples);
  if (++flow.point < 3) FlowSetPoint();
  else FlowFinish();
}

// What the apps use for this input (InputCal.h): its curve file, this ICA1
// line, or the shared path. A curve takes precedence over the line.
FLASHMEM static const char *AppsSource(int phys) {
  switch (InputCal::SourceOf(phys)) {
    case InputCal::SOURCE_CURVE: return "curve";
    case InputCal::SOURCE_LINE: return "line";
    default: return "shared";
  }
}

FLASHMEM static void PrintCalibration() {
  const CvMeterMath::InputCalibration &cal = OC::input_calibration;
  Serial.printf("INCAL#,version=%u,mask=%02X,stored=%s,scale=%u,octave_mv=%ld\r\n", cal.version,
                cal.valid_mask, input_calibration_loaded ? "yes" : "no",
                (unsigned)OC::calibration_data.adc.pitch_cv_scale, (long)OctMv());
  for (int l = 0; l < kChannels; ++l) {
    const int phys = InPhys(l);
    int32_t lo, hi;
    InputRange(phys, &lo, &hi);
    if (InputCalValid(cal, phys)) {
      Serial.printf("INCAL,CH,%d,C,gain_uv=", l + 1);
      PrintFixed(ScaleRound(cal.gain[phys] * 1000.f, 1), 1);
      Serial.print(",offset_mv=");
      PrintFixed(ScaleRound(cal.offset_mv[phys], 2), 2);
      Serial.print(",verify_mv=");
      PrintFixed(cal.verify_mv10[phys], 1);
      Serial.printf(",src=%u,range=%ld..%ld,apps=%s\r\n", (unsigned)cal.source_output[phys],
                    (long)lo, (long)hi, AppsSource(phys));
    } else {
      Serial.printf("INCAL,CH,%d,S,adc_offset=%u,range=%ld..%ld,apps=%s\r\n", l + 1,
                    (unsigned)OC::calibration_data.adc.offset[phys], (long)lo, (long)hi,
                    AppsSource(phys));
    }
  }
}

// =============================================================================
// Frequency counter ('n')
//
// TR1 (the digital input FreqMeasure.begin() defaults to -- the same jack and
// the same driver instance the autotuner and the References app use). It is
// a shared resource: refuse when anybody else has it, and let go the moment
// an app switch takes it back.

static constexpr uint32_t kGateMs = 250;
static constexpr uint32_t kNoSignalMs = 2000;
static constexpr double kA4Hz = 440.0;

static bool freq_on = false;
static uint32_t freq_gen = 0;
static uint64_t freq_sum = 0;
static uint32_t freq_n = 0;
static uint32_t freq_win_ms = 0;
static uint32_t freq_last_edge_ms = 0;
static uint32_t freq_nosig_ms = 0;
static uint32_t freq_dropped = 0;
static bool freq_skip_first = true;

FLASHMEM static void FreqStart() {
  if (FreqMeasure.running_now()) {
    Serial.println("FREQ#,ERR,busy: an app (autotuner / References) owns the frequency counter");
    return;
  }
  FreqMeasure.begin(FreqMeasureClass::TR1);
  freq_gen = FreqMeasure.generation();
  freq_sum = 0;
  freq_n = 0;
  freq_win_ms = freq_last_edge_ms = freq_nosig_ms = millis();
  freq_dropped = 0;
  freq_skip_first = true;
  freq_on = true;
  Serial.printf("FREQ#,jack=TR1,gate_ms=%lu,a4=440 -- TR1 stops acting as a trigger input while this runs\r\n",
                (unsigned long)kGateMs);
}

FLASHMEM static void FreqStop() {
  if (!freq_on) return;
  freq_on = false;
  if (FreqMeasure.generation() == freq_gen) {
    FreqMeasure.end();
    DigitalInputs::reInit();
  }
  Serial.printf("FREQ#,stopped,dropped=%lu\r\n", (unsigned long)freq_dropped);
}

FLASHMEM static void FreqTask(uint32_t now) {
  if (FreqMeasure.generation() != freq_gen) {
    // An app switch called end() (and maybe the next app begin()) under us.
    freq_on = false;
    Serial.println("FREQ#,stopped,an app switch took the counter");
    return;
  }
  while (FreqMeasure.available()) {
    const uint32_t p = FreqMeasure.read();
    if (freq_skip_first) {      // measured from the counter's arbitrary start
      freq_skip_first = false;
      continue;
    }
    freq_sum += p;
    ++freq_n;
    freq_last_edge_ms = now;
  }
  char line[80];
  if (freq_n && now - freq_win_ms >= kGateMs) {
    const double hz = PeriodsToHz(freq_sum, freq_n, double(F_BUS_ACTUAL));
    const int len = FormatFreqLine(line, sizeof(line), now, hz, freq_n, kA4Hz);
    if (!TryWriteLine(line, len)) ++freq_dropped;
    freq_sum = 0;
    freq_n = 0;
    freq_win_ms = now;
  } else if (!freq_n && now - freq_last_edge_ms >= kNoSignalMs && now - freq_nosig_ms >= 1000) {
    const int len = snprintf(line, sizeof(line), "FREQ,%lu,NOSIG", (unsigned long)now);
    if (!TryWriteLine(line, len)) ++freq_dropped;
    freq_nosig_ms = now;
    freq_win_ms = now;
  }
}

// =============================================================================

FLASHMEM void Task() {
  const bool sampling = stream_on || flow.state == FLOW_MEASURE;
  const uint32_t now = millis();
  uint32_t q[kChannels];
  bool have = false;
  if (sampling) {
    const uint32_t us = micros();
    if (us - sample_last_us >= kSampleIntervalUs) {
      sample_last_us = us;
      ReadAll(q);
      have = true;
      if (stream_on) stream_win.Add(q);
    }
  }
  if (stream_on && int32_t(now - stream_next_ms) >= 0) {
    StreamEmit(now);
    stream_next_ms += kStreamPeriodMs;
    if (int32_t(now - stream_next_ms) > 100) stream_next_ms = now + kStreamPeriodMs;  // resync after a stall
  }
  if (flow.state != FLOW_IDLE) FlowTask(now, have ? q : nullptr);
  if (freq_on) FreqTask(now);
}

FLASHMEM bool Consume(int cmd) {
  if (flow.state != FLOW_IDLE) {
    FlowKey(cmd);
    return true;
  }
  if (d_state == D_WAIT_CH) {
    if (cmd == 'x' || cmd == 'X') {
      DAC::ReleaseAll();
      Serial.println("DAC,ALL,RELEASED");
      d_state = D_IDLE;
      return true;
    }
    d_out = ParseOutput(cmd);
    if (d_out < 0) {
      Serial.println("DAC,ERR,cancelled (expected an output 1-8 or a-h, or x)");
      d_state = D_IDLE;
      return true;
    }
    d_arg.Reset();
    d_state = D_WAIT_ARG;
    return true;
  }
  if (d_state == D_WAIT_ARG) {
    if ((cmd == 'x' || cmd == 'X') && !d_arg.digits && !d_arg.signed_) {
      ReleaseOutput(d_out);
      d_state = D_IDLE;
      return true;
    }
    const MvArg::Result r = d_arg.Feed(char(cmd));
    if (r == MvArg::MORE) return true;
    d_state = D_IDLE;
    if (r == MvArg::DONE) HoldOutput(d_out, d_arg.get());
    else Serial.println("DAC,ERR,cancelled (expected signed millivolts then Enter)");
    return true;
  }
  if (q_state == Q_WAIT_SUB) {
    q_state = Q_IDLE;
    if (cmd == 'p') { PrintCalibration(); return true; }
    if (cmd == 'c') {
      if (millis() - q_clear_arm_ms >= 3000 || !q_clear_arm_ms) {
        q_clear_arm_ms = millis() | 1;
        Serial.println("INCAL,ARMED,Qc again within 3s clears every input's Q calibration "
                       "(an input with a curve file keeps its curve until the curve is "
                       "cleared; the rest fall back to the shared scale + offset)");
        return true;
      }
      q_clear_arm_ms = 0;
      CvMeterMath::InputCalReset(OC::input_calibration);
      input_calibration_save();
      Serial.println("INCAL,CLEARED");
      return true;
    }
    if (cmd == 'a') { q_all = true; q_in = 0; q_state = Q_WAIT_OUT; return true; }
    const int in = ParseInput(cmd);
    if (in < 0) {
      Serial.println("INCAL,ERR,cancelled (Q then p, c, a<out> or <in><out>)");
      return true;
    }
    q_all = false;
    q_in = in;
    q_state = Q_WAIT_OUT;
    return true;
  }
  if (q_state == Q_WAIT_OUT) {
    q_state = Q_IDLE;
    const int out = ParseOutput(cmd);
    if (out < 0) {
      Serial.println("INCAL,ERR,cancelled (expected an output 1-8 or a-h)");
      return true;
    }
    FlowStart(q_in, out, q_all);
    return true;
  }
  return false;
}

FLASHMEM void Command(char key) {
  switch (key) {
    case 'v':
      if (stream_on) StreamStop();
      else StreamStart();
      break;
    case 'd':
      d_state = D_WAIT_CH;
      break;
    case 'Q':
      q_state = Q_WAIT_SUB;
      break;
    case 'n':
      if (freq_on) FreqStop();
      else FreqStart();
      break;
    default:
      break;
  }
}

FLASHMEM void PrintHelp() {
  Serial.println("-- CV meter (docs/CV-Meter.md) --");
  Serial.printf("v CV stream [%s]   n frequency counter on TR1 [%s]\r\n",
                stream_on ? "on" : "off", freq_on ? "on" : "off");
  Serial.printf("d<o><mV>Enter hold output   d<o>x release   dx release all [held %02lX]\r\n",
                (unsigned long)DAC::held_mask());
  Serial.printf("Q<i><o> calibrate input i from output o   Qa<o> all inputs   Qp print   QcQc clear [mask %02X]\r\n",
                OC::input_calibration.valid_mask);
}

}  // namespace CvMeter
}  // namespace OC

#endif  // CVMETER_ENABLED
