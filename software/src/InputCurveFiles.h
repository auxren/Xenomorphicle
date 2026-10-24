#ifndef INPUTCURVEFILES_H_
#define INPUTCURVEFILES_H_

#include "InputCurveStore.h"

// The LittleFS-backed store (InputCurveFiles.cpp). Loop context only: Save
// writes flash inside an OC::RT::PersistenceWindow.
//
// Load returns the record in the fixed input map (AdcSlotMap.h): a record
// written before the fix is rotated in RAM, so every reader sees each row at
// the right input even before the boot migration has rewritten the file
// (*legacy, optional, says so). LoadRaw returns the file as it is (the
// migration). Save stamps the record as mapped: everything this firmware
// writes is indexed by the fixed map.
bool InputCurveLoadFromFlash(InputCurveRecord *out, int *slot, bool *legacy = nullptr);
bool InputCurveLoadFromFlashRaw(InputCurveRecord *out, int *slot);
bool InputCurveSaveToFlash(const InputCurveRecord &rec);

#endif
