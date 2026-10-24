#ifndef CALROUTINEMARKER_H_
#define CALROUTINEMARKER_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// "The DAC tables and input curves now in use came from the Cal Routine": 4
// bytes of emulated EEPROM at [4140,4144), in the gap between the app-data
// page (which ends at 4140) and the ICA1 page (from 4164); OC_config.h names
// the range and OC_storage.h asserts it is clear of both. Written with
// EEPROM.update by the Cal Routine's save; read by Setup, whose DAC and ADC
// steps then say they replace that calibration; flags bit 0 cleared by
// Setup's own calibration save. It never travels in a Backup dump and a
// factory erase keeps it, as it keeps the calibration it describes.
//
// Layout: 'C', 'R', version, flags. Erased EEPROM (0xFF) and zeroes both
// read as "no marker". Pure; host-tested in test/test_calroutine_flow.cpp.
// ---------------------------------------------------------------------------

constexpr unsigned CalRoutineMarkerStart = 4140;
constexpr unsigned CalRoutineMarkerSize = 4;
constexpr uint8_t kCalRoutineMarkerVersion = 1;
constexpr uint8_t kCalRoutineMarkerStored = 0x01;  // flags bit 0: calibration stored

inline void CalRoutineMarkerEncode(uint8_t flags, uint8_t out[CalRoutineMarkerSize]) {
  out[0] = 'C';
  out[1] = 'R';
  out[2] = kCalRoutineMarkerVersion;
  out[3] = flags;
}

// A marker of this version with the "stored" flag set.
inline bool CalRoutineMarkerStored(const uint8_t in[CalRoutineMarkerSize]) {
  return in[0] == 'C' && in[1] == 'R' && in[2] == kCalRoutineMarkerVersion &&
         (in[3] & kCalRoutineMarkerStored) != 0;
}

#endif  // CALROUTINEMARKER_H_
