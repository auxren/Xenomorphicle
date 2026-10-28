// Host tests for the 251e run's Failed screen text (src/Cal251eText.h): every fail code with
// every COMMIT outcome fits the 21-column panel, names itself in row 0 and tells the truth about
// the module's record. Standalone (no gtest), links like test_cal251e_flow.
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/Cal251eText.h"

typedef Cal251eFlow F;
static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static bool has(const Cal251eFailRows &r, const char *s) {
  for (int i = 0; i < 5; ++i)
    if (strstr(r.row[i], s)) return true;
  return false;
}

int main() {
  {
    Cal251eFailRows r;
    Cal251eFailText(F::F_LOOPBACK, 0, 1, 2, &r);
    CHECK(has(r, "by > 2.5 mV"));
  }
  const int crs[] = {0, 1, E_COMMIT_REFUSED, E_COMMIT_UNKNOWN, E_BUS_ERROR, E_BUS_BUSY, E_INTERNAL, E_NO_SLOT};
  for (int f = 0; f <= F::F_OLD_UNREADABLE; ++f) {
    for (int cr : crs) {
      Cal251eFailRows r;
      memset(&r, 'x', sizeof r);
      Cal251eFailText(F::FailCode(f), cr, 1, 2, &r);
      for (int i = 0; i < 5; ++i) {
        CHECK(strlen(r.row[i]) <= 21);
        for (const char *c = r.row[i]; *c; ++c) CHECK(*c >= 0x20 && *c < 0x7F);
      }
      CHECK(std::string(r.row[0]) == F::FailName(F::FailCode(f)));
      if (f == F::F_COMMIT_REFUSED) continue;   // its own rows (the verdict is the refusal)
      // Row 3 never claims "Nothing was written." when a COMMIT verdict says otherwise.
      if (cr == 1 && f != F::F_READBACK) CHECK(has(r, "Record committed."));
      if (cr == E_COMMIT_REFUSED) CHECK(has(r, "may be torn"));
      if (cr == E_COMMIT_UNKNOWN || cr == E_BUS_ERROR) CHECK(has(r, "Written: UNKNOWN."));
      if ((cr == 0 || cr == E_BUS_BUSY || cr == E_INTERNAL) && f != F::F_COMMIT_REFUSED)
        CHECK(has(r, "Nothing was written."));
      if (f == F::F_READBACK) CHECK(!has(r, "Record committed."));
    }
  }
  // The scale failure points at the log; the identity failure names the channel and the input.
  Cal251eFailRows r;
  Cal251eFailText(F::F_SCALE, 0, -1, -1, &r);
  CHECK(has(r, "383.1") && has(r, "CALR,251LINE"));
  Cal251eFailText(F::F_IDENTITY, 0, 1, 2, &r);
  CHECK(has(r, "CV2") && has(r, "input 3"));
  Cal251eFailText(F::F_IDENTITY, 0, 2, -1, &r);
  CHECK(has(r, "CV3") && has(r, "no cable"));
  // The records of a refused COMMIT are read by a new run's Scan (CALR,251OLD), never the 259e record reader.
  Cal251eFailText(F::F_COMMIT_UNKNOWN, 0, -1, -1, &r);
  CHECK(has(r, "251OLD") && !has(r, "=5Cr") && !has(r, "by hand"));
  Cal251eFailText(F::F_READBACK, 1, -1, -1, &r);
  CHECK(has(r, "251OLD") && !has(r, "=5Cr") && !has(r, "by hand"));
  // No row names a firmware string the build does not carry; the missing-251e rows say what is checked.
  Cal251eFailText(F::F_NO_251E, 0, -1, -1, &r);
  CHECK(has(r, "status reply") && has(r, "5C") && !has(r, "30.7C"));
  Cal251eFailText(F::F_NO_OVERRIDE, 0, -1, -1, &r);
  CHECK(has(r, "bit1") && !has(r, "30.7C"));
  // Every failure that leaves the module's record in doubt says how to read it, never "by hand" or =5Cr.
  for (int f = 0; f <= F::F_OLD_UNREADABLE; ++f)
    for (int cr : crs) {
      Cal251eFailText(F::FailCode(f), cr, -1, -1, &r);
      CHECK(!has(r, "by hand") && !has(r, "=5Cr"));
      const bool doubt = f == F::F_COMMIT_UNKNOWN || f == F::F_READBACK || f == F::F_COMMIT_REFUSED ||
                         cr == E_COMMIT_REFUSED;
      if (doubt) CHECK(has(r, "251OLD"));
    }
  Cal251eFailText(F::F_OLD_UNREADABLE, 0, -1, -1, &r);
  CHECK(has(r, "Old record unreadable") && has(r, "Nothing was written."));
  CHECK(has(r, "Retry") && has(r, "251OLD"));
  printf("test_cal251e_text: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
