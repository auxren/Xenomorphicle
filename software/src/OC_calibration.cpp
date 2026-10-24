/*
*
* calibration menu:
*
* enter by pressing left encoder button during start up; use encoder switches to navigate.
*
*/

#include "OC_core.h"
#include "OC_apps.h"
#include "OC_DAC.h"
#include "OC_debug.h"
#include "OC_gpio.h"
#include "OC_ADC.h"
#include "OC_storage.h"
#include "OC_calibration.h"
#include "CvMeterMath.h"
#include "InputCal.h"
#include "AdcSlotMap.h"
#include "InputCurveFiles.h"
#include "OC_digital_inputs.h"
#include "OC_menus.h"
#include "OC_strings.h"
#include "OC_ui.h"
#include "OC_options.h"
#include "icons.h"
#include "src/drivers/display.h"
#include "src/drivers/ADC/OC_util_ADC.h"
#include "util/util_debugpins.h"
namespace menu = OC::menu;

using OC::DAC;

namespace OC {

DMAMEM CalibrationStorage calibration_storage;
CalibrationData calibration_data;

bool calibration_data_loaded = false;

#ifdef ARDUINO_TEENSY41
DMAMEM InputCalibrationStorage input_calibration_storage;
DMAMEM CvMeterMath::InputCalibration input_calibration;  // set by calibration_load()
bool input_calibration_loaded = false;
#endif

PROGMEM
const CalibrationData kCalibrationDefaults = {
  // DAC
  { {
  #ifdef ARDUINO_TEENSY41
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
  #endif
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535},
    {0, 6553, 13107, 19661, 26214, 32768, 39321, 45875, 52428, 58981, 65535}
    },
  },
  // ADC
  { {
#ifdef ARDUINO_TEENSY41
      _ADC_OFFSET, _ADC_OFFSET, _ADC_OFFSET, _ADC_OFFSET,
#endif
      _ADC_OFFSET, _ADC_OFFSET, _ADC_OFFSET, _ADC_OFFSET
    },
    0,  // pitch_cv_scale
    0   // pitch_cv_offset : unused
  },
  // display_offset
  SH1106_128x64_Driver::kDefaultOffset,
  OC_CALIBRATION_DEFAULT_FLAGS,
  SCREENSAVER_TIMEOUT_S,
  // reserved0[0]: the stored offsets follow the fixed input map (AdcSlotMap.h).
  // Defaults are the same on every input, so they are mapped by definition.
  { kInputMapV1, 0, 0 }, // reserved0
  0 // reserved1
};

PROGMEM
const CalibrationData kNLMCalibrationDefaults = {
  // DAC
  { {
#ifdef ARDUINO_TEENSY41
    // Xenomorpher
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100},
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100},
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100},
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100},
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100},
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100},
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100},
    {200, 6500, 12900, 19200, 25500, 32700, 38000, 44300, 50600, 56900, 63100}
#else
    // cardOC or hOC with T40
    {390, 6800, 13200, 19650, 26100, 32500, 38900, 45400, 51800, 58200, 64600},
    {390, 6800, 13200, 19650, 26100, 32500, 38900, 45400, 51800, 58200, 64600},
    {390, 6800, 13200, 19650, 26100, 32500, 38900, 45400, 51800, 58200, 64600},
    {390, 6800, 13200, 19650, 26100, 32500, 38900, 45400, 51800, 58200, 64600}
#endif
    },
  },
  // ADC
  { {
#ifdef ARDUINO_TEENSY41
      _ADC_OFFSET_NLM, _ADC_OFFSET_NLM, _ADC_OFFSET_NLM, _ADC_OFFSET_NLM,
#endif
      _ADC_OFFSET_NLM, _ADC_OFFSET_NLM, _ADC_OFFSET_NLM, _ADC_OFFSET_NLM
    },
    0,  // pitch_cv_scale
    0   // pitch_cv_offset : unused
  },
  // display_offset
  SH1106_128x64_Driver::kDefaultOffset,
  OC_CALIBRATION_DEFAULT_FLAGS,
  SCREENSAVER_TIMEOUT_S,
  // reserved0[0]: the stored offsets follow the fixed input map (AdcSlotMap.h).
  // Defaults are the same on every input, so they are mapped by definition.
  { kInputMapV1, 0, 0 }, // reserved0
  0 // reserved1
};

#ifdef ARDUINO_TEENSY41
PROGMEM
const DAC::CalibrationData kDACSergeDefaults = {
  {
    // Xenomorpher for Serge
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900},
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900},
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900},
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900},
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900},
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900},
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900},
    {2200, 8200, 14200, 20200, 26200, 32200, 38100, 44050, 50050, 55950, 61900}
  }
};
#endif

PROGMEM
const DAC::CalibrationData kDAC20VppDefaults = {
  {
  #ifdef ARDUINO_TEENSY41
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610},
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610},
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610},
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610},
  #endif
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610},
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610},
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610},
    {936, 7303, 13670, 20037, 26404, 32771, 39138, 45505, 51872, 58239, 64610}
  }
};

static constexpr uint16_t DAC_OFFSET = 4890; // DAC offset, initial approx., ish --> -3.5V to 6V

FLASHMEM void calibration_reset() {
  if (NorthernLightModular || NLMSerge) {
    memcpy(&OC::calibration_data, &kNLMCalibrationDefaults, sizeof(OC::calibration_data));
#ifdef ARDUINO_TEENSY41
    if (NLMSerge) {
      memcpy(&OC::calibration_data.dac, &kDACSergeDefaults, sizeof(OC::calibration_data.dac));
    }
#endif
  } else {
    memcpy(&OC::calibration_data, &kCalibrationDefaults, sizeof(OC::calibration_data));
    if (DAC_20Vpp) {
      memcpy(&OC::calibration_data.dac, &kDAC20VppDefaults, sizeof(OC::calibration_data.dac));
      return;
    }

    for (int ch = 0; ch < DAC_CHANNEL_COUNT; ++ch) {
      for (int i = 0; i < OCTAVES; ++i) {
        OC::calibration_data.dac.calibrated_octaves[ch][i] += DAC_OFFSET;
      }
    }
  }
}

FLASHMEM void calibration_load() {
  SERIAL_PRINTLN("Cal.Storage: PAGESIZE=%u, PAGES=%u, LENGTH=%u",
                 OC::CalibrationStorage::PAGESIZE, OC::CalibrationStorage::PAGES, OC::CalibrationStorage::LENGTH);

  calibration_reset();
  calibration_data_loaded = OC::calibration_storage.Load(OC::calibration_data);
  if (!calibration_data_loaded) {
    SERIAL_PRINTLN("No calibration data, using defaults");
  } else {
    SERIAL_PRINTLN("Calibration data loaded...");
  }

  // Fix-up left-overs from development
  if (!OC::calibration_data.adc.pitch_cv_scale) {
    SERIAL_PRINTLN("CV scale not set, using default");
    OC::calibration_data.adc.pitch_cv_scale = OC::ADC::kDefaultPitchCVScale;
  }

  if (!OC::calibration_data.screensaver_timeout)
    OC::calibration_data.screensaver_timeout = SCREENSAVER_TIMEOUT_S;

#ifdef ARDUINO_TEENSY41
  // Separate page, separate fate: a missing or rejected input calibration
  // never touches the DAC/ADC data above, and vice versa.
  CvMeterMath::InputCalReset(OC::input_calibration);
  input_calibration_loaded = input_calibration_storage.Load(OC::input_calibration);
  if (input_calibration_loaded) {
    const int dropped = CvMeterMath::InputCalSanitize(OC::input_calibration);
    (void)dropped;  // SERIAL_PRINTLN compiles away without PRINT_DEBUG
    SERIAL_PRINTLN("Input calibration loaded, mask %02X (%d dropped)",
                   OC::input_calibration.valid_mask, dropped);
  } else {
    CvMeterMath::InputCalReset(OC::input_calibration);
  }
  // What the apps see (InputCal.h): rebuilt from the page just loaded and the
  // curve files. At boot LittleFS is not mounted yet, so only ICA1 counts
  // here; setup() reloads once it is.
  InputCal::Reload();
#endif
}

#ifdef ARDUINO_TEENSY41
FLASHMEM void input_calibration_save() {
  OC::input_calibration.version = CvMeterMath::InputCalibration::kVersion;
  // Everything this firmware fits is indexed by the fixed input map.
  IcaMapFlag(OC::input_calibration) = kInputMapV1;
  input_calibration_storage.Save(OC::input_calibration);
  input_calibration_loaded = true;
  InputCal::Reload();  // the apps follow the new page at once
}

// The one-time move of per-input data to the fixed ADC33131D slot map
// (AdcSlotMap.h), checked on every boot (it costs nothing once done). Needs the final ADC_CHANNEL table (after ADC::Init's flip)
// and LittleFS (the curve files), so setup() calls it after PhzConfig::Init;
// a calibration-region restore (apps/Backup.h) calls it again.
//
// Order: CAL1, ICA1, the curves, then the marker -- and the marker only when
// every save succeeded. Each page carries its own map flag, written in the
// same save as its rotated data, so power lost anywhere leaves every page
// either untouched (flag absent: rotated on the rerun) or done (flag set:
// left alone on the rerun). A rerun can never rotate a page twice.
FLASHMEM void input_map_migrate() {
  uint8_t marker[4];
  EEPROMStorage::read(EEPROM_INPUTMAP_MARKER_START, marker, sizeof marker);
  const bool marker_present = InputMapMarkerPresent(marker);

  int chan[8];
  OC::ADC::ChannelTable(chan);
  DMAMEM static InputCurveRecord rec;  // 300 bytes, off the loop stack and out of DTCM
  InputMapPages pages;
  pages.marker = marker;
  pages.cal_loaded = calibration_data_loaded;
  pages.cal_offset = OC::calibration_data.adc.offset;
  pages.cal_flag = &OC::calibration_data.reserved0[0];
  pages.ica_loaded = input_calibration_loaded;
  pages.ica = &OC::input_calibration;
  // The marker only spares this flash read on every later boot; CAL1 and
  // ICA1 are checked on their own flags each time (InputMapPrepare).
  pages.curves_loaded = !marker_present && InputCurveLoadFromFlashRaw(&rec, nullptr);
  pages.curves = &rec;
  const InputMapPlan plan = InputMapPrepare(pages, chan);
  if (!plan.run && !plan.save_cal && !plan.save_ica) return;  // the usual boot

  // Only the curve save's result gates the marker: the two EEPROM page saves
  // report nothing back, and a page that did not land keeps flag 0 and is
  // simply moved again on the next boot.
  bool ok = true;
  if (plan.save_cal) OC::calibration_storage.Save(OC::calibration_data);
  if (plan.save_ica) input_calibration_save();
  if (plan.save_curves) ok = InputCurveSaveToFlash(rec);
  if (plan.run && ok)
    EEPROMStorage::update(EEPROM_INPUTMAP_MARKER_START, kInputMapMarker, sizeof kInputMapMarker);
  Serial.printf("Input map v1: cal=%d ica=%d curves=%d ok=%d\r\n", plan.save_cal,
                plan.save_ica, plan.save_curves, ok);
  InputCal::Reload();
}
#endif

FLASHMEM void calibration_save() {
  SERIAL_PRINTLN("Saving calibration data");
  OC::calibration_storage.Save(OC::calibration_data);

  uint32_t start = millis();
  while(millis() < start + SETTINGS_SAVE_TIMEOUT_MS) {
    GRAPHICS_BEGIN_FRAME(true);
    graphics.setPrintPos(13, 18);
    graphics.print("Calibration saved");
    graphics.setPrintPos(31, 27);
    graphics.print("to EEPROM!");
    GRAPHICS_END_FRAME();
  }
}

DigitalInputDisplay digital_input_displays[4];

//        128/6=21                  |                     |
const char * const start_footer   = "[CANCEL]         [OK]";
const char * const end_footer     = "[PREV]         [EXIT]";
const char * const default_footer = "[PREV]         [NEXT]";
const char * const default_help_r = "[R] => Adjust";
#ifdef ARDUINO_TEENSY41
const char * const long_press_hint = "Hold [B] to set";
#else
const char * const long_press_hint = "Hold [DOWN] to set";
#endif
const char * const select_help    = "[R] => Select";

PROGMEM
const CalibrationStep calibration_steps[CALIBRATION_STEP_LAST] = {
  { HELLO, "Setup: Calibrate", "Start fresh? ", select_help, start_footer, CALIBRATE_NONE, 0, OC::Strings::no_yes, 0, 1 },
  { CENTER_DISPLAY, "Center Display", "Pixel offset ", default_help_r, default_footer, CALIBRATE_DISPLAY, 0, nullptr, 0, 2 },

  #if defined(IO_10V)
    { DAC_A_VOLT_MIN, "DAC A 0.0 volts", "-> 0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_A_VOLT_HIGH,  "DAC A 8.0 volts", "-> 8.000V ", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
  
    { DAC_B_VOLT_MIN, "DAC B 0.0 volts", "-> 0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_B_VOLT_HIGH,  "DAC B 8.0 volts", "-> 8.000V ", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
  
    { DAC_C_VOLT_MIN, "DAC C 0.0 volts", "-> 0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_C_VOLT_HIGH,  "DAC C 8.0 volts", "-> 8.000V ", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
  
    { DAC_D_VOLT_MIN, "DAC D 0.0 volts", "-> 0.000V ", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_D_VOLT_HIGH,  "DAC D 8.0 volts", "-> 8.000V ", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
  #else
    { DAC_A_VOLT_MIN, "DAC A (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_A_VOLT_HIGH,  "DAC A (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
  
    { DAC_B_VOLT_MIN, "DAC B (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_B_VOLT_HIGH,  "DAC B (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
  
    { DAC_C_VOLT_MIN, "DAC C (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_C_VOLT_HIGH,  "DAC C (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
  
    { DAC_D_VOLT_MIN, "DAC D (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_D_VOLT_HIGH,  "DAC D (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },

#ifdef ARDUINO_TEENSY41
    { DAC_E_VOLT_MIN, "DAC E (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_E_VOLT_HIGH,  "DAC E (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },

    { DAC_F_VOLT_MIN, "DAC F (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_F_VOLT_HIGH,  "DAC F (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },

    { DAC_G_VOLT_MIN, "DAC G (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_G_VOLT_HIGH,  "DAC G (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },

    { DAC_H_VOLT_MIN, "DAC H (min)", "", default_help_r, default_footer, CALIBRATE_OCTAVE, 0, nullptr, 0, DAC::MAX_VALUE },
    { DAC_H_VOLT_HIGH,  "DAC H (high)", "", long_press_hint, default_footer, CALIBRATE_OCTAVE, 8, nullptr, 0, DAC::MAX_VALUE },
#endif
  #endif


  { ADC_OFFSETS, "ADC input 0V offset", "", long_press_hint, default_footer, CALIBRATE_ADC_OFFSET, 0, nullptr, 0, 4095 },

    { ADC_PITCH_C2, "ADC octave +1", "CV1: Input 1.%dV", long_press_hint, default_footer, CALIBRATE_ADC_1V, 1, nullptr, 0, 0 },
    { ADC_PITCH_C4, "ADC octave +3", "CV1: Input 3.%dV", long_press_hint, default_footer, CALIBRATE_ADC_3V, 3, nullptr, 0, 0 },

  { CALIBRATION_SCREENSAVER_TIMEOUT, "Screen Blank", "(minutes)", default_help_r, default_footer, CALIBRATE_SCREENSAVER, 0, nullptr, (OC::Ui::kLongPressTicks * 2 + 500) / 1000, SCREENSAVER_TIMEOUT_MAX_S },

  { CALIBRATION_EXIT, "Calibration complete", "Save values? ", select_help, end_footer, CALIBRATE_NONE, 0, OC::Strings::no_yes, 0, 1 }
};


} // namespace OC
