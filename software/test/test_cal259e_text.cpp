// Host tests for the 259e Failed screen's text (src/Cal259eText.h): what the
// screen says about the module's record for each COMMIT outcome.
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_cal259e_text
//     test_cal259e_text.cpp ../src/Cal259eText.cpp ../src/Cal259eBackup.cpp
//     ../src/Cal259eFlow.cpp ../src/Cal259eBus.cpp ../src/CalRec.cpp
//     ../src/CalRoutineMeasure.cpp ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp
//     ../src/UsbTmc.cpp
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../src/Cal259eText.h"

typedef Cal259eFlow F;

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static Cal259eFailRows Rows(F::FailCode code, int cr, BackupWhy why = BK_NONE, uint8_t t = 0x28) {
  Cal259eFailRows r;
  Cal259eFailText(code, cr, why, t, &r);
  return r;
}
static bool Has(const Cal259eFailRows &r, const char *s) {
  for (int i = 0; i < 5; ++i)
    if (strstr(r.row[i], s)) return true;
  return false;
}

// S3: E_NO_SLOT programmed nothing. E_COMMIT_REFUSED is the module refusing AFTER
// programming (31.2C4 checks the read-back last): a slot may have been used.
static void refused_is_not_nothing_written() {
  const Cal259eFailRows ns = Rows(F::F_NO_SLOT, E_NO_SLOT);
  CHECK(Has(ns, "Nothing was written"));
  CHECK(Has(ns, "All 8 calibration") && Has(ns, "compaction disabled"));
  const Cal259eFailRows no = Rows(F::F_COMMIT_REFUSED, E_COMMIT_REFUSED, BK_NONE, 0x29);
  CHECK(!Has(no, "Nothing was written"));
  CHECK(Has(no, "A slot may be used"));
  CHECK(Has(no, "=29s") && Has(no, "=29r"));
  // the same when the refusal arrives after a cancel
  const Cal259eFailRows cn = Rows(F::F_CANCELLED, E_COMMIT_REFUSED);
  CHECK(!Has(cn, "Nothing was written") && Has(cn, "A slot may be used") && Has(cn, "=28s"));
  // a refusal that is E_NO_SLOT after a cancel: still nothing programmed
  CHECK(Has(Rows(F::F_CANCELLED, E_NO_SLOT), "Nothing was written"));
}

// S2: after a read-back mismatch the record is not known to be what was put.
static void readback_is_unknown_not_committed() {
  const Cal259eFailRows rb = Rows(F::F_READBACK, 1);
  CHECK(!Has(rb, "Record committed"));
  CHECK(Has(rb, "Written: UNKNOWN"));
  CHECK(Has(rb, "=28r"));
  CHECK(Has(Rows(F::F_CANCELLED, 1), "Record committed."));
  CHECK(Has(Rows(F::F_COMMIT_UNKNOWN, E_COMMIT_UNKNOWN), "Written: UNKNOWN"));
  CHECK(Has(Rows(F::F_COMMIT_UNKNOWN, E_COMMIT_UNKNOWN), "=28s and =28r"));
  CHECK(Has(Rows(F::F_BUS, 0), "Nothing was written"));
}

static void backup_reasons_reach_the_screen() {
  const Cal259eFailRows fw = Rows(F::F_BACKUP, 0, BK_FW_CARD);
  CHECK(Has(fw, "Firmware card served") && Has(fw, "Reboot the Xeno") && Has(fw, "card disarmed"));
  CHECK(Has(Rows(F::F_BACKUP, 0, BK_SIZE), "990 bytes"));
  CHECK(Has(Rows(F::F_BACKUP, 0, BK_SIZE), "Nothing was written"));
  CHECK(Has(Rows(F::F_NONLIN, 0), "5 cents"));
  CHECK(Has(Rows(F::F_EXPIRED, 0), "10 min"));
}

static void every_failure_fits_the_screen() {
  for (int c = F::F_NONE; c <= F::F_INTERNAL; ++c)
    for (int cr : {0, 1, int(E_NO_SLOT), int(E_COMMIT_REFUSED), int(E_COMMIT_UNKNOWN), int(E_BUS_ERROR)})
      for (int w = BK_NONE; w <= BK_TIMEOUT; ++w) {
        const Cal259eFailRows r = Rows(F::FailCode(c), cr, BackupWhy(w), 0x2B);
        for (int i = 0; i < 5; ++i) CHECK(strlen(r.row[i]) <= 21);
        CHECK(r.row[0][0] != 0);
      }
}

int main() {
  refused_is_not_nothing_written();
  readback_is_unknown_not_committed();
  backup_reasons_reach_the_screen();
  every_failure_fits_the_screen();
  printf("test_cal259e_text: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
