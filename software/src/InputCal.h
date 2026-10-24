#ifndef INPUTCAL_H_
#define INPUTCAL_H_

#include <stdint.h>

#include "CalRoutineMath.h"
#include "CvMeterMath.h"

struct InputCurveRecord;

// ---------------------------------------------------------------------------
// The per-input calibration every app sees (design/cal-routine-design.md 6.5,
// 6.6). Each calibrated input gets a 17-point integer table of value counts
// x 256 at raw 0, 256, ... 4096, built in loop context from the input's curve
// (ICURVE_A/B.BIN) when it has one, else from its ICA1 line fit. The core ISR
// (OC::IO::Read) and the direct readers' accessor (OC::ADC::calibrated_value)
// apply it with integers only; the floats never reach the ISR.
//
// Indexed by PHYSICAL ADC channel -- the index of OC::ADC's raw_/smoothed_
// arrays, which is also IO::Read's loop index and the argument apps pass to
// OC::ADC::value() (the ADC_CHANNEL_n globals hold it per panel label, remapped
// by flip mode). ICA1 and the curve file are indexed the same way.
//
// A channel whose bit is clear in calibrated_mask runs the stock code
// unchanged. With nothing stored (an old unit) the mask is 0.
//
// Units: value = true mV * 4096 / (10 * octave_mv), 409.6 counts per octave,
// the stock model's units (0 V reads 0 on a Xenomorpher, 1.2 V per octave);
// pitch = value * 15360 >> 12, 1536 pitch units per octave.
// ---------------------------------------------------------------------------

namespace InputCal {

constexpr int kInputs = 8;
constexpr int32_t kPitchScale = 15360;  // the nominal pitch_cv_scale

// Both in DTCM (plain globals on the Teensy 4.1 are; never DMAMEM): the ISR
// reads them every tick. Written only by Reload*(), in loop context.
extern int32_t value_q8[kInputs][CalRoutineMath::kCurvePoints];
extern volatile uint8_t calibrated_mask;

enum Source : uint8_t { SOURCE_NONE = 0, SOURCE_CURVE = 1, SOURCE_LINE = 2 };

// Rebuilds every input's table. Per input: the curve when curves is non-null,
// its bit is set and the table it gives is usable; else the ICA1 line when
// that channel's fit is valid and usable; else uncalibrated. Nothing is
// trusted: only bits 0-7 are channels, a line whose fit predates the input-map
// fix (AdcSlotMap.h, IcaPredatesFix), a non-finite or implausible line, an
// offset whose table would overflow, and any table that is not strictly
// falling (volts rise as raw falls) or exceeds CalRoutineMath::kMaxValueQ8
// are all treated as "no calibration" for that input. source_output[] is not
// used. octave_mv: 1200 on Northern Light hardware (the Xenomorpher), 1000
// otherwise; a non-positive octave_mv leaves every input uncalibrated.
//
// Publishes per input: clear its mask bit, write its table, then set the bit,
// so an ISR that preempts this never reads a half-written table (it sees the
// bit clear and runs the stock path for that tick).
void ReloadFrom(const CvMeterMath::InputCalibration &ica, const InputCurveRecord *curves,
                int32_t octave_mv);

#if defined(__IMXRT1062__)
// ReloadFrom(OC::input_calibration, the curve files if LittleFS has one valid,
// the hardware's octave). Loop context. Called by OC::calibration_load()
// (before LittleFS is mounted at boot, so only ICA1 then), again once LittleFS
// is up (setup()), and after every ICA1 save. Whatever writes the curve
// files (Backup's restore does, through calibration_load(); Cal Routine's
// save must) calls it afterwards, or the apps keep the old tables.
void Reload();

// A calibrated input's value from the reading IO::Read would use right now
// (smoothed or unsmoothed, per the current app's IO setting): the body of
// OC::ADC::calibrated_value(), out of line so each direct reader inlines only
// the mask test. Calibrated inputs only. In ITCM: a reader may be in the ISR.
int32_t DirectValue(int phys);
#endif

// Where an input's table came from (loop context; for the console).
Source SourceOf(int phys);

// always_inline, here and in Value(): see CalRoutineMath::ApplyValueTable.
__attribute__((always_inline)) inline bool Calibrated(int phys) {
  return phys >= 0 && phys < kInputs && (calibrated_mask & (1u << phys));
}

// The calibrated value when the input is calibrated, else uncal_value as given.
__attribute__((always_inline)) inline int32_t Value(int phys, uint32_t raw12, int32_t uncal_value) {
  return Calibrated(phys) ? CalRoutineMath::ApplyValueTable(value_q8[phys], raw12) : uncal_value;
}

// Pitch for a calibrated input's value: (value * 15360) >> 12, written as
// (value * 15) >> 2 -- identical for every int32 value whose product fits
// (15360 = 15 << 10), and one shift-subtract instead of a constant load and a
// multiply in the ISR. Uncalibrated inputs keep OC::ADC::value_to_pitch (the
// shared, Setup-calibrated pitch_cv_scale).
static_assert(kPitchScale == (15 << 10), "Pitch() hard-codes 15360 as 15 << 10");
__attribute__((always_inline)) inline int32_t Pitch(int32_t value) { return (value * 15) >> 2; }

// Loop context, calibrated inputs only: true millivolts at a fractional raw
// reading (the console meter's averaged 12.8 readings), interpolated in float
// on the same table, so it agrees with what the apps get to well under 0.1 mV.
float Millivolts(int phys, float raw);

#if !defined(__IMXRT1062__)
// Host tests only: called after every table entry ReloadFrom writes.
extern void (*test_write_hook)(int phys, int index);
#endif

}  // namespace InputCal

#endif  // INPUTCAL_H_
