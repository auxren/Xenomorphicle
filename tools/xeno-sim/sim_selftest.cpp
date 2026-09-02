// ---------------------------------------------------------------------------
// xeno-sim --test-phzconfig: the PhzConfig codec against the RAM volume.
//
// PhzConfig has two readers of the same byte format -- load_config() through
// a File and deserialize() from RAM -- and the preset engine relies on them
// agreeing: a slot section is serialize()d at save, and at recall is either
// deserialize()d (the G section) or written out as a file for the app to
// load_config() (the B, S and C sections). Nothing on the chip checks that
// the two readers accept the same images and reject the same damage; this
// does, with the real PhzConfig.cpp compiled against the sim's FS shim.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <cstring>
#include <vector>

#include <Arduino.h>

#include "PhzConfig.h"

#include "sim_selftest.h"

namespace {

int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { ++g_fails; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

using PhzConfig::KEY;
using PhzConfig::VALUE;

std::vector<uint8_t> &file_bytes(const char *name) {
  return PhzConfig::myfs.volume().files[name];
}
bool file_exists(const char *name) {
  return PhzConfig::myfs.volume().files.count(name) != 0;
}
void put_file(const char *name, const std::vector<uint8_t> &bytes) {
  PhzConfig::myfs.volume().files[name] = bytes;
}

bool value_is(KEY k, VALUE want) {
  VALUE v = 0;
  return PhzConfig::getValue(k, v) && v == want;
}
bool data_is(KEY k, VALUE want) {
  VALUE v = 0;
  return PhzConfig::getData(k, v) && v == want;
}
bool value_absent(KEY k) { VALUE v; return !PhzConfig::getValue(k, v); }
bool data_absent(KEY k)  { VALUE v; return !PhzConfig::getData(k, v); }

// A representative map: a few config keys across the namespaces the apps
// use, a couple of applet data keys.
void fill_sample() {
  PhzConfig::clear_config();
  PhzConfig::setValue(0x0001, 0x1122334455667788ull);
  PhzConfig::setValue(0x0800, 29);                 // PRESETBUS_KEY namespace
  PhzConfig::setValue(0x1234, 0);                  // a zero value must survive
  PhzConfig::setValue(0xFFFF, ~0ull);
  PhzConfig::setData(0x0010, 0xA5A5A5A5A5A5A5A5ull);
  PhzConfig::setData(0x0011, 7);
}
bool sample_intact() {
  return value_is(0x0001, 0x1122334455667788ull) && value_is(0x0800, 29) &&
         value_is(0x1234, 0) && value_is(0xFFFF, ~0ull) &&
         data_is(0x0010, 0xA5A5A5A5A5A5A5A5ull) && data_is(0x0011, 7);
}

std::vector<uint8_t> serialized(bool (*pred)(KEY) = nullptr,
                                KEY (*remap)(KEY) = nullptr) {
  std::vector<uint8_t> buf(4096);
  const size_t n = PhzConfig::serialize(buf.data(), buf.size(), pred, remap);
  buf.resize(n);
  return buf;
}

constexpr size_t kHeader = 12;
constexpr size_t kRecord = sizeof(KEY) + sizeof(VALUE);

void test_ram_round_trip() {
  printf("serialize/deserialize round trip\n");
  fill_sample();
  const auto img = serialized();
  CHECK(img.size() == 2 * kHeader + 6 * kRecord);
  CHECK(img[0] == 'P' && img[1] == 'Z');
  const size_t px = kHeader + 4 * kRecord;
  CHECK(img[px] == 'P' && img[px + 1] == 'X');

  PhzConfig::clear_config();
  CHECK(value_absent(0x0001));
  CHECK(PhzConfig::deserialize(img.data(), img.size()));
  CHECK(sample_intact());
  // the map came from RAM, so it belongs to no file
  CHECK(PhzConfig::unsaved_changes());

  // cap too small: nothing, not a partial image
  uint8_t small[kHeader + 2 * kRecord];
  memset(small, 0xEE, sizeof(small));
  CHECK(PhzConfig::serialize(small, sizeof(small)) == 0);
  CHECK(small[0] == 0xEE);   // untouched: the size check runs before the copy
}

void test_empty_maps() {
  printf("empty maps serialize to two bare headers and come back empty\n");
  PhzConfig::clear_config();
  const auto img = serialized();
  CHECK(img.size() == 2 * kHeader);
  PhzConfig::setValue(1, 1);
  CHECK(PhzConfig::deserialize(img.data(), img.size()));
  CHECK(value_absent(1));
}

bool only_low(KEY k) { return k < 0x0100; }
KEY plus_one(KEY k) { return k + 1; }

void test_filter_remap() {
  printf("a filtered, remapped image carries only the matching keys\n");
  fill_sample();
  const auto img = serialized(only_low, plus_one);
  CHECK(img.size() == 2 * kHeader + 3 * kRecord);   // 0x0001, 0x0010, 0x0011
  CHECK(PhzConfig::deserialize(img.data(), img.size()));
  CHECK(value_is(0x0002, 0x1122334455667788ull));
  CHECK(value_absent(0x0001));
  CHECK(value_absent(0x0801));
  CHECK(data_is(0x0011, 0xA5A5A5A5A5A5A5A5ull));
  CHECK(data_is(0x0012, 7));
  CHECK(data_absent(0x0010));
  // the live map was not touched by the filtering
  fill_sample();
  serialized(only_low, plus_one);
  CHECK(sample_intact());
}

void test_damaged_images() {
  printf("a damaged image is refused whole, not half-applied\n");
  fill_sample();
  const auto good = serialized();

  // a flipped value byte inside the PZ body: checksum mismatch
  auto img = good;
  img[kHeader + 3] ^= 0x40;
  CHECK(!PhzConfig::deserialize(img.data(), img.size()));
  CHECK(value_absent(0x0001) && data_absent(0x0010));   // nothing survives

  // the same in the PX body: the PZ half must not be kept either
  img = good;
  img[img.size() - 1] ^= 0x01;
  CHECK(!PhzConfig::deserialize(img.data(), img.size()));
  CHECK(value_absent(0x0001) && value_absent(0xFFFF));

  // cut mid-record
  img.assign(good.begin(), good.begin() + kHeader + 2 * kRecord + 3);
  CHECK(!PhzConfig::deserialize(img.data(), img.size()));

  // a record count that runs past the end
  img = good;
  img[2] = 0x40;
  CHECK(!PhzConfig::deserialize(img.data(), img.size()));

  // stray bytes after a good image
  img = good;
  img.push_back('P'); img.push_back('Z');
  CHECK(!PhzConfig::deserialize(img.data(), img.size()));

  // a bad signature up front
  img = good;
  img[0] = 'Q';
  CHECK(!PhzConfig::deserialize(img.data(), img.size()));

  // and nothing at all
  CHECK(!PhzConfig::deserialize(good.data(), 0));

  // the good image still loads after all that
  CHECK(PhzConfig::deserialize(good.data(), good.size()));
  CHECK(sample_intact());
}

void test_file_round_trip() {
  printf("save_config writes the bytes serialize produces, and load_config reads them back\n");
  fill_sample();
  const auto img = serialized();
  CHECK(PhzConfig::save_config("T_RT.CFG", PhzConfig::myfs));
  CHECK(file_exists("T_RT.CFG"));
  CHECK(!file_exists("PEWPEW.TMP"));
  CHECK(file_bytes("T_RT.CFG") == img);
  CHECK(!PhzConfig::unsaved_changes());

  PhzConfig::clear_config();
  CHECK(PhzConfig::load_config("T_RT.CFG", PhzConfig::myfs));
  CHECK(sample_intact());
  CHECK(!PhzConfig::unsaved_changes());

  // a clean map is not rewritten: scribble on the stored bytes and watch
  // save_config leave them alone
  file_bytes("T_RT.CFG").push_back(0x99);
  CHECK(PhzConfig::save_config("T_RT.CFG", PhzConfig::myfs));
  CHECK(file_bytes("T_RT.CFG").size() == img.size() + 1);
  // ...until something changes
  PhzConfig::setValue(0x0001, 5);
  CHECK(PhzConfig::unsaved_changes());
  CHECK(PhzConfig::save_config("T_RT.CFG", PhzConfig::myfs));
  CHECK(file_bytes("T_RT.CFG").size() == img.size());
  // setting the same value again keeps it clean
  PhzConfig::setValue(0x0001, 5);
  CHECK(!PhzConfig::unsaved_changes());

  // a missing file loads as an empty map and says so
  fill_sample();
  CHECK(!PhzConfig::load_config("NOPE.CFG", PhzConfig::myfs));
  CHECK(value_absent(0x0001));
}

// What the preset engine does with the B, S and C sections: the serialized
// bytes are written out as the app's file and the app load_config()s it.
void test_file_reads_serialized_images() {
  printf("load_config accepts every image serialize can produce\n");
  fill_sample();
  put_file("T_IMG.CFG", serialized());
  PhzConfig::clear_config();
  CHECK(PhzConfig::load_config("T_IMG.CFG", PhzConfig::myfs));
  CHECK(sample_intact());

  // an empty PZ chunk ahead of a PX chunk with records: a filtered save that
  // matched no config keys. The record loop used to run to end-of-file when
  // the expected count was zero, eating the PX chunk as config records.
  PhzConfig::clear_config();
  PhzConfig::setData(0x0010, 1);
  PhzConfig::setData(0x0011, 2);
  put_file("T_EPZ.CFG", serialized());
  PhzConfig::clear_config();
  CHECK(PhzConfig::load_config("T_EPZ.CFG", PhzConfig::myfs));
  CHECK(data_is(0x0010, 1) && data_is(0x0011, 2));
  CHECK(value_absent(0x0010));

  // both chunks empty
  PhzConfig::clear_config();
  put_file("T_EE.CFG", serialized());
  PhzConfig::setValue(1, 1);
  CHECK(PhzConfig::load_config("T_EE.CFG", PhzConfig::myfs));
  CHECK(value_absent(1));

  // a config key whose bytes spell "PX" as the last PZ record: the header
  // buffer used to double as the record buffer, so the PX signature check
  // saw the key and read the real PX chunk with the key's neighbours as its
  // count and checksum.
  PhzConfig::clear_config();
  PhzConfig::setValue(0x5850, 0x0102030405060708ull);
  PhzConfig::setData(0x0020, 3);
  put_file("T_PX.CFG", serialized());
  PhzConfig::clear_config();
  CHECK(PhzConfig::load_config("T_PX.CFG", PhzConfig::myfs));
  CHECK(value_is(0x5850, 0x0102030405060708ull));
  CHECK(data_is(0x0020, 3));
  CHECK(data_absent(0x5850));
}

void test_file_damage() {
  printf("load_config refuses the same damage deserialize does\n");
  fill_sample();
  const auto good = serialized();

  auto img = good;
  img[kHeader + 3] ^= 0x40;
  put_file("T_BAD.CFG", img);
  CHECK(!PhzConfig::load_config("T_BAD.CFG", PhzConfig::myfs));

  img.assign(good.begin(), good.begin() + kHeader + 2 * kRecord + 3);
  put_file("T_BAD.CFG", img);
  CHECK(!PhzConfig::load_config("T_BAD.CFG", PhzConfig::myfs));

  // three stray bytes on the end: a partial header. It used to be checked
  // against the previous header's bytes, and passed.
  img = good;
  img.push_back(0); img.push_back(0); img.push_back(0);
  put_file("T_BAD.CFG", img);
  CHECK(!PhzConfig::load_config("T_BAD.CFG", PhzConfig::myfs));

  img = good;
  img[2] = 0x40;
  put_file("T_BAD.CFG", img);
  CHECK(!PhzConfig::load_config("T_BAD.CFG", PhzConfig::myfs));

  img = good;
  img[0] = 'Q';
  put_file("T_BAD.CFG", img);
  CHECK(!PhzConfig::load_config("T_BAD.CFG", PhzConfig::myfs));

  put_file("T_BAD.CFG", {});
  CHECK(!PhzConfig::load_config("T_BAD.CFG", PhzConfig::myfs));

  // a refused load never marks the map as that file's clean copy
  CHECK(PhzConfig::unsaved_changes());
}

void test_save_filtered() {
  printf("save_filtered writes the filtered image and leaves the live map alone\n");
  fill_sample();
  const auto want = serialized(only_low, plus_one);
  CHECK(PhzConfig::save_filtered("T_FLT.CFG", PhzConfig::myfs, only_low, plus_one));
  CHECK(file_bytes("T_FLT.CFG") == want);
  CHECK(sample_intact());
  CHECK(PhzConfig::load_config("T_FLT.CFG", PhzConfig::myfs));
  CHECK(value_is(0x0002, 0x1122334455667788ull) && data_is(0x0012, 7));
}

}  // namespace

int SimPhzConfigSelfTest() {
  PhzConfig::myfs.begin(4u * 1024u * 1024u);
  test_ram_round_trip();
  test_empty_maps();
  test_filter_remap();
  test_damaged_images();
  test_file_round_trip();
  test_file_reads_serialized_images();
  test_file_damage();
  test_save_filtered();
  printf("%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
