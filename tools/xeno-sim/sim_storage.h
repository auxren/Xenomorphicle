#ifndef XENOSIM_SIM_STORAGE_H_
#define XENOSIM_SIM_STORAGE_H_
// ---------------------------------------------------------------------------
// Non-volatile memory that outlives the process.
//
// WHY THIS EXISTS. The simulator's file system is a std::map and its EEPROM is
// an array, both of which die at exit -- so until now the README's warning was
// literally true: "a Save screen that works here is not evidence that settings
// come back after a power cycle." That is not a footnote for the changes this
// branch is landing. Three of them are claims about exactly that boundary:
//
//   * presets must be visible with a card seated AND absent, and must be the
//     SAME presets -- which cannot be asked at all inside one process, because
//     the card state is fixed at boot;
//   * the 200e scan result must survive a power cycle;
//   * a boot with valid stored settings must restore state rather than take
//     the first-run path, and a CANCELLED reset must leave storage alone.
//
// Every one of those is a question about what a SECOND boot sees. So the two
// volumes and the EEPROM are serialised to a file at exit and read back at
// boot, and a test becomes two runs sharing one image -- which is what a power
// cycle is.
//
// This is still not LittleFS. Wear, erase timing and the 0-byte-file failure
// PresetEngine guards against are all absent, and a clean round-trip here says
// nothing about any of them. What it does say is which BYTES the firmware
// chose to keep, and on which volume -- and that is the thing under change.
//
// The format is text (see sim_storage.cpp) so a failing check can be read by
// eye, and it is stable across runs so two images can simply be `cmp`ed.
// ---------------------------------------------------------------------------

#include <stdio.h>

#include <string>

// Read an image into the internal LittleFS volume, the SD volume and the
// EEPROM. Must run AFTER SimHostReset() (which zeroes them) and BEFORE
// SimRuntimeBoot(), so the firmware's boot sees the stored state.
// Returns false if the file is missing or malformed; a missing file is not an
// error to the caller -- it is a virgin module.
bool SimStorageLoad(const std::string &path, std::string *why = nullptr);

// Write the current contents of both volumes and the EEPROM.
bool SimStorageSave(const std::string &path);

// One line per file: "fs <lfs|sd> <name> <bytes> <crc32-hex>", then
// "eeprom <bytes> <crc32-hex>". Sorted, so two runs can be diffed directly.
// The CRC is what lets a check assert "the same presets" without caring how a
// preset is encoded.
void SimStorageList(FILE *out);

#endif  // XENOSIM_SIM_STORAGE_H_
