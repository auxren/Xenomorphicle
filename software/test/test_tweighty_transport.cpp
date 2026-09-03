// Host tests for the Tweighty app's WRITE/RECIRC transport
// (src/TweightyTransport.cpp). No hardware, no audio engine.
//
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_tweighty_transport
//   test_tweighty_transport.cpp ../src/TweightyTransport.cpp &&
//   ./build/test_tweighty_transport
#include <cassert>
#include <cstdio>

#include "../src/TweightyTransport.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                           \
  } while (0)

static void test_starts_in_write() {
  printf("test_starts_in_write\n");
  TweightyTransportState s;
  CHECK(s.mode == XP_WRITE);
  CHECK(transport_should_write(s));
  // A fresh state has nothing to resync yet.
  CHECK(!transport_consume_resync_edge(s));
}

static void test_begin_recirc_flips_mode_and_raises_the_edge() {
  printf("test_begin_recirc_flips_mode_and_raises_the_edge\n");
  TweightyTransportState s;
  transport_begin_recirc(s);
  CHECK(s.mode == XP_RECIRC);
  CHECK(!transport_should_write(s));
  CHECK(transport_consume_resync_edge(s));
  // One-shot: a second read without another event sees nothing.
  CHECK(!transport_consume_resync_edge(s));
}

static void test_begin_write_flips_mode_and_raises_the_edge() {
  printf("test_begin_write_flips_mode_and_raises_the_edge\n");
  TweightyTransportState s;
  transport_begin_recirc(s);
  transport_consume_resync_edge(s);   // clear the RECIRC edge first
  transport_begin_write(s);
  CHECK(s.mode == XP_WRITE);
  CHECK(transport_should_write(s));
  CHECK(transport_consume_resync_edge(s));
}

static void test_repeating_a_mode_is_a_no_op() {
  printf("test_repeating_a_mode_is_a_no_op\n");
  // Calling begin_write while already in WRITE must not fabricate a
  // crossfade the engine did not need -- a held button or a noisy digital
  // input re-asserting the same state should be silent.
  TweightyTransportState s;
  transport_consume_resync_edge(s);
  transport_begin_write(s);
  CHECK(s.mode == XP_WRITE);
  CHECK(!transport_consume_resync_edge(s));

  transport_begin_recirc(s);
  transport_consume_resync_edge(s);
  transport_begin_recirc(s);
  CHECK(s.mode == XP_RECIRC);
  CHECK(!transport_consume_resync_edge(s));
}

static void test_toggle_alternates() {
  printf("test_toggle_alternates\n");
  TweightyTransportState s;
  CHECK(s.mode == XP_WRITE);
  for (int i = 0; i < 6; ++i) {
    const transport_mode_t before = s.mode;
    transport_request_toggle(s);
    CHECK(s.mode != before);
    CHECK(transport_consume_resync_edge(s));
  }
  // Six toggles from WRITE lands back on RECIRC (even count from WRITE would
  // land on WRITE; six is even, so it does) -- pin the parity so a future
  // change to the toggle direction is caught here, not on the bench.
  CHECK(s.mode == XP_WRITE);
}

static void test_loop_wrap_raises_the_edge_without_changing_mode() {
  printf("test_loop_wrap_raises_the_edge_without_changing_mode\n");
  TweightyTransportState s;
  transport_begin_recirc(s);
  transport_consume_resync_edge(s);
  const transport_mode_t before = s.mode;
  transport_signal_loop_wrap(s);
  CHECK(s.mode == before);
  CHECK(transport_consume_resync_edge(s));
  CHECK(!transport_consume_resync_edge(s));
}

static void test_overlapping_events_collapse_to_one_edge() {
  printf("test_overlapping_events_collapse_to_one_edge\n");
  // A mode change and a loop-wrap landing in the same unconsumed window is
  // exactly the case the shared flag exists for: the engine only needs to
  // know "something happened", not how many somethings.
  TweightyTransportState s;
  transport_begin_recirc(s);
  transport_signal_loop_wrap(s);
  transport_signal_loop_wrap(s);
  CHECK(transport_consume_resync_edge(s));
  CHECK(!transport_consume_resync_edge(s));
}

static void test_rising_edge_helper() {
  printf("test_rising_edge_helper\n");
  bool prev = false;
  CHECK(!transport_rising_edge(false, prev));   // low -> low
  CHECK(transport_rising_edge(true, prev));     // low -> high: edge
  CHECK(!transport_rising_edge(true, prev));    // held high
  CHECK(!transport_rising_edge(false, prev));   // high -> low: no edge
  CHECK(transport_rising_edge(true, prev));     // low -> high again

  // Independent sources need independent `prev` state -- confirm one
  // instance does not leak into another (this is a usage contract, not
  // behaviour the function enforces, so this pins the contract in a test).
  bool prev_a = false, prev_b = true;
  CHECK(transport_rising_edge(true, prev_a));
  CHECK(!transport_rising_edge(true, prev_b));
}

int main() {
  test_starts_in_write();
  test_begin_recirc_flips_mode_and_raises_the_edge();
  test_begin_write_flips_mode_and_raises_the_edge();
  test_repeating_a_mode_is_a_no_op();
  test_toggle_alternates();
  test_loop_wrap_raises_the_edge_without_changing_mode();
  test_overlapping_events_collapse_to_one_edge();
  test_rising_edge_helper();

  printf("\ntest_tweighty_transport: %d checks, %d failures\n", checks,
         failures);
  return failures == 0 ? 0 : 1;
}
