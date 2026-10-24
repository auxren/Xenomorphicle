#ifndef INPUTCURVEFILES_H_
#define INPUTCURVEFILES_H_

#include "InputCurveStore.h"

// The LittleFS-backed store (InputCurveFiles.cpp). Loop context only: Save
// writes flash inside an OC::RT::PersistenceWindow.
bool InputCurveLoadFromFlash(InputCurveRecord *out, int *slot);
bool InputCurveSaveToFlash(const InputCurveRecord &rec);

#endif
