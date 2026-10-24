// LittleFS glue for the input-curve A/B store. Not host-compiled.
#include <Arduino.h>

#include "InputCurveFiles.h"
#include "PhzConfig.h"
#include "RtStats.h"

namespace {

class PhzCurveStorage : public CurveStorage {
 public:
  bool Read(const char *name, void *buf, size_t len) override {
    File f = PhzConfig::myfs.open(name, FILE_READ);
    if (!f) return false;
    const bool ok = (size_t)f.size() == len && f.read(buf, len) == (int)len;
    f.close();
    return ok;
  }
  bool Write(const char *name, const void *buf, size_t len) override {
    // A torn write here costs only this slot: the other copy still stands.
    OC::RT::PersistenceWindow window("icurve");
    File f = PhzConfig::myfs.open(name, FILE_WRITE_BEGIN);
    if (!f) return false;
    const bool ok = f.write((const uint8_t *)buf, len) == len;
    f.close();
    return ok;
  }
};

}  // namespace

bool InputCurveLoadFromFlash(InputCurveRecord *out, int *slot) {
  PhzCurveStorage st;
  return InputCurveLoad(st, out, slot);
}

bool InputCurveSaveToFlash(const InputCurveRecord &rec) {
  PhzCurveStorage st;
  return InputCurveSave(st, rec);
}
