// The per-input calibration every app sees. See InputCal.h.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define INPUTCAL_CODE FLASHMEM
#else
#define INPUTCAL_CODE
#endif
#include <atomic>

#include "InputCal.h"
#include "AdcSlotMap.h"
#include "InputCurveStore.h"

#include <cmath>

#if defined(__IMXRT1062__)
#include "InputCurveFiles.h"
#include "OC_ADC.h"
#include "OC_app_switcher.h"
#include "OC_calibration.h"
#include "OC_gpio.h"
#endif

namespace InputCal {

namespace CR = CalRoutineMath;
namespace CM = CvMeterMath;

// Plain globals: DTCM on the Teensy 4.1 (DMAMEM would put them in RAM2, behind
// the cache, which the ISR must not pay for).
int32_t value_q8[kInputs][CR::kCurvePoints];
volatile uint8_t calibrated_mask = 0;

// Loop context only.
static uint8_t source_[kInputs];
static int32_t octave_mv_ = 1200;

#if !defined(__IMXRT1062__)
void (*test_write_hook)(int phys, int index) = nullptr;
#endif

// Orders the mask and table stores against the ISR. The ISR runs on the same
// core, so this has to stop the compiler (and, for good measure, the M7's
// store buffer) from moving the table writes across the mask writes.
static inline void Barrier() {
#if defined(__arm__)
  __asm__ volatile("dmb" ::: "memory");
#else  // host tests, and the simulator (which defines __IMXRT1062__ on a desktop)
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

// Strictly falling (volts rise as raw falls) and inside the range the ISR's
// arithmetic is proven for (CalRoutineMath::ApplyValueTable).
INPUTCAL_CODE static bool TableUsable(const int32_t q8[CR::kCurvePoints]) {
  for (int i = 0; i < CR::kCurvePoints; ++i) {
    if (q8[i] > CR::kMaxValueQ8 || q8[i] < -CR::kMaxValueQ8) return false;
    if (i && !(q8[i] < q8[i - 1])) return false;
  }
  return true;
}

// The tables are built for 1.2 V per octave (CalRoutineMath's 4096 / 12000);
// other hardware has its 409.6 value counts per 1.0 V.
INPUTCAL_CODE static bool Rescale(int32_t q8[CR::kCurvePoints], int32_t octave_mv) {
  if (octave_mv == 1200) return true;
  for (int i = 0; i < CR::kCurvePoints; ++i) {
    const double v = double(q8[i]) * 1200.0 / double(octave_mv);
    if (!(std::fabs(v) <= double(CR::kMaxValueQ8))) return false;
    q8[i] = int32_t(std::lround(v));
  }
  return true;
}

// The ICA1 line, checked before any conversion: lround of a huge or
// non-finite double is undefined.
INPUTCAL_CODE static bool LineTable(const CM::InputCalibration &ica, int ch,
                                    int32_t q8[CR::kCurvePoints]) {
  if (!(ica.valid_mask & (1u << ch))) return false;
  const float g = ica.gain[ch], off = ica.offset_mv[ch];
  if (!CM::GainPlausible(g) || !std::isfinite(off)) return false;
  // Coarse bound so the conversion is defined; TableUsable() has the exact
  // word (kMaxValueQ8 is ~22.9 V at 1.2 V/oct).
  const double lim = 30000.0;
  const double at0 = double(off), at4096 = double(g) * 4096.0 + double(off);
  if (!(std::fabs(at0) < lim && std::fabs(at4096) < lim)) return false;
  const CM::Fit line = { g, off };
  CR::LineToValueTable(line, q8);
  return true;
}

INPUTCAL_CODE static void Publish(int ch, const int32_t q8[CR::kCurvePoints], bool use) {
  const uint8_t bit = uint8_t(1u << ch);
  calibrated_mask = uint8_t(calibrated_mask & ~bit);
  Barrier();
  if (!use) return;
  for (int i = 0; i < CR::kCurvePoints; ++i) {
    value_q8[ch][i] = q8[i];
#if !defined(__IMXRT1062__)
    if (test_write_hook) test_write_hook(ch, i);
#endif
  }
  Barrier();
  calibrated_mask = uint8_t(calibrated_mask | bit);
}

INPUTCAL_CODE void ReloadFrom(const CM::InputCalibration &ica, const InputCurveRecord *curves,
                              int32_t octave_mv) {
  octave_mv_ = octave_mv > 0 ? octave_mv : 1200;
  // ICA1 is version-checked and sanitized by its loader; nothing here relies
  // on that. Only bits 0-7 of either mask are channels (kInputs == 8).
  const bool ica_ok = ica.version == CM::InputCalibration::kVersion;
  // A fit measured before the input-map fix (AdcSlotMap.h) may be the
  // neighbouring jack's: not applied, so that input stays on the shared scale
  // until it is measured again. OC::input_map_check() names these at boot.
  // Only bench units can have one (no release image wrote ICA1 before it).
  const uint8_t ica_stale = IcaPredatesFix(ica);
  for (int ch = 0; ch < kInputs; ++ch) {
    int32_t q8[CR::kCurvePoints] = {};
    Source src = SOURCE_NONE;
    if (octave_mv > 0) {
      if (curves && (curves->valid_mask & (1u << ch))) {
        CR::CurveToValueTable(curves->mv_x2[ch], q8);
        if (Rescale(q8, octave_mv) && TableUsable(q8)) src = SOURCE_CURVE;
      }
      if (src == SOURCE_NONE && ica_ok && !(ica_stale & (1u << ch)) && LineTable(ica, ch, q8) &&
          Rescale(q8, octave_mv) && TableUsable(q8))
        src = SOURCE_LINE;
    }
    source_[ch] = src;
    Publish(ch, q8, src != SOURCE_NONE);
  }
}

INPUTCAL_CODE Source SourceOf(int phys) {
  if (!Calibrated(phys)) return SOURCE_NONE;
  return Source(source_[phys]);
}

INPUTCAL_CODE float Millivolts(int phys, float raw) {
  if (!Calibrated(phys)) return 0.f;
  if (!(raw >= 0.f)) raw = 0.f;  // also NaN
  if (raw > 4096.f) raw = 4096.f;
  int seg = int(raw) >> CR::kCurveShift;
  if (seg > CR::kCurvePoints - 2) seg = CR::kCurvePoints - 2;
  const int32_t *q8 = value_q8[phys];
  const float f = (raw - float(seg << CR::kCurveShift)) * (1.f / 256.f);
  const float value = (float(q8[seg]) + f * float(q8[seg + 1] - q8[seg])) * (1.f / 256.f);
  return value * (float(10 * octave_mv_) / 4096.f);
}

#if defined(__IMXRT1062__)
INPUTCAL_CODE void Reload() {
  // LittleFS is not mounted yet on the first call at boot: the open fails
  // cleanly (LittleFS::open checks), and setup() calls this again once it is.
  DMAMEM static InputCurveRecord rec;  // 300 bytes, off the loop stack and out of DTCM
  const bool have = InputCurveLoadFromFlash(&rec, nullptr);
  ReloadFrom(OC::input_calibration, have ? &rec : nullptr, NorthernLightModular ? 1200 : 1000);
}

// Not FLASHMEM: a direct reader may call it from the core ISR (Sequins'
// Process), and it is a few loads and one table lookup.
int32_t DirectValue(int phys) {
  const OC::AppBase *app = OC::app_switcher.current_app();
  const bool filter = !app || app->io_settings().adc_filter_enabled(phys);
  const uint32_t raw12 = filter ? OC::ADC::smoothed_raw_value(phys) : OC::ADC::raw_value(phys);
  return CR::ApplyValueTable(value_q8[phys], raw12);
}
#endif

}  // namespace InputCal
