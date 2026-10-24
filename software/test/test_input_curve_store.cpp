// Host tests for the input-curve record codec and the A/B store
// (src/InputCurveStore.cpp) over an in-memory CurveStorage.
//   g++ -std=c++17 -Wall -Werror -O2 -o build/test_input_curve_store \
//     test_input_curve_store.cpp ../src/InputCurveStore.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/InputCurveStore.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct MemStorage : CurveStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  bool corrupt_writes = false;  // readback sees a flipped bit
  bool Read(const char *n, void *b, size_t len) override {
    auto it = files.find(n);
    if (it == files.end() || it->second.size() != len) return false;
    memcpy(b, it->second.data(), len);
    return true;
  }
  bool Write(const char *n, const void *b, size_t len) override {
    std::vector<uint8_t> v((const uint8_t *)b, (const uint8_t *)b + len);
    if (corrupt_writes) v[10] ^= 1;
    files[n] = v;
    return true;
  }
};

static InputCurveRecord make(uint32_t seq, int16_t fill) {
  InputCurveRecord r;
  memset(&r, 0, sizeof r);
  r.seq = seq;
  r.valid_mask = 0x05;
  for (int i = 0; i < 8; ++i) r.source_output[i] = (uint8_t)i;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 17; ++j) r.mv_x2[i][j] = (int16_t)(fill + i * 100 + j);
  InputCurveEncode(r);
  return r;
}
static void put(MemStorage &s, const char *n, const InputCurveRecord &r) { s.Write(n, &r, sizeof r); }

static void roundtrip() {
  CHECK(sizeof(InputCurveRecord) == 300);
  MemStorage s;
  InputCurveRecord r = make(0, 7);
  CHECK(InputCurveValid(r));
  CHECK(InputCurveSave(s, r));
  InputCurveRecord o; int slot = -1;
  CHECK(InputCurveLoad(s, &o, &slot));
  CHECK(slot == 0 && o.seq == 1 && o.valid_mask == 0x05);
  CHECK(o.mv_x2[3][4] == 7 + 300 + 4);
  CHECK(memcmp(s.files["ICURVE_A.BIN"].data(), &o, sizeof o) == 0);
}
static void crc_rejects_one_bit() {
  InputCurveRecord r = make(3, 0);
  CHECK(InputCurveValid(r));
  for (size_t bit = 0; bit < sizeof r * 8; bit += 7) {
    InputCurveRecord t = r;
    ((uint8_t *)&t)[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    CHECK(!InputCurveValid(t));
  }
  // known-answer: CRC-32 of "123456789" shape via a record is awkward; check field layout instead
  CHECK(r.magic == 0x31564349u);
}
static void ab_newest_valid_wins() {
  MemStorage s;
  put(s, "ICURVE_A.BIN", make(5, 1));
  put(s, "ICURVE_B.BIN", make(6, 2));
  InputCurveRecord o; int slot;
  CHECK(InputCurveLoad(s, &o, &slot) && slot == 1 && o.seq == 6);
  CHECK(InputCurveSave(s, make(0, 3)));
  InputCurveRecord a;
  CHECK(s.Read("ICURVE_A.BIN", &a, sizeof a) && a.seq == 7 && InputCurveValid(a));
  CHECK(InputCurveLoad(s, &o, &slot) && slot == 0 && o.seq == 7);
  // B untouched
  InputCurveRecord b; CHECK(s.Read("ICURVE_B.BIN", &b, sizeof b) && b.seq == 6);
}
static void torn_copy_falls_back() {
  MemStorage s;
  put(s, "ICURVE_A.BIN", make(5, 1));
  put(s, "ICURVE_B.BIN", make(6, 2));
  s.files["ICURVE_B.BIN"][100] ^= 0x10;
  InputCurveRecord o; int slot;
  CHECK(InputCurveLoad(s, &o, &slot) && slot == 0 && o.seq == 5);
  // a save now overwrites the torn B, keeping A
  CHECK(InputCurveSave(s, make(0, 9)));
  InputCurveRecord b; CHECK(s.Read("ICURVE_B.BIN", &b, sizeof b) && b.seq == 6 && InputCurveValid(b));
  // a short file is also just invalid
  s.files["ICURVE_B.BIN"].resize(40);
  CHECK(InputCurveLoad(s, &o, &slot) && slot == 0);
}
static void seq_wrap() {
  MemStorage s;
  put(s, "ICURVE_A.BIN", make(0xFFFFFFFFu, 1));
  put(s, "ICURVE_B.BIN", make(0, 2));
  InputCurveRecord o; int slot;
  CHECK(InputCurveLoad(s, &o, &slot) && slot == 1 && o.seq == 0);
  CHECK(InputCurveSave(s, make(0, 3)));   // writes A with seq 1
  InputCurveRecord a; CHECK(s.Read("ICURVE_A.BIN", &a, sizeof a) && a.seq == 1);
  MemStorage t;
  put(t, "ICURVE_A.BIN", make(0xFFFFFFFFu, 1));
  CHECK(InputCurveSave(t, make(0, 3)));   // B gets seq 0
  InputCurveRecord b; CHECK(t.Read("ICURVE_B.BIN", &b, sizeof b) && b.seq == 0);
  CHECK(InputCurveLoad(t, &o, &slot) && slot == 1);
}
static void no_files_uses_line_fit() {
  MemStorage s;
  InputCurveRecord o; int slot = 7;
  CHECK(!InputCurveLoad(s, &o, &slot));
  put(s, "ICURVE_A.BIN", make(1, 1));
  s.files["ICURVE_A.BIN"][0] ^= 1;   // magic broken
  CHECK(!InputCurveLoad(s, &o, nullptr));
}
static void readback_mismatch_fails_save() {
  MemStorage s;
  s.corrupt_writes = true;
  CHECK(!InputCurveSave(s, make(0, 1)));
  MemStorage t;
  CHECK(InputCurveSave(t, make(0, 1)));
}

static void calibration_file_predicate() {
  CHECK(IsCalibrationFile("ICURVE_A.BIN"));
  CHECK(IsCalibrationFile("ICURVE_B.BIN"));
  CHECK(!IsCalibrationFile("GLOBALS.DAT"));
  CHECK(!IsCalibrationFile("ICURVE"));
  CHECK(!IsCalibrationFile("XICURVE_A"));
  CHECK(!IsCalibrationFile(""));
}

int main() {
  calibration_file_predicate();
  roundtrip(); crc_rejects_one_bit(); ab_newest_valid_wins(); torn_copy_falls_back();
  seq_wrap(); no_files_uses_line_fit(); readback_mismatch_fails_save();
  if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
  printf("test_input_curve_store: all passed\n");
  return 0;
}
