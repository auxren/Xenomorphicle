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
  CHECK(Has(t, "MODULE UPDATING") && Has(t, "10%") && Has(t, "100/1000 B") && Has(t, "0 s"));
  CHECK(t.Update(2000, true, 999, 1, 1000) == READING);
  CHECK(t.Percent() == 99);
  // Unknown length: no divide by zero, never done.
  Tracker u;
  CHECK(u.Update(0, true, 50, 0, 0) == READING);
  CHECK(u.Percent() == 0);
  CHECK(u.Update(60000, true, 50, 60, 0) == READING);   // no length: never stalled either
}

// Done needs the full length AND kDoneQuietS of silence; short of either it is READING.
static void done_needs_full_and_quiet() {
  Tracker t;
  t.Update(0, true, 500, 0, 1000);
  CHECK(t.Update(kDoneQuietS * 1000u + 5000, true, 500, 9, 1000) == READING);   // quiet but not full (not done)
  Tracker f;
  f.Update(0, true, 1000, 0, 1000);
  CHECK(f.Update(kDoneQuietS * 1000u - 1, true, 1000, 7, 1000) == READING);     // full, not quiet long enough
  CHECK(f.Update(kDoneQuietS * 1000u, true, 1000, 12, 1000) == DONE);
  CHECK(Has(f, "UPDATE COMPLETE") && Has(f, "OFF"));
}

// A 251e reads twice: the bar clamps, the pass count shows, a resumed read takes DONE back.
static void two_passes_and_resume() {
  Tracker t;
  t.Update(0, true, 1000, 0, 1000);
  CHECK(t.Update(kDoneQuietS * 1000u, true, 1000, 12, 1000) == DONE);
  CHECK(t.Update(kDoneQuietS * 1000u + 100, true, 1200, 12, 1000) == READING);
  CHECK(t.Percent() == 100 && t.Pass() == 2);
  CHECK(Has(t, "100% pass 2") && Has(t, "1200/1000 B"));
  CHECK(t.Update(30000, true, 2000, 30, 1000) == READING);
  CHECK(t.Pass() == 2 && t.Percent() == 100);
  CHECK(t.Update(30000 + kDoneQuietS * 1000u, true, 2000, 42, 1000) == DONE);
}

// The card disarms after 30 s of quiet: counters read 0 and mode is false, DONE must stay.
static void done_survives_disarm() {
  Tracker t;
  t.Update(0, true, 1000, 0, 1000);
  t.Update(12000, true, 1000, 12, 1000);
  CHECK(t.state() == DONE);
  CHECK(t.Update(40000, false, 0, kNever, 0) == DONE);
  CHECK(t.Active() && Has(t, "UPDATE COMPLETE") && Has(t, "1000 B in 12 s"));
  CHECK(t.Update(900000, false, 0, kNever, 0) == DONE);
}

// A partial load that ends (k, abort) must not leave a stale screen or a false DONE.
static void partial_then_gone_is_idle() {
  Tracker t;
  t.Update(0, true, 400, 0, 1000);
  CHECK(t.Update(60000, false, 0, kNever, 0) == IDLE);
  CHECK(!Has(t, "UPDATE COMPLETE"));
}

// First read: READY turns into READING at once, and the Serial edge fires exactly once.
static void first_read_edge() {
  Tracker t;
  t.Update(0, true, 0, kNever, 1000);
  CHECK(t.state() == READY && !t.TakeStarted());
  CHECK(t.Update(50, true, 64, 0, 1000) == READING);
  CHECK(Has(t, "MODULE UPDATING"));
  CHECK(t.TakeStarted());
  CHECK(!t.TakeStarted());
  t.Update(100, true, 128, 0, 1000);
  CHECK(!t.TakeStarted());
}

// Started, then silent short of the image: STALLED after kStallS, not before; clears on a read.
static void stall_timer() {
  Tracker t;
  t.Update(0, true, 400, 0, 1000);
  CHECK(t.Update(kStallS * 1000u - 1, true, 400, 19, 1000) == READING);
  CHECK(t.Update(kStallS * 1000u, true, 400, 20, 1000) == STALLED);
  CHECK(Has(t, "READ STALLED 40%") && Has(t, "Remote"));
  CHECK(t.Update(kStallS * 1000u + 500, true, 450, 21, 1000) == READING);
  // and the full-length case is the DONE timer, never a stall
  Tracker f;
  f.Update(0, true, 1000, 0, 1000);
  CHECK(f.Update(kStallS * 1000u, true, 1000, 20, 1000) == DONE);
  // a stall that ends with the card gone is nothing
  Tracker g;
  g.Update(0, true, 400, 0, 1000);
  g.Update(kStallS * 1000u, true, 400, 20, 1000);
  CHECK(g.Update(kStallS * 1000u + 1, false, 0, kNever, 0) == IDLE);
}

// Dismiss: READY/STALLED/DONE hide on a press and stay hidden; READING swallows without hiding.
static void dismiss() {
  Tracker t;
  CHECK(!t.Swallow(true));                       // idle: events go to the app
  t.Update(0, true, 0, kNever, 1000);
  CHECK(t.Swallow(false) && t.Active());          // turning an encoder does not dismiss
  CHECK(t.Swallow(true) && !t.Active());          // a press does
  t.Update(10, true, 0, kNever, 1000);
  CHECK(!t.Active() && !t.Swallow(true));         // still hidden, app gets its events
  t.Update(20, true, 100, 0, 1000);               // the module starts: shows again
  CHECK(t.state() == READING && t.Active());
  CHECK(t.Swallow(true) && t.Active());           // swallowed, not dismissed
  t.Update(20 + kStallS * 1000u, true, 100, 20, 1000);
  CHECK(t.state() == STALLED && t.Active());
  CHECK(t.Swallow(true) && !t.Active());
  t.Update(30000, true, 1000, 30, 1000);
  t.Update(30000 + kDoneQuietS * 1000u, true, 1000, 42, 1000);
  CHECK(t.state() == DONE && t.Active());
  CHECK(t.Swallow(true) && !t.Active());
  t.Update(90000, false, 0, kNever, 0);          // disarmed: stays dismissed
  CHECK(t.state() == DONE && !t.Active());
}

// A fake marker: no flash, counts every call.
struct FakeMarker : Marker {
  bool present = false, save_ok = true;
  int saves = 0, removes = 0, disarms = 0;   // disarms: FakeArm, the FwCardArm(false) calls
  bool Exists() override { return present; }
  bool Save() override { saves++; if (save_ok) present = true; return save_ok; }
  void Remove() override { removes++; present = false; }
  void Disarm() override { disarms++; }
};

// DONE saves the marker exactly once per boot, and only DONE does.
static void marker_saved_once_at_done() {
  FakeMarker m;
  Tracker t;
  t.Attach(&m);
  t.Update(0, true, 0, kNever, 1000);
  t.Update(10, true, 500, 0, 1000);
  t.Update(kStallS * 1000u + 10, true, 500, 20, 1000);   // stalled: not a save
  CHECK(m.saves == 0 && t.TakeSaved() == 0 && m.disarms == 0);
  t.Update(30000, true, 1000, 30, 1000);
  CHECK(m.saves == 0);                                     // full but not quiet yet
  t.Update(30000 + kDoneQuietS * 1000u, true, 1000, 42, 1000);
  CHECK(t.state() == DONE && m.saves == 1 && m.present);
  CHECK(m.disarms == 1 && t.TakeDisarmed() && !t.TakeDisarmed());
  CHECK(t.TakeSaved() == 1 && t.TakeSaved() == 0);
  for (uint32_t i = 1; i < 50; ++i) t.Update(50000 + i * 16, true, 1000, 42, 1000);
  t.Update(60000, false, 0, kNever, 0);
  CHECK(m.saves == 1);                                     // every frame later: no more writes
  // resumed reads then DONE again (251e pass 2): still one save
  t.Update(70000, true, 1500, 50, 1000);
  t.Update(70000 + kDoneQuietS * 1000u, true, 1500, 62, 1000);
  CHECK(t.state() == DONE && m.saves == 1 && m.disarms == 1);   // withdrawn and re-reached: not again
}

static void marker_save_failure_is_reported_not_retried() {
  FakeMarker m;
  m.save_ok = false;
  Tracker t;
  t.Attach(&m);
  t.Update(0, true, 1000, 0, 1000);
  t.Update(kDoneQuietS * 1000u, true, 1000, 12, 1000);
  CHECK(t.state() == DONE && t.TakeSaved() == -1);
  CHECK(m.disarms == 1);                    // a failed save still disarms, once
  t.Update(kDoneQuietS * 1000u + 100, true, 1000, 12, 1000);
  CHECK(m.saves == 1 && m.disarms == 1);
}

// Next boot with the marker: shown, marker consumed at once, shown for kPostShowS, then gone.
static void post_shown_once_then_times_out() {
  FakeMarker m;
  m.present = true;
  Tracker t;
  t.Attach(&m);
  CHECK(t.Update(100, false, 0, kNever, 0) == POST);
  CHECK(t.Active() && m.removes == 1 && !m.present);          // consumed when shown
  CHECK(t.TakeShowing() && !t.TakeShowing());
  CHECK(Has(t, "FIRMWARE LOADED") && Has(t, "new firmware") && Has(t, "Remote ~5 s") && Has(t, "Then you are done"));
  CHECK(t.Update(100 + kPostShowS * 1000u - 1, false, 0, kNever, 0) == POST);
  CHECK(t.Update(100 + kPostShowS * 1000u, false, 0, kNever, 0) == IDLE);
  CHECK(!t.Active());
  CHECK(t.Update(100 + kPostShowS * 1000u + 5, false, 0, kNever, 0) == IDLE);   // shows once
  CHECK(m.removes == 1);
}

static void post_absent_means_idle() {
  FakeMarker m;
  Tracker t;
  t.Attach(&m);
  CHECK(t.Update(0, false, 0, kNever, 0) == IDLE);
  CHECK(!t.Active() && m.removes == 0 && !t.TakeShowing());
  Tracker u;                                                   // no marker object at all
  CHECK(u.Update(0, false, 0, kNever, 0) == IDLE);
}

static void post_dismissed_by_button() {
  FakeMarker m;
  m.present = true;
  Tracker t;
  t.Attach(&m);
  t.Update(0, false, 0, kNever, 0);
  CHECK(t.Swallow(false) && t.Active());                        // an encoder turn keeps it
  CHECK(t.Swallow(true) && !t.Active());                        // a press ends it
  CHECK(t.Update(100, false, 0, kNever, 0) == IDLE);            // and it does not come back
  CHECK(!t.Swallow(true));                                      // events reach the app again
}

// A card served on that boot wins; the marker stays until it can be shown.
static void post_yields_to_served_card() {
  FakeMarker m;
  m.present = true;
  Tracker t;
  t.Attach(&m);
  CHECK(t.Update(0, true, 0, kNever, 1000) == READY);
  CHECK(m.present && m.removes == 0 && !t.TakeShowing());
  CHECK(t.Update(10, true, 100, 0, 1000) == READING);
  CHECK(m.present);
  CHECK(t.Update(20, false, 0, kNever, 0) == POST);              // card left without DONE: now shown
  CHECK(m.removes == 1);
  // a card appearing while POST is up replaces it
  FakeMarker n;
  n.present = true;
  Tracker u;
  u.Attach(&n);
  u.Update(0, false, 0, kNever, 0);
  CHECK(u.state() == POST);
  CHECK(u.Update(5, true, 0, kNever, 1000) == READY);
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
  first_read_edge();
  stall_timer();
  dismiss();
  marker_saved_once_at_done();
  marker_save_failure_is_reported_not_retried();
  post_shown_once_then_times_out();
  post_absent_means_idle();
  post_dismissed_by_button();
  post_yields_to_served_card();
  rows_fit();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
