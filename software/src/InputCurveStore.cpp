#include "InputCurveStore.h"

#include <stddef.h>
#include <string.h>

// Hardware-free: the LittleFS glue is InputCurveFiles.cpp.

static uint32_t crc32_bytes(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return c ^ 0xFFFFFFFFu;
}

static uint32_t record_crc(const InputCurveRecord &r) {
  return crc32_bytes((const uint8_t *)&r, offsetof(InputCurveRecord, crc32));
}

bool InputCurveEncode(InputCurveRecord &rec) {
  rec.magic = kInputCurveMagic;
  rec.version = kInputCurveVersion;
  rec.crc32 = record_crc(rec);
  return true;
}

bool InputCurveValid(const InputCurveRecord &rec) {
  return rec.magic == kInputCurveMagic && rec.version == kInputCurveVersion &&
         rec.crc32 == record_crc(rec);
}

static const char *slot_name(int slot) { return slot ? kInputCurveFileB : kInputCurveFileA; }

static bool read_slot(CurveStorage &st, int slot, InputCurveRecord *r) {
  return st.Read(slot_name(slot), r, sizeof *r) && InputCurveValid(*r);
}

// True when a is newer than b, across the 2^32 wrap.
static bool newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

bool InputCurveLoad(CurveStorage &st, InputCurveRecord *out, int *slot) {
  InputCurveRecord r[2];
  const bool ok0 = read_slot(st, 0, &r[0]);
  const bool ok1 = read_slot(st, 1, &r[1]);
  if (!ok0 && !ok1) return false;
  int pick = ok0 ? 0 : 1;
  if (ok0 && ok1 && newer(r[1].seq, r[0].seq)) pick = 1;
  if (out) *out = r[pick];
  if (slot) *slot = pick;
  return true;
}

bool InputCurveSave(CurveStorage &st, InputCurveRecord rec) {
  InputCurveRecord newest;
  int newest_slot = 0;
  const bool have = InputCurveLoad(st, &newest, &newest_slot);
  rec.seq = have ? newest.seq + 1 : 1;
  const int target = have ? 1 - newest_slot : 0;
  InputCurveEncode(rec);
  if (!st.Write(slot_name(target), &rec, sizeof rec)) return false;
  InputCurveRecord back;
  if (!st.Read(slot_name(target), &back, sizeof back)) return false;
  return memcmp(&back, &rec, sizeof rec) == 0;
}
