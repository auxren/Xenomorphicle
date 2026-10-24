// Host tests for the bench CV meter's pure logic (src/CvMeterMath.h): the two
// volts conversions, the two-point input calibration and every acceptance
// check, the EEPROM layout and the old-unit -> new-firmware path for the
// per-input calibration page, calibrated millivolts -> DAC code, the
// frequency counter's cents, and the exact text of the streamed lines
// (against fixtures/cvmeter_lines.txt, which the host tool's test parses).
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_cvmeter_math test_cvmeter_math.cpp ../src/CvMeterMath.cpp
//   && ./build/test_cvmeter_math
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/CvMeterMath.h"

// ---- the real EEPROM layout macros and page store, over a fake EEPROM ------
// OC_config.h's T4.1 branch is plain arithmetic on EEPROMStorage::LENGTH;
// util_pagestorage.h is the template every stored block goes through.
static uint8_t g_eeprom[4284];  // E2END + 1 on Teensy 4.x
struct EEPROMStorage {
  static const size_t LENGTH = sizeof(g_eeprom);
  static void update(size_t addr, const void *data, size_t length) {
    memcpy(g_eeprom + addr, data, length);
  }
  static void write(size_t addr, const void *data, size_t length) {
    memcpy(g_eeprom + addr, data, length);
  }
  static void read(size_t addr, void *data, size_t length) {
    memcpy(data, g_eeprom + addr, length);
  }
};
#define ARDUINO_TEENSY41
#include "../src/OC_config.h"
#undef ARDUINO_TEENSY41
#include "../src/util/util_pagestorage.h"

using namespace CvMeterMath;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= double(tol))

static constexpr uint16_t kScale = 15360;   // OC::ADC::kDefaultPitchCVScale
static constexpr int32_t kNlmOffset = 4095; // NLM / Xenomorpher default 0 V reading
static constexpr int32_t kOcOffset = 2730;  // stock o_C default (2/3 of the window)
static constexpr int32_t kOct = 1000;       // 1 V "octave" (stock hardware)
static constexpr int32_t kNlmOct = 1200;    // 1.2 V "octave" (Northern Light / Xenomorpher)

// ---- shared path --------------------------------------------------------------

static void test_shared_conversion() {
  NEAR(SharedGain(kScale, kOct), -1000.0 * 4096.0 / 4096.0 / 409.6, 1e-5);  // -2.44140625
  NEAR(SharedMillivolts(kNlmOffset, 4095.f, kScale, kOct), 0.0, 1e-4);
  NEAR(SharedMillivolts(kNlmOffset, 0.f, kScale, kOct), 9997.5586, 1e-3);   // one LSB short of 10 V
  NEAR(SharedMillivolts(kNlmOffset, 4095.f - 409.6f, kScale, kOct), 1000.0, 1e-2);
  // Same thing the apps compute: value_to_pitch() = (value * scale) >> 12,
  // 1536 pitch units to the volt.
  for (int value = 0; value <= 4095; value += 117) {
    const int32_t pitch = (value * kScale) >> 12;
    const float mv = SharedMillivolts(kNlmOffset, float(kNlmOffset - value), kScale, kOct);
    NEAR(mv, pitch * 1000.0 / 1536.0, 1000.0 / 1536.0);  // within the >>12 truncation
  }
}

static void test_shared_readable_range() {
  int32_t lo, hi;
  // 0 V on the 4095 rail with 1 V octaves: 10.000 V would be past the last
  // code. (The Xenomorpher uses 1.2 V octaves: test_northern_light_scale.)
  SharedReadableRange(kNlmOffset, kScale, kOct, &lo, &hi);
  CHECK(lo == 0);
  CHECK(hi == 9998);
  SharedReadableRange(kOcOffset, kScale, kOct, &lo, &hi);
  CHECK(lo == -3333 || lo == -3332);   // -3332.5 exactly; float decides
  CHECK(hi == 6665);
  // a user-calibrated scale moves both ends
  SharedReadableRange(4000, 16000, kOct, &lo, &hi);
  CHECK(lo < 0 && hi > 9000);
}

// ---- fit ----------------------------------------------------------------------

static void test_two_point_fit() {
  Fit f;
  // raw = 4095 - mv / 2.45 (a front end 0.4 % off nominal, 3 counts of offset)
  auto raw_of = [](float mv) { return 4092.f - mv / 2.45f; };
  CHECK(TwoPointFit(raw_of(1000.f), 1000.f, raw_of(9000.f), 9000.f, &f));
  NEAR(f.gain, -2.45, 1e-4);
  NEAR(FitMillivolts(f, raw_of(5000.f)), 5000.0, 1e-2);
  NEAR(FitMillivolts(f, 4092.f), 0.0, 1e-2);
  // order of the points does not matter
  Fit g;
  CHECK(TwoPointFit(raw_of(9000.f), 9000.f, raw_of(1000.f), 1000.f, &g));
  NEAR(g.gain, f.gain, 1e-6);
  NEAR(g.offset_mv, f.offset_mv, 1e-2);
  // input did not move
  CHECK(!TwoPointFit(2000.f, 1000.f, 2010.f, 9000.f, &f));
  CHECK(!TwoPointFit(2000.f, 1000.f, NAN, 9000.f, &f));
}

static void test_gain_plausible() {
  CHECK(GainPlausible(-2.44f));
  CHECK(GainPlausible(-kGainAbsMin));
  CHECK(GainPlausible(-kGainAbsMax));
  CHECK(!GainPlausible(-0.99f));
  CHECK(!GainPlausible(-6.01f));
  CHECK(!GainPlausible(2.44f));   // wrong direction for this front end
  CHECK(!GainPlausible(0.f));
  CHECK(!GainPlausible(NAN));
  CHECK(!GainPlausible(-INFINITY));
}

// Three ideal points for a given gain/offset, with an optional error on the
// verification reading (in mV) and a spread.
static void make_points(float gain, float offset_mv, const int32_t mv[3], float verify_err_mv,
                        float spread, PointStats p[3]) {
  for (int i = 0; i < 3; ++i) {
    float m = float(mv[i]) + (i == 2 ? verify_err_mv : 0.f);
    const float raw = (m - offset_mv) / gain;
    p[i].mean_raw = raw;
    p[i].min_raw = raw - spread / 2;
    p[i].max_raw = raw + spread / 2;
    p[i].samples = 3000;
  }
}

static void test_check_calibration_ok_and_residual_bound() {
  const int32_t mv[3] = { 1000, 9000, 5000 };
  const float gain = -2.43f, off = 9950.f;
  PointStats p[3];
  Fit f;
  float r;

  make_points(gain, off, mv, 0.f, 3.f, p);
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_OK);
  NEAR(f.gain, gain, 1e-4);
  NEAR(f.offset_mv, off, 0.05);
  NEAR(r, 0.0, 0.01);

  // just inside / just outside the stated residual bound
  // (residual = what the fitted line reads at the verify point - what was set)
  make_points(gain, off, mv, kMaxVerifyResidualMv - 0.5f, 3.f, p);
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_OK);
  NEAR(r, kMaxVerifyResidualMv - 0.5f, 0.05);
  make_points(gain, off, mv, -(kMaxVerifyResidualMv - 0.5f), 3.f, p);
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_OK);
  make_points(gain, off, mv, -(kMaxVerifyResidualMv + 0.5f), 3.f, p);
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_RESIDUAL);
  NEAR(r, -(kMaxVerifyResidualMv + 0.5f), 0.05);
  make_points(gain, off, mv, kMaxVerifyResidualMv + 0.5f, 3.f, p);
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_RESIDUAL);
}

static void test_check_calibration_gain_bounds() {
  // points low enough that every gain tried here stays off the rails
  const int32_t mv[3] = { 1000, 7000, 4000 };
  PointStats p[3];
  Fit f;
  float r;
  const float nominal = SharedGain(kScale, kOct);
  // 0 V at raw 4090 whatever the gain
  auto zero_at = [](float g) { return -g * 4090.f; };
  for (float k : { 0.86f, 0.95f, 1.0f, 1.05f, 1.14f }) {
    make_points(nominal * k, zero_at(nominal * k), mv, 0.f, 2.f, p);
    CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_OK);
    NEAR(f.gain, nominal * k, 1e-4);
  }
  for (float k : { 0.80f, 0.84f, 1.16f, 1.25f }) {   // e.g. the wrong jack, a divider in the path
    make_points(nominal * k, zero_at(nominal * k), mv, 0.f, 2.f, p);
    CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_GAIN);
  }
  // a user-calibrated shared scale moves the reference the tolerance applies to
  make_points(nominal * 1.25f, zero_at(nominal * 1.25f), mv, 0.f, 2.f, p);
  CHECK(CheckCalibration(p, mv, kScale * 5 / 4, kOct, &f, &r) == VERDICT_OK);
  // raw went UP with volts
  make_points(-nominal, nominal * 5.f, mv, 0.f, 2.f, p);
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_SIGN);
}

static void test_check_calibration_rejections() {
  const int32_t mv[3] = { 1000, 9000, 5000 };
  PointStats p[3];
  Fit f;
  float r;

  make_points(-2.44f, 9997.f, mv, 0.f, 2.f, p);
  p[1].samples = 0;
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_NO_SAMPLES);
  CHECK(f.gain == 0.f && r == 0.f);

  // top point clipped at raw 0
  make_points(-2.44f, 9997.f, mv, 0.f, 2.f, p);
  p[1].mean_raw = 3.f;
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_RAIL);
  make_points(-2.44f, 9997.f, mv, 0.f, 2.f, p);
  p[0].mean_raw = 4090.f;
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_RAIL);

  make_points(-2.44f, 9997.f, mv, 0.f, kMaxNoisePpCounts + 1.f, p);
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_NOISY);

  // nothing patched: every point reads the same
  for (int i = 0; i < 3; ++i) p[i] = { 2000.f, 1999.f, 2001.f, 3000 };
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_FLAT);

  // verification reading outside the fit points in raw (it jumped the
  // wrong way), even though the line itself looks fine
  make_points(-2.44f, 9997.f, mv, 0.f, 2.f, p);
  p[2].mean_raw = p[0].mean_raw + 5.f;
  CHECK(CheckCalibration(p, mv, kScale, kOct, &f, &r) == VERDICT_MONOTONIC);

  CHECK(std::string(VerdictName(VERDICT_RESIDUAL)) == "RESIDUAL");
  CHECK(std::string(VerdictName(VERDICT_OK)) == "OK");
}

static void test_choose_cal_points() {
  int32_t p[3];
  // NLM: DAC 0..10 V, input 0..9998 mV -> 1, 9 V fit, 5 V verify, on DAC nodes
  CHECK(ChooseCalPoints(0, 9998, 1000, p));
  CHECK(p[0] == 1000 && p[1] == 9000 && p[2] == 5000);
  // stock o_C input -3.3..+6.7 against a -3..+7 V DAC
  CHECK(ChooseCalPoints(-3000, 6665, 1000, p));
  CHECK(p[0] == -2000 && p[1] == 6000 && p[2] == 2000);
  for (int i = 0; i < 3; ++i) CHECK(p[i] > -3000 && p[i] < 6665);
  // +/-10 V hardware, 2 V nodes
  CHECK(ChooseCalPoints(-10000, 10000, 2000, p));
  CHECK(p[0] == -8000 && p[1] == 8000 && p[2] == 0);
  // never at the rails, whatever the snapping
  CHECK(ChooseCalPoints(0, 2500, 1000, p));
  for (int i = 0; i < 3; ++i) CHECK(p[i] >= 125 && p[i] <= 2375);
  CHECK(p[0] != p[1] && p[1] != p[2] && p[0] != p[2]);
  CHECK(!ChooseCalPoints(0, 1999, 1000, p));
  CHECK(!ChooseCalPoints(5000, 1000, 1000, p));
}

// ---- DAC ------------------------------------------------------------------------

static void test_mv_to_dac_code() {
  // a unipolar row (the NLM default codes), 1 V steps, kOctaveZero 0
  const uint16_t row[11] = { 200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100 };
  uint16_t c;
  CHECK(MillivoltsToDacCode(row, 11, 0, 1000, 0, &c) && c == 200);
  CHECK(MillivoltsToDacCode(row, 11, 0, 1000, 1000, &c) && c == 6500);
  CHECK(MillivoltsToDacCode(row, 11, 0, 1000, 10000, &c) && c == 63100);
  CHECK(MillivoltsToDacCode(row, 11, 0, 1000, 500, &c) && c == 3350);
  CHECK(MillivoltsToDacCode(row, 11, 0, 1000, 9001, &c) && c == 56906);  // 56900 + 6.2
  CHECK(!MillivoltsToDacCode(row, 11, 0, 1000, -1, &c));
  CHECK(!MillivoltsToDacCode(row, 11, 0, 1000, 10001, &c));
  // bipolar: kOctaveZero 3 -> -3..+7 V
  const uint16_t brow[11] = { 0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535 };
  CHECK(MillivoltsToDacCode(brow, 11, 3, 1000, 0, &c) && c == 19661);
  CHECK(MillivoltsToDacCode(brow, 11, 3, 1000, -3000, &c) && c == 0);
  CHECK(MillivoltsToDacCode(brow, 11, 3, 1000, -2500, &c) && c == 3277);  // 3276.5 rounds up
  CHECK(!MillivoltsToDacCode(brow, 11, 3, 1000, -3001, &c));
  // +/-10 V: 2 V per entry, kOctaveZero 5
  CHECK(MillivoltsToDacCode(brow, 11, 5, 2000, 0, &c) && c == 32768);
  CHECK(MillivoltsToDacCode(brow, 11, 5, 2000, 1000, &c) && c == 36045);   // halfway, 36044.5 up
  int32_t lo, hi;
  DacRange(11, 5, 2000, &lo, &hi);
  CHECK(lo == -10000 && hi == 10000);
  // the same interpolation PitchToScaledDAC uses: mid-step equals the mean
  CHECK(MillivoltsToDacCode(row, 11, 0, 1000, 4500, &c) && c == (25500 + 32700) / 2);
}

// ---- Northern Light / Xenomorpher: 1.2 V per table step and pitch octave --------

static void test_northern_light_scale() {
  CHECK(OctaveMv(false) == 1000 && OctaveMv(true) == 1200);
  CHECK(DacStepMv(false, false) == 1000);
  CHECK(DacStepMv(true, false) == 1200);
  CHECK(DacStepMv(false, true) == 2000);
  NEAR(SharedGain(kScale, kNlmOct), -2.9296875, 1e-6);
  // what gfxPrintVoltage() prints on this hardware: 1536 pitch units = 1.20 V
  // (value 409.6 -> pitch (409.6 * 15360) >> 12 = 1536)
  NEAR(SharedMillivolts(kNlmOffset, kNlmOffset - 409.6f, kScale, kNlmOct), 1200.0, 1e-2);
  // the firmware's default model for an NLM unit: 0 V on the raw-4095 rail,
  // 12 V of window -- 10 V lands at raw ~682, well clear of the other rail
  int32_t lo, hi;
  SharedReadableRange(kNlmOffset, kScale, kNlmOct, &lo, &hi);
  CHECK(lo == 0);
  CHECK(hi == 11997);
  NEAR(kNlmOffset - 10000.0 / 2.9296875, 681.6, 0.1);
  // CalibratePitch() on this hardware is fed 1.2 V and 3.6 V (two "octaves"):
  // whatever the true counts per volt k, the shared gain comes out -1000/k
  for (double k : { 330.0, 341.33, 360.0 }) {
    const int32_t c2 = 4095 - int32_t(1.2 * k + 0.5), c4 = 4095 - int32_t(3.6 * k + 0.5);
    const int32_t v2 = 4095 - c2, v4 = 4095 - c4;   // value = offset - raw
    const uint16_t scale = uint16_t((24 * 128 * 4096L) / (v4 - v2));
    NEAR(SharedGain(scale, kNlmOct), -1000.0 / k, 0.01);
  }
  // DAC: 1.2 V entries; calibration stays inside the metered entries 0..8
  const uint16_t row[11] = { 200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100 };
  uint16_t c;
  CHECK(MillivoltsToDacCode(row, 11, 0, kNlmOct, 1200, &c) && c == 6500);
  CHECK(MillivoltsToDacCode(row, 11, 0, kNlmOct, 9600, &c) && c == 50600);
  CHECK(MillivoltsToDacCode(row, 11, 0, kNlmOct, 10000, &c) && c == 52700);
  CHECK(MillivoltsToDacCode(row, 11, 0, kNlmOct, 12000, &c) && c == 63100);
  CHECK(!MillivoltsToDacCode(row, 11, 0, kNlmOct, 12001, &c));
  int32_t dlo, dhi;
  DacRange(kDacMeteredTopIndex + 1, 0, kNlmOct, &dlo, &dhi);
  CHECK(dlo == 0 && dhi == 9600);
  int32_t p[3];
  CHECK(ChooseCalPoints(dlo, dhi < hi ? dhi : hi, kNlmOct, p));
  CHECK(p[0] == 1200 && p[1] == 8400 && p[2] == 4800);
  // a full closed-loop fit at those points on a 1.2 V-scaled unit
  const int32_t mv[3] = { p[0], p[1], p[2] };
  PointStats ps[3];
  const float g = -2.95f;
  for (int i = 0; i < 3; ++i) {
    const float raw = 4093.f + float(mv[i]) / g;
    ps[i] = { raw, raw - 1.f, raw + 1.f, 3000 };
  }
  Fit f;
  float r;
  CHECK(CheckCalibration(ps, mv, kScale, kNlmOct, &f, &r) == VERDICT_OK);
  NEAR(f.gain, g, 1e-4);
  // the same unit judged against a 1.0 V scale would be 21 % off: rejected
  CHECK(CheckCalibration(ps, mv, kScale, kOct, &f, &r) == VERDICT_GAIN);
}

// ---- per-input path, storage, migration ----------------------------------------

static void test_input_millivolts_paths() {
  InputCalibration cal;
  InputCalReset(cal);
  CHECK(cal.version == InputCalibration::kVersion);
  CHECK(cal.valid_mask == 0);
  bool per = true;
  // uncalibrated: bit-for-bit the shared path
  for (float raw = 0.f; raw <= 4095.f; raw += 33.3f) {
    const float a = InputMillivolts(cal, 3, raw, kNlmOffset, kScale, kOct, &per);
    CHECK(!per);
    CHECK(a == SharedMillivolts(kNlmOffset, raw, kScale, kOct));
  }
  cal.gain[3] = -2.45f;
  cal.offset_mv[3] = 10010.f;
  cal.valid_mask = 1u << 3;
  NEAR(InputMillivolts(cal, 3, 4000.f, kNlmOffset, kScale, kOct, &per), 10010.0 - 2.45 * 4000.0, 1e-2);
  CHECK(per);
  // neighbours stay shared
  CHECK(InputMillivolts(cal, 2, 4000.f, kNlmOffset, kScale, kOct, &per) == SharedMillivolts(kNlmOffset, 4000.f, kScale, kOct));
  CHECK(!per);
  CHECK(!InputCalValid(cal, -1) && !InputCalValid(cal, 8));
}

static void test_sanitize() {
  InputCalibration cal;
  InputCalReset(cal);
  for (int i = 0; i < kChannels; ++i) { cal.gain[i] = -2.44f; cal.offset_mv[i] = 9997.f; }
  cal.valid_mask = 0xFF;
  cal.gain[1] = NAN;
  cal.gain[2] = 2.44f;
  cal.gain[3] = -0.5f;
  cal.offset_mv[4] = INFINITY;
  CHECK(InputCalSanitize(cal) == 4);
  CHECK(cal.valid_mask == 0xE1);
  // a page from a NEWER firmware with the same size: ignored, not trusted
  InputCalibration future = cal;
  future.version = 2;
  CHECK(InputCalSanitize(future) == 4);
  CHECK(future.valid_mask == 0);
  CHECK(future.version == InputCalibration::kVersion);
  InputCalibration zero;
  memset(&zero, 0, sizeof(zero));   // version 0: never written by anything
  zero.valid_mask = 0x01;
  CHECK(InputCalSanitize(zero) == 1 && zero.valid_mask == 0);
}

using InputCalStore = PageStorage<EEPROMStorage, EEPROM_INPUTCAL_START, EEPROM_INPUTCAL_END, InputCalibration>;

static void test_eeprom_layout() {
  // Every number an existing unit's EEPROM depends on.
  CHECK(EEPROM_CALIBRATIONDATA_START == 0);
  CHECK(EEPROM_CALIBRATIONDATA_END == 224);
  CHECK(EEPROM_APPDATA_START == 224);
  CHECK(EEPROM_APPDATA_BINARY_SIZE == 3900);
  CHECK(EEPROM_PRESETBUS_START == 4276);
  // the app-data page on the target: 12-byte header + 3900 + a 4-byte size_t
  const size_t app_page_end = EEPROM_APPDATA_START + 12 + 3900 + 4;
  CHECK(app_page_end == 4140);
  CHECK(app_page_end <= size_t(EEPROM_INPUTCAL_START));
  CHECK(EEPROM_INPUTCAL_END == EEPROM_PRESETBUS_START);
  CHECK(EEPROM_APPDATA_END == EEPROM_INPUTCAL_START);
  CHECK(InputCalStore::PAGESIZE == 108);
  CHECK(InputCalStore::PAGES == 1);
}

// An old unit: DAC/ADC page at 0, app data at 224, the gap that is now the
// input calibration region as the old firmware left it, the preset-bus record.
static void fill_old_unit(uint8_t gap_fill) {
  for (size_t i = 0; i < sizeof(g_eeprom); ++i) g_eeprom[i] = uint8_t(i * 7 + 3);
  for (size_t i = 4140; i < 4276; ++i) g_eeprom[i] = gap_fill;
}

static void test_old_unit_loads_no_input_cal() {
  const uint8_t fills[3] = { 0x00, 0xFF, 0x5A };
  for (uint8_t fill : fills) {
    fill_old_unit(fill);
    std::vector<uint8_t> before(g_eeprom, g_eeprom + sizeof(g_eeprom));
    InputCalStore store;
    InputCalibration cal;
    InputCalReset(cal);
    CHECK(!store.Load(cal));           // no valid page in the gap
    InputCalReset(cal);                // what calibration_load() does then
    CHECK(cal.valid_mask == 0);
    // loading wrote nothing
    CHECK(memcmp(before.data(), g_eeprom, sizeof(g_eeprom)) == 0);
    // and the meter converts exactly as before for every input
    bool per = true;
    for (int ch = 0; ch < kChannels; ++ch)
      CHECK(InputMillivolts(cal, ch, 1234.5f, kNlmOffset, kScale, kOct, &per) ==
            SharedMillivolts(kNlmOffset, 1234.5f, kScale, kOct) && !per);
  }
}

static void test_save_touches_only_its_region_and_round_trips() {
  fill_old_unit(0x00);
  std::vector<uint8_t> before(g_eeprom, g_eeprom + sizeof(g_eeprom));
  InputCalStore store;
  InputCalibration cal;
  InputCalReset(cal);
  CHECK(!store.Load(cal));
  InputCalReset(cal);
  cal.gain[5] = -2.4471f;
  cal.offset_mv[5] = 10003.25f;
  cal.verify_mv10[5] = -17;
  cal.source_output[5] = 6;
  cal.valid_mask = 1u << 5;
  CHECK(store.Save(cal));
  for (size_t i = 0; i < sizeof(g_eeprom); ++i) {
    const bool inside = i >= size_t(EEPROM_INPUTCAL_START) && i < size_t(EEPROM_INPUTCAL_END);
    if (!inside && g_eeprom[i] != before[i]) {
      CHECK(!"byte outside the input-calibration region changed");
      break;
    }
  }
  // DAC/ADC page and app data are byte-identical
  CHECK(memcmp(before.data(), g_eeprom, EEPROM_INPUTCAL_START) == 0);
  CHECK(memcmp(before.data() + EEPROM_PRESETBUS_START, g_eeprom + EEPROM_PRESETBUS_START, 8) == 0);

  InputCalStore again;
  InputCalibration back;
  InputCalReset(back);
  CHECK(again.Load(back));
  CHECK(InputCalSanitize(back) == 0);
  CHECK(memcmp(&back, &cal, sizeof(cal)) == 0);
  // a second save with no change writes nothing
  std::vector<uint8_t> mid(g_eeprom, g_eeprom + sizeof(g_eeprom));
  CHECK(!again.Save(back));
  CHECK(memcmp(mid.data(), g_eeprom, sizeof(g_eeprom)) == 0);
  // a flipped byte fails the checksum: back to "no input calibration"
  g_eeprom[EEPROM_INPUTCAL_START + 20] ^= 0x40;
  InputCalStore third;
  InputCalibration bad;
  InputCalReset(bad);
  CHECK(!third.Load(bad));
}

// ---- frequency ----------------------------------------------------------------

static void test_frequency_and_cents() {
  NEAR(PeriodsToHz(uint64_t(340909) * 110, 110, 150e6), 440.000117, 1e-5);
  CHECK(PeriodsToHz(0, 10, 150e6) == 0.0);
  CHECK(PeriodsToHz(100, 0, 150e6) == 0.0);
  int midi;
  double cents;
  CHECK(NearestNote(440.0, 440.0, &midi, &cents) && midi == 69);
  NEAR(cents, 0.0, 1e-9);
  CHECK(NearestNote(445.0, 440.0, &midi, &cents) && midi == 69);
  NEAR(cents, 19.5622, 1e-3);
  CHECK(NearestNote(437.0, 440.0, &midi, &cents) && midi == 69);
  NEAR(cents, -11.8443, 1e-3);
  CHECK(NearestNote(261.6256, 440.0, &midi, &cents) && midi == 60);
  NEAR(cents, 0.0, 1e-3);
  CHECK(NearestNote(27.5, 440.0, &midi, &cents) && midi == 21);
  CHECK(NearestNote(466.1638, 440.0, &midi, &cents) && midi == 70);
  // one cent is 0.0578 % -- resolvable: 1 tick in a 250 ms gate at 150 MHz
  // is 2.7e-8, five orders of magnitude finer
  CHECK(NearestNote(440.0 * std::pow(2.0, 1.0 / 1200.0), 440.0, &midi, &cents));
  NEAR(cents, 1.0, 1e-6);
  // the libm-free series against libm, 8 Hz .. 12.5 kHz in 1/7-semitone steps
  int worst_midi = 0;
  double worst = 0.0;
  for (double hz = 8.0; hz < 12500.0; hz *= std::pow(2.0, 1.0 / 84.0)) {
    const double semis = 12.0 * std::log2(hz / 440.0);
    const double near = std::floor(semis + 0.5);
    CHECK(NearestNote(hz, 440.0, &midi, &cents));
    if (midi != 69 + int(near)) worst_midi++;
    const double err = std::fabs(cents - 100.0 * (semis - near));
    if (err > worst) worst = err;
  }
  CHECK(worst_midi == 0);
  CHECK(worst < 1e-6);
  CHECK(NearestNote(4186.009, 440.0, &midi, &cents) && midi == 108);
  CHECK(!NearestNote(0.0, 440.0, &midi, &cents));
  CHECK(!NearestNote(-5.0, 440.0, &midi, &cents));
  char n[8];
  NoteName(69, n);  CHECK(std::string(n) == "A4");
  NoteName(60, n);  CHECK(std::string(n) == "C4");
  NoteName(61, n);  CHECK(std::string(n) == "C#4");
  NoteName(0, n);   CHECK(std::string(n) == "C-1");
  NoteName(-1, n);  CHECK(std::string(n) == "B-2");
  NoteName(127, n); CHECK(std::string(n) == "G9");
}

// ---- formatting ---------------------------------------------------------------

static void test_format_fixed() {
  char b[32];
  FormatFixed(b, sizeof(b), 0, 1);        CHECK(std::string(b) == "0.0");
  FormatFixed(b, sizeof(b), -5, 1);       CHECK(std::string(b) == "-0.5");
  FormatFixed(b, sizeof(b), 123456, 2);   CHECK(std::string(b) == "1234.56");
  FormatFixed(b, sizeof(b), -100, 2);     CHECK(std::string(b) == "-1.00");
  FormatFixed(b, sizeof(b), 7, 0);        CHECK(std::string(b) == "7");
  FormatFixed(b, sizeof(b), 4400001, 4);  CHECK(std::string(b) == "440.0001");
  FormatFixed(b, sizeof(b), INT64_MIN, 0);
  CHECK(std::string(b) == "-9223372036854775808");
  CHECK(ScaleRound(1.25, 1) == 13);
  CHECK(ScaleRound(-1.25, 1) == -13);
  CHECK(ScaleRound(0.04, 1) == 0);
  CHECK(ClipFlag(0, 1000 << 8) == 'L');
  CHECK(ClipFlag(1000 << 8, 4095u << 8) == 'H');
  CHECK(ClipFlag(1000 << 8, 4093u << 8) == '.');
  CHECK(ClipFlag(2u << 8, 4093u << 8) == '.');
}

static std::vector<std::string> golden_lines(const char *tag) {
  std::vector<std::string> out;
  std::ifstream f("fixtures/cvmeter_lines.txt");
  std::string line;
  const std::string prefix = std::string(tag) + ",";
  while (std::getline(f, line))
    if (line.compare(0, prefix.size(), prefix) == 0) out.push_back(line);
  return out;
}

static void test_cv_line_golden() {
  const uint32_t raw_q8[8] = {
    4095u << 8, 3685u * 256 + 128, 2047u * 256 + 192, 0,
    409u * 256 + 154, 100u * 256 + 3, 256, 4094u << 8
  };
  const int32_t mv10[8] = { 0, 10002, -33333, 99976, 5, -5, 123456, -1 };
  const char cal[8] = { 'C', 'C', 'S', 'S', 'S', 'S', 'S', 'S' };
  const char clip[8] = { 'H', '.', '.', 'L', '.', '.', '.', 'H' };
  char line[256];
  const int len = FormatCvLine(line, sizeof(line), 123456, 7, 2, raw_q8, mv10, cal, clip);
  const auto g = golden_lines("CV");
  CHECK(g.size() == 1);
  if (!g.empty()) {
    CHECK(std::string(line) == g[0]);
    if (std::string(line) != g[0]) printf("  got:  %s\n  want: %s\n", line, g[0].c_str());
  }
  CHECK(len == int(strlen(line)));
  // 22 fields
  int commas = 0;
  for (const char *p = line; *p; ++p) commas += (*p == ',');
  CHECK(commas == 21);
  // the worst case fits the firmware's 200-byte line buffer
  const uint32_t big[8] = { 4095u << 8, 4095u << 8, 4095u << 8, 4095u << 8,
                            4095u << 8, 4095u << 8, 4095u << 8, 4095u << 8 };
  const int32_t neg[8] = { -999999, -999999, -999999, -999999, -999999, -999999, -999999, -999999 };
  const int wl = FormatCvLine(line, sizeof(line), 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                              big, neg, cal, clip);
  CHECK(wl < 200 - 2);
  // a short buffer truncates cleanly, never overruns
  char tiny[16];
  memset(tiny, 'Z', sizeof(tiny));
  FormatCvLine(tiny, 10, 1, 2, 3, raw_q8, mv10, cal, clip);
  CHECK(strlen(tiny) < 10);
  CHECK(tiny[15] == 'Z');
}

static void test_freq_line_golden() {
  const auto g = golden_lines("FREQ");
  CHECK(g.size() == 4);
  struct { uint32_t ms; double hz; uint32_t n; } in[4] = {
    { 99, 440.0, 110 }, { 250, 445.0, 111 }, { 500, 437.0, 109 }, { 750, 27.5, 7 },
  };
  for (size_t i = 0; i < 4 && i < g.size(); ++i) {
    char line[96];
    FormatFreqLine(line, sizeof(line), in[i].ms, in[i].hz, in[i].n, 440.0);
    CHECK(std::string(line) == g[i]);
    if (std::string(line) != g[i]) printf("  got:  %s\n  want: %s\n", line, g[i].c_str());
  }
  char line[96];
  FormatFreqLine(line, sizeof(line), 1, 0.0, 0, 440.0);
  CHECK(std::string(line) == "FREQ,1,0.0000,0,-,-");
}

// ---- console argument ---------------------------------------------------------

static MvArg::Result feed(MvArg &a, const char *s) {
  MvArg::Result r = MvArg::MORE;
  for (; *s; ++s) {
    r = a.Feed(*s);
    if (r != MvArg::MORE) break;
  }
  return r;
}

static void test_mv_arg() {
  MvArg a;
  a.Reset(); CHECK(feed(a, "5000\r") == MvArg::DONE && a.get() == 5000);
  a.Reset(); CHECK(feed(a, "-2500\n") == MvArg::DONE && a.get() == -2500);
  a.Reset(); CHECK(feed(a, "+7 ") == MvArg::DONE && a.get() == 7);
  a.Reset(); CHECK(feed(a, "0\n") == MvArg::DONE && a.get() == 0);
  a.Reset(); CHECK(feed(a, "\n") == MvArg::BAD);        // nothing typed
  a.Reset(); CHECK(feed(a, "-\n") == MvArg::BAD);
  a.Reset(); CHECK(feed(a, "--5") == MvArg::BAD);
  a.Reset(); CHECK(feed(a, "5-") == MvArg::BAD);
  a.Reset(); CHECK(feed(a, "12a") == MvArg::BAD);
  a.Reset(); CHECK(feed(a, "50!") == MvArg::BAD);       // the tooling's canceller
  a.Reset(); CHECK(feed(a, "1234567") == MvArg::BAD);   // 7 digits
  a.Reset(); CHECK(feed(a, "999999\r") == MvArg::DONE && a.get() == 999999);
}

int main() {
  test_shared_conversion();
  test_shared_readable_range();
  test_two_point_fit();
  test_gain_plausible();
  test_check_calibration_ok_and_residual_bound();
  test_check_calibration_gain_bounds();
  test_check_calibration_rejections();
  test_choose_cal_points();
  test_mv_to_dac_code();
  test_northern_light_scale();
  test_input_millivolts_paths();
  test_sanitize();
  test_eeprom_layout();
  test_old_unit_loads_no_input_cal();
  test_save_touches_only_its_region_and_round_trips();
  test_frequency_and_cents();
  test_format_fixed();
  test_cv_line_golden();
  test_freq_line_golden();
  test_mv_arg();
  printf("test_cvmeter_math: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
