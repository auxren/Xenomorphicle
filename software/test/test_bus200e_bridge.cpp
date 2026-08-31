// Host tests for the browser <-> 200e preset-dump session layer
// (src/Bus200eBridge.cpp): the piece that turns received SysEx into card-image
// bytes + MasterRestore(), and a completed MasterBackup() into an outbound
// DUMP_DATA/DUMP_END stream.
//
// No USB, no MIDI, no I2C: a fake Bus200eBridgeOps table stands in for
// usbMIDI and OC::PresetBus, exactly the way test_bus200e_master.cpp fakes
// the I2C transport. Same standalone (no gtest) shape as its neighbours:
//   g++ -std=c++17 -Wall -Werror -O2 -o build/test_bus200e_bridge \
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

static const Bus200eBridgeOps fake_ops = {
  f_now,
  f_card_serving, f_card_serve_enable, f_card_image, f_card_size, f_card_dirty,
  f_master_backup, f_master_restore, f_master_state, f_master_error,
  f_master_bytes, f_master_reset,
  f_send,
};

static void reset_all(uint32_t card_size = kFakeCardSmall) {
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

int main() {
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

  test_info_and_status();
  test_unknown_command_and_bad_version();
  test_other_apps_sysex_is_ignored();
  test_uninitialised_is_inert();

  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
