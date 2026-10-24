// Host tests for the ADC33131D frame-slot map and the one-time migration of
// stored per-input data (src/AdcSlotMap.cpp).
//   g++ -std=c++17 -Wall -Werror -O2 -o build/test_adc_slot_map \
//     test_adc_slot_map.cpp ../src/AdcSlotMap.cpp ../src/InputCurveStore.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/AdcSlotMap.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace CM = CvMeterMath;

static const int kIdentity[8] = {0, 1, 2, 3, 4, 5, 6, 7};
// ADC::Init with flip180: ADC_CHANNEL_1 = 7 ... ADC_CHANNEL_8 = 0.
static const int kFlipped[8] = {7, 6, 5, 4, 3, 2, 1, 0};
// Pinout_Detect for CalSynthXL: ADC_CHANNEL_1..8 = 4,5,6,7,0,1,2,3.
static const int kCalSynth[8] = {4, 5, 6, 7, 0, 1, 2, 3};

static CM::InputCalibration make_ica(uint8_t mask) {
  CM::InputCalibration c;
  CM::InputCalReset(c);
  c.valid_mask = mask;
  for (int i = 0; i < 8; ++i) {
    c.gain[i] = -3.f - 0.01f * i;
    c.offset_mv[i] = 12000.f + i;
    c.verify_mv10[i] = int16_t(10 + i);
    c.source_output[i] = uint8_t(1 + (i % 4));  // output labels, 1-based
  }
  return c;
}

static InputCurveRecord make_curves(uint8_t mask, uint8_t map_flag) {
  InputCurveRecord r;
  memset(&r, 0, sizeof r);
  r.seq = 7;
  r.valid_mask = mask;
  r.reserved2[0] = map_flag;
  for (int i = 0; i < 8; ++i) r.source_output[i] = uint8_t(8 - i);
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 17; ++j) r.mv_x2[i][j] = int16_t(i * 1000 + j);
  InputCurveEncode(r);
  return r;
}

// ---- the map -----------------------------------------------------------------

// Bench, 2026-10-24: 8.4 V into panel 1 read on ADC_CHANNEL_2, panel 2 on
// ADC_CHANNEL_3, panel 8 on ADC_CHANNEL_1, under the old slot table.
static void map_matches_bench() {
  const int expect[8] = {5, 6, 7, 0, 1, 2, 3, 4};
  for (int n = 0; n < 8; ++n) CHECK(AdcSlotForPanelInput(n) == expect[n]);
  // The old table gave slot AdcLegacySlotForPanelInput(ch) to channel ch.
  // Panel n arrives in slot AdcSlotForPanelInput(n), which the old table
  // handed to channel n+1 (wrapping 8 -> 1): the bench observation.
  for (int n = 0; n < 8; ++n) {
    int legacy_ch = -1;
    for (int ch = 0; ch < 8; ++ch)
      if (AdcLegacySlotForPanelInput(ch) == AdcSlotForPanelInput(n)) legacy_ch = ch;
    CHECK(legacy_ch == (n + 1) % 8);
  }
  // A permutation: every slot carries exactly one panel input.
  int seen = 0;
  for (int n = 0; n < 8; ++n) seen |= 1 << AdcSlotForPanelInput(n);
  CHECK(seen == 0xFF);
}

// ---- rotation ----------------------------------------------------------------

static void rotate_moves_each_entry_one_place() {
  uint16_t off[8];
  for (int i = 0; i < 8; ++i) off[i] = uint16_t(4000 + i);
  CM::InputCalibration ica = make_ica(0xFF);
  InputCurveRecord rec = make_curves(0xFF, 0);
  const CM::InputCalibration ica0 = ica;
  const InputCurveRecord rec0 = rec;
  RotateLegacyInputData(off, ica, &rec);  // the unflipped default table
  // Panel n's data was stored at index n+1 (wrapping); it is now at n.
  for (int n = 0; n < 8; ++n) {
    const int old = (n + 1) % 8;
    CHECK(off[n] == 4000 + old);
    CHECK(ica.gain[n] == ica0.gain[old]);
    CHECK(ica.offset_mv[n] == ica0.offset_mv[old]);
    CHECK(ica.verify_mv10[n] == ica0.verify_mv10[old]);
    // source_output moves with its input; the value (an output label) does not change.
    CHECK(ica.source_output[n] == ica0.source_output[old]);
    CHECK(rec.source_output[n] == rec0.source_output[old]);
    for (int j = 0; j < 17; ++j) CHECK(rec.mv_x2[n][j] == rec0.mv_x2[old][j]);
  }
  // Nothing else in either record moves.
  CHECK(ica.version == ica0.version);
  CHECK(rec.seq == rec0.seq);
  // A null curve record is allowed.
  RotateLegacyInputData(off, ica, nullptr);
  CHECK(off[0] == 4002);
}

static void rotate_moves_mask_bits() {
  // Bit 1 (stored for panel 1) moves to bit 0; bit 0 (panel 8) to bit 7.
  uint16_t off[8] = {};
  CM::InputCalibration ica = make_ica(0x03);
  InputCurveRecord rec = make_curves(0x02, 0);
  RotateLegacyInputData(off, ica, &rec);
  CHECK(ica.valid_mask == 0x81);
  CHECK(rec.valid_mask == 0x01);
  ica = make_ica(0x80);  // panel 7's fit, stored at index 7
  RotateLegacyIca(ica, kIdentity);
  CHECK(ica.valid_mask == 0x40);
}

// Flip mode permutes ADC_CHANNEL_n; the stored data is indexed by the
// physical index those names hold, so the move follows the table in force.
static void rotate_follows_flip_and_calsynth_tables() {
  // Under flip, label n's physical index is 8-n (0-based value), and the old
  // scan put physical jack m at ADC_CHANNEL_(m+1): index i's data moves to
  // index i+1 (wrapping).
  uint16_t off[8];
  for (int i = 0; i < 8; ++i) off[i] = uint16_t(i);
  RotateLegacyOffsets(off, kFlipped);
  for (int i = 0; i < 8; ++i) CHECK(off[i] == (i + 7) % 8);

  // General rule for any table: new[chan[m]] = old[chan[m+1]].
  for (int i = 0; i < 8; ++i) off[i] = uint16_t(100 + i);
  uint16_t old[8];
  memcpy(old, off, sizeof old);
  RotateLegacyOffsets(off, kCalSynth);
  for (int m = 0; m < 8; ++m) CHECK(off[kCalSynth[m]] == old[kCalSynth[(m + 1) % 8]]);

  CM::InputCalibration ica = make_ica(0x01);  // index 0: label 8 under flip
  RotateLegacyIca(ica, kFlipped);
  CHECK(ica.valid_mask == 0x02);
}

// ---- the curve record normalizes on load -------------------------------------

static void legacy_curve_record_normalizes_once() {
  InputCurveRecord rec = make_curves(0x02, 0);
  CHECK(InputCurveValid(rec));
  CHECK(InputCurveNormalize(rec, kIdentity));   // legacy: rotated
  CHECK(rec.valid_mask == 0x01);
  CHECK(rec.reserved2[0] == kInputMapV1);
  CHECK(InputCurveValid(rec));                  // CRC follows the change
  const InputCurveRecord once = rec;
  CHECK(!InputCurveNormalize(rec, kIdentity));  // already mapped: untouched
  CHECK(memcmp(&rec, &once, sizeof rec) == 0);
}

// ---- the boot migration --------------------------------------------------------

struct Unit {
  uint8_t marker[4];
  uint16_t off[8];
  uint8_t cal_flag;
  bool cal_loaded;
  CM::InputCalibration ica;
  bool ica_loaded;
  InputCurveRecord rec;
  bool curves_loaded;
  InputMapPages pages() {
    InputMapPages p;
    p.marker = marker;
    p.cal_loaded = cal_loaded;
    p.cal_offset = off;
    p.cal_flag = &cal_flag;
    p.ica_loaded = ica_loaded;
    p.ica = &ica;
    p.curves_loaded = curves_loaded;
    p.curves = &rec;
    return p;
  }
};

static Unit legacy_unit() {
  Unit u;
  memset(u.marker, 0xFF, sizeof u.marker);  // never written
  for (int i = 0; i < 8; ++i) u.off[i] = uint16_t(4000 + i);
  u.cal_flag = 0;
  u.cal_loaded = true;
  u.ica = make_ica(0x02);
  u.ica_loaded = true;
  u.rec = make_curves(0x04, 0);
  u.curves_loaded = true;
  return u;
}

static void migration_rotates_every_page_and_saves_in_order() {
  Unit u = legacy_unit();
  InputMapPages p = u.pages();
  const InputMapPlan plan = InputMapPrepare(p, kIdentity);
  CHECK(plan.run && plan.save_cal && plan.save_ica && plan.save_curves);
  CHECK(u.off[0] == 4001);
  CHECK(u.cal_flag == kInputMapV1);
  CHECK(u.ica.valid_mask == 0x01 && IcaMapFlag(u.ica) == kInputMapV1);
  CHECK(u.rec.valid_mask == 0x02 && CurveMapFlag(u.rec) == kInputMapV1);
  CHECK(InputCurveValid(u.rec));
}

static void rotate_idempotent_guarded() {
  // Marker present: nothing runs, nothing moves, nothing is saved.
  Unit u = legacy_unit();
  memcpy(u.marker, kInputMapMarker, 4);
  const Unit before = u;
  InputMapPages p = u.pages();
  const InputMapPlan plan = InputMapPrepare(p, kIdentity);
  CHECK(!plan.run && !plan.save_cal && !plan.save_ica && !plan.save_curves);
  CHECK(memcmp(u.off, before.off, sizeof u.off) == 0);
  CHECK(memcmp(&u.ica, &before.ica, sizeof u.ica) == 0);
  CHECK(memcmp(&u.rec, &before.rec, sizeof u.rec) == 0);
  CHECK(InputMapMarkerPresent(kInputMapMarker));
  const uint8_t near_miss[4] = {'A', 'M', 0, 0};
  CHECK(!InputMapMarkerPresent(near_miss));

  // Power lost after every page was saved but before the marker: the rerun
  // finds each page already stamped and moves nothing a second time.
  Unit v = legacy_unit();
  InputMapPages p1 = v.pages();
  InputMapPrepare(p1, kIdentity);
  const Unit after_first = v;  // as saved; marker still absent
  InputMapPages p2 = v.pages();
  const InputMapPlan rerun = InputMapPrepare(p2, kIdentity);
  CHECK(rerun.run && !rerun.save_cal && !rerun.save_ica && !rerun.save_curves);
  CHECK(memcmp(v.off, after_first.off, sizeof v.off) == 0);
  CHECK(memcmp(&v.ica, &after_first.ica, sizeof v.ica) == 0);
  CHECK(memcmp(&v.rec, &after_first.rec, sizeof v.rec) == 0);

  // Power lost after the calibration page only: the rerun leaves it and
  // moves the two pages that were not saved yet.
  Unit w = legacy_unit();
  InputMapPages p3 = w.pages();
  InputMapPrepare(p3, kIdentity);
  w.ica = make_ica(0x02);          // ICA1 and the curves still as on disk
  w.rec = make_curves(0x04, 0);
  InputMapPages p4 = w.pages();
  const InputMapPlan partial = InputMapPrepare(p4, kIdentity);
  CHECK(!partial.save_cal && partial.save_ica && partial.save_curves);
  CHECK(w.off[0] == 4001);         // once, not twice
  CHECK(w.ica.valid_mask == 0x01 && w.rec.valid_mask == 0x02);
}

static void fresh_unit_no_rotation_needed() {
  Unit u = legacy_unit();
  u.cal_loaded = false;            // defaults in RAM, no page
  u.ica_loaded = false;
  CM::InputCalReset(u.ica);
  u.curves_loaded = false;
  for (int i = 0; i < 8; ++i) u.off[i] = 4095;
  const Unit before = u;
  InputMapPages p = u.pages();
  const InputMapPlan plan = InputMapPrepare(p, kIdentity);
  CHECK(plan.run);                 // the caller writes the marker
  CHECK(!plan.save_cal && !plan.save_ica && !plan.save_curves);
  CHECK(memcmp(u.off, before.off, sizeof u.off) == 0);
  // RAM is stamped so a later save records the fixed map.
  CHECK(u.cal_flag == kInputMapV1 && IcaMapFlag(u.ica) == kInputMapV1);

  // A stored ICA1 page with no fitted channel is not rewritten either.
  Unit v = legacy_unit();
  v.cal_loaded = false;
  v.curves_loaded = false;
  v.ica = make_ica(0x00);
  InputMapPages q = v.pages();
  const InputMapPlan plan2 = InputMapPrepare(q, kIdentity);
  CHECK(plan2.run && !plan2.save_ica);
}

// A calibration-region restore clears the marker before its first byte lands,
// so the dump's own page flag decides: a dump from before the fix has no flag
// and is rotated; a newer dump carries the flag and is applied as it is.
static void prefix_backup_restore_rotated() {
  uint8_t marker[4];
  memcpy(marker, kInputMapMarker, 4);
  InputMapMarkerClear(marker);
  CHECK(!InputMapMarkerPresent(marker));

  Unit legacy = legacy_unit();      // the restored bytes: flag 0
  memcpy(legacy.marker, marker, 4);
  legacy.ica.reserved0[0] = kInputMapV1;   // ICA1 is not in the region: already migrated
  legacy.ica.valid_mask = 0x01;
  const CM::InputCalibration ica_before = legacy.ica;
  legacy.curves_loaded = false;
  InputMapPages p = legacy.pages();
  const InputMapPlan plan = InputMapPrepare(p, kIdentity);
  CHECK(plan.run && plan.save_cal && !plan.save_ica);
  CHECK(legacy.off[0] == 4001 && legacy.off[7] == 4000);
  CHECK(memcmp(&legacy.ica, &ica_before, sizeof legacy.ica) == 0);

  Unit fresh = legacy_unit();       // a dump made by fixed firmware: flag set
  memcpy(fresh.marker, marker, 4);
  fresh.cal_flag = kInputMapV1;
  fresh.ica_loaded = false;
  fresh.curves_loaded = false;
  InputMapPages q = fresh.pages();
  const InputMapPlan plan2 = InputMapPrepare(q, kIdentity);
  CHECK(!plan2.save_cal);
  CHECK(fresh.off[0] == 4000);

  // The dump's curve record ('V' packets) normalizes the same way.
  InputCurveRecord old_rec = make_curves(0x02, 0);
  CHECK(InputCurveNormalize(old_rec, kIdentity) && old_rec.valid_mask == 0x01);
  InputCurveRecord new_rec = make_curves(0x02, kInputMapV1);
  CHECK(!InputCurveNormalize(new_rec, kIdentity) && new_rec.valid_mask == 0x02);
}

int main() {
  map_matches_bench();
  rotate_moves_each_entry_one_place();
  rotate_moves_mask_bits();
  rotate_follows_flip_and_calsynth_tables();
  legacy_curve_record_normalizes_once();
  migration_rotates_every_page_and_saves_in_order();
  rotate_idempotent_guarded();
  fresh_unit_no_rotation_needed();
  prefix_backup_restore_rotated();
  if (g_fail) {
    printf("test_adc_slot_map: %d FAILED\n", g_fail);
    return 1;
  }
  printf("test_adc_slot_map: all passed\n");
  return 0;
}
