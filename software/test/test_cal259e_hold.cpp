// Host tests for the 259e write confirm's hold-to-write gate (src/Cal259eHold.h).
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_cal259e_hold test_cal259e_hold.cpp
#include <cstdio>
#include <initializer_list>

#include "../src/Cal259eHold.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef Cal259eHold H;

// Drives Tick every ms with the pin down until `until`; returns the fire count.
static int hold(H &h, uint32_t from, uint32_t until) {
  int fired = 0;
  for (uint32_t t = from; t <= until; ++t) fired += h.Tick(t, true) ? 1 : 0;
  return fired;
}

static void release_before_two_seconds_writes_nothing() {
  for (uint32_t release : {1u, 400u, 1000u, 1999u}) {
    H h;
    h.Arm(10000);
    h.BDown(10500, true);                         // past the dead time
    CHECK(h.holding());
    int fired = hold(h, 10500, 10500 + release);
    CHECK(fired == 0);
    h.BUp();
    CHECK(!h.holding() && h.hint());              // "hold B to write"
    CHECK(hold(h, 10500 + release, 20000) == 0);  // and nothing later
  }
}

static void hold_two_seconds_writes_exactly_once() {
  H h;
  h.Arm(5000);
  h.BDown(5400, true);
  CHECK(hold(h, 5400, 5400 + 1999) == 0);
  CHECK(h.permille(5400 + 1000) == 500);
  CHECK(h.Tick(5400 + 2000, true));               // the 2 s mark
  CHECK(h.fired() && !h.holding());
  CHECK(hold(h, 5400 + 2001, 20000) == 0);        // still down: no second fire
  h.BUp();
  CHECK(hold(h, 20001, 30000) == 0);
  // a long, jittery tick (a stalled loop) fires once too
  H g;
  g.Arm(0);
  g.BDown(400, true);
  CHECK(g.Tick(400 + 9000, true) && !g.Tick(400 + 9001, true));
}

static void hold_interrupted_by_another_key() {
  H h;
  h.Arm(0);
  h.BDown(500, true);
  CHECK(hold(h, 500, 1500) == 0);
  h.Other();                                      // encL, A, an encoder turn
  CHECK(!h.holding() && h.hint());
  CHECK(hold(h, 1501, 6000) == 0);                // B still down: it does not restart by itself
  h.BUp();
  // a chord: B went down while another was already down -> never starts
  H c;
  c.Arm(0);
  c.BDown(1000, false);
  CHECK(!c.holding() && hold(c, 1000, 6000) == 0);
  // the dead time: a B down in the first 350 ms does not start
  H d;
  d.Arm(1000);
  d.BDown(1349, true);
  CHECK(!d.holding() && hold(d, 1349, 6000) == 0);
  d.BUp();
  d.BDown(1350, true);
  CHECK(d.holding());
  CHECK(hold(d, 1350, 1350 + 2000) == 1);
}

static void pin_released_without_an_event() {
  H h;
  h.Arm(0);
  h.BDown(500, true);
  CHECK(!h.Tick(900, true));
  // the pin reads low for a bounce (< 40 ms) and recovers: the hold survives
  CHECK(!h.Tick(1000, false) && !h.Tick(1020, false) && h.holding());
  CHECK(!h.Tick(1021, true) && h.holding());
  // low for 40 ms: released (the event was lost)
  CHECK(!h.Tick(1100, false) && h.holding());
  CHECK(!h.Tick(1140, false) && !h.holding() && h.hint());
  CHECK(hold(h, 1141, 9000) == 0);
}

static void app_switch_during_the_hold() {
  H h;
  h.Arm(0);
  h.BDown(500, true);
  CHECK(hold(h, 500, 1200) == 0);
  h.Reset();                                      // SUSPEND / the app switcher / a stage change
  CHECK(!h.holding() && !h.hint());
  CHECK(hold(h, 1201, 9000) == 0);                // the pin is still down after the switch
  // and a fresh screen needs a fresh press
  h.Arm(10000);
  CHECK(hold(h, 10000, 20000) == 0);
  h.BDown(10400, true);
  CHECK(hold(h, 10400, 12400) == 1);
}

int main() {
  release_before_two_seconds_writes_nothing();
  hold_two_seconds_writes_exactly_once();
  hold_interrupted_by_another_key();
  pin_released_without_an_event();
  app_switch_during_the_hold();
  printf("test_cal259e_hold: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
