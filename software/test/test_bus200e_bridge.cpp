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

// Power of two like BUSCARD_SIZE, and deliberately bigger than
// BUS200E_BRIDGE_MAX_DUMP_BYTES so the "capture too big for a one-byte packet
// count" path is reachable without a 32K buffer.
static const uint32_t kFakeCardSize = 8192;
static uint8_t fake_card[kFakeCardSize];

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
  uint8_t seq, total;
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
static uint32_t f_card_size() { return kFakeCardSize; }
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

static void reset_all() {
  memset(fake_card, 0xFF, sizeof(fake_card));
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
// packChunk()/Bus200eSysExPack does.
static void feed_dump_data(uint8_t seq, uint8_t total,
                           const uint8_t *chunk, uint32_t chunk_len) {
  const uint8_t hdr[2] = { seq, total };
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, hdr, 2,
                                         chunk, chunk_len, msg, sizeof(msg));
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
      if (sent[i].raw.size() < 2) return false;
      n_packets = sent[i].raw[0];
      xor7 = sent[i].raw[1];
    }
  }
  if (n_packets < 0) return false;
  const int expect_packets =
      (int) ((out.size() + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
  if (n_packets != expect_packets) return false;
  return Bus200eSysExXor7(0, out.data(), (uint32_t) out.size()) == (xor7 & 0x7F);
}

// pump Task() until the session settles (or a step budget runs out)
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

static void test_get_dump_too_big_for_one_byte_packet_count() {
  printf("test_get_dump_too_big_for_one_byte_packet_count\n");
  reset_all();
  feed(BUS200E_SYSEX_CMD_GET_DUMP, { 0x2E });
  fake_state = BUS200E_MASTER_DONE;
  fake_bytes = BUS200E_BRIDGE_MAX_DUMP_BYTES + 1;  // < card size, > 127 packets
  CHECK(fake_bytes < kFakeCardSize);   // so the clamp isn't what refuses it
  Bus200eBridgeTask();
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_CHECKSUM);
  CHECK(Bus200eBridgeGetState() == BUS200E_BRIDGE_IDLE);
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
  const uint8_t total =
      (uint8_t) ((data.size() + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
  for (uint8_t seq = 0; seq < total; ++seq) {
    if (seq == skip_seq) continue;
    const uint32_t off = (uint32_t) seq * BUS200E_SYSEX_CHUNK_BYTES;
    uint32_t chunk = (uint32_t) data.size() - off;
    if (chunk > BUS200E_SYSEX_CHUNK_BYTES) chunk = BUS200E_SYSEX_CHUNK_BYTES;
    feed_dump_data(seq, total, data.data() + off, chunk);
  }
  uint8_t xor7 = Bus200eSysExXor7(0, data.data(), (uint32_t) data.size());
  if (corrupt_checksum) xor7 = (uint8_t) ((xor7 ^ 0x01) & 0x7F);
  feed(BUS200E_SYSEX_CMD_DUMP_END, { total, xor7 });
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

  // every DUMP_DATA got its own ACK{DUMP_DATA, seq}
  int data_acks = 0;
  for (const auto &m : sent)
    if (m.cmd == BUS200E_SYSEX_CMD_ACK && m.raw.size() >= 2 &&
        m.raw[0] == BUS200E_SYSEX_CMD_DUMP_DATA)
      CHECK(m.raw[1] == data_acks++);
  CHECK(data_acks == 7);  // ceil(300/44)

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
  const uint8_t total = 3;  // ceil(120/44)
  for (int pass = 0; pass < 2; ++pass)   // send packet 1 twice
    for (uint8_t seq = 0; seq < total; ++seq) {
      if (pass && seq != 1) continue;
      const uint32_t off = (uint32_t) seq * BUS200E_SYSEX_CHUNK_BYTES;
      uint32_t chunk = (uint32_t) data.size() - off;
      if (chunk > BUS200E_SYSEX_CHUNK_BYTES) chunk = BUS200E_SYSEX_CHUNK_BYTES;
      feed_dump_data(seq, total, data.data() + off, chunk);
    }
  feed(BUS200E_SYSEX_CMD_DUMP_END,
       { total, Bus200eSysExXor7(0, data.data(), (uint32_t) data.size()) });
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
  feed(BUS200E_SYSEX_CMD_DUMP_END, { 1, 0 });
  CHECK(nak_code(last_sent()) == BUS200E_SYSEX_NAK_UNKNOWN_CMD);
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
  CHECK(m->raw.size() >= 8);
  CHECK(m->raw[0] == 1);                            // schema
  CHECK(m->raw[1] == 0 && m->raw[2] == 0);          // format not decoded by firmware
  CHECK(m->raw[5] == BUS200E_SYSEX_CHUNK_BYTES);
  CHECK(m->raw[6] == BUS200E_BRIDGE_MAX_PACKETS);

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

  // hand-built frame with the wrong protocol version
  uint8_t bad[] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                    BUS200E_SYSEX_APP_ID, 0x02, BUS200E_SYSEX_CMD_INFO };
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
  test_get_dump_too_big_for_one_byte_packet_count();

  test_put_dump_round_trip();
  test_put_dump_bad_checksum_never_restores();
  test_put_dump_missing_packet_never_restores();
  test_put_dump_duplicate_packet_is_harmless();
  test_put_dump_no_card();
  test_put_dump_restore_fails();
  test_put_dump_rx_timeout_frees_the_session();
  test_dump_data_outside_a_session();
  test_dump_data_out_of_range_seq();

  test_info_and_status();
  test_unknown_command_and_bad_version();
  test_other_apps_sysex_is_ignored();
  test_uninitialised_is_inert();

  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
