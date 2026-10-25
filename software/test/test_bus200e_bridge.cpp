// Host tests for the browser <-> 200e preset-dump session layer
// (src/Bus200eBridge.cpp): the piece that turns received SysEx into card-image
// bytes + MasterRestore(), and a completed MasterBackup() into an outbound
// DUMP_DATA/DUMP_END stream.
//
// No USB, no MIDI, no I2C: a fake Bus200eBridgeOps table stands in for
// usbMIDI and OC::PresetBus, exactly the way test_bus200e_master.cpp fakes
// the I2C transport. Same standalone (no gtest) shape as its neighbours:
//   g++ -std=c++17 -Wall -Werror -O2 -o build/test_bus200e_bridge
//       test_bus200e_bridge.cpp ../src/Bus200eBridge.cpp ../src/Bus200eSysEx.cpp
//   ./build/test_bus200e_bridge
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "../src/Bus200eBridge.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// ---- fake platform ----------------------------------------------------------

// The default fake card is small (8K) so most tests stay cheap. Two things
// need more: the real 63120-byte 251e bank (which needs the full
// BUS200E_BRIDGE_MAX_DUMP_BYTES image), and the "capture larger than this
// build can describe" refusal, which is now only reachable on a card image
// bigger than the 64K a card pointer can name -- so the backing array is
// sized past that and fake_card_size is per-test.
static const uint32_t kFakeCardSmall = 8192;
static const uint32_t kFakeCardFull = BUS200E_BRIDGE_MAX_DUMP_BYTES;   // 65536
static const uint32_t kFakeCardOversize = BUS200E_BRIDGE_MAX_DUMP_BYTES + 4096;
static uint8_t fake_card[kFakeCardOversize];
static uint32_t fake_card_size;

static uint32_t fake_now;
static int fake_serving;
static int fake_serve_enable_result;   // what card_serve_enable(1) returns
static int fake_serve_enable_calls;
static int fake_image_null;            // simulate "not serving" mid-session

static int fake_backup_result;         // MasterBackup return
static int fake_restore_result;
static uint8_t fake_backup_addr, fake_restore_addr;
static int fake_backup_calls, fake_restore_calls;
static Bus200eMasterState fake_state;
static Bus200eMasterError fake_error;
static uint32_t fake_bytes;
static int fake_reset_calls;
static int fake_dirty_calls;

struct SentMsg {
  uint8_t cmd;
  uint16_t seq, total;        // 14-bit as of protocol v2
  std::vector<uint8_t> raw;   // unpacked payload (or chunk, for DUMP_DATA)
  int parse_rc;
};
static std::vector<SentMsg> sent;

static uint32_t f_now() { return fake_now; }
static int f_card_serving() { return fake_serving; }
static int f_card_serve_enable(int on) {
  fake_serve_enable_calls++;
  if (!on) { fake_serving = 0; return 0; }
  if (fake_serve_enable_result == 0) fake_serving = 1;
  return fake_serve_enable_result;
}
static uint8_t *f_card_image() {
  return (fake_serving && !fake_image_null) ? fake_card : nullptr;
}
static uint32_t f_card_size() { return fake_card_size; }
static void f_card_dirty() { fake_dirty_calls++; }

static int f_master_backup(uint8_t a) {
  fake_backup_calls++; fake_backup_addr = a;
  if (fake_backup_result == 0) { fake_serving = 1; fake_state = BUS200E_MASTER_SENDING; }
  return fake_backup_result;
}
static int f_master_restore(uint8_t a) {
  fake_restore_calls++; fake_restore_addr = a;
  if (fake_restore_result == 0) fake_state = BUS200E_MASTER_SENDING;
  return fake_restore_result;
}
static Bus200eMasterState f_master_state() { return fake_state; }
static Bus200eMasterError f_master_error() { return fake_error; }
static uint32_t f_master_bytes() { return fake_bytes; }
static void f_master_reset() {
  fake_reset_calls++;
  fake_state = BUS200E_MASTER_IDLE;
  fake_error = BUS200E_MASTER_ERR_NONE;
}

static void f_send(const uint8_t *payload, uint32_t len) {
  SentMsg m = {};
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES];
  uint32_t raw_len = 0;
  m.parse_rc = Bus200eSysExParseMessage(payload, len, &m.cmd, &m.seq, &m.total,
                                        raw, sizeof(raw), &raw_len);
  m.raw.assign(raw, raw + (m.parse_rc == 0 ? raw_len : 0));
  sent.push_back(m);
}

// ---- fake firmware-card side (docs/bus200e-sysex-fw.md) ----------------------
static int fw_begin_result, fw_begin_calls, fw_commit_result, fw_commit_calls;
static uint8_t fw_begin_kind;
static uint32_t fw_commit_len, fw_commit_crc;
static int fw_abort_calls, fw_armed_calls;
static int fw_armed_v, fw_serving_v, fw_wpm_v;
static std::vector<uint8_t> fw_pushed;
static int fw_arm_calls, fw_arm_last;
static uint32_t fw_served_v, fw_age_v, fw_len_v;
static uint8_t fw_kind_v;

static int f_fw_begin(uint8_t k) { fw_begin_calls++; fw_begin_kind = k; return fw_begin_result; }
static void f_fw_push(uint8_t b) { fw_pushed.push_back(b); }
static int f_fw_commit(uint32_t l, uint32_t c) {
  fw_commit_calls++; fw_commit_len = l; fw_commit_crc = c; return fw_commit_result;
}
static void f_fw_abort() { fw_abort_calls++; }
static int f_fw_armed() { fw_armed_calls++; return fw_armed_v; }
static void f_fw_arm(int on) { fw_arm_calls++; fw_arm_last = on; }
static int f_fw_serving() { return fw_serving_v; }
static uint32_t f_fw_served() { return fw_served_v; }
static uint32_t f_fw_age() { return fw_age_v; }
static uint32_t f_fw_len() { return fw_len_v; }
static uint8_t f_fw_kind() { return fw_kind_v; }
static int f_wpm() { return fw_wpm_v; }

// ---- fake scan / poll-done / bus-status side ----------------------------------
// query_start resolves at once: DONE for an address in fake_q_done, else FAILED
// (set fake_q_hold to leave it SENDING until the test flips fake_q_state).
struct QStart { uint8_t addr; uint32_t at; };
static std::vector<QStart> q_starts;
static Bus200eQueryState fake_q_state;
static int fake_q_start_rc, fake_q_hold, fake_q_reset_calls;
static bool fake_q_done[256];
static int fake_poll_rc, fake_poll_calls;
static int fake_quiet_v, fake_dialect_v;
static uint8_t fake_ver_addr;            // last_version_addr
static bool fake_ver_known[256];
static uint8_t fake_ver_bytes[7];

static int f_query_start(uint8_t a) {
  if (fake_q_start_rc != 0) return fake_q_start_rc;
  q_starts.push_back({ a, fake_now });
  fake_q_state = fake_q_hold ? BUS200E_QUERY_SENDING
                             : (fake_q_done[a] ? BUS200E_QUERY_DONE : BUS200E_QUERY_FAILED);
  return 0;
}
static Bus200eQueryState f_query_state() { return fake_q_state; }
static void f_query_reset() { fake_q_reset_calls++; fake_q_state = BUS200E_QUERY_IDLE; }
static int f_version_get(uint8_t a, uint8_t out[7]) {
  if (!fake_ver_known[a]) return 0;
  memcpy(out, fake_ver_bytes, 7);
  return 1;
}
static int f_send_poll_done() { fake_poll_calls++; return fake_poll_rc; }
static int f_bus_quiet() { return fake_quiet_v; }
static uint8_t f_bus_dialect() { return (uint8_t) fake_dialect_v; }
static uint8_t f_last_ver_addr() { return fake_ver_addr; }
static std::vector<uint16_t> fake_timeouts;
static void f_set_timeout(uint16_t ms) { fake_timeouts.push_back(ms); }
static uint16_t fake_ver_gen;
static uint16_t f_ver_gen() { return fake_ver_gen; }

static int fake_cal_run;
static int f_cal_run() { return fake_cal_run; }

static const Bus200eBridgeOps fake_ops = {
  f_now,
  f_card_serving, f_card_serve_enable, f_card_image, f_card_size, f_card_dirty,
  f_master_backup, f_master_restore, f_master_state, f_master_error,
  f_master_bytes, f_master_reset,
  f_send,
  f_fw_begin, f_fw_push, f_fw_commit, f_fw_abort, f_fw_armed, f_fw_arm,
  f_fw_serving, f_fw_served, f_fw_age, f_fw_len, f_fw_kind, f_wpm,
  f_query_start, f_query_state, f_query_reset, f_version_get,
  f_send_poll_done, f_bus_quiet, f_bus_dialect, f_last_ver_addr,
  f_ver_gen,
  f_set_timeout,
  f_cal_run,
};

static void reset_all(uint32_t card_size = kFakeCardSmall) {
  // Drop whatever the previous test left in flight BEFORE the counters are
  // zeroed: Abort() releases the far side (fw_abort / query_reset).
  Bus200eBridgeAbort();
  memset(fake_card, 0xFF, sizeof(fake_card));
  fake_card_size = card_size;
  fake_now = 1000;
  fake_serving = 0;
  fake_serve_enable_result = 0;
  fake_serve_enable_calls = 0;
  fake_image_null = 0;
  fake_backup_result = 0;
  fake_restore_result = 0;
  fake_backup_addr = fake_restore_addr = 0;
  fake_backup_calls = fake_restore_calls = 0;
  fake_state = BUS200E_MASTER_IDLE;
  fake_error = BUS200E_MASTER_ERR_NONE;
  fake_bytes = 0;
  fake_reset_calls = 0;
  fake_dirty_calls = 0;
  fw_begin_result = 0; fw_begin_calls = 0; fw_commit_result = 0; fw_commit_calls = 0;
  fw_begin_kind = 0; fw_commit_len = 0; fw_commit_crc = 0;
  fw_abort_calls = 0; fw_armed_calls = 0;
  fw_armed_v = fw_serving_v = fw_wpm_v = 0;
  fw_pushed.clear();
  fw_arm_calls = 0; fw_arm_last = -1;
  fw_served_v = fw_age_v = fw_len_v = 0; fw_kind_v = 0;
  q_starts.clear();
  fake_timeouts.clear();
  fake_q_state = BUS200E_QUERY_IDLE;
  fake_q_start_rc = 0; fake_q_hold = 0; fake_q_reset_calls = 0;
  memset(fake_q_done, 0, sizeof(fake_q_done));
  fake_poll_rc = 0; fake_poll_calls = 0;
  fake_quiet_v = 1; fake_dialect_v = 0;
  fake_ver_addr = 0;
  fake_ver_gen = 0;
  fake_cal_run = 0;
  memset(fake_ver_known, 0, sizeof(fake_ver_known));
  memcpy(fake_ver_bytes, "31.2C2 ", 7);
  sent.clear();
  Bus200eBridgeInit(&fake_ops);
}

// ---- host-side helpers (what the browser does) -------------------------------

// Feed one command with a plain (already 7-bit, unpacked) payload.
static void feed(uint8_t cmd, std::initializer_list<uint8_t> payload,
                 bool wrap_f0f7 = false) {
  std::vector<uint8_t> p(payload);
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildMessage(cmd, p.data(), (uint32_t) p.size(),
                                         nullptr, 0, msg, sizeof(msg));
  assert(n > 0);
  if (!wrap_f0f7) {
    Bus200eBridgeHandleSysEx(msg, (uint32_t) n);
    return;
  }
  std::vector<uint8_t> framed;
  framed.push_back(0xF0);
  framed.insert(framed.end(), msg, msg + n);
  framed.push_back(0xF7);
  Bus200eBridgeHandleSysEx(framed.data(), (uint32_t) framed.size());
}

// Feed one DUMP_DATA packet, packing the chunk the way the browser's
// packChunk()/Bus200eSysExPack does. seq/total ride the wire as 14-bit
// septet pairs (protocol v2), exactly as sysex-transport.js's packDump emits.
static void feed_dump_data(uint16_t seq, uint16_t total,
                           const uint8_t *chunk, uint32_t chunk_len) {
  const uint8_t hdr[BUS200E_SYSEX_DUMP_HDR_BYTES] = {
    BUS200E_SYSEX_LO7(seq),   BUS200E_SYSEX_HI7(seq),
    BUS200E_SYSEX_LO7(total), BUS200E_SYSEX_HI7(total),
  };
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, hdr, sizeof(hdr),
                                         chunk, chunk_len, msg, sizeof(msg));
  assert(n > 0);
  Bus200eBridgeHandleSysEx(msg, (uint32_t) n);
}

// A DUMP_END payload: [n_lo, n_hi, xor7].
static std::vector<uint8_t> dump_end_payload(uint16_t n_packets, uint8_t xor7) {
  return { BUS200E_SYSEX_LO7(n_packets), BUS200E_SYSEX_HI7(n_packets), xor7 };
}

static void feed_dump_end(uint16_t n_packets, uint8_t xor7) {
  const std::vector<uint8_t> p = dump_end_payload(n_packets, xor7);
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_END, p.data(),
                                         (uint32_t) p.size(), nullptr, 0, msg, sizeof(msg));
  assert(n > 0);
  Bus200eBridgeHandleSysEx(msg, (uint32_t) n);
}

static const SentMsg *last_sent() { return sent.empty() ? nullptr : &sent.back(); }

static int nak_code(const SentMsg *m) {
  return (m && m->cmd == BUS200E_SYSEX_CMD_NAK && m->raw.size() >= 2) ? m->raw[1] : -1;
}

// Reassemble whatever DUMP_DATA/DUMP_END frames the bridge emitted, the way
// sysex-transport.js's DumpAssembler does, and verify the checksum.
static bool reassemble(std::vector<uint8_t> &out, size_t from) {
  out.clear();
  int n_packets = -1, xor7 = -1;
  for (size_t i = from; i < sent.size(); ++i) {
    if (sent[i].cmd == BUS200E_SYSEX_CMD_DUMP_DATA) {
      const uint32_t off = (uint32_t) sent[i].seq * BUS200E_SYSEX_CHUNK_BYTES;
      if (out.size() != off) return false;   // packets must arrive in order
      out.insert(out.end(), sent[i].raw.begin(), sent[i].raw.end());
    } else if (sent[i].cmd == BUS200E_SYSEX_CMD_DUMP_END) {
      if (sent[i].raw.size() < 3) return false;
      n_packets = BUS200E_SYSEX_FROM14(sent[i].raw[0], sent[i].raw[1]);
      xor7 = sent[i].raw[2];
    }
  }
  if (n_packets < 0) return false;
  const int expect_packets =
      (int) ((out.size() + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
  if (n_packets != expect_packets) return false;
  return Bus200eSysExXor7(0, out.data(), (uint32_t) out.size()) == (xor7 & 0x7F);
}

// pump Task() until the session settles (or a step budget runs out).
// A whole 251e bank is 1503 frames at BUS200E_BRIDGE_SEND_BUDGET=4 per call,
// so callers streaming one must raise the budget past 376.
static void pump(int steps = 64) {
  while (steps-- > 0 && Bus200eBridgeGetState() != BUS200E_BRIDGE_IDLE)
    Bus200eBridgeTask();
  Bus200eBridgeTask();
}

// ---- GET_DUMP ------------------------------------------------------------------

static void test_get_dump_round_trip() {
  printf("test_get_dump_round_trip\n");
  reset_all();

  // A captured dump is raw FRAM: full 8-bit spread, which is the whole reason
  // this protocol packs where hOC's does not.
  const uint32_t kLen = 300;
  for (uint32_t i = 0; i < kLen; ++i) fake_card[i] = (uint8_t) (i * 37 + 11);

  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  CHECK(fake_backup_calls == 1);
  CHECK(fake_backup_addr == 0x2E);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_CAPTURING);
  CHECK(last_sent() && last_sent()->cmd == BUS200E_SYSEX_CMD_ACK);
  CHECK(last_sent()->raw.size() == 2 && last_sent()->raw[0] == BUS200E_SYSEX_CMD_GET_DUMP);
  CHECK(last_sent()->raw[1] == 0x2E);

  // still running: nothing streams yet
  const size_t after_ack = sent.size();
  fake_state = BUS200E_MASTER_TRANSFERRING;
  Bus200eBridgeTask();
  CHECK(sent.size() == after_ack);

  fake_state = BUS200E_MASTER_DONE;
  fake_bytes = kLen;
  pump();
  CHECK(fake_reset_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(Bus200eBridgeDumpBytes() == kLen);

  std::vector<uint8_t> got;
  CHECK(reassemble(got, after_ack));
  CHECK(got.size() == kLen);
  CHECK(memcmp(got.data(), fake_card, kLen) == 0);

  // every frame we sent is a legal, parseable message of ours
  for (size_t i = after_ack; i < sent.size(); ++i) CHECK(sent[i].parse_rc == 0);
}

// S5: while a Cal Routine run is open (or a cancelled 259e run's COMMIT verdict is
// pending) the bridge neither reads nor writes any module: no BACKUP, no card
// serving, no RESTORE of a dump that was already staged.
static void test_cal_run_refuses_get_dump() {
  printf("test_cal_run_refuses_get_dump\n");
  reset_all();
  fake_cal_run = 1;
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x28 });
  CHECK(fake_backup_calls == 0);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CAL_RUN);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  fake_cal_run = 0;                     // once the run is over it works again
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x28 });
  CHECK(fake_backup_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_CAPTURING);
}

static void test_cal_run_refuses_put_dump() {
  printf("test_cal_run_refuses_put_dump\n");
  reset_all();
  fake_cal_run = 1;
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x28 });
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CAL_RUN);
  CHECK(fake_serve_enable_calls == 0 && fake_restore_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  fake_cal_run = 0;
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x28 });
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RECEIVING);
}

static void test_get_dump_needs_an_address() {
  printf("test_get_dump_needs_an_address\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_GET_DUMP, {});
  CHECK(fake_backup_calls == 0);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_UNKNOWN_CMD);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_get_dump_busy() {
  printf("test_get_dump_busy\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_CAPTURING);
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2F });
  CHECK(fake_backup_calls == 1);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_BUSY);
}

static void test_get_dump_master_refuses() {
  printf("test_get_dump_master_refuses\n");
  reset_all();
  fake_backup_result = -BUS200E_MASTER_ERR_NO_FREE_CARD;
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_NO_FREE_CARD);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

// L1: the core refuses a BACKUP while the firmware card is served: the browser gets the
// firmware-card NAK (10), not the generic "master busy" (5). (Bus Buccaneer shows
// only "error 10" for it, not a distinct reason.)
static void test_get_dump_refused_while_firmware_card_served() {
  printf("test_get_dump_refused_while_firmware_card_served\n");
  reset_all();
  fake_backup_result = -BUS200E_MASTER_ERR_FW_CARD;
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x28 });
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_FW_BUSY);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_get_dump_master_fails_mid_job() {
  printf("test_get_dump_master_fails_mid_job\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  fake_state = BUS200E_MASTER_FAILED;
  fake_error = BUS200E_MASTER_ERR_NO_RESPONSE;
  Bus200eBridgeTask();
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_NO_RESPONSE);
  CHECK(fake_reset_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_get_dump_empty_capture() {
  printf("test_get_dump_empty_capture\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  fake_state = BUS200E_MASTER_DONE;
  fake_bytes = 0;
  Bus200eBridgeTask();
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_NO_RESPONSE);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_get_dump_bigger_than_this_build_can_describe() {
  printf("test_get_dump_bigger_than_this_build_can_describe\n");
  // Under protocol v1 this was the 127-packet/5588-byte wire ceiling and any
  // real 251e bank tripped it. Under v2 the ceiling is the card image
  // itself, so reaching it needs a fake card LARGER than the 64K a card
  // pointer can name -- i.e. it is now unreachable on real hardware. The
  // refusal is still tested, because refusing beats handing the browser a
  // truncated dump it would write straight back to the module.
  reset_all(kFakeCardOversize);
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  fake_state = BUS200E_MASTER_DONE;
  fake_bytes = BUS200E_BRIDGE_MAX_DUMP_BYTES + 1;
  CHECK(fake_bytes < fake_card_size);   // so the clamp isn't what refuses it
  Bus200eBridgeTask();
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CHECKSUM);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

// ---- the transfer this whole protocol version exists for --------------------

#define REAL_251E_BANK_BYTES 63120u   // 30 records x 2104, per a live MasterBackup(0x5C)
#define REAL_251E_BANK_PACKETS 1503u  // ceil(63120 / 42)
#define REAL_251E_BANK_XOR7 0x10u     // over (i*37+11)&0xFF, i < 63120

// The same synthetic bank the SysEx suite and the JS suite both use.
static uint8_t bank_byte(uint32_t i) { return (uint8_t) ((i * 37u + 11u) & 0xFFu); }

static void test_get_dump_streams_a_whole_63120_byte_251e_bank() {
  printf("test_get_dump_streams_a_whole_63120_byte_251e_bank\n");
  // Protocol v1 NAK 6'd this outright -- 63120 bytes is 11.3x its 5588-byte
  // ceiling -- which meant the bridge could not carry the one dump it was
  // built for. This is the test that says it now can.
  reset_all(kFakeCardFull);
  for (uint32_t i = 0; i < REAL_251E_BANK_BYTES; ++i) fake_card[i] = bank_byte(i);

  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x5C });   // the 251e's real bus address
  CHECK(fake_backup_addr == 0x5C);
  const size_t after_ack = sent.size();

  fake_state = BUS200E_MASTER_DONE;
  fake_bytes = REAL_251E_BANK_BYTES;
  pump(1024);   // 1503 frames / SEND_BUDGET 4 = 376 Task() calls
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(Bus200eBridgeDumpBytes() == REAL_251E_BANK_BYTES);

  // exactly 1503 DUMP_DATA frames + one DUMP_END, every one parseable
  size_t data_frames = 0, end_frames = 0;
  for (size_t i = after_ack; i < sent.size(); ++i) {
    CHECK(sent[i].parse_rc == 0);
    if (sent[i].cmd == BUS200E_SYSEX_CMD_DUMP_DATA) {
      CHECK(sent[i].seq == data_frames);           // in order, 14-bit intact
      CHECK(sent[i].total == REAL_251E_BANK_PACKETS);
      data_frames++;
    } else if (sent[i].cmd == BUS200E_SYSEX_CMD_DUMP_END) {
      CHECK(sent[i].raw.size() == 3);
      CHECK(BUS200E_SYSEX_FROM14(sent[i].raw[0], sent[i].raw[1]) == REAL_251E_BANK_PACKETS);
      CHECK(sent[i].raw[2] == REAL_251E_BANK_XOR7);
      end_frames++;
    }
  }
  CHECK(data_frames == REAL_251E_BANK_PACKETS);
  CHECK(end_frames == 1);
  // and the seq that would have overflowed a v1 byte really did go out
  CHECK(sent[after_ack + 200].seq == 200);
  CHECK(sent[after_ack + 1234].seq == 1234);

  std::vector<uint8_t> got;
  CHECK(reassemble(got, after_ack));
  CHECK(got.size() == REAL_251E_BANK_BYTES);
  CHECK(memcmp(got.data(), fake_card, REAL_251E_BANK_BYTES) == 0);
}

static void test_put_dump_accepts_a_whole_63120_byte_251e_bank() {
  printf("test_put_dump_accepts_a_whole_63120_byte_251e_bank\n");
  reset_all(kFakeCardFull);
  std::vector<uint8_t> data(REAL_251E_BANK_BYTES);
  for (uint32_t i = 0; i < REAL_251E_BANK_BYTES; ++i) data[i] = bank_byte(i);

  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x5C });
  const uint16_t total = REAL_251E_BANK_PACKETS;
  for (uint16_t seq = 0; seq < total; ++seq) {
    const uint32_t off = (uint32_t) seq * BUS200E_SYSEX_CHUNK_BYTES;
    uint32_t chunk = REAL_251E_BANK_BYTES - off;
    if (chunk > BUS200E_SYSEX_CHUNK_BYTES) chunk = BUS200E_SYSEX_CHUNK_BYTES;
    feed_dump_data(seq, total, data.data() + off, chunk);
  }
  // every packet got its own ACK{DUMP_DATA, seq_lo, seq_hi} -- the 3-byte
  // septet-pair ACK form, since a seq no longer fits one context byte
  uint32_t data_acks = 0;
  for (const auto &m : sent) {
    if (m.cmd == BUS200E_SYSEX_CMD_ACK && m.raw.size() == 3 &&
        m.raw[0] == BUS200E_SYSEX_CMD_DUMP_DATA) {
      CHECK(BUS200E_SYSEX_FROM14(m.raw[1], m.raw[2]) == data_acks);
      data_acks++;
    }
  }
  CHECK(data_acks == REAL_251E_BANK_PACKETS);

  feed_dump_end(total, Bus200eSysExXor7(0, data.data(), REAL_251E_BANK_BYTES));
  CHECK(Bus200eSysExXor7(0, data.data(), REAL_251E_BANK_BYTES) == REAL_251E_BANK_XOR7);

  CHECK(memcmp(fake_card, data.data(), REAL_251E_BANK_BYTES) == 0);
  CHECK(fake_restore_calls == 1);
  CHECK(fake_restore_addr == 0x5C);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RESTORING);
  CHECK(Bus200eBridgeDumpBytes() == REAL_251E_BANK_BYTES);
  CHECK(last_sent() && last_sent()->cmd == BUS200E_SYSEX_CMD_ACK);
  CHECK(last_sent()->raw.size() == 3);
  CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_DUMP_END);
  CHECK(BUS200E_SYSEX_FROM14(last_sent()->raw[1], last_sent()->raw[2]) == total);
}

static void test_put_dump_refuses_a_seq_past_the_card_image() {
  printf("test_put_dump_refuses_a_seq_past_the_card_image\n");
  // 14 bits can name 16383 packets; this build tracks 1561. A seq past that
  // must be refused, not written into the seen-bitmap.
  reset_all(kFakeCardFull);
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x5C });
  const uint8_t chunk[4] = { 1, 2, 3, 4 };
  feed_dump_data((uint16_t) BUS200E_BRIDGE_MAX_PACKETS, 16383, chunk, sizeof(chunk));
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CHECKSUM);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fake_restore_calls == 0);
}

// ---- PUT_DUMP ------------------------------------------------------------------

// Push `len` bytes through a full PUT_DUMP transfer. Returns the payload the
// caller can compare the card image against.
static std::vector<uint8_t> put_dump_payload(uint32_t len) {
  std::vector<uint8_t> v(len);
  for (uint32_t i = 0; i < len; ++i) v[i] = (uint8_t) (i * 91 + 3);
  return v;
}

static void stream_put_dump(const std::vector<uint8_t> &data, uint8_t mod_addr,
                            bool corrupt_checksum = false, int skip_seq = -1) {
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { mod_addr });
  const uint16_t total =
      (uint16_t) ((data.size() + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
  for (uint16_t seq = 0; seq < total; ++seq) {
    if (seq == skip_seq) continue;
    const uint32_t off = (uint32_t) seq * BUS200E_SYSEX_CHUNK_BYTES;
    uint32_t chunk = (uint32_t) data.size() - off;
    if (chunk > BUS200E_SYSEX_CHUNK_BYTES) chunk = BUS200E_SYSEX_CHUNK_BYTES;
    feed_dump_data(seq, total, data.data() + off, chunk);
  }
  uint8_t xor7 = Bus200eSysExXor7(0, data.data(), (uint32_t) data.size());
  if (corrupt_checksum) xor7 = (uint8_t) ((xor7 ^ 0x01) & 0x7F);
  feed_dump_end(total, xor7);
}

// A run that opens between PUT_DUMP and DUMP_END: the staged dump is dropped,
// never restored.
static void test_cal_run_stops_a_staged_restore() {
  printf("test_cal_run_stops_a_staged_restore\n");
  reset_all();
  const std::vector<uint8_t> data = put_dump_payload(300);
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x28 });
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RECEIVING);
  const uint16_t total =
      (uint16_t) ((data.size() + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
  for (uint16_t seq = 0; seq < total; ++seq) {
    const uint32_t off = (uint32_t) seq * BUS200E_SYSEX_CHUNK_BYTES;
    uint32_t chunk = (uint32_t) data.size() - off;
    if (chunk > BUS200E_SYSEX_CHUNK_BYTES) chunk = BUS200E_SYSEX_CHUNK_BYTES;
    feed_dump_data(seq, total, data.data() + off, chunk);
  }
  fake_cal_run = 1;                     // the run opens now
  feed_dump_end(total, Bus200eSysExXor7(0, data.data(), (uint32_t) data.size()));
  CHECK(fake_restore_calls == 0);
  CHECK(fake_dirty_calls == 0);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CAL_RUN);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

// M1: a run that opens while a PUT_DUMP session is receiving: the next DUMP_DATA is
// NAKed 17 and must NOT land in the card image (the run's backup/served image), and
// the session is dropped.
static void test_cal_run_stops_dump_data_into_the_image() {
  printf("test_cal_run_stops_dump_data_into_the_image\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x28 });
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RECEIVING);
  uint8_t before[64];
  memcpy(before, fake_card, sizeof before);
  uint8_t chunk[32];
  for (unsigned i = 0; i < sizeof chunk; ++i) chunk[i] = (uint8_t) (0xA5 ^ i);
  fake_cal_run = 1;                     // the run opens mid-transfer
  feed_dump_data(0, 4, chunk, sizeof chunk);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CAL_RUN);
  CHECK(memcmp(before, fake_card, sizeof before) == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  fake_cal_run = 0;
  feed_dump_data(1, 4, chunk, sizeof chunk);   // the dropped session stays dropped
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(memcmp(before, fake_card, sizeof before) == 0);
}

static void test_put_dump_round_trip() {
  printf("test_put_dump_round_trip\n");
  reset_all();
  const std::vector<uint8_t> data = put_dump_payload(300);

  stream_put_dump(data, 0x2E);

  // card serving had to be turned on before the first byte landed
  CHECK(fake_serve_enable_calls == 1);
  CHECK(fake_serving == 1);
  // the bytes are actually in the card image -- the gap this whole module fills
  CHECK(memcmp(fake_card, data.data(), data.size()) == 0);
  CHECK(fake_dirty_calls == 1);
  CHECK(fake_restore_calls == 1);
  CHECK(fake_restore_addr == 0x2E);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RESTORING);
  CHECK(Bus200eBridgeDumpBytes() == data.size());
  CHECK(last_sent() && last_sent()->cmd == BUS200E_SYSEX_CMD_ACK);
  CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_DUMP_END);

  // every DUMP_DATA got its own ACK{DUMP_DATA, seq_lo, seq_hi}
  int data_acks = 0;
  for (const auto &m : sent)
    if (m.cmd == BUS200E_SYSEX_CMD_ACK && m.raw.size() == 3 &&
        m.raw[0] == BUS200E_SYSEX_CMD_DUMP_DATA)
      CHECK(BUS200E_SYSEX_FROM14(m.raw[1], m.raw[2]) == data_acks++);
  CHECK(data_acks == 8);  // ceil(300/42)

  fake_state = BUS200E_MASTER_DONE;
  pump();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fake_reset_calls == 1);
  CHECK(last_sent()->cmd == BUS200E_SYSEX_CMD_ACK);
  CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_PUT_DUMP);
}

static void test_put_dump_bad_checksum_never_restores() {
  printf("test_put_dump_bad_checksum_never_restores\n");
  reset_all();
  stream_put_dump(put_dump_payload(100), 0x2E, /*corrupt_checksum=*/true);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CHECKSUM);
  CHECK(fake_restore_calls == 0);
  CHECK(fake_dirty_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_put_dump_missing_packet_never_restores() {
  printf("test_put_dump_missing_packet_never_restores\n");
  reset_all();
  stream_put_dump(put_dump_payload(300), 0x2E, false, /*skip_seq=*/3);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CHECKSUM);
  CHECK(fake_restore_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_put_dump_duplicate_packet_is_harmless() {
  printf("test_put_dump_duplicate_packet_is_harmless\n");
  reset_all();
  const std::vector<uint8_t> data = put_dump_payload(120);
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x2E });
  const uint16_t total = 3;  // ceil(120/42)
  for (int pass = 0; pass < 2; ++pass)   // send packet 1 twice
    for (uint16_t seq = 0; seq < total; ++seq) {
      if (pass && seq != 1) continue;
      const uint32_t off = (uint32_t) seq * BUS200E_SYSEX_CHUNK_BYTES;
      uint32_t chunk = (uint32_t) data.size() - off;
      if (chunk > BUS200E_SYSEX_CHUNK_BYTES) chunk = BUS200E_SYSEX_CHUNK_BYTES;
      feed_dump_data(seq, total, data.data() + off, chunk);
    }
  feed_dump_end(total, Bus200eSysExXor7(0, data.data(), (uint32_t) data.size()));
  // the checksum is recomputed from the image, so a retransmit can't skew it
  CHECK(fake_restore_calls == 1);
  CHECK(memcmp(fake_card, data.data(), data.size()) == 0);
}

static void test_put_dump_no_card() {
  printf("test_put_dump_no_card\n");
  reset_all();
  fake_serve_enable_result = -2;   // WPM owns 0x50
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x2E });
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_NO_FREE_CARD);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fake_restore_calls == 0);
}

static void test_put_dump_restore_fails() {
  printf("test_put_dump_restore_fails\n");
  reset_all();
  stream_put_dump(put_dump_payload(88), 0x2E);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RESTORING);
  fake_state = BUS200E_MASTER_FAILED;
  fake_error = BUS200E_MASTER_ERR_SEND_TIMEOUT;
  Bus200eBridgeTask();
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_SEND_TIMEOUT);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_put_dump_rx_timeout_frees_the_session() {
  printf("test_put_dump_rx_timeout_frees_the_session\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x2E });
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RECEIVING);
  fake_now += BUS200E_BRIDGE_RX_TIMEOUT_MS - 1;
  Bus200eBridgeTask();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RECEIVING);
  fake_now += 2;
  Bus200eBridgeTask();
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_SEND_TIMEOUT);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  // and a GET_DUMP is accepted again afterwards
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  CHECK(fake_backup_calls == 1);
}

static void test_dump_data_outside_a_session() {
  printf("test_dump_data_outside_a_session\n");
  reset_all();
  const uint8_t chunk[4] = { 1, 2, 3, 4 };
  feed_dump_data(0, 1, chunk, sizeof(chunk));
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_UNKNOWN_CMD);
  feed_dump_end(1, 0);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_UNKNOWN_CMD);
}

static void test_dump_end_needs_the_full_three_byte_payload() {
  printf("test_dump_end_needs_the_full_three_byte_payload\n");
  // A v1 host's two-byte [n_packets, xor7] must be refused, not read as
  // [n_lo, n_hi] with the checksum missing.
  reset_all();
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x2E });
  const uint8_t chunk[4] = { 1, 2, 3, 4 };
  feed_dump_data(0, 1, chunk, sizeof(chunk));
  feed(BUS200E_SYSEX_CMD_DUMP_END, { 1, Bus200eSysExXor7(0, chunk, sizeof(chunk)) });
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CHECKSUM);
  CHECK(fake_restore_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_dump_data_out_of_range_seq() {
  printf("test_dump_data_out_of_range_seq\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x2E });
  const uint8_t chunk[4] = { 1, 2, 3, 4 };
  feed_dump_data(5, 2, chunk, sizeof(chunk));   // seq >= total
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CHECKSUM);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

// ---- misc protocol -------------------------------------------------------------

static void test_info_and_status() {
  printf("test_info_and_status\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_INFO, {}, /*wrap_f0f7=*/true);
  const SentMsg *m = last_sent();
  CHECK(m && m->cmd == BUS200E_SYSEX_CMD_INFO_R);
  CHECK(m->raw.size() >= 10);
  CHECK(m->raw[0] == 2);                            // schema 2 = protocol v2
  CHECK(m->raw[1] == 0 && m->raw[2] == 0);          // format not decoded by firmware
  // [3..5] last dump length, three septets low-first (14 bits could not
  // report a 63120-byte bank)
  CHECK(m->raw[6] == BUS200E_SYSEX_CHUNK_BYTES);
  CHECK(BUS200E_SYSEX_FROM14(m->raw[7], m->raw[8]) == BUS200E_BRIDGE_MAX_PACKETS);
  CHECK(BUS200E_BRIDGE_MAX_PACKETS == 1561);        // ceil(65536 / 42)

  // ...and after a real bank has moved, the length field reports all of it
  reset_all(kFakeCardFull);
  std::vector<uint8_t> bank(REAL_251E_BANK_BYTES);
  for (uint32_t i = 0; i < REAL_251E_BANK_BYTES; ++i) bank[i] = bank_byte(i);
  stream_put_dump(bank, 0x5C);
  feed(BUS200E_SYSEX_CMD_INFO, {});
  m = last_sent();
  CHECK(m && m->cmd == BUS200E_SYSEX_CMD_INFO_R);
  const uint32_t reported = (uint32_t) m->raw[3] | ((uint32_t) m->raw[4] << 7) |
                            ((uint32_t) m->raw[5] << 14);
  CHECK(reported == REAL_251E_BANK_BYTES);

  reset_all();

  fake_state = BUS200E_MASTER_TRANSFERRING;
  fake_error = BUS200E_MASTER_ERR_NONE;
  feed(BUS200E_SYSEX_CMD_STATUS, {});
  m = last_sent();
  CHECK(m && m->cmd == BUS200E_SYSEX_CMD_STATUS_R);
  CHECK(m->raw.size() >= 5);
  CHECK(m->raw[0] == BUS200E_MASTER_TRANSFERRING);
  CHECK(m->raw[3] == 0);                            // not a restore
  CHECK(m->raw[4] == BUS200E_BRIDGE_IDLE);
}

static void test_unknown_command_and_bad_version() {
  printf("test_unknown_command_and_bad_version\n");
  reset_all();
  feed(0x33, { 1, 2 });
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_UNKNOWN_CMD);

  // hand-built frame with the wrong protocol version -- 0x01 is the one that
  // matters now: an applet still speaking v1's 7-bit packet counters gets
  // NAK 1 rather than a silently misparsed transfer.
  uint8_t bad[] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                    BUS200E_SYSEX_APP_ID, 0x01, BUS200E_SYSEX_CMD_INFO };
  const size_t before = sent.size();
  Bus200eBridgeHandleSysEx(bad, sizeof(bad));
  CHECK(sent.size() == before + 1);
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_VERSION);
}

static void test_other_apps_sysex_is_ignored() {
  printf("test_other_apps_sysex_is_ignored\n");
  reset_all();
  // Captain MIDI's own frame: same mfr/family, app byte 'M'
  const uint8_t captain[] = { 0xF0, 0x7D, 0x62, 'M', 0x01, 0x01, 0xF7 };
  Bus200eBridgeHandleSysEx(captain, sizeof(captain));
  CHECK(sent.empty());
  // and a Universal Device Inquiry
  const uint8_t inquiry[] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };
  Bus200eBridgeHandleSysEx(inquiry, sizeof(inquiry));
  CHECK(sent.empty());
}

static void test_uninitialised_is_inert() {
  printf("test_uninitialised_is_inert\n");
  reset_all();
  Bus200eBridgeInit(nullptr);
  const uint8_t msg[] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                          BUS200E_SYSEX_APP_ID, BUS200E_SYSEX_PROTO_VER,
                          BUS200E_SYSEX_CMD_INFO };
  Bus200eBridgeHandleSysEx(msg, sizeof(msg));
  Bus200eBridgeTask();
  CHECK(sent.empty());
  Bus200eBridgeInit(&fake_ops);
}

// ---- firmware-card load session ------------------------------------------------

// FW_BEGIN for `kind`, `len`, `crc`, through the codec (what the app sends).
static void feed_fw_begin(uint8_t kind, uint32_t len, uint32_t crc) {
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildFwBegin(kind, len, crc, msg, sizeof(msg));
  assert(n > 0);
  Bus200eBridgeHandleSysEx(msg, (uint32_t) n);
}

static void feed_fw_data(uint16_t seq, uint16_t total, const uint8_t *chunk, uint32_t n) {
  const uint8_t hdr[BUS200E_SYSEX_DUMP_HDR_BYTES] = {
    BUS200E_SYSEX_LO7(seq), BUS200E_SYSEX_HI7(seq),
    BUS200E_SYSEX_LO7(total), BUS200E_SYSEX_HI7(total),
  };
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE];
  const int r = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_FW_DATA, hdr, sizeof(hdr),
                                         chunk, n, msg, sizeof(msg));
  assert(r > 0);
  Bus200eBridgeHandleSysEx(msg, (uint32_t) r);
}

static void feed_fw_end(uint16_t n, uint8_t xor7) {
  feed(BUS200E_SYSEX_CMD_FW_END, { BUS200E_SYSEX_LO7(n), BUS200E_SYSEX_HI7(n), xor7 });
}

static bool is_ack(const SentMsg *m, std::initializer_list<uint8_t> want) {
  return m && m->cmd == BUS200E_SYSEX_CMD_ACK &&
         m->raw == std::vector<uint8_t>(want);
}

// n raw bytes of a deterministic pattern with high bits set.
static std::vector<uint8_t> fw_image(uint32_t n) {
  std::vector<uint8_t> v(n);
  for (uint32_t i = 0; i < n; ++i) v[i] = (uint8_t) (i * 37u + 0x91u);
  static const char kMagic[] = "#$200EFU";   // v2 header, what begin_ok's kind 2 expects
  for (uint32_t i = 0; i < 8 && i < n; ++i) v[i] = (uint8_t) kMagic[i];
  return v;
}

// Begin a kind-2 load of `len` bytes; returns with the device FW_RECEIVING.
static void begin_ok(uint32_t len = 126, uint32_t crc = 0x12345678) {
  feed_fw_begin(2, len, crc);
  assert(is_ack(last_sent(), { BUS200E_SYSEX_CMD_FW_BEGIN }));
  assert(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  sent.clear();
}

static void test_fw_begin_acks_and_enters_receiving() {
  printf("test_fw_begin_acks_and_enters_receiving\n");
  reset_all();
  feed_fw_begin(2, 54186, 0x12345678);
  CHECK(is_ack(last_sent(), { 0x20 }));
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  CHECK(fw_begin_calls == 1 && fw_begin_kind == 2);
  CHECK(fw_abort_calls == 0);
}

static void test_fw_begin_naks_busy_when_serving() {
  printf("test_fw_begin_naks_busy_when_serving\n");
  reset_all();
  fake_serving = 1;
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 10);
  CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_FW_BEGIN);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_begin_calls == 0);
  // firmware-card serving counts too
  reset_all();
  fw_serving_v = 1;
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 10);
  CHECK(fw_begin_calls == 0);
  // and so does a running master job / another session
  reset_all();
  fake_state = BUS200E_MASTER_SENDING;
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 10);
  CHECK(fw_begin_calls == 0);
  reset_all();
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x5C });
  sent.clear();
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 10);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RECEIVING);
}

static void test_fw_begin_naks_no_free_card_when_wpm() {
  printf("test_fw_begin_naks_no_free_card_when_wpm\n");
  reset_all();
  fw_wpm_v = 1;
  feed_fw_begin(1, 1000, 1);
  CHECK(nak_code(last_sent()) == 7);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_begin_calls == 0);
}

static void test_fw_begin_naks_nomem() {
  printf("test_fw_begin_naks_nomem\n");
  reset_all();
  fw_begin_result = -6;
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 11);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  // any other refusal is FW_BUSY
  reset_all();
  fw_begin_result = -1;
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 10);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_fw_begin_rejects_oversize_len() {
  printf("test_fw_begin_rejects_oversize_len\n");
  reset_all();
  feed_fw_begin(1, 65537, 1);
  CHECK(nak_code(last_sent()) == 12);
  feed_fw_begin(2, 0x38001, 1);
  CHECK(nak_code(last_sent()) == 12);
  feed_fw_begin(2, 0x5F, 1);
  CHECK(nak_code(last_sent()) == 12);
  feed_fw_begin(2, 0, 1);
  CHECK(nak_code(last_sent()) == 12);
  CHECK(fw_begin_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  // the edges are accepted
  feed_fw_begin(1, 65536, 1);
  CHECK(is_ack(last_sent(), { 0x20 }));
  Bus200eBridgeAbort();
  feed_fw_begin(2, 0x38000, 1);
  CHECK(is_ack(last_sent(), { 0x20 }));
  Bus200eBridgeAbort();
  feed_fw_begin(2, 0x60, 1);
  CHECK(is_ack(last_sent(), { 0x20 }));
  CHECK(fw_begin_calls == 3);
}

static void test_fw_begin_rejects_bad_kind() {
  printf("test_fw_begin_rejects_bad_kind\n");
  reset_all();
  feed_fw_begin(0, 1000, 1);
  CHECK(nak_code(last_sent()) == 13);
  feed_fw_begin(3, 1000, 1);
  CHECK(nak_code(last_sent()) == 13);
  CHECK(fw_begin_calls == 0);
}

static void test_fw_begin_again_while_receiving_aborts() {
  printf("test_fw_begin_again_while_receiving_aborts\n");
  reset_all();
  begin_ok();
  feed_fw_begin(2, 126, 1);
  CHECK(nak_code(last_sent()) == 10);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_abort_calls == 1);
  CHECK(fw_begin_calls == 1);
}

static void test_fw_data_streams_bytes_in_order() {
  printf("test_fw_data_streams_bytes_in_order\n");
  reset_all();
  begin_ok(126);
  const std::vector<uint8_t> img = fw_image(126);
  for (uint16_t i = 0; i < 3; ++i) {
    feed_fw_data(i, 3, img.data() + i * 42, 42);
    CHECK(is_ack(last_sent(), { 0x21, BUS200E_SYSEX_LO7(i), BUS200E_SYSEX_HI7(i) }));
  }
  CHECK(fw_pushed == img);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  CHECK(fw_abort_calls == 0);
}

static void test_fw_data_short_last_packet() {
  printf("test_fw_data_short_last_packet\n");
  reset_all();
  begin_ok(96);   // 3 packets: 42 + 42 + 12
  const std::vector<uint8_t> img = fw_image(126);
  feed_fw_data(0, 3, img.data(), 42);
  feed_fw_data(1, 3, img.data() + 42, 42);
  feed_fw_data(2, 3, img.data() + 84, 12);
  CHECK(is_ack(last_sent(), { 0x21, 2, 0 }));
  CHECK(fw_pushed == std::vector<uint8_t>(img.begin(), img.begin() + 96));
  // a short NON-last packet is a sequence error
  reset_all();
  begin_ok(126);
  feed_fw_data(0, 3, img.data(), 41);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_abort_calls == 1);
}

static void test_fw_data_out_of_sequence_naks() {
  printf("test_fw_data_out_of_sequence_naks\n");
  const std::vector<uint8_t> img = fw_image(126);
  reset_all();
  begin_ok(126);
  feed_fw_data(0, 3, img.data(), 42);
  sent.clear();
  feed_fw_data(2, 3, img.data() + 84, 42);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_FW_DATA);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_abort_calls == 1);
  CHECK(fw_pushed.size() == 42);   // nothing from the bad packet
  // a changed total
  reset_all();
  begin_ok(126);
  feed_fw_data(0, 4, img.data(), 42);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(fw_abort_calls == 1 && fw_pushed.empty());
  // a repeated packet
  reset_all();
  begin_ok(126);
  feed_fw_data(0, 3, img.data(), 42);
  feed_fw_data(0, 3, img.data(), 42);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(fw_abort_calls == 1);
  // a packet past len (total 3 -> len 126; 4th packet)
  reset_all();
  begin_ok(126);
  for (uint16_t i = 0; i < 3; ++i) feed_fw_data(i, 3, img.data() + i * 42, 42);
  feed_fw_data(3, 3, img.data(), 42);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(fw_pushed.size() == 126);
  CHECK(fw_abort_calls == 1);
  // a last packet that would run past len
  reset_all();
  begin_ok(96);
  feed_fw_data(0, 3, img.data(), 42);
  feed_fw_data(1, 3, img.data() + 42, 42);
  feed_fw_data(2, 3, img.data() + 84, 13);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(fw_pushed.size() == 84);
  // FW_DATA with no FW_BEGIN
  reset_all();
  feed_fw_data(0, 3, img.data(), 42);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(fw_pushed.empty());
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_fw_data_malformed_chunk_naks() {
  printf("test_fw_data_malformed_chunk_naks\n");
  reset_all();
  begin_ok(126);
  // seq 0, total 3, then a lone hi-bits byte with no data after it
  const uint8_t bad[] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                          BUS200E_SYSEX_APP_ID, BUS200E_SYSEX_PROTO_VER,
                          BUS200E_SYSEX_CMD_FW_DATA, 0, 0, 3, 0, 0x01 };
  Bus200eBridgeHandleSysEx(bad, sizeof(bad));
  CHECK(nak_code(last_sent()) == 16);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_abort_calls == 1);
}

static void test_fw_end_commits_and_arms() {
  printf("test_fw_end_commits_and_arms\n");
  reset_all();
  begin_ok(126, 0x12345678);
  const std::vector<uint8_t> img = fw_image(126);
  for (uint16_t i = 0; i < 3; ++i) feed_fw_data(i, 3, img.data() + i * 42, 42);
  sent.clear();
  const uint8_t x = Bus200eSysExXor7(0, img.data(), 126);
  feed_fw_end(3, x);
  CHECK(is_ack(last_sent(), { 0x22, 3, 0 }));
  CHECK(fw_commit_calls == 1 && fw_commit_len == 126 && fw_commit_crc == 0x12345678u);
  CHECK(fw_armed_calls == 0);
  CHECK(fw_abort_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_fw_end_short_leaves_unarmed() {
  printf("test_fw_end_short_leaves_unarmed\n");
  const std::vector<uint8_t> img = fw_image(126);
  reset_all();
  begin_ok(126);
  feed_fw_data(0, 3, img.data(), 42);
  feed_fw_data(1, 3, img.data() + 42, 42);
  feed_fw_end(2, Bus200eSysExXor7(0, img.data(), 84));   // n != total
  CHECK(nak_code(last_sent()) == 6);
  CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_FW_END);
  CHECK(fw_commit_calls == 0);
  CHECK(fw_abort_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  // right n as claimed, but fewer received
  reset_all();
  begin_ok(126);
  feed_fw_data(0, 3, img.data(), 42);
  feed_fw_end(3, Bus200eSysExXor7(0, img.data(), 42));
  CHECK(nak_code(last_sent()) == 6);
  CHECK(fw_commit_calls == 0 && fw_abort_calls == 1);
  // bad xor7
  reset_all();
  begin_ok(126);
  for (uint16_t i = 0; i < 3; ++i) feed_fw_data(i, 3, img.data() + i * 42, 42);
  feed_fw_end(3, (uint8_t) (Bus200eSysExXor7(0, img.data(), 126) ^ 1));
  CHECK(nak_code(last_sent()) == 6);
  CHECK(fw_commit_calls == 0 && fw_abort_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  // FW_END with no load
  reset_all();
  feed_fw_end(3, 0);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(fw_commit_calls == 0);
}

static void test_fw_end_maps_commit_codes() {
  printf("test_fw_end_maps_commit_codes\n");
  const std::vector<uint8_t> img = fw_image(126);
  const struct { int rc; int nak; } map[] = {
    { -1, 11 }, { -2, 12 }, { -3, 13 }, { -4, 14 }, { -5, 15 }, { -6, 11 },
  };
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
    reset_all();
    fw_commit_result = map[i].rc;
    begin_ok(126);
    for (uint16_t k = 0; k < 3; ++k) feed_fw_data(k, 3, img.data() + k * 42, 42);
    feed_fw_end(3, Bus200eSysExXor7(0, img.data(), 126));
    CHECK(nak_code(last_sent()) == map[i].nak);
    CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_FW_END);
    CHECK(fw_commit_calls == 1);
    CHECK(fw_abort_calls == 1);
    CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  }
}

static void test_fw_end_kind_magic_mismatch_naks_before_commit() {
  printf("test_fw_end_kind_magic_mismatch_naks_before_commit\n");
  // kind, first-8-bytes magic
  const struct { uint8_t kind; const char *magic; int ok; } cases[] = {
    { 1, "#$200EFU", 0 },   // v2 header on a kind-1 load: the over-read case
    { 2, "#!200EFU", 0 },
    { 2, "GARBAGE!", 0 },
    { 1, "#!200EFU", 1 },
    { 2, "#$200EFU", 1 },
  };
  for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    reset_all();
    std::vector<uint8_t> img = fw_image(126);
    for (int i = 0; i < 8; ++i) img[i] = (uint8_t) cases[c].magic[i];
    feed_fw_begin(cases[c].kind, 126, 0x12345678);
    CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
    sent.clear();
    for (uint16_t k = 0; k < 3; ++k) feed_fw_data(k, 3, img.data() + k * 42, 42);
    feed_fw_end(3, Bus200eSysExXor7(0, img.data(), 126));
    if (cases[c].ok) {
      CHECK(is_ack(last_sent(), { 0x22, 3, 0 }));
      CHECK(fw_commit_calls == 1);
    } else {
      CHECK(nak_code(last_sent()) == 13);
      CHECK(last_sent()->raw[0] == BUS200E_SYSEX_CMD_FW_END);
      CHECK(fw_commit_calls == 0);
      CHECK(fw_abort_calls == 1);
      CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
    }
  }
}

static void test_fw_arm_off_calls_fw_arm_0() {
  printf("test_fw_arm_off_calls_fw_arm_0\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_FW_ARM, { 0 });
  CHECK(is_ack(last_sent(), { 0x23, 0 }));
  CHECK(fw_arm_calls == 1 && fw_arm_last == 0);
  feed(BUS200E_SYSEX_CMD_FW_ARM, { 1 });
  CHECK(is_ack(last_sent(), { 0x23, 1 }));
  CHECK(fw_arm_calls == 2 && fw_arm_last == 1);
  feed(BUS200E_SYSEX_CMD_FW_ARM, { 2 });
  CHECK(nak_code(last_sent()) == 2);
  feed(BUS200E_SYSEX_CMD_FW_ARM, {});
  CHECK(nak_code(last_sent()) == 2);
  CHECK(fw_arm_calls == 2);
  // allowed mid-load, and does not disturb it
  begin_ok();
  feed(BUS200E_SYSEX_CMD_FW_ARM, { 1 });
  CHECK(is_ack(last_sent(), { 0x23, 1 }));
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  CHECK(fw_abort_calls == 0);
}

static void test_fw_status_r_reports_ops() {
  printf("test_fw_status_r_reports_ops\n");
  reset_all();
  fw_armed_v = 1; fw_serving_v = 1; fw_kind_v = 2;
  fw_served_v = 54186; fw_age_v = 7; fw_len_v = 54186;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  const SentMsg *m = last_sent();
  CHECK(m && m->cmd == BUS200E_SYSEX_CMD_FW_STATUS_R);
  Bus200eFwStatus s = {};
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.armed == 1 && s.serving == 1 && s.kind == 2);
  CHECK(s.served == 54186 && s.first_read_age_s == 7 && s.image_len == 54186);
  CHECK(s.ver_addr == 0 && s.ver_valid == 0);
  for (int i = 0; i < 7; ++i) CHECK(s.ver[i] == 0x20);
  // never read -> 0x3FFF; large age saturates at 0x3FFE; nothing on flash
  reset_all();
  fw_age_v = 0xFFFFFFFFu;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.first_read_age_s == 0x3FFF && s.kind == 0 && s.image_len == 0 && s.served == 0);
  fw_age_v = 100000;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.first_read_age_s == 0x3FFE);
  // served / len are 21-bit
  fw_served_v = 0xFFFFFFFFu; fw_len_v = 0x200000u;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.served == 0x1FFFFFu && s.image_len == 0x1FFFFFu);
}

static void test_fw_receiving_stall_aborts() {
  printf("test_fw_receiving_stall_aborts\n");
  const std::vector<uint8_t> img = fw_image(126);
  reset_all();
  begin_ok(126);
  fake_now += 10000;
  Bus200eBridgeTask();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  fake_now += 1;
  Bus200eBridgeTask();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_abort_calls == 1);
  CHECK(sent.empty());
  feed_fw_data(0, 3, img.data(), 42);
  CHECK(nak_code(last_sent()) == 16);
  CHECK(fw_abort_calls == 1);   // already idle: no second abort
  // FW_STATUS polls do not keep a load alive; FW_DATA does
  reset_all();
  begin_ok(126);
  fake_now += 6000;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  fake_now += 6000;
  Bus200eBridgeTask();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_abort_calls == 1);
  reset_all();
  begin_ok(126);
  fake_now += 6000;
  feed_fw_data(0, 3, img.data(), 42);
  fake_now += 6000;
  Bus200eBridgeTask();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  CHECK(fw_abort_calls == 0);
}

static void test_fw_receiving_blocks_dump_commands() {
  printf("test_fw_receiving_blocks_dump_commands\n");
  reset_all();
  begin_ok();
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x5C });
  CHECK(nak_code(last_sent()) == 10);
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x5C });
  CHECK(nak_code(last_sent()) == 10);
  CHECK(fake_backup_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
}

static void test_fw_commands_nak_unknown_without_ops() {
  printf("test_fw_commands_nak_unknown_without_ops\n");
  reset_all();
  Bus200eBridgeOps bare = fake_ops;
  bare.fw_begin = nullptr;
  Bus200eBridgeInit(&bare);
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 2);
  feed_fw_data(0, 3, fw_image(42).data(), 42);
  CHECK(nak_code(last_sent()) == 2);
  feed_fw_end(3, 0);
  CHECK(nak_code(last_sent()) == 2);
  feed(BUS200E_SYSEX_CMD_FW_ARM, { 1 });
  CHECK(nak_code(last_sent()) == 2);
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  CHECK(nak_code(last_sent()) == 2);
  CHECK(fw_begin_calls == 0 && fw_arm_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  Bus200eBridgeInit(&fake_ops);
}

// ---- SCAN / POLL_DONE / BUS_STATUS / INFO_R level ----------------------------------

static std::vector<uint8_t> bytes(std::initializer_list<uint8_t> l) { return std::vector<uint8_t>(l); }

// Task() until the bridge is IDLE, 200 ms of fake time per tick (> the gap).
static void scan_run(int max_ticks = 600) {
  while (max_ticks-- > 0 && Bus200eBridgeGetState() != BUS200E_BRIDGE_IDLE) {
    Bus200eBridgeTask();
    fake_now += 200;
  }
}

static const std::vector<uint8_t> kSpaces7 = { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };

static void test_scan_acks_then_walks_addresses() {
  printf("test_scan_acks_then_walks_addresses\n");
  reset_all();
  fake_q_done[0x28] = true; fake_q_done[0x2A] = true;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  CHECK(sent.size() == 1);
  CHECK(is_ack(last_sent(), { 0x25, 0x28, 0x2B }));
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_SCANNING);
  CHECK(q_starts.empty());                       // nothing mastered before Task()
  scan_run();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(q_starts.size() == 4);
  if (q_starts.size() == 4)
    CHECK(q_starts[0].addr == 0x28 && q_starts[1].addr == 0x29 &&
          q_starts[2].addr == 0x2A && q_starts[3].addr == 0x2B);
  CHECK(fake_q_reset_calls == 4);
  CHECK(sent.size() == 4);                       // ACK, SCAN_R, SCAN_R, SCAN_END
  if (sent.size() == 4) {
    std::vector<uint8_t> r0 = bytes({ 0x00, 0x7F, 0x28, 0x00 });
    r0.insert(r0.end(), kSpaces7.begin(), kSpaces7.end());
    std::vector<uint8_t> r1 = r0;
    r1[0] = 0x01; r1[2] = 0x2A;
    CHECK(sent[1].cmd == BUS200E_SYSEX_CMD_SCAN_R && sent[1].raw == r0);
    CHECK(sent[2].cmd == BUS200E_SYSEX_CMD_SCAN_R && sent[2].raw == r1);
    CHECK(sent[3].cmd == BUS200E_SYSEX_CMD_SCAN_END && sent[3].raw == bytes({ 0x02 }));
  }
  // an empty sweep (first > last) is ACK then SCAN_END [0]
  reset_all();
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x30, 0x2F });
  scan_run();
  CHECK(sent.size() == 2 && is_ack(&sent[0], { 0x25, 0x30, 0x2F }));
  CHECK(sent.size() == 2 && sent[1].cmd == BUS200E_SYSEX_CMD_SCAN_END &&
        sent[1].raw == bytes({ 0x00 }));
  CHECK(q_starts.empty());
  // a failed query makes no SCAN_R; FAILED still advances
  reset_all();
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x10, 0x11 });
  scan_run();
  CHECK(sent.size() == 2 && sent[1].cmd == BUS200E_SYSEX_CMD_SCAN_END &&
        sent[1].raw == bytes({ 0x00 }));
  CHECK(fake_q_reset_calls == 2);
  // malformed SCAN requests
  reset_all();
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x10 });
  CHECK(nak_code(last_sent()) == 2);
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x10, 0x11, 0x12 });
  CHECK(nak_code(last_sent()) == 2);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_scan_r_carries_cached_version() {
  printf("test_scan_r_carries_cached_version\n");
  reset_all();
  fake_q_done[0x28] = true; fake_q_done[0x29] = true;
  fake_ver_known[0x28] = true;                   // 0x29 has no cached version
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x29 });
  scan_run();
  CHECK(sent.size() == 4);
  if (sent.size() == 4) {
    CHECK(sent[1].raw == bytes({ 0x00, 0x7F, 0x28, 0x01, '3', '1', '.', '2', 'C', '2', ' ' }));
    std::vector<uint8_t> r1 = bytes({ 0x01, 0x7F, 0x29, 0x00 });
    r1.insert(r1.end(), kSpaces7.begin(), kSpaces7.end());
    CHECK(sent[2].raw == r1);
    CHECK(sent[3].raw == bytes({ 0x02 }));
  }
}

static void test_scan_naks_busy_during_master_job() {
  printf("test_scan_naks_busy_during_master_job\n");
  reset_all();
  fake_state = BUS200E_MASTER_TRANSFERRING;        // a master job is running
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  CHECK(nak_code(last_sent()) == 5);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  // a GET_DUMP / PUT_DUMP session
  reset_all();
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x5C });
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  CHECK(nak_code(last_sent()) == 5);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_RECEIVING);
  // a firmware load in flight is FW_BUSY (docs: "SCAN ... answer NAK 10"), and is untouched
  reset_all();
  begin_ok();
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  CHECK(nak_code(last_sent()) == 10);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  CHECK(fw_abort_calls == 0);
  // a second SCAN during a scan, and every master command during a scan
  reset_all();
  fake_q_hold = 1;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  Bus200eBridgeTask();                              // query in flight
  sent.clear();
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x30, 0x31 });
  CHECK(nak_code(last_sent()) == 5);
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x5C });
  CHECK(nak_code(last_sent()) == 5);
  feed(BUS200E_SYSEX_CMD_PUT_DUMP, { 0x5C });
  CHECK(nak_code(last_sent()) == 5);
  feed_fw_begin(2, 1000, 1);
  CHECK(nak_code(last_sent()) == 5);
  feed(BUS200E_SYSEX_CMD_POLL_DONE, {});
  CHECK(nak_code(last_sent()) == 5);
  CHECK(fake_backup_calls == 0 && fw_begin_calls == 0 && fake_poll_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_SCANNING);
  // INFO / STATUS / BUS_STATUS / FW_STATUS still answer mid-scan
  sent.clear();
  feed(BUS200E_SYSEX_CMD_BUS_STATUS, {});
  CHECK(last_sent() && last_sent()->cmd == BUS200E_SYSEX_CMD_BUS_STATUS_R);
  // no query support -> NAK 2
  reset_all();
  Bus200eBridgeOps bare = fake_ops;
  bare.query_start = nullptr;
  Bus200eBridgeInit(&bare);
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  CHECK(nak_code(last_sent()) == 2);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  Bus200eBridgeInit(&fake_ops);
}

static void test_scan_gap_150ms() {
  printf("test_scan_gap_150ms\n");
  reset_all();
  fake_q_done[0x28] = true; fake_q_done[0x29] = true;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x29 });
  Bus200eBridgeTask();                              // first query starts at once
  CHECK(q_starts.size() == 1 && q_starts[0].addr == 0x28 && q_starts[0].at == 1000);
  Bus200eBridgeTask();                              // resolved at now = 1000
  CHECK(q_starts.size() == 1);
  fake_now = 1149;
  Bus200eBridgeTask();
  CHECK(q_starts.size() == 1);                      // 149 ms: still waiting
  fake_now = 1150;
  Bus200eBridgeTask();
  CHECK(q_starts.size() == 2 && q_starts[1].addr == 0x29 && q_starts[1].at == 1150);
  // the gap runs from the query's COMPLETION, not its start
  reset_all();
  fake_q_hold = 1;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x29 });
  Bus200eBridgeTask();
  CHECK(q_starts.size() == 1);
  fake_now = 5000;                                  // reply still pending
  Bus200eBridgeTask();
  CHECK(q_starts.size() == 1);
  fake_q_state = BUS200E_QUERY_DONE;                // completes at 5000
  Bus200eBridgeTask();
  fake_now = 5149;
  Bus200eBridgeTask();
  CHECK(q_starts.size() == 1);
  fake_q_hold = 0;
  fake_now = 5150;
  Bus200eBridgeTask();
  CHECK(q_starts.size() == 2 && q_starts[1].at == 5150);
  // a busy query_start is retried on later ticks, not skipped
  reset_all();
  fake_q_start_rc = -(int) BUS200E_MASTER_ERR_BUSY;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x28 });
  Bus200eBridgeTask();
  fake_now += 10;
  Bus200eBridgeTask();
  CHECK(q_starts.empty() && Bus200eBridgeGetState() == BUS200E_BRIDGE_SCANNING);
  fake_q_start_rc = 0;
  fake_q_done[0x28] = true;
  scan_run();
  CHECK(q_starts.size() == 1 && q_starts[0].addr == 0x28);
  CHECK(sent.size() == 3 && sent[2].cmd == BUS200E_SYSEX_CMD_SCAN_END &&
        sent[2].raw == bytes({ 0x01 }));
  // any other refusal skips the address (no SCAN_R) and the sweep still ends
  reset_all();
  fake_q_start_rc = -(int) BUS200E_MASTER_ERR_BAD_ARGS;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x00, 0x01 });
  scan_run();
  CHECK(sent.size() == 2 && sent[1].cmd == BUS200E_SYSEX_CMD_SCAN_END &&
        sent[1].raw == bytes({ 0x00 }));
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_scan_busy_forever_skips_address() {
  printf("test_scan_busy_forever_skips_address\n");
  reset_all();
  fake_q_start_rc = -(int) BUS200E_MASTER_ERR_BUSY;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x29 });
  const uint32_t t0 = fake_now;
  Bus200eBridgeTask();                              // first refusal at t0
  fake_now = t0 + 2999;
  Bus200eBridgeTask();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_SCANNING);
  fake_now = t0 + 3000;
  Bus200eBridgeTask();                              // 0x28 skipped
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_SCANNING);
  scan_run();
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fake_now - t0 <= 7000);
  CHECK(q_starts.empty());
  CHECK(sent.size() == 2 && sent[1].cmd == BUS200E_SYSEX_CMD_SCAN_END &&
        sent[1].raw == bytes({ 0x00 }));
}

static void test_scan_sets_and_restores_reply_timeout() {
  printf("test_scan_sets_and_restores_reply_timeout\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x29 });
  CHECK(fake_timeouts.size() == 1 && fake_timeouts[0] == 300);
  scan_run();
  CHECK(fake_timeouts.size() == 2 && fake_timeouts[1] == 1000);   // at SCAN_END
  // abort mid-scan
  reset_all();
  fake_q_hold = 1;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  Bus200eBridgeTask();
  Bus200eBridgeAbort();
  CHECK(fake_timeouts.size() == 2 && fake_timeouts[0] == 300 && fake_timeouts[1] == 1000);
  // Init mid-scan
  reset_all();
  fake_q_hold = 1;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  Bus200eBridgeInit(&fake_ops);
  CHECK(fake_timeouts.size() == 2 && fake_timeouts[1] == 1000);
}

static void test_scan_abort_releases_query() {
  printf("test_scan_abort_releases_query\n");
  reset_all();
  fake_q_hold = 1;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  Bus200eBridgeTask();
  CHECK(fake_q_reset_calls == 0);
  Bus200eBridgeAbort();
  CHECK(fake_q_reset_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  Bus200eBridgeTask();
  CHECK(q_starts.size() == 1);                      // nothing more is mastered
  // Init() while scanning does the same, against the previous ops
  reset_all();
  fake_q_hold = 1;
  feed(BUS200E_SYSEX_CMD_SCAN, { 0x28, 0x2B });
  Bus200eBridgeInit(&fake_ops);
  CHECK(fake_q_reset_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  // an abort of a finished/idle bridge touches nothing
  Bus200eBridgeAbort();
  CHECK(fake_q_reset_calls == 1 && fw_abort_calls == 0);
}

static void test_abort_and_init_release_fw_load() {
  printf("test_abort_and_init_release_fw_load\n");
  reset_all();
  begin_ok();
  Bus200eBridgeAbort();
  CHECK(fw_abort_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  Bus200eBridgeAbort();
  CHECK(fw_abort_calls == 1);                       // idle: no second abort
  reset_all();
  begin_ok();
  Bus200eBridgeInit(&fake_ops);
  CHECK(fw_abort_calls == 1);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
}

static void test_poll_done_sends_frame_and_acks() {
  printf("test_poll_done_sends_frame_and_acks\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_POLL_DONE, {});
  CHECK(fake_poll_calls == 1);
  CHECK(sent.size() == 1 && is_ack(last_sent(), { 0x26 }));
  // the 2 s send timeout is NAK 8, with no ACK
  reset_all();
  fake_poll_rc = -1;
  feed(BUS200E_SYSEX_CMD_POLL_DONE, {});
  CHECK(fake_poll_calls == 1);
  CHECK(sent.size() == 1 && nak_code(last_sent()) == 8);
  // during a firmware load: NAK 10, nothing sent, load untouched
  reset_all();
  begin_ok();
  feed(BUS200E_SYSEX_CMD_POLL_DONE, {});
  CHECK(nak_code(last_sent()) == 10);
  CHECK(fake_poll_calls == 0 && fw_abort_calls == 0);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING);
  // during a master job: NAK 5
  reset_all();
  fake_state = BUS200E_MASTER_SENDING;
  feed(BUS200E_SYSEX_CMD_POLL_DONE, {});
  CHECK(nak_code(last_sent()) == 5 && fake_poll_calls == 0);
  // no op -> NAK 2
  reset_all();
  Bus200eBridgeOps bare = fake_ops;
  bare.send_poll_done = nullptr;
  Bus200eBridgeInit(&bare);
  feed(BUS200E_SYSEX_CMD_POLL_DONE, {});
  CHECK(nak_code(last_sent()) == 2);
  Bus200eBridgeInit(&fake_ops);
}

static void test_bus_status_r_fields() {
  printf("test_bus_status_r_fields\n");
  reset_all();
  fw_wpm_v = 1; fake_quiet_v = 0; fake_dialect_v = 1;
  feed(BUS200E_SYSEX_CMD_BUS_STATUS, {});
  CHECK(sent.size() == 1);
  CHECK(last_sent() && last_sent()->cmd == BUS200E_SYSEX_CMD_BUS_STATUS_R &&
        last_sent()->raw == bytes({ 1, 0, 1, 0 }));
  // short/V2 framing, quiet bus, a last master error of 5 (no response)
  reset_all();
  fake_quiet_v = 1; fake_dialect_v = 2; fake_error = BUS200E_MASTER_ERR_NO_RESPONSE;
  feed(BUS200E_SYSEX_CMD_BUS_STATUS, {});
  CHECK(last_sent() && last_sent()->raw == bytes({ 0, 1, 2, 5 }));
  // no bus_quiet / bus_dialect ops: quiet 1, dialect 0
  reset_all();
  Bus200eBridgeOps bare = fake_ops;
  bare.bus_quiet = nullptr; bare.bus_dialect = nullptr;
  Bus200eBridgeInit(&bare);
  feed(BUS200E_SYSEX_CMD_BUS_STATUS, {});
  CHECK(last_sent() && last_sent()->cmd == BUS200E_SYSEX_CMD_BUS_STATUS_R &&
        last_sent()->raw == bytes({ 0, 1, 0, 0 }));
  Bus200eBridgeInit(&fake_ops);
  // answers during a firmware load without touching it
  reset_all();
  begin_ok();
  feed(BUS200E_SYSEX_CMD_BUS_STATUS, {});
  CHECK(last_sent() && last_sent()->cmd == BUS200E_SYSEX_CMD_BUS_STATUS_R);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_FW_RECEIVING && fw_abort_calls == 0);
}

static void test_info_r_appends_bridge_level() {
  printf("test_info_r_appends_bridge_level\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_INFO, {});
  const SentMsg *m = last_sent();
  CHECK(m && m->cmd == BUS200E_SYSEX_CMD_INFO_R);
  // schema, n_seq, max_steps, dump len x3, chunk, max packets lo/hi, serving, LEVEL
  CHECK(m && m->raw == bytes({ 2, 0, 0, 0, 0, 0, 42, 25, 12, 0, 1 }));
  CHECK(m && m->raw.size() == 11 && m->raw[10] == BUS200E_SYSEX_BRIDGE_LEVEL);
}

static void test_fw_status_r_reports_last_version() {
  printf("test_fw_status_r_reports_last_version\n");
  reset_all();
  fake_ver_addr = 0x28; fake_ver_known[0x28] = true;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  Bus200eFwStatus s = {};
  const SentMsg *m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.ver_addr == 0x28 && s.ver_valid == 1 && memcmp(s.ver, "31.2C2 ", 7) == 0);
  // an address with no cached frame: spaces, ver_valid 0 (the addr is still reported)
  fake_ver_known[0x28] = false;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.ver_addr == 0x28 && s.ver_valid == 0);
  for (int i = 0; i < 7; ++i) CHECK(s.ver[i] == 0x20);
  // bytes past 7 bits are masked
  fake_ver_known[0x28] = true;
  fake_ver_bytes[0] = 0xB3;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.ver_valid == 1 && s.ver[0] == 0x33);
  // the version-frame generation is carried (1234 = 0x4D2 -> 52 09), 0 when unset
  CHECK(s.ver_gen == 0);
  fake_ver_gen = 1234;
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.ver_gen == 1234 && m->raw.size() == 22 && m->raw[20] == 0x52 && m->raw[21] == 0x09);
  Bus200eBridgeOps nogen = fake_ops;
  nogen.fw_version_gen = nullptr;
  Bus200eBridgeInit(&nogen);
  feed(BUS200E_SYSEX_CMD_FW_STATUS, {});
  m = last_sent();
  CHECK(m && Bus200eSysExParseFwStatusR(m->raw.data(), (uint32_t) m->raw.size(), &s) == 0);
  CHECK(s.ver_gen == 0);
  Bus200eBridgeInit(&fake_ops);
}

static void test_fw_end_commit_failure_after_short_image() {
  printf("test_fw_end_commit_failure_after_short_image\n");
  reset_all();
  begin_ok(126);                                    // announced 126, packets 42/42/41
  const std::vector<uint8_t> img = fw_image(126);
  feed_fw_data(0, 3, img.data(), 42);
  feed_fw_data(1, 3, img.data() + 42, 42);
  feed_fw_data(2, 3, img.data() + 84, 41);
  CHECK(fw_pushed.size() == 125);
  fw_commit_result = -2;
  feed_fw_end(3, Bus200eSysExXor7(0, img.data(), 125));
  CHECK(fw_commit_calls == 1);
  CHECK(nak_code(last_sent()) == 12);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
  CHECK(fw_abort_calls == 1);
}

static void test_garbled_fw_data_without_fw_ops_naks_unknown() {
  printf("test_garbled_fw_data_without_fw_ops_naks_unknown\n");
  reset_all();
  Bus200eBridgeOps bare = fake_ops;
  bare.fw_begin = nullptr;
  Bus200eBridgeInit(&bare);
  const uint8_t bad[] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                          BUS200E_SYSEX_APP_ID, BUS200E_SYSEX_PROTO_VER,
                          BUS200E_SYSEX_CMD_FW_DATA, 0, 0, 3, 0, 0x01 };
  Bus200eBridgeHandleSysEx(bad, sizeof(bad));
  CHECK(nak_code(last_sent()) == 2);
  CHECK(fw_abort_calls == 0);
  Bus200eBridgeInit(&fake_ops);
}

int main() {
  test_cal_run_refuses_get_dump();
  test_cal_run_refuses_put_dump();
  test_cal_run_stops_a_staged_restore();
  test_cal_run_stops_dump_data_into_the_image();
  test_get_dump_refused_while_firmware_card_served();
  test_get_dump_round_trip();
  test_get_dump_needs_an_address();
  test_get_dump_busy();
  test_get_dump_master_refuses();
  test_get_dump_master_fails_mid_job();
  test_get_dump_empty_capture();
  test_get_dump_bigger_than_this_build_can_describe();
  test_get_dump_streams_a_whole_63120_byte_251e_bank();

  test_put_dump_round_trip();
  test_put_dump_accepts_a_whole_63120_byte_251e_bank();
  test_put_dump_refuses_a_seq_past_the_card_image();
  test_put_dump_bad_checksum_never_restores();
  test_put_dump_missing_packet_never_restores();
  test_put_dump_duplicate_packet_is_harmless();
  test_put_dump_no_card();
  test_put_dump_restore_fails();
  test_put_dump_rx_timeout_frees_the_session();
  test_dump_data_outside_a_session();
  test_dump_data_out_of_range_seq();
  test_dump_end_needs_the_full_three_byte_payload();

  test_fw_begin_acks_and_enters_receiving();
  test_fw_begin_naks_busy_when_serving();
  test_fw_begin_naks_no_free_card_when_wpm();
  test_fw_begin_naks_nomem();
  test_fw_begin_rejects_oversize_len();
  test_fw_begin_rejects_bad_kind();
  test_fw_begin_again_while_receiving_aborts();
  test_fw_data_streams_bytes_in_order();
  test_fw_data_short_last_packet();
  test_fw_data_out_of_sequence_naks();
  test_fw_data_malformed_chunk_naks();
  test_fw_end_commits_and_arms();
  test_fw_end_short_leaves_unarmed();
  test_fw_end_maps_commit_codes();
  test_fw_end_kind_magic_mismatch_naks_before_commit();
  test_fw_arm_off_calls_fw_arm_0();
  test_fw_status_r_reports_ops();
  test_fw_receiving_stall_aborts();
  test_fw_receiving_blocks_dump_commands();
  test_fw_commands_nak_unknown_without_ops();
  test_fw_end_commit_failure_after_short_image();
  test_garbled_fw_data_without_fw_ops_naks_unknown();
  test_abort_and_init_release_fw_load();

  test_scan_acks_then_walks_addresses();
  test_scan_r_carries_cached_version();
  test_scan_naks_busy_during_master_job();
  test_scan_gap_150ms();
  test_scan_busy_forever_skips_address();
  test_scan_abort_releases_query();
  test_scan_sets_and_restores_reply_timeout();
  test_poll_done_sends_frame_and_acks();
  test_bus_status_r_fields();
  test_info_r_appends_bridge_level();
  test_fw_status_r_reports_last_version();

  test_info_and_status();
  test_unknown_command_and_bad_version();
  test_other_apps_sysex_is_ignored();
  test_uninitialised_is_inert();

  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
