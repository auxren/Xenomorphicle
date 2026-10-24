// Copyright 2019 Patrick Dowling
//
// Author: Patrick Dowling (pld@gurkenkiste.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
#ifndef OC_STORAGE_H_
#define OC_STORAGE_H_

#include <stdint.h>
#include "OC_config.h"
#include "OC_calibration.h"
#include "CvMeterMath.h"
#include "OC_global_settings.h"
#include "OC_io_settings.h"
#include "util/util_misc.h"
#include "util/util_pagestorage.h"
#include "src/drivers/EEPROMStorage.h"

namespace OC {

// App settings are packed into a single blob of binary data; each app's chunk
// gets its own header with id and the length of the entire chunk. This makes
// this a bit more flexible during development.
// Chunks are aligned on 2-byte boundaries for arbitrary reasons (thankfully M4
// allows unaligned access...)
struct AppChunkHeader {
  uint16_t id;
  uint16_t length;
} __attribute__((packed));

struct AppData {
  static constexpr uint32_t FOURCC = FourCC<'O','C','A',4>::value;
  static constexpr size_t kAppDataSize = EEPROM_APPDATA_BINARY_SIZE;

  uint8_t data[kAppDataSize];
  size_t used;
};

using CalibrationStorage = PageStorage<EEPROMStorage, EEPROM_CALIBRATIONDATA_START, EEPROM_CALIBRATIONDATA_END, CalibrationData>;
#ifndef __IMXRT1062__
using GlobalSettingsStorage = PageStorage<EEPROMStorage, EEPROM_GLOBALSETTINGS_START, EEPROM_GLOBALSETTINGS_END, GlobalSettings>;
#endif
using AppDataStorage = PageStorage<EEPROMStorage, EEPROM_APPDATA_START, EEPROM_APPDATA_END, AppData>;

#ifdef ARDUINO_TEENSY41
using InputCalibrationStorage = PageStorage<EEPROMStorage, EEPROM_INPUTCAL_START, EEPROM_INPUTCAL_END, CvMeterMath::InputCalibration>;

// The layout every unit in the field already has. If one of these fires,
// a firmware update would invalidate stored data -- fix the layout, do not
// edit the numbers.
static_assert(EEPROM_APPDATA_BINARY_SIZE == 3900, "app-data blob size changed: stored app data would no longer validate");
static_assert(AppDataStorage::PAGES == 1, "app-data page count changed: the page would move");
static_assert(EEPROM_APPDATA_START + AppDataStorage::PAGESIZE <= EEPROM_INPUTCAL_START, "app-data page overlaps the input calibration");
#ifndef XENO_SIM
// Target layout only: on the 64-bit host simulator AppData::used is 8 bytes
// and the page runs past 4140 (into the marker, never into ICA1). The
// simulator has no meter, so nothing there writes the marker.
static_assert(EEPROM_APPDATA_START + AppDataStorage::PAGESIZE <= EEPROM_CALROUTINE_MARKER_START, "app-data page overlaps the Cal Routine marker");
#endif
static_assert(EEPROM_CALROUTINE_MARKER_END <= EEPROM_INPUTCAL_START, "Cal Routine marker overlaps the input calibration");
static_assert(InputCalibrationStorage::PAGES == 1, "input calibration page does not fit its region");
static_assert(EEPROM_INPUTCAL_END <= EEPROM_PRESETBUS_START, "input calibration overlaps the preset-bus record");
static_assert(sizeof(CalibrationData) + 12 <= EEPROM_CALIBRATIONDATA_END - EEPROM_CALIBRATIONDATA_START, "DAC/ADC calibration page outgrew its region");
#endif

// Helpers for getting total app storage sizes

template <typename T>
struct AppStorageChunkSize {
  static constexpr size_t value =
    (sizeof(AppChunkHeader) +
      IOSettings::storageSize() +
      T::kAppDataStorageSize + 1) & ~0x1;
};

} // OC

#endif // OC_STORAGE_H_
