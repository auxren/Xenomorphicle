// Host tests for the foreign-module BACKUP/RESTORE master orchestration
// (src/Bus200eMaster.cpp): card-address discovery, frame construction (via
// the real Bus200eBuildTransferFrame(), not a re-implementation), the
// send/retry/timeout FSM and activity-based done-detection, all against a
// fake I2C transport (no hardware, no real bus).
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_bus200e_master test_bus200e_master.cpp \
//      ../src/Bus200eMaster.cpp ../src/PresetBus200e.cpp && \
//   ./build/test_bus200e_master
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../src/Bus200eMaster.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// ---- fake transport ---------------------------------------------------------

static uint32_t fake_now;
static int fake_gate_open;
static uint8_t occupied[128];         // occupied[addr7] = 1 -> probe_card ACKs it
static int send_result;               // 0 = ok, nonzero = simulated I2C error
static int send_fail_countdown;       // fail this many sends first, then succeed
static uint8_t last_sent[16];
static uint8_t last_sent_len;
static int send_calls;
static uint8_t last_suppressed[16];
static uint8_t last_suppressed_len;
static int suppress_calls;
static uint32_t fake_activity;

static uint32_t f_now(void) { return fake_now; }
static int f_gate_open(void) { return fake_gate_open; }
static int f_probe_card(uint8_t addr7) { return occupied[addr7 & 0x7F] ? 1 : 0; }
static int f_send_frame(const uint8_t *b, uint8_t n) {
  send_calls++;
  if (n > sizeof(last_sent)) n = sizeof(last_sent);
  memcpy(last_sent, b, n);
  last_sent_len = n;
  if (send_fail_countdown > 0) { send_fail_countdown--; return 1; }
  return send_result;
}
static void f_suppress_echo(const uint8_t *b, uint8_t n) {
  suppress_calls++;
  if (n > sizeof(last_suppressed)) n = sizeof(last_suppressed);
  memcpy(last_suppressed, b, n);
  last_suppressed_len = n;
}
static uint32_t f_card_activity(void) { return fake_activity; }

static const Bus200eMasterOps fake_ops = {
  f_now, f_gate_open, f_probe_card, f_send_frame, f_suppress_echo, f_card_activity,
};

static void reset(void) {
  fake_now = 0;
  fake_gate_open = 1;
  memset(occupied, 0, sizeof(occupied));
  send_result = 0;
  send_fail_countdown = 0;
  memset(last_sent, 0, sizeof(last_sent));
  last_sent_len = 0;
  send_calls = 0;
  memset(last_suppressed, 0, sizeof(last_suppressed));
  last_suppressed_len = 0;
  suppress_calls = 0;
  fake_activity = 0;
  Bus200eMasterInit(&fake_ops);
}

// ============================================================================

static void test_find_free_card(void) {
  printf("test_find_free_card\n");
  reset();
  const uint8_t candidates[] = { 0, 1, 2, 3 };
  uint8_t got = 0xFF;

  // nothing claimed: first candidate wins
  CHECK(Bus200eMasterFindFreeCard(candidates, 4, &got) == 1);
  CHECK(got == 0);

  // 0 and 1 claimed: first free is 2
  occupied[BUS200E_CARD_BASE | 0] = 1;
  occupied[BUS200E_CARD_BASE | 1] = 1;
  got = 0xFF;
  CHECK(Bus200eMasterFindFreeCard(candidates, 4, &got) == 1);
  CHECK(got == 2);

  // every candidate claimed: none free
  occupied[BUS200E_CARD_BASE | 2] = 1;
  occupied[BUS200E_CARD_BASE | 3] = 1;
  CHECK(Bus200eMasterFindFreeCard(candidates, 4, &got) == 0);

  // empty candidate list: none free
  reset();
  CHECK(Bus200eMasterFindFreeCard(candidates, 0, &got) == 0);
}

static void test_bad_ops_rejected(void) {
  printf("test_bad_ops_rejected\n");
  Bus200eMasterOps incomplete = fake_ops;
  incomplete.send_frame = nullptr;
  Bus200eMasterInit(&incomplete);
  CHECK(Bus200eMasterBackup(0x3C, 0) == -BUS200E_MASTER_ERR_BAD_ARGS);

  Bus200eMasterInit(nullptr);
  CHECK(Bus200eMasterBackup(0x3C, 0) == -BUS200E_MASTER_ERR_BAD_ARGS);
}

static void test_backup_happy_path(void) {
  printf("test_backup_happy_path\n");
  reset();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_IDLE);
  CHECK(Bus200eMasterBackup(0x3C, 0x02) == 0);
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);
  CHECK(Bus200eMasterModAddr() == 0x3C && Bus200eMasterCardAddr() == 0x02);
  CHECK(Bus200eMasterIsRestore() == 0);

  Bus200eMasterTask();  // gate open, sends immediately
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_WAIT_ACTIVITY);
  CHECK(send_calls == 1);
  const uint8_t want[] = { 0x07, 0x00, 0x22, 0x04, 0x3C, 0x02, 0x00, 0x00 };
  CHECK(last_sent_len == 8 && memcmp(last_sent, want, 8) == 0);
  CHECK(suppress_calls == 1);
  CHECK(last_suppressed_len == 8 && memcmp(last_suppressed, want, 8) == 0);

  // no activity yet: stays in WAIT_ACTIVITY
  fake_now += 500;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_WAIT_ACTIVITY);

  // target starts writing to the card
  fake_activity = 40;
  fake_now += 100;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_TRANSFERRING);

  // more bytes arrive: still transferring, not yet quiet
  fake_activity = 400;
  fake_now += 800;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_TRANSFERRING);

  // bus goes quiet for less than the done threshold: still transferring
  fake_now += (BUS200E_MASTER_QUIET_DONE_MS - 100);
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_TRANSFERRING);

  // quiet past the threshold: done
  fake_now += 200;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_DONE);
  CHECK(Bus200eMasterLastError() == BUS200E_MASTER_ERR_NONE);
}

static void test_restore_uses_restore_opcode(void) {
  printf("test_restore_uses_restore_opcode\n");
  reset();
  CHECK(Bus200eMasterRestore(0x3C, 0x00) == 0);
  CHECK(Bus200eMasterIsRestore() == 1);
  Bus200eMasterTask();
  const uint8_t want[] = { 0x07, 0x00, 0x22, 0x05, 0x3C, 0x00, 0x00, 0x00 };
  CHECK(memcmp(last_sent, want, 8) == 0);
}

static void test_busy_rejects_second_job(void) {
  printf("test_busy_rejects_second_job\n");
  reset();
  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);
  CHECK(Bus200eMasterBackup(0x44, 1) == -BUS200E_MASTER_ERR_BUSY);
  CHECK(Bus200eMasterRestore(0x44, 1) == -BUS200E_MASTER_ERR_BUSY);
  // the original job's target is untouched
  CHECK(Bus200eMasterModAddr() == 0x3C && Bus200eMasterCardAddr() == 0);

  Bus200eMasterTask();  // sends; now WAIT_ACTIVITY -- still busy
  CHECK(Bus200eMasterBackup(0x44, 1) == -BUS200E_MASTER_ERR_BUSY);
}

static void test_gate_closed_retries(void) {
  printf("test_gate_closed_retries\n");
  reset();
  fake_gate_open = 0;
  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);
  CHECK(send_calls == 0);

  fake_now += 500;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);  // still closed
  CHECK(send_calls == 0);

  fake_gate_open = 1;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_WAIT_ACTIVITY);
  CHECK(send_calls == 1);
}

static void test_send_retries_then_succeeds(void) {
  printf("test_send_retries_then_succeeds\n");
  reset();
  send_fail_countdown = 2;   // NAK/arb-loss twice, then a clean send
  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);

  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);  // 1st attempt failed
  CHECK(send_calls == 1);
  CHECK(suppress_calls == 0);

  fake_now += 10;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);  // 2nd attempt failed
  CHECK(send_calls == 2);

  fake_now += 10;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_WAIT_ACTIVITY);  // 3rd: ok
  CHECK(send_calls == 3);
  CHECK(suppress_calls == 1);
}

static void test_send_timeout(void) {
  printf("test_send_timeout\n");
  reset();
  fake_gate_open = 0;   // bus never goes quiet
  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);

  fake_now += BUS200E_MASTER_SEND_TIMEOUT_MS - 1;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);

  fake_now += 2;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_FAILED);
  CHECK(Bus200eMasterLastError() == BUS200E_MASTER_ERR_SEND_TIMEOUT);
  CHECK(send_calls == 0);
}

static void test_no_response_timeout(void) {
  printf("test_no_response_timeout\n");
  reset();
  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);
  Bus200eMasterTask();  // sent -> WAIT_ACTIVITY
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_WAIT_ACTIVITY);

  fake_now += BUS200E_MASTER_ACTIVITY_TIMEOUT_MS - 1;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_WAIT_ACTIVITY);

  fake_now += 2;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_FAILED);
  CHECK(Bus200eMasterLastError() == BUS200E_MASTER_ERR_NO_RESPONSE);
}

static void test_hard_cap_forces_done(void) {
  printf("test_hard_cap_forces_done\n");
  reset();
  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);
  Bus200eMasterTask();  // -> WAIT_ACTIVITY
  fake_activity = 1;
  Bus200eMasterTask();  // -> TRANSFERRING
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_TRANSFERRING);

  // activity keeps moving just under the quiet threshold every tick, so it
  // never naturally goes quiet -- the hard cap must still end the job
  for (int i = 0; i < 40; ++i) {
    fake_now += (BUS200E_MASTER_QUIET_DONE_MS - 50);
    fake_activity += 1;
    Bus200eMasterTask();
    if (Bus200eMasterGetState() != BUS200E_MASTER_TRANSFERRING) break;
  }
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_DONE);
  CHECK(Bus200eMasterLastError() == BUS200E_MASTER_ERR_NONE);
}

static void test_reset_only_from_terminal_states(void) {
  printf("test_reset_only_from_terminal_states\n");
  reset();
  Bus200eMasterReset();  // IDLE -> IDLE, harmless
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_IDLE);

  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);
  Bus200eMasterReset();  // busy: no-op
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);

  fake_gate_open = 0;
  fake_now += BUS200E_MASTER_SEND_TIMEOUT_MS + 1;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_FAILED);
  Bus200eMasterReset();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_IDLE);

  // a fresh job is accepted straight from a terminal state too, no Reset()
  // call required first
  reset();
  CHECK(Bus200eMasterBackup(0x3C, 0) == 0);
  fake_gate_open = 0;
  fake_now += BUS200E_MASTER_SEND_TIMEOUT_MS + 1;
  Bus200eMasterTask();
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_FAILED);
  CHECK(Bus200eMasterBackup(0x44, 1) == 0);
  CHECK(Bus200eMasterGetState() == BUS200E_MASTER_SENDING);
  CHECK(Bus200eMasterModAddr() == 0x44 && Bus200eMasterCardAddr() == 1);
}

int main() {
  test_find_free_card();
  test_bad_ops_rejected();
  test_backup_happy_path();
  test_restore_uses_restore_opcode();
  test_busy_rejects_second_job();
  test_gate_closed_retries();
  test_send_retries_then_succeeds();
  test_send_timeout();
  test_no_response_timeout();
  test_hard_cap_forces_done();
  test_reset_only_from_terminal_states();

  printf("\ntest_bus200e_master: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
