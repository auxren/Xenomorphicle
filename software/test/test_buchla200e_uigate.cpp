// Host tests for the 200e app's UI gating (src/Buchla200eUiGate.cpp): why a
// Read is refused, and what becomes of a job the app is waiting on.
//
// These exist because of a real bug: pressing Read did nothing, said nothing,
// and the app never recovered. Both halves of that are pinned here -- a
// refusal must produce a reason, and an in-flight job must always reach a
// terminal fate rather than waiting forever.
//
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_buchla200e_uigate
//   test_buchla200e_uigate.cpp ../src/Buchla200eUiGate.cpp &&
//   ./build/test_buchla200e_uigate
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Buchla200eUiGate.h"

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

// Everything clear: a Read should be allowed.
static Buchla200eReadContext ok_ctx() {
  Buchla200eReadContext c;
  c.bus_enabled = true;
  c.read_active = false;
  c.write_active = false;
  c.scan_idle = true;
  c.probe_active = false;
  return c;
}

static void test_clear_context_allows_read() {
  printf("test_clear_context_allows_read\n");
  CHECK(Buchla200eCheckRead(ok_ctx()) == BUCHLA200E_READ_OK);
}

// The three conditions that used to be silent `return`s in StartRead().
static void test_each_blocker_is_reported() {
  printf("test_each_blocker_is_reported\n");

  Buchla200eReadContext c = ok_ctx();
  c.scan_idle = false;
  CHECK(Buchla200eCheckRead(c) == BUCHLA200E_READ_BUSY_SCAN);

  c = ok_ctx();
  c.probe_active = true;
  CHECK(Buchla200eCheckRead(c) == BUCHLA200E_READ_BUSY_PROBE);

  c = ok_ctx();
  c.read_active = true;
  CHECK(Buchla200eCheckRead(c) == BUCHLA200E_READ_IN_FLIGHT);

  c = ok_ctx();
  c.bus_enabled = false;
  CHECK(Buchla200eCheckRead(c) == BUCHLA200E_READ_BUS_OFF);

  // Added with the fix: a read must not start on top of a write either.
  c = ok_ctx();
  c.write_active = true;
  CHECK(Buchla200eCheckRead(c) == BUCHLA200E_READ_WRITE_IN_FLIGHT);
}

// No refusal may be silent: every non-OK code must map to a non-empty string
// short enough for a 128px line (21 chars at the 6px fixed font).
static void test_every_block_has_visible_text() {
  printf("test_every_block_has_visible_text\n");
  const Buchla200eReadBlock all[] = {
      BUCHLA200E_READ_OK,          BUCHLA200E_READ_BUS_OFF,
      BUCHLA200E_READ_IN_FLIGHT,   BUCHLA200E_READ_WRITE_IN_FLIGHT,
      BUCHLA200E_READ_BUSY_SCAN,   BUCHLA200E_READ_BUSY_PROBE,
      BUCHLA200E_READ_BAD_ADDR,
  };
  for (Buchla200eReadBlock b : all) {
    const char *t = Buchla200eReadBlockText(b);
    CHECK(t != nullptr);
    CHECK(t && t[0] != '\0');
    CHECK(t && strlen(t) <= 21);
  }
  // An out-of-range value must still produce something rather than fall off
  // the end of the switch.
  const char *t = Buchla200eReadBlockText((Buchla200eReadBlock)99);
  CHECK(t != nullptr && t[0] != '\0');
}

// "Bus off" cannot be fixed by waiting, so it must outrank the busy cases.
static void test_block_priority_reports_the_fundamental_cause() {
  printf("test_block_priority_reports_the_fundamental_cause\n");
  Buchla200eReadContext c = ok_ctx();
  c.bus_enabled = false;
  c.scan_idle = false;
  c.probe_active = true;
  c.read_active = true;
  CHECK(Buchla200eCheckRead(c) == BUCHLA200E_READ_BUS_OFF);

  c = ok_ctx();
  c.read_active = true;
  c.scan_idle = false;
  CHECK(Buchla200eCheckRead(c) == BUCHLA200E_READ_IN_FLIGHT);
}

// --- job fate --------------------------------------------------------------

static void test_terminal_states_are_terminal() {
  printf("test_terminal_states_are_terminal\n");
  CHECK(Buchla200eJobProgress(BUS200E_MASTER_DONE, 0, 1000) ==
        BUCHLA200E_JOB_DONE);
  CHECK(Buchla200eJobProgress(BUS200E_MASTER_FAILED, 0, 1000) ==
        BUCHLA200E_JOB_FAILED);
  // Terminal wins even past the deadline: a finished job is not a timeout.
  CHECK(Buchla200eJobProgress(BUS200E_MASTER_DONE, 999999, 1000) ==
        BUCHLA200E_JOB_DONE);
  CHECK(Buchla200eJobProgress(BUS200E_MASTER_FAILED, 999999, 1000) ==
        BUCHLA200E_JOB_FAILED);
}

// The bug: an in-flight job that never reaches DONE/FAILED used to hang the
// app forever. Every running state must time out.
static void test_running_states_eventually_time_out() {
  printf("test_running_states_eventually_time_out\n");
  const Bus200eMasterState running[] = {
      BUS200E_MASTER_FINDING_CARD, BUS200E_MASTER_SENDING,
      BUS200E_MASTER_WAIT_ACTIVITY, BUS200E_MASTER_TRANSFERRING,
  };
  for (Bus200eMasterState st : running) {
    CHECK(Buchla200eJobProgress(st, 0, 1000) == BUCHLA200E_JOB_PENDING);
    CHECK(Buchla200eJobProgress(st, 999, 1000) == BUCHLA200E_JOB_PENDING);
    CHECK(Buchla200eJobProgress(st, 1000, 1000) == BUCHLA200E_JOB_TIMEOUT);
    CHECK(Buchla200eJobProgress(st, 5000, 1000) == BUCHLA200E_JOB_TIMEOUT);
  }
}

// The live trigger: the master FSM is shared with the console commands and
// the USB bridge. A reset from either drops it to IDLE under a job the app
// believes it owns. MasterBackup() sets SENDING before returning 0, so IDLE
// can never mean "not started yet" -- it means the job is gone.
static void test_idle_under_an_active_job_is_lost_not_pending() {
  printf("test_idle_under_an_active_job_is_lost_not_pending\n");
  CHECK(Buchla200eJobProgress(BUS200E_MASTER_IDLE, 0, 1000) ==
        BUCHLA200E_JOB_LOST);
  // And it is lost immediately -- not after waiting out the timeout.
  CHECK(Buchla200eJobProgress(BUS200E_MASTER_IDLE, 1, 100000) ==
        BUCHLA200E_JOB_LOST);
}

// Whatever the FSM reports, the caller must always get a fate it can act on.
// This is the property that makes the hang impossible rather than unlikely.
static void test_no_state_can_strand_the_caller() {
  printf("test_no_state_can_strand_the_caller\n");
  for (int s = 0; s <= BUS200E_MASTER_FAILED + 2; ++s) {
    const Bus200eMasterState st = (Bus200eMasterState)s;
    // Past the deadline nothing may still read as PENDING.
    const Buchla200eJobFate f = Buchla200eJobProgress(st, 100000, 1000);
    CHECK(f != BUCHLA200E_JOB_PENDING);
  }
}

static void test_query_progress_mirrors_job_progress() {
  printf("test_query_progress_mirrors_job_progress\n");
  CHECK(Buchla200eQueryProgress(BUS200E_QUERY_DONE, 0, 1000) ==
        BUCHLA200E_JOB_DONE);
  CHECK(Buchla200eQueryProgress(BUS200E_QUERY_FAILED, 0, 1000) ==
        BUCHLA200E_JOB_FAILED);
  // A scan calls MasterQueryReset() as it walks; that used to strand an
  // in-flight probe forever, which then silently blocked every Read.
  CHECK(Buchla200eQueryProgress(BUS200E_QUERY_IDLE, 0, 1000) ==
        BUCHLA200E_JOB_LOST);
  CHECK(Buchla200eQueryProgress(BUS200E_QUERY_SENDING, 0, 1000) ==
        BUCHLA200E_JOB_PENDING);
  CHECK(Buchla200eQueryProgress(BUS200E_QUERY_WAITING, 1000, 1000) ==
        BUCHLA200E_JOB_TIMEOUT);

  for (int s = 0; s <= BUS200E_QUERY_FAILED + 2; ++s) {
    const Buchla200eJobFate f =
        Buchla200eQueryProgress((Bus200eQueryState)s, 100000, 1000);
    CHECK(f != BUCHLA200E_JOB_PENDING);
  }
}

// The app's backstop must be looser than the master's own, so that in the
// normal case the master fails the job and the app reports that real error
// rather than masking it with a generic timeout.
static void test_app_timeout_is_looser_than_the_master_hard_cap() {
  printf("test_app_timeout_is_looser_than_the_master_hard_cap\n");
  CHECK(BUCHLA200E_JOB_TIMEOUT_MS > BUS200E_MASTER_HARD_CAP_MS);
  CHECK(BUCHLA200E_QUERY_TIMEOUT_MS >
        BUS200E_MASTER_QUERY_SEND_TIMEOUT_MS +
            BUS200E_MASTER_QUERY_REPLY_TIMEOUT_MS);
  // A whole-bank 251e transfer is the long pole; the bound must be generous
  // enough that a legitimately slow one is not killed.
  CHECK(BUCHLA200E_JOB_TIMEOUT_MS >= 20000u);
}

int main() {
  test_clear_context_allows_read();
  test_each_blocker_is_reported();
  test_every_block_has_visible_text();
  test_block_priority_reports_the_fundamental_cause();
  test_terminal_states_are_terminal();
  test_running_states_eventually_time_out();
  test_idle_under_an_active_job_is_lost_not_pending();
  test_no_state_can_strand_the_caller();
  test_query_progress_mirrors_job_progress();
  test_app_timeout_is_looser_than_the_master_hard_cap();

  printf("\ntest_buchla200e_uigate: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
