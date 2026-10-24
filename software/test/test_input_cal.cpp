// Host tests for the per-input calibration every app sees (src/InputCal.h):
// table building from the curve file or the ICA1 line, the bit-exact stock
// path when nothing is stored, an old unit's EEPROM image, and the publish
// order the core ISR relies on. Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_input_cal
//     test_input_cal.cpp ../src/InputCal.cpp ../src/CalRoutineMath.cpp
//     ../src/CvMeterMath.cpp ../src/InputCurveStore.cpp
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/CvMeterMath.h"
#include "../src/CalRoutineMath.h"
#include "../src/InputCal.h"
#include "../src/InputCurveStore.h"

// ---- the real EEPROM layout macros and page store, over a fake EEPROM ------
// (as test_cvmeter_math.cpp does)
static uint8_t g_eeprom[4284];  // E2END + 1 on Teensy 4.x
struct EEPROMStorage {
  static const size_t LENGTH = sizeof(g_eeprom);
  static void update(size_t addr, const void *data, size_t length) { memcpy(g_eeprom + addr, data, length); }
  static void write(size_t addr, const void *data, size_t length) { memcpy(g_eeprom + addr, data, length); }
  static void read(size_t addr, void *data, size_t length) { memcpy(data, g_eeprom + addr, length); }
};
#define ARDUINO_TEENSY41
#include "../src/OC_config.h"
#undef ARDUINO_TEENSY41
#include "../src/util/util_pagestorage.h"

namespace CM = CvMeterMath;
namespace CR = CalRoutineMath;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs(double(a) - double(b)) <= double(tol))

static constexpr int32_t kOct = 1200;

struct MemStorage : CurveStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  bool Read(const char *n, void *b, size_t len) override {
    auto it = files.find(n);
    if (it == files.end() || it->second.size() != len) return false;
    memcpy(b, it->second.data(), len);
    return true;
  }
  bool Write(const char *n, const void *b, size_t len) override {
    files[n] = std::vector<uint8_t>((const uint8_t *)b, (const uint8_t *)b + len);
    return true;
  }
};

// Stock arithmetic (OC_ADC.h): value = offset - raw, pitch = value * scale >> 12.
static int32_t stock_value(int32_t offset, uint32_t raw) { return offset - int32_t(raw); }
static int32_t stock_pitch(int32_t value, uint16_t scale) { return (value * scale) >> 12; }

// What OC::ADC::calibrated_value/_pitch and IO::Read compute for one input.
static void read_channel(int phys, uint32_t raw, int32_t offset, uint16_t scale,
                         int32_t *value, int32_t *pitch) {
  *value = InputCal::Value(phys, raw, stock_value(offset, raw));
  *pitch = InputCal::Calibrated(phys) ? InputCal::Pitch(*value) : stock_pitch(*value, scale);
}

static CM::InputCalibration empty_ica() {
  CM::InputCalibration c;
  CM::InputCalReset(c);
  return c;
}

// A Xenomorpher-like input: 0 V at raw 4093, -2.94 mV per count, with a bend.
static double bent_mv(double raw) {
  return -2.94 * (raw - 4093.0) + 6.0 * std::sin(raw * 3.14159265358979 / 4096.0 * 2.0);
}

static InputCurveRecord curve_record(uint8_t mask) {
  InputCurveRecord r;
  memset(&r, 0, sizeof r);
  r.valid_mask = mask;
  for (int ch = 0; ch < 8; ++ch)
    for (int i = 0; i < CR::kCurvePoints; ++i)
      r.mv_x2[ch][i] = int16_t(std::lround(2.0 * (bent_mv(256.0 * i) + ch)));
  InputCurveEncode(r);
  return r;
}

// Reference: exact piecewise-linear interpolation of the stored mV x 2 curve,
// in value counts.
static double curve_ref_value(const InputCurveRecord &r, int ch, uint32_t raw) {
  const int seg = std::min(int(raw >> 8), CR::kCurvePoints - 2);
  const double f = (raw - 256.0 * seg) / 256.0;
  const double mv = (r.mv_x2[ch][seg] + f * (r.mv_x2[ch][seg + 1] - r.mv_x2[ch][seg])) / 2.0;
  return mv * 4096.0 / 12000.0;
}

// ---------------------------------------------------------------------------

static void test_uncalibrated_bit_exact() {
  InputCal::ReloadFrom(empty_ica(), nullptr, kOct);
  CHECK(InputCal::calibrated_mask == 0);
  const int32_t offsets[3] = { 4095, 4031, 2730 };
  const uint16_t scales[3] = { 15360, 14987, 16111 };
  for (int k = 0; k < 3; ++k)
    for (int ch = 0; ch < InputCal::kInputs; ++ch) {
      CHECK(!InputCal::Calibrated(ch));
      CHECK(InputCal::SourceOf(ch) == InputCal::SOURCE_NONE);
      int bad = 0;
      for (uint32_t raw = 0; raw <= 4095; ++raw) {
        int32_t v, p;
        read_channel(ch, raw, offsets[k], scales[k], &v, &p);
        const int32_t sv = stock_value(offsets[k], raw);
        if (v != sv || p != stock_pitch(sv, scales[k])) ++bad;
      }
      CHECK(bad == 0);
    }
  // An empty curve file (valid record, no bits) changes nothing either.
  InputCurveRecord none = curve_record(0);
  InputCal::ReloadFrom(empty_ica(), &none, kOct);
  CHECK(InputCal::calibrated_mask == 0);
}

static void test_curve_applied() {
  InputCurveRecord r = curve_record(0x04);
  InputCal::ReloadFrom(empty_ica(), &r, kOct);
  CHECK(InputCal::calibrated_mask == 0x04);
  CHECK(InputCal::SourceOf(2) == InputCal::SOURCE_CURVE);
  double worst = 0, worst_mv = 0;
  for (uint32_t raw = 0; raw <= 4095; ++raw) {
    const double ref = curve_ref_value(r, 2, raw);
    const double err = std::fabs(InputCal::Value(2, raw, -99999) - ref);
    if (err > worst) worst = err;
    const double mv_err = std::fabs(InputCal::Millivolts(2, float(raw) + 0.5f) -
                                    0.5 * (curve_ref_value(r, 2, raw) + curve_ref_value(r, 2, raw + 1)) *
                                        12000.0 / 4096.0);
    if (raw < 4095 && mv_err > worst_mv) worst_mv = mv_err;
  }
  CHECK(worst <= 0.51);  // rounded to the nearest count (plus the q8 quantum)
  CHECK(worst <= 1.0);   // the brief's bound
  CHECK(worst_mv <= 0.05);
  // Pitch of a calibrated input: the nominal scale on true volts.
  // Pitch() is (value * 15360) >> 12 for every value a table can give
  // (|value| <= kMaxValueQ8 / 256 ~ 7813).
  int pitch_bad = 0;
  for (int32_t v = -8000; v <= 8000; ++v)
    if (InputCal::Pitch(v) != (v * 15360) >> 12) ++pitch_bad;
  CHECK(pitch_bad == 0);
  // 1.2 V reads one octave (1536) within a pitch unit or two.
  int32_t best = 0; double best_d = 1e9;
  for (uint32_t raw = 0; raw <= 4095; ++raw) {
    const double d = std::fabs(curve_ref_value(r, 2, raw) * 12000.0 / 4096.0 - 1200.0);
    if (d < best_d) { best_d = d; best = int32_t(raw); }
  }
  NEAR(InputCal::Pitch(InputCal::Value(2, uint32_t(best), 0)), 1536, 5);
  // A raw past 4095 is clamped, never reads past the table.
  CHECK(InputCal::Value(2, 9999, 0) == InputCal::Value(2, 4095, 0));
}

static CM::InputCalibration line_ica(int ch, float gain, float offset_mv) {
  CM::InputCalibration c = empty_ica();
  c.gain[ch] = gain;
  c.offset_mv[ch] = offset_mv;
  c.valid_mask = uint8_t(1u << ch);
  return c;
}

static void test_line_fallback() {
  const float g = -2.951f, off = 12071.5f;
  CM::InputCalibration ica = line_ica(5, g, off);
  InputCal::ReloadFrom(ica, nullptr, kOct);
  CHECK(InputCal::calibrated_mask == 0x20);
  CHECK(InputCal::SourceOf(5) == InputCal::SOURCE_LINE);
  double worst = 0;
  for (uint32_t raw = 0; raw <= 4095; ++raw) {
    const double ref = (double(g) * raw + off) * 4096.0 / 12000.0;
    worst = std::max(worst, std::fabs(InputCal::Value(5, raw, 0) - ref));
  }
  CHECK(worst <= 0.51);
  NEAR(InputCal::Millivolts(5, 1234.25f), g * 1234.25 + off, 0.02);
  // A curve file without this input's bit: still the line.
  InputCurveRecord r = curve_record(0x01);
  InputCal::ReloadFrom(ica, &r, kOct);
  CHECK(InputCal::calibrated_mask == 0x21);
  CHECK(InputCal::SourceOf(5) == InputCal::SOURCE_LINE);
  CHECK(InputCal::SourceOf(0) == InputCal::SOURCE_CURVE);
  // With the bit: the curve wins over the line.
  r = curve_record(0x20);
  InputCal::ReloadFrom(ica, &r, kOct);
  CHECK(InputCal::SourceOf(5) == InputCal::SOURCE_CURVE);
  NEAR(InputCal::Value(5, 2000, 0), curve_ref_value(r, 5, 2000), 0.51);
  // Stock 1 V/oct hardware: the same volts are 1.2x the value counts.
  InputCal::ReloadFrom(ica, nullptr, 1000);
  NEAR(InputCal::Value(5, 2000, 0), (double(g) * 2000 + off) * 4096.0 / 10000.0, 0.51);
  NEAR(InputCal::Millivolts(5, 2000.f), g * 2000.0 + off, 0.02);
  InputCal::ReloadFrom(ica, nullptr, 0);
  CHECK(InputCal::calibrated_mask == 0);
}

static void test_mixed_channels() {
  InputCurveRecord r = curve_record(0x01);
  CM::InputCalibration ica = line_ica(3, -2.93f, 11999.f);
  InputCal::ReloadFrom(ica, &r, kOct);
  CHECK(InputCal::calibrated_mask == 0x09);
  for (int ch = 0; ch < InputCal::kInputs; ++ch) {
    const bool cal = ch == 0 || ch == 3;
    CHECK(InputCal::Calibrated(ch) == cal);
    if (!cal) {
      int bad = 0;
      for (uint32_t raw = 0; raw <= 4095; ++raw) {
        int32_t v, p;
        read_channel(ch, raw, 4095, 15360, &v, &p);
        if (v != stock_value(4095, raw) || p != stock_pitch(v, 15360)) ++bad;
      }
      CHECK(bad == 0);
    }
  }
  NEAR(InputCal::Value(0, 777, 0), curve_ref_value(r, 0, 777), 0.51);
  NEAR(InputCal::Value(3, 777, 0), (-2.93 * 777 + 11999.0) * 4096.0 / 12000.0, 0.51);
}

static void test_untrusted_data_ignored() {
  // Lines that are not finite, implausible or would overflow the table.
  CM::InputCalibration ica = empty_ica();
  ica.valid_mask = 0xFF;
  ica.gain[0] = NAN;            ica.offset_mv[0] = 12000.f;
  ica.gain[1] = -2.9f;          ica.offset_mv[1] = INFINITY;
  ica.gain[2] = +2.9f;          ica.offset_mv[2] = 0.f;      // wrong sign
  ica.gain[3] = -40.f;          ica.offset_mv[3] = 12000.f;  // implausible gain
  ica.gain[4] = -2.9f;          ica.offset_mv[4] = 3.0e9f;   // overflows the table
  ica.gain[5] = -2.9f;          ica.offset_mv[5] = -1.0e7f;
  ica.gain[6] = -2.9f;          ica.offset_mv[6] = 11900.f;  // fine
  ica.gain[7] = -2.9f;          ica.offset_mv[7] = 30000.f;  // past kMaxValueQ8 at raw 0
  InputCal::ReloadFrom(ica, nullptr, kOct);
  CHECK(InputCal::calibrated_mask == 0x40);
  // A curve that is not strictly falling falls back to the line, else stock.
  InputCurveRecord r = curve_record(0xFF);
  r.mv_x2[6][7] = r.mv_x2[6][8];      // flat segment
  r.mv_x2[1][3] = int16_t(r.mv_x2[1][2] + 10);  // rises
  InputCurveEncode(r);
  InputCal::ReloadFrom(ica, &r, kOct);
  CHECK(InputCal::SourceOf(6) == InputCal::SOURCE_LINE);
  CHECK(InputCal::SourceOf(1) == InputCal::SOURCE_NONE);
  CHECK(InputCal::calibrated_mask == 0xFD);
  // A table from a previous load does not survive a reload that drops it.
  InputCal::ReloadFrom(empty_ica(), nullptr, kOct);
  CHECK(InputCal::calibrated_mask == 0);
}

using InputCalStore = PageStorage<EEPROMStorage, EEPROM_INPUTCAL_START, EEPROM_INPUTCAL_END,
                                  CM::InputCalibration>;

static void test_old_unit_eeprom_image() {
  const uint8_t fills[3] = { 0x00, 0xFF, 0x5A };
  for (uint8_t fill : fills) {
    // Start from a calibrated state so a stale mask would show.
    InputCurveRecord r = curve_record(0xFF);
    InputCal::ReloadFrom(empty_ica(), &r, kOct);
    CHECK(InputCal::calibrated_mask == 0xFF);

    // An old unit: DAC/ADC page at 0, app data at 224, the gap that is now
    // the ICA1 region as the old firmware left it.
    for (size_t i = 0; i < sizeof(g_eeprom); ++i) g_eeprom[i] = uint8_t(i * 7 + 3);
    for (size_t i = 4140; i < 4276; ++i) g_eeprom[i] = fill;
    // What calibration_load() does.
    InputCalStore store;
    CM::InputCalibration ica;
    CM::InputCalReset(ica);
    if (store.Load(ica)) CM::InputCalSanitize(ica);
    else CM::InputCalReset(ica);
    // No curve files on an old unit.
    MemStorage fs;
    InputCurveRecord rec;
    const bool have = InputCurveLoad(fs, &rec, nullptr);
    CHECK(!have);
    InputCal::ReloadFrom(ica, have ? &rec : nullptr, kOct);
    CHECK(InputCal::calibrated_mask == 0);
  }
}

static int hook_calls = 0, hook_bad = 0;
static int hook_last_phys = -1;
static void publish_hook(int phys, int index) {
  ++hook_calls;
  if (InputCal::calibrated_mask & (1u << phys)) ++hook_bad;
  if (index < 0 || index >= CR::kCurvePoints) ++hook_bad;
  hook_last_phys = phys;
}

static void test_publish_order() {
  InputCurveRecord r = curve_record(0x81);
  CM::InputCalibration ica = line_ica(4, -2.95f, 12010.f);
  // First load from nothing, then reload over already-set bits.
  for (int pass = 0; pass < 2; ++pass) {
    hook_calls = hook_bad = 0;
    InputCal::test_write_hook = publish_hook;
    InputCal::ReloadFrom(ica, &r, kOct);
    InputCal::test_write_hook = nullptr;
    CHECK(hook_calls == 3 * CR::kCurvePoints);  // inputs 0, 4 and 7
    CHECK(hook_bad == 0);
    CHECK(hook_last_phys == 7);
    CHECK(InputCal::calibrated_mask == 0x91);
  }
}

int main() {
  test_uncalibrated_bit_exact();
  test_curve_applied();
  test_line_fallback();
  test_mixed_channels();
  test_untrusted_data_ignored();
  test_old_unit_eeprom_image();
  test_publish_order();
  printf("test_input_cal: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
