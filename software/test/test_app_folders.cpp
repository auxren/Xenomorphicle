// Host tests for the app switcher's stored folder arrangement and its roster
// stamp (src/OC_app_folders.h). Standalone (no gtest):
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_app_folders
//     test_app_folders.cpp ../src/OC_app_folders.cpp
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/OC_app_folders.h"

using namespace OC::AppFolders;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// The stamp as OC_apps.cpp computed it before it moved here, written out
// independently: stamps already stored in GLOBALS.CFG must keep matching.
static uint64_t legacy_stamp(const std::vector<uint16_t> &ids) {
  uint64_t h = 1469598103934665603ull;
  for (uint16_t id : ids) { h ^= id; h *= 1099511628211ull; }
  return (h & ~0xffffull) | (uint64_t)(ids.size() & 0xffff);
}

// A T41_audio-like roster ending in Backup, before Cal Routine was appended.
static std::vector<uint16_t> base_roster() {
  return { TWOCCS("HS"), TWOCCS("QQ"), TWOCCS("EG"), TWOCCS("SQ"), TWOCCS("TU"),
           TWOCCS("TW"), TWOCCS("2E"), TWOCCS("SE"), TWOCCS("SC"), TWOCCS("BU") };
}

// The owner's own arrangement: every app somewhere its default does not put it.
static State custom(size_t n) {
  State s;
  for (size_t i = 0; i < n; ++i) s.Set(i, uint8_t((i * 3 + 1) % FOLDER_COUNT));
  return s;
}

static bool is_defaults(const State &s, const std::vector<uint16_t> &ids) {
  for (size_t i = 0; i < ids.size(); ++i)
    if (s.Of(i) != DefaultFor(ids[i])) return false;
  return true;
}

static void test_stamp_unchanged() {
  const auto r = base_roster();
  CHECK(RosterStamp(r.data(), r.size()) == legacy_stamp(r));
  CHECK((RosterStamp(r.data(), r.size()) & 0xffff) == r.size());
}

static void test_same_roster_keeps_layout() {
  const auto r = base_roster();
  const State saved = custom(r.size());
  State s;
  CHECK(ApplyStored(s, saved.bits[0], saved.bits[1], true, RosterStamp(r.data(), r.size()),
                    r.data(), r.size()) == STAMP_SAME);
  CHECK(s.bits[0] == saved.bits[0] && s.bits[1] == saved.bits[1]);
}

// The case this exists for: Cal Routine appended at the end of the roster.
static void test_one_appended_keeps_layout() {
  const auto old_r = base_roster();
  auto r = old_r;
  r.push_back(TWOCCS("CR"));
  const State saved = custom(old_r.size());  // never wrote the new slot
  const uint64_t stamp = RosterStamp(old_r.data(), old_r.size());
  CHECK(MatchStamp(stamp, r.data(), r.size()) == STAMP_ONE_APPENDED);
  State s;
  CHECK(ApplyStored(s, saved.bits[0], saved.bits[1], true, stamp, r.data(), r.size()) ==
        STAMP_ONE_APPENDED);
  for (size_t i = 0; i < old_r.size(); ++i) CHECK(s.Of(i) == saved.Of(i));
  CHECK(s.Of(r.size() - 1) == DefaultFor(TWOCCS("CR")));
  CHECK(s.Of(r.size() - 1) == FOLDER_SYSTEM);
  // A stale nibble in the new slot (whatever the old words held there) is
  // overwritten by the default too.
  State dirty = saved;
  dirty.Set(old_r.size(), FOLDER_HIDDEN);
  State s2;
  ApplyStored(s2, dirty.bits[0], dirty.bits[1], true, stamp, r.data(), r.size());
  CHECK(s2.Of(r.size() - 1) == FOLDER_SYSTEM);
  // ...also across the word boundary (the new app at position 16).
  std::vector<uint16_t> big;
  for (int i = 0; i < 16; ++i) big.push_back(uint16_t(0x4100 + i));
  const State bsaved = custom(16);
  const uint64_t bstamp = RosterStamp(big.data(), big.size());
  big.push_back(TWOCCS("CR"));
  State s3;
  CHECK(ApplyStored(s3, bsaved.bits[0], bsaved.bits[1], true, bstamp, big.data(), big.size()) ==
        STAMP_ONE_APPENDED);
  for (size_t i = 0; i < 16; ++i) CHECK(s3.Of(i) == bsaved.Of(i));
  CHECK(s3.Of(16) == FOLDER_SYSTEM);
}

// Every other roster change still refuses the words.
static void test_other_changes_refused() {
  const auto r = base_roster();
  const State saved = custom(r.size());
  auto refused = [&](const std::vector<uint16_t> &now, uint64_t stamp) {
    State s;
    const StampMatch m = ApplyStored(s, saved.bits[0], saved.bits[1], true, stamp, now.data(),
                                     now.size());
    return m == STAMP_OTHER && is_defaults(s, now);
  };
  const uint64_t st = RosterStamp(r.data(), r.size());
  // Inserted in the middle (T41 -> T41_audio shape).
  auto ins = r;
  ins.insert(ins.begin() + 4, TWOCCS("SP"));
  CHECK(refused(ins, st));
  // Inserted at the front.
  auto front = r;
  front.insert(front.begin(), TWOCCS("SP"));
  CHECK(refused(front, st));
  // Last app removed (a downgrade), and one from the middle.
  auto shorter = r;
  shorter.pop_back();
  CHECK(refused(shorter, st));
  auto mid = r;
  mid.erase(mid.begin() + 3);
  CHECK(refused(mid, st));
  // Two appended.
  auto two = r;
  two.push_back(TWOCCS("CR"));
  two.push_back(TWOCCS("SP"));
  CHECK(refused(two, st));
  // Reordered (same set, same count).
  auto swp = r;
  std::swap(swp[2], swp[3]);
  CHECK(refused(swp, st));
  // Last app replaced by another (same count).
  auto repl = r;
  repl.back() = TWOCCS("CR");
  CHECK(refused(repl, st));
  // One appended, but the first n-1 differ from what was stamped.
  auto app_mod = swp;
  app_mod.push_back(TWOCCS("CR"));
  CHECK(refused(app_mod, st));
  // An arrangement with no stamp (a build before the stamp existed).
  State s;
  CHECK(ApplyStored(s, saved.bits[0], saved.bits[1], false, st, r.data(), r.size()) ==
        STAMP_OTHER);
  CHECK(is_defaults(s, r));
  // A one-app roster never takes the empty roster's stamp as a prefix.
  const uint16_t one[1] = { TWOCCS("SE") };
  CHECK(MatchStamp(RosterStamp(one, 0), one, 1) == STAMP_OTHER);
}

int main() {
  test_stamp_unchanged();
  test_same_roster_keeps_layout();
  test_one_appended_keeps_layout();
  test_other_changes_refused();
  printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}
