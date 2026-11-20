// Host tests for the firmware-card load screen's state logic (src/FwCardScreen.h).
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_fwcard_screen
//     test_fwcard_screen.cpp
#include <cstdio>
#include <cstring>

#include "../src/FwCardScreen.h"

using namespace FwCardScreen;

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static bool Has(const Tracker &t, const char *s) {
  char rows[kRows][kCols + 1];
  bool bar; uint32_t pct;
  t.Rows(rows, &bar, &pct);
  for (int i = 0; i < kRows; ++i)
    if (strstr(rows[i], s)) return true;
  return false;
}

static void idle_and_ready() {
  Tracker t;
  CHECK(t.Update(0, false, 0, kNever, 0) == IDLE);
  CHECK(!t.Active());
  CHECK(t.Update(100, true, 0, kNever, 63120) == READY);
  CHECK(Has(t, "Firmware card ready") && Has(t, "Remote"));
  // A card that is never read stays READY however long we wait.
  CHECK(t.Update(600000, true, 0, kNever, 63120) == READY);
}

static void reading_progress() {
  Tracker t;
  CHECK(t.Update(1000, true, 100, 0, 1000) == READING);
  CHECK(t.Percent() == 10 && t.Pass() == 1);
  CHECK(Has(t, "10%") && Has(t, "100/1000 B") && Has(t, "0 s"));
  CHECK(t.Update(2000, true, 999, 1, 1000) == READING);
  CHECK(t.Percent() == 99);
  // Unknown length: no divide by zero, never done.
  Tracker u;
  CHECK(u.Update(0, true, 50, 0, 0) == READING);
  CHECK(u.Percent() == 0);
  CHECK(u.Update(60000, true, 50, 60, 0) == READING);
}

// Done needs the full length AND kDoneQuietS of silence; short of either it is READING.
static void done_needs_full_and_quiet() {
  Tracker t;
  t.Update(0, true, 500, 0, 1000);
  CHECK(t.Update(kDoneQuietS * 1000u + 5000, true, 500, 9, 1000) == READING);   // quiet but not full
  Tracker f;
  f.Update(0, true, 1000, 0, 1000);
  CHECK(f.Update(kDoneQuietS * 1000u - 1, true, 1000, 7, 1000) == READING);     // full, not quiet long enough
  CHECK(f.Update(kDoneQuietS * 1000u, true, 1000, 8, 1000) == DONE);
  CHECK(Has(f, "Update complete") && Has(f, "OFF"));
}

// A 251e reads twice: the bar clamps, the pass count shows, a resumed read takes DONE back.
static void two_passes_and_resume() {
  Tracker t;
  t.Update(0, true, 1000, 0, 1000);
  CHECK(t.Update(9000, true, 1000, 9, 1000) == DONE);
  CHECK(t.Update(9100, true, 1200, 9, 1000) == READING);
  CHECK(t.Percent() == 100 && t.Pass() == 2);
  CHECK(Has(t, "100% pass 2") && Has(t, "1200/1000 B"));
  CHECK(t.Update(30000, true, 2000, 30, 1000) == READING);
  CHECK(t.Pass() == 2 && t.Percent() == 100);
  CHECK(t.Update(39000, true, 2000, 39, 1000) == DONE);
}

// The card disarms after 30 s of quiet: counters read 0 and mode is false, DONE must stay.
static void done_survives_disarm() {
  Tracker t;
  t.Update(0, true, 1000, 0, 1000);
  t.Update(9000, true, 1000, 9, 1000);
  CHECK(t.state() == DONE);
  CHECK(t.Update(40000, false, 0, kNever, 0) == DONE);
  CHECK(t.Active() && Has(t, "Update complete") && Has(t, "1000 B in 9 s"));
  CHECK(t.Update(900000, false, 0, kNever, 0) == DONE);
}

// A partial load that ends (k, abort) must not leave a stale screen or a false DONE.
static void partial_then_gone_is_idle() {
  Tracker t;
  t.Update(0, true, 400, 0, 1000);
  CHECK(t.Update(60000, false, 0, kNever, 0) == IDLE);
  CHECK(!Has(t, "Update complete"));
}

// Rows never overflow the 21-column screen, even at absurd values.
static void rows_fit() {
  Tracker t;
  t.Update(0, true, 4000000000u, 4000000u, 4000000000u);
  char rows[kRows][kCols + 1];
  bool bar; uint32_t pct;
  t.Rows(rows, &bar, &pct);
  for (int i = 0; i < kRows; ++i) CHECK(strlen(rows[i]) <= (size_t)kCols);
  CHECK(bar);
}

int main() {
  idle_and_ready();
  reading_progress();
  done_needs_full_and_quiet();
  two_passes_and_resume();
  done_survives_disarm();
  partial_then_gone_is_idle();
  rows_fit();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
