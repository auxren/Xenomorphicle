// Host tests for the Scope app's source table, quick groupings and settings
// persistence (src/ScopeSources.cpp, src/ScopeSettings.cpp). Pure state; no
// hardware, no Audio.h.
#include <cstdio>
#include <cstring>

#include "../src/ScopeSources.h"
#include "../src/ScopeSettings.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                          \
  } while (0)

using namespace ScopeSources;

static Mask Bit(Source s) { return (Mask)1u << s; }

static int FindQuick(Mask m) {
  for (int i = 0; i < QuickCount(); ++i)
    if (!QuickIsCustom(i) && QuickMask(i) == m) return i;
  return -1;
}

static void test_source_count_and_kinds() {
  printf("test_source_count_and_kinds\n");
  CHECK(SRC_COUNT == 24);
  CHECK(KindOf(SRC_CV_IN3) == KIND_CV_IN);
  CHECK(SubIndex(SRC_CV_IN3) == 2);
  CHECK(KindOf(SRC_OUT_R) == KIND_AUDIO_OUT);
  CHECK(SubIndex(SRC_OUT_R) == 1);
  CHECK(KindOf(SRC_CV_OUT8) == KIND_CV_OUT);
  CHECK(SubIndex(SRC_CV_OUT8) == 7);
  CHECK(KindOf(SRC_TR4) == KIND_TR);
  CHECK(SubIndex(SRC_TR4) == 3);
  CHECK(IsAc(SRC_IN_L));
  CHECK(IsAc(SRC_IN_R));
  CHECK(!IsAc(SRC_OUT_L));
  CHECK(!IsAc(SRC_CV_IN1));
  CHECK(RateHz(SRC_CV_IN1) == 37500.f);
  CHECK(RateHz(SRC_IN_R) == 48000.f);
  CHECK(RateHz(SRC_TR1) == 16666.f);
  char buf[8];
  Label(SRC_CV_IN8, buf, sizeof(buf));
  CHECK(strcmp(buf, "CV8") == 0);
  Label(SRC_IN_L, buf, sizeof(buf));
  CHECK(strcmp(buf, "INL") == 0);
  Label(SRC_CV_OUT1, buf, sizeof(buf));
  CHECK(strcmp(buf, "OUT1") == 0);
  Label(SRC_TR2, buf, sizeof(buf));
  CHECK(strcmp(buf, "TR2") == 0);
  for (int s = 0; s < SRC_COUNT; ++s) {
    Label((Source)s, buf, sizeof(buf));
    CHECK(strlen(buf) >= 2 && strlen(buf) <= 4);
  }
}

static void test_quick_list() {
  printf("test_quick_list\n");
  CHECK(FindQuick(Bit(SRC_IN_L)) >= 0);
  CHECK(FindQuick(Bit(SRC_IN_R)) >= 0);
  CHECK(FindQuick(Bit(SRC_IN_L) | Bit(SRC_IN_R)) >= 0);
  for (int i = 0; i < 8; ++i) CHECK(FindQuick(Bit((Source)(SRC_CV_IN1 + i))) >= 0);
  Mask cv14 = 0, cv58 = 0, tr = 0;
  for (int i = 0; i < 4; ++i) {
    cv14 |= Bit((Source)(SRC_CV_IN1 + i));
    cv58 |= Bit((Source)(SRC_CV_IN5 + i));
    tr |= Bit((Source)(SRC_TR1 + i));
  }
  CHECK(FindQuick(cv14) >= 0);
  CHECK(FindQuick(cv58) >= 0);
  CHECK(FindQuick(cv14 | tr) >= 0);
  CHECK(QuickIsCustom(QuickCount() - 1));
  CHECK(QuickMask(QuickCount() - 1) == 0);
  for (int i = 0; i < QuickCount() - 1; ++i) CHECK(!QuickIsCustom(i));
  for (int i = 0; i < QuickCount(); ++i) {
    const char *l = QuickLabel(i);
    CHECK(l && strlen(l) > 0 && strlen(l) <= 6);
  }
  CHECK(!QuickIsCustom(-1));
  CHECK(QuickMask(QuickCount()) == 0);
}

static void test_mask_to_list_order_and_cap() {
  printf("test_mask_to_list_order_and_cap\n");
  Source out[kMaxTraces];
  const Mask all = ((Mask)1u << SRC_COUNT) - 1;
  CHECK(MaskToList(all, out) == kMaxTraces);
  CHECK(out[0] == SRC_IN_L);
  for (int i = 1; i < kMaxTraces; ++i) CHECK(out[i] > out[i - 1]);
  CHECK(MaskToList(Bit(SRC_TR1) | Bit(SRC_CV_IN2), out) == 2);
  CHECK(out[0] == SRC_CV_IN2 && out[1] == SRC_TR1);
  CHECK(MaskToList(0, out) == 0);
  CHECK(MaskToList((Mask)1u << 30, out) == 0);  // bits past SRC_COUNT ignored
}

static void test_no_codec_mask() {
  printf("test_no_codec_mask\n");
  const Mask m = AvailableMask(false);
  CHECK(!(m & Bit(SRC_IN_L)));
  CHECK(!(m & Bit(SRC_IN_R)));
  CHECK(!(m & Bit(SRC_OUT_L)));
  CHECK(!(m & Bit(SRC_OUT_R)));
  CHECK(m & Bit(SRC_CV_IN1));
  CHECK(m & Bit(SRC_TR4));
  CHECK(AvailableMask(true) == ((Mask)1u << SRC_COUNT) - 1);
}

static bool Same(const ScopeSettings &a, const ScopeSettings &b) {
  return a.quick_idx == b.quick_idx && a.custom_mask == b.custom_mask && a.view == b.view &&
         a.stacked == b.stacked && a.trig_src == b.trig_src && a.trig_mode == b.trig_mode &&
         a.trig_level_mv == b.trig_level_mv && a.time_idx == b.time_idx &&
         a.time_auto == b.time_auto && a.volt_idx == b.volt_idx && a.volt_auto == b.volt_auto &&
         a.screensaver_trace == b.screensaver_trace;
}

static void test_settings_roundtrip() {
  printf("test_settings_roundtrip\n");
  ScopeSettings s, r;
  ScopeSettingsDefaults(&s);
  uint8_t buf[kScopeSettingsBytes];
  CHECK(ScopeSettingsPack(s, buf) == kScopeSettingsBytes);
  CHECK(ScopeSettingsUnpack(buf, sizeof(buf), &r));
  CHECK(Same(s, r));

  s.quick_idx = (uint8_t)(QuickCount() - 1);
  s.custom_mask = Bit(SRC_CV_IN2) | Bit(SRC_TR3);
  s.view = 1;
  s.stacked = 1;
  s.trig_src = 1;
  s.trig_mode = 2;
  s.trig_level_mv = -1234;
  s.time_idx = 9;
  s.time_auto = 0;
  s.volt_idx = 2;
  s.volt_auto = 0;
  s.screensaver_trace = 1;
  CHECK(ScopeSettingsPack(s, buf) == kScopeSettingsBytes);
  CHECK(ScopeSettingsUnpack(buf, sizeof(buf), &r));
  CHECK(Same(s, r));
}

static void test_legacy_restore() {
  printf("test_legacy_restore\n");
  ScopeSettings d, r;
  ScopeSettingsDefaults(&d);

  const uint8_t cv4[3] = {3, 3, 1};  // old channel 3 = CV IN 4
  CHECK(ScopeSettingsUnpack(cv4, 3, &r));
  CHECK(!QuickIsCustom(r.quick_idx));
  CHECK(QuickMask(r.quick_idx) == Bit(SRC_CV_IN4));
  CHECK(r.screensaver_trace == 1);
  CHECK(r.view == d.view && r.time_idx == d.time_idx && r.volt_idx == d.volt_idx);
  CHECK(r.time_auto == d.time_auto && r.volt_auto == d.volt_auto);

  const uint8_t inr[3] = {17, 0, 0};  // old channel 17 = AUDIO IN R
  CHECK(ScopeSettingsUnpack(inr, 3, &r));
  CHECK(QuickMask(r.quick_idx) == Bit(SRC_IN_R));
  CHECK(r.screensaver_trace == 0);

  const uint8_t cvout[3] = {10, 3, 0};  // old channel 10 = CV OUT 3
  CHECK(ScopeSettingsUnpack(cvout, 3, &r));
  CHECK(QuickIsCustom(r.quick_idx));
  CHECK(r.custom_mask == Bit(SRC_CV_OUT3));

  const uint8_t outl[3] = {18, 3, 0};  // old channel 18 = AUDIO OUT L
  CHECK(ScopeSettingsUnpack(outl, 3, &r));
  CHECK(QuickIsCustom(r.quick_idx));
  CHECK(r.custom_mask == Bit(SRC_OUT_L));
}

static void test_unpack_rejects_garbage() {
  printf("test_unpack_rejects_garbage\n");
  ScopeSettings d, r;
  ScopeSettingsDefaults(&d);
  uint8_t buf[32] = {0};
  memset(&r, 0x5A, sizeof(r));
  CHECK(!ScopeSettingsUnpack(buf, 0, &r));
  CHECK(Same(r, d));
  memset(&r, 0x5A, sizeof(r));
  CHECK(!ScopeSettingsUnpack(buf, 5, &r));
  CHECK(Same(r, d));
  memset(&r, 0x5A, sizeof(r));
  CHECK(!ScopeSettingsUnpack(buf, 17, &r));
  CHECK(Same(r, d));
  memset(&r, 0x5A, sizeof(r));
  CHECK(!ScopeSettingsUnpack(nullptr, 16, &r));
  CHECK(Same(r, d));

  ScopeSettings s;
  ScopeSettingsDefaults(&s);
  uint8_t ok[kScopeSettingsBytes];
  ScopeSettingsPack(s, ok);
  ok[0] = (uint8_t)QuickCount();  // quick_idx out of range
  memset(&r, 0x5A, sizeof(r));
  CHECK(!ScopeSettingsUnpack(ok, sizeof(ok), &r));
  CHECK(Same(r, d));

  const uint8_t badlegacy[3] = {20, 0, 0};  // channel past the old 0..19
  memset(&r, 0x5A, sizeof(r));
  CHECK(!ScopeSettingsUnpack(badlegacy, 3, &r));
  CHECK(Same(r, d));
}

int main() {
  test_source_count_and_kinds();
  test_quick_list();
  test_mask_to_list_order_and_cap();
  test_no_codec_mask();
  test_settings_roundtrip();
  test_legacy_restore();
  test_unpack_rejects_garbage();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
