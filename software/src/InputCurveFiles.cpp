// LittleFS glue for the input-curve A/B store. Not host-compiled.
#include <Arduino.h>

#include "InputCurveFiles.h"
#include "AdcSlotMap.h"
#include "OC_ADC.h"
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

bool InputCurveLoadFromFlashRaw(InputCurveRecord *out, int *slot) {
  PhzCurveStorage st;
  return InputCurveLoad(st, out, slot);
}

// Loop context only (not reentrant): the 300-byte scratch records live in
// DMAMEM, off the loop stack and out of DTCM, like InputCal::Reload's.
bool InputCurveLoadFromFlash(InputCurveRecord *out, int *slot, bool *legacy) {
  DMAMEM static InputCurveRecord scratch;
  InputCurveRecord *rec = out ? out : &scratch;
  const bool ok = InputCurveLoadFromFlashRaw(rec, slot);
  bool rotated = false;
  if (ok) {
    int chan[8];
    OC::ADC::ChannelTable(chan);
    rotated = InputCurveNormalize(*rec, chan);
  }
  if (legacy) *legacy = rotated;
  return ok;
}

bool InputCurveSaveToFlash(const InputCurveRecord &rec) {
  PhzCurveStorage st;
  DMAMEM static InputCurveRecord stamped;
  stamped = rec;
  CurveMapFlag(stamped) = kInputMapV1;  // the store re-encodes (seq, CRC)
  return InputCurveSave(st, stamped);
}
