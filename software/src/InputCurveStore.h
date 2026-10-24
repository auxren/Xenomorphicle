#ifndef INPUTCURVESTORE_H_
#define INPUTCURVESTORE_H_

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Per-input calibration curves on disk (design/cal-routine-design.md 6.4a).
// A fixed little-endian record, kept in two files written alternately
// (ICURVE_A.BIN / ICURVE_B.BIN), each guarded by a CRC and a sequence
// number, so a torn or corrupt write can only ever cost the newest copy.
// The codec and the store are hardware-free and host-tested
// (test/test_input_curve_store.cpp); InputCurveFiles.cpp is the LittleFS glue.
// ---------------------------------------------------------------------------

constexpr uint32_t kInputCurveMagic = 0x31564349u;  // 'I','C','V','1' little-endian
constexpr uint16_t kInputCurveVersion = 1;
constexpr int kInputCurveInputs = 8;
constexpr int kInputCurvePoints = 17;  // == CalRoutineMath::kCurvePoints
constexpr const char *kInputCurveFileA = "ICURVE_A.BIN";
constexpr const char *kInputCurveFileB = "ICURVE_B.BIN";
// Files with this prefix are migrated first, with GLOBALS.* (PhzConfig.cpp).
constexpr const char *kInputCurvePrefix = "ICURVE_";

#pragma pack(push, 1)
struct InputCurveRecord {
  uint32_t magic;          // kInputCurveMagic
  uint16_t version;        // kInputCurveVersion
  uint16_t reserved;
  uint32_t seq;            // wrap-safe; the larger (mod 2^32) is newer
  uint8_t valid_mask;      // bit i set: input i has a curve
  uint8_t source_output[8];
  uint8_t reserved2[3];
  int16_t mv_x2[kInputCurveInputs][kInputCurvePoints];  // true mV * 2 at raw 0,256..4096
  uint32_t crc32;          // CRC-32 (0xEDB88320) of every byte before this
};
#pragma pack(pop)
static_assert(sizeof(InputCurveRecord) == 300, "InputCurveRecord is 300 bytes");

// Sets magic and version, then the CRC.
bool InputCurveEncode(InputCurveRecord &rec);
// Magic, version and CRC all match.
bool InputCurveValid(const InputCurveRecord &rec);

struct CurveStorage {
  virtual ~CurveStorage() {}
  // True only when the whole len bytes were read.
  virtual bool Read(const char *name, void *buf, size_t len) = 0;
  // True only when the whole len bytes were written.
  virtual bool Write(const char *name, const void *buf, size_t len) = 0;
};

// The valid record with the highest seq (wrap-safe). *slot (optional) is 0 for A, 1 for B.
// False when neither copy is valid: the caller falls back to the line fit (ICA1).
bool InputCurveLoad(CurveStorage &st, InputCurveRecord *out, int *slot);
// seq = newest + 1 (1 when none), written to the slot not holding the newest
// valid copy (A when neither is valid), then read back and compared.
bool InputCurveSave(CurveStorage &st, InputCurveRecord rec);

#endif
