#ifndef ADCSLOTMAP_H_
#define ADCSLOTMAP_H_

#include <stdint.h>

#include "CvMeterMath.h"
#include "InputCurveStore.h"

// ---------------------------------------------------------------------------
// Which reading in an ADC33131D frame carries which panel input, and the
// one-time move of stored per-input data that fixing it needs.
//
// The ADC33131D path (OC_ADC.cpp, Init_Teensy41_ADC33131D_chip / Scan_DMA)
// deposits one 16-bit reading per conversion; eight consecutive readings make
// a frame, and the frame's slot s is the reading taken at mux address s. The
// original table assumed the reading in slot s was the input selected for
// that same conversion (AdcLegacySlotForPanelInput). It is not: each SPI frame
// returns the conversion started at the end of the PREVIOUS frame, so slot s
// holds the input selected one conversion earlier. That is a fixed pipeline
// latency, set by the FlexIO configuration (the mux shifter, the SPI read and
// the DMA request are all clocked by the same trigger, started together from
// a software reset), not a start-up phase. See the comment at the map in
// OC_ADC.cpp for the mechanism.
//
// Before the fix a CV into panel input n arrived at ADC_CHANNEL_(n+1), and
// panel 8 at ADC_CHANNEL_1 (bench, 2026-10-24). Everything stored per input
// -- the shared 0 V offsets (CAL1 adc.offset[]), the Q fits (ICA1) and the
// curve files (ICURVE_A/B.BIN) -- was recorded through that map, so the entry
// for panel n sits at the index ADC_CHANNEL_(n+1) names. RotateLegacy* moves
// each entry to ADC_CHANNEL_n.
//
// Pure and host-tested (test/test_adc_slot_map.cpp).
// ---------------------------------------------------------------------------

constexpr int kAdcSlots = 8;

// The slot the original table read panel input `panel` (0-7) from: the top
// row (panels 1-4) on mux addresses 4-7, the second row on 0-3.
constexpr int AdcLegacySlotForPanelInput(int panel) { return (panel + 4) & 7; }

// Conversions between a mux address being selected and its reading arriving.
constexpr int kAdcSlotLatency = 1;

// The frame slot that carries panel input `panel` (0-7).
constexpr int AdcSlotForPanelInput(int panel) {
  return (AdcLegacySlotForPanelInput(panel) + kAdcSlotLatency) & 7;
}

// ---- stored data ---------------------------------------------------------

// Every page this firmware writes carries kInputMapV1 in one reserved byte
// (CAL1 reserved0[0], ICA1 reserved0[0], the curve record's reserved2[0]):
// "indexed by the fixed map". Firmware that predates the fix left those bytes
// 0, and ignores them when it reads a page, so the layout is unchanged.
constexpr uint8_t kInputMapV1 = 1;

inline uint8_t &IcaMapFlag(CvMeterMath::InputCalibration &c) { return c.reserved0[0]; }
inline uint8_t IcaMapFlag(const CvMeterMath::InputCalibration &c) { return c.reserved0[0]; }
inline uint8_t &CurveMapFlag(InputCurveRecord &r) { return r.reserved2[0]; }
inline uint8_t CurveMapFlag(const InputCurveRecord &r) { return r.reserved2[0]; }

// EEPROM [4144,4148) once the migration has run ("input map v1").
constexpr uint8_t kInputMapMarker[4] = {'A', 'M', 1, 0};
bool InputMapMarkerPresent(const uint8_t m[4]);
// What a calibration-region restore writes first (see apps/Backup.h).
void InputMapMarkerClear(uint8_t m[4]);

// chan[n] is the value of ADC_CHANNEL_(n+1) in force (flip mode and some
// hardware IDs permute it at boot); nullptr means the unflipped 0..7. The
// stored data is indexed by those values, so new[chan[m]] = old[chan[m+1]].
// Arrays and mask bits move; source_output values are output labels and are
// moved with their input but never changed.
void RotateLegacyOffsets(uint16_t offset[8], const int *chan);
void RotateLegacyIca(CvMeterMath::InputCalibration &ica, const int *chan);
void RotateLegacyCurves(InputCurveRecord &rec, const int *chan);
void RotateLegacyInputData(uint16_t offset[8], CvMeterMath::InputCalibration &ica,
                           InputCurveRecord *curves /*nullable*/, const int *chan = nullptr);

// A valid curve record without the flag is rotated, stamped and re-encoded
// (CRC); one with it is left alone. True when it was rotated.
bool InputCurveNormalize(InputCurveRecord &rec, const int *chan);

// ---- the boot migration --------------------------------------------------

struct InputMapPages {
  const uint8_t *marker;    // the 4 EEPROM bytes [4144,4148)
  bool cal_loaded;          // a CAL1 page was loaded (else defaults in RAM)
  uint16_t *cal_offset;     // calibration_data.adc.offset
  uint8_t *cal_flag;        // calibration_data.reserved0[0]
  bool ica_loaded;          // an ICA1 page was loaded
  CvMeterMath::InputCalibration *ica;
  bool curves_loaded;       // a valid curve record was read (raw, not normalized)
  InputCurveRecord *curves;
};

struct InputMapPlan {
  bool run;          // the marker was absent: write it once the saves succeed
  bool save_cal;     // in this order: CAL1, ICA1, the curves, then the marker
  bool save_ica;
  bool save_curves;
};

// Marker present: nothing. Otherwise every loaded page whose flag is absent
// is rotated and stamped in RAM and marked for saving; pages already stamped
// (a rerun after power loss) are left alone, so a rerun is harmless. RAM that
// came from defaults is stamped without a save: the defaults are the same on
// every input.
InputMapPlan InputMapPrepare(InputMapPages &p, const int *chan);

#endif
