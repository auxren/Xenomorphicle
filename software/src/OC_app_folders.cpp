// The app switcher's stored folder arrangement and its roster stamp. See
// OC_app_folders.h. Pure: host-tested by test/test_app_folders.cpp.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define FOLDERS_CODE FLASHMEM
#else
#define FOLDERS_CODE
#endif

#include "OC_app_folders.h"

namespace OC {
namespace AppFolders {

FOLDERS_CODE uint64_t RosterStamp(const uint16_t *ids, size_t n) {
  uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis
  for (size_t i = 0; i < n; ++i) {
    h ^= ids[i];
    h *= 1099511628211ull;
  }
  return (h & ~0xffffull) | (uint64_t)(n & 0xffff);
}

FOLDERS_CODE StampMatch MatchStamp(uint64_t stored, const uint16_t *ids, size_t n) {
  if (stored == RosterStamp(ids, n)) return STAMP_SAME;
  // The stamp of ids[0, n-1) carries count n-1 in its low bits, so this
  // matches only a roster that was exactly this one minus its last app (up to
  // the 48-bit hash). n >= 2: an empty roster never wrote an arrangement.
  if (n >= 2 && stored == RosterStamp(ids, n - 1)) return STAMP_ONE_APPENDED;
  return STAMP_OTHER;
}

FOLDERS_CODE void SeedDefaults(State &s, const uint16_t *ids, size_t n) {
  for (size_t i = 0; i < n && i < kMaxApps; ++i) s.Set(i, DefaultFor(ids[i]));
}

FOLDERS_CODE StampMatch ApplyStored(State &s, uint64_t w0, uint64_t w1, bool have_stamp,
                                    uint64_t stamp, const uint16_t *ids, size_t n) {
  const StampMatch m = have_stamp ? MatchStamp(stamp, ids, n) : STAMP_OTHER;
  if (m == STAMP_OTHER) {
    SeedDefaults(s, ids, n);
    return m;
  }
  s.bits[0] = w0;
  s.bits[1] = w1;
  // The appended app was never in the roster that wrote the words, so its
  // nibble means nothing: give it its default, and leave every other alone.
  if (m == STAMP_ONE_APPENDED) s.Set(n - 1, DefaultFor(ids[n - 1]));
  return m;
}

}  // namespace AppFolders
}  // namespace OC
