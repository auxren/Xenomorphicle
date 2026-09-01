// ---------------------------------------------------------------------------
// The CV inputs, replaced. The real OC_ADC.cpp is a FlexIO/ADC_ETC/DMA driver
// for a specific converter; none of that exists on a host.
//
// The header is the real one, so the smoothing, the calibration offsets and
// value()/value_to_pitch() are the firmware's own arithmetic -- what changes is
// only where the raw counts come from. Here they come from SimCvRaw(), which
// the simulator holds at a fixed value per channel (0 V by default).
//
// CONSEQUENCE, and it is the big one for a reviewer: every screen whose display
// follows a CV input is STILL. Scope traces do not move, quantizers do not
// step, a CV-driven cursor does not wander. That is the shim, not the app. See
// the per-app notes in README.md.
// ---------------------------------------------------------------------------

#include "OC_ADC.h"

#include <string.h>

#include "OC_gpio.h"
#include "OC_io.h"

// Raw counts per channel, at the ADC's scan resolution (16 bits). The
// simulator drives these; nothing else does.
uint16_t *SimCvRaw();
float SimIdVoltage();

// Channel index globals, verbatim from the real driver: OC_gpio.cpp's
// hardware detection reassigns them, so they cannot live anywhere else.
ADC_CHANNEL ADC_CHANNEL_1=0, ADC_CHANNEL_2=1, ADC_CHANNEL_3=2, ADC_CHANNEL_4=3;
#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
ADC_CHANNEL ADC_CHANNEL_5=4, ADC_CHANNEL_6=5, ADC_CHANNEL_7=6, ADC_CHANNEL_8=7;
#endif

namespace OC {

// ADC::adc_ (the converter library object) is deliberately NOT defined: it is
// a private static that nothing in this build odr-uses, and defining it would
// drag in a register-setup library for a converter that is not here.
/*static*/ ADC::CalibrationData *ADC::calibration_data_;
/*static*/ uint32_t ADC::raw_[ADC_CHANNEL_COUNT];
/*static*/ uint32_t ADC::smoothed_[ADC_CHANNEL_COUNT];
#ifdef OC_ADC_ENABLE_DMA_INTERRUPT
/*static*/ volatile bool ADC::ready_;
#endif
#ifdef OC_DEBUG_ADC_STATS
/*static*/ ADC::ChannelStats ADC::channel_stats_[ADC_CHANNEL_COUNT];
/*static*/ uint32_t ADC::stats_ticks_ = 0;
#endif

/*static*/ void ADC::Init(CalibrationData *calibration_data, bool flip180) {
  calibration_data_ = calibration_data;
  // Verbatim from the real driver: the flip-panel build swaps the channel
  // order, and the Setup app's calibration pages read those indices.
  if (flip180) {
    ADC_CHANNEL temp1 = ADC_CHANNEL_1, temp2 = ADC_CHANNEL_2,
                temp3 = ADC_CHANNEL_3, temp4 = ADC_CHANNEL_4;
    ADC_CHANNEL_1 = ADC_CHANNEL_8;
    ADC_CHANNEL_2 = ADC_CHANNEL_7;
    ADC_CHANNEL_3 = ADC_CHANNEL_6;
    ADC_CHANNEL_4 = ADC_CHANNEL_5;
    ADC_CHANNEL_5 = temp4;
    ADC_CHANNEL_6 = temp3;
    ADC_CHANNEL_7 = temp2;
    ADC_CHANNEL_8 = temp1;
  }
  memset(raw_, 0, sizeof(raw_));
  memset(smoothed_, 0, sizeof(smoothed_));
}

#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
/*static*/ void ADC::ADC33131D_Vref_calibrate() {}
#endif

/*static*/ void ADC::Init_DMA() {}
/*static*/ void ADC::DMA_ISR() {}

// One scan. The update() the real driver calls per channel is private and
// templated on a channel reference, so this walks the same arithmetic inline:
// shift the scan resolution down to kAdcResolution, up by the smoothing
// fraction, then the same one-pole smoother.
/*static*/ void ADC::Scan_DMA() {
  const uint16_t *src = SimCvRaw();
  for (int ch = 0; ch < ADC_CHANNEL_COUNT; ++ch) {
    uint32_t value = ((uint32_t)src[ch] >> (kAdcScanResolution - kAdcResolution))
                     << kAdcSmoothBits;
    raw_[ch] = value;
    smoothed_[ch] = (smoothed_[ch] * (kAdcSmoothing - 1) + value) / kAdcSmoothing;
  }
}

// Verbatim from the real driver (there is no hardware in it).
/*static*/ void ADC::Read(IOFrame *ioframe) {
  for (int channel = 0; channel < ADC_CHANNEL_COUNT; ++channel) {
    ioframe->cv.values[channel] = value(static_cast<ADC_CHANNEL>(channel));
    ioframe->cv.pitch_values[channel] = value_to_pitch(ioframe->cv.values[channel]);
  }
}

/*static*/ void ADC::CalibratePitch(int32_t c2, int32_t c4) {
  // Verbatim from the real driver: the calibration screens depend on it.
  if (c2 < c4) {
    int32_t scale = (24 * 128 * 4096L) / (c4 - c2);
    calibration_data_->pitch_cv_scale = scale;
  }
}

/*static*/ float ADC::Read_ID_Voltage() { return SimIdVoltage(); }

}  // namespace OC
