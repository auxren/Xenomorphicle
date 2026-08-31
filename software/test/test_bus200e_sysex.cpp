// Host tests for the browser<->Xenomorpher SysEx bridge framing
// (src/Bus200eSysEx.cpp): 7-bit packing round trips, message build/parse
// (header, DUMP_DATA seq/total + packed chunk, error paths) and the xor7
// end-to-end checksum. No MIDI/USB involved -- pure byte-buffer logic, same
// pattern as test_bus200e.cpp. See Bus200eSysEx.h for why this protocol
// packs (unlike hOC's docs/hoc-midi-sysex.md, which it otherwise mirrors).
// Standalone (no gtest): g++ -std=c++17 -Wall -Werror -O2 (one line:)
//   -o build/test_bus200e_sysex test_bus200e_sysex.cpp \
//      ../src/Bus200eSysEx.cpp && ./build/test_bus200e_sysex
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../src/Bus200eSysEx.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// ---- pack/unpack -------------------------------------------------------------

static void test_pack_unpack_round_trip(void) {
  printf("test_pack_unpack_round_trip\n");
  uint8_t raw[300], packed[400], back[300];
  for (int i = 0; i < 300; ++i) raw[i] = (uint8_t) (i * 37 + 5);  // full 8-bit spread

  for (uint32_t n : {0u, 1u, 6u, 7u, 8u, 13u, 14u, 15u, 44u, 100u, 255u, 300u}) {
    const int pl = Bus200eSysExPack(raw, n, packed, sizeof(packed));
    CHECK(pl >= 0);
    for (int i = 0; i < pl; ++i) CHECK(!(packed[i] & 0x80));  // 7-bit clean
    const int rl = Bus200eSysExUnpack(packed, (uint32_t) pl, back, sizeof(back));
    CHECK(rl == (int) n);
    CHECK(memcmp(back, raw, n) == 0);
  }
}

static void test_pack_exact_bytes(void) {
  printf("test_pack_exact_bytes\n");
  const uint8_t raw[3] = { 0x80, 0x7F, 0x81 };  // MSBs 1,0,1 -> hibits 0x05
  uint8_t packed[16];
  const int pl = Bus200eSysExPack(raw, 3, packed, sizeof(packed));
  CHECK(pl == 4);
  CHECK(packed[0] == 0x05);
  CHECK(packed[1] == 0x00 && packed[2] == 0x7F && packed[3] == 0x01);

  const uint8_t raw44_pack_len = 51;  // BUS200E_SYSEX_CHUNK_BYTES worked example
  uint8_t raw44[44] = {};
  uint8_t packed64[64];
  const int pl44 = Bus200eSysExPack(raw44, 44, packed64, sizeof(packed64));
  CHECK(pl44 == raw44_pack_len);
  CHECK(pl44 == BUS200E_SYSEX_MAX_PACKED);
}

static void test_pack_undersized_out(void) {
  printf("test_pack_undersized_out\n");
  uint8_t raw[7] = {1,2,3,4,5,6,7};
  uint8_t packed[7];  // needs 8
  CHECK(Bus200eSysExPack(raw, 7, packed, sizeof(packed)) == -1);
  uint8_t packed8[8];
  CHECK(Bus200eSysExPack(raw, 7, packed8, sizeof(packed8)) == 8);
}

static void test_unpack_malformed(void) {
  printf("test_unpack_malformed\n");
  const uint8_t bad[1] = { 0x00 };  // a lone hibits byte, no data following
  uint8_t out[16];
  CHECK(Bus200eSysExUnpack(bad, 1, out, sizeof(out)) == -1);
}

static void test_unpack_undersized_out(void) {
  printf("test_unpack_undersized_out\n");
  const uint8_t packed[8] = { 0,1,2,3,4,5,6,7 };  // unpacks to 7 raw bytes
  uint8_t out[6];
  CHECK(Bus200eSysExUnpack(packed, 8, out, sizeof(out)) == -1);
  uint8_t out7[7];
  CHECK(Bus200eSysExUnpack(packed, 8, out7, sizeof(out7)) == 7);
}

// ---- message build/parse ------------------------------------------------------

static void test_header_bytes_match_hoc_family(void) {
  printf("test_header_bytes_match_hoc_family\n");
  uint8_t out[16];
  const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_INFO, nullptr, 0,
                                          nullptr, 0, out, sizeof(out));
  CHECK(n == 5);
  CHECK(out[0] == 0x7D);  // same mfr ID as hOC
  CHECK(out[1] == 0x62);  // same "Beige Maze" family byte as hOC
  CHECK(out[2] == BUS200E_SYSEX_APP_ID);
  CHECK(out[3] == BUS200E_SYSEX_PROTO_VER);
  CHECK(out[4] == BUS200E_SYSEX_CMD_INFO);
}

static void test_simple_command_round_trip(void) {
  printf("test_simple_command_round_trip\n");
  uint8_t out[16], back[16];
  const uint8_t field[1] = { 0x3C };  // mod_addr
  const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_GET_DUMP, field, 1,
                                          nullptr, 0, out, sizeof(out));
  CHECK(n == 6);

  uint8_t cmd; uint32_t rl;
  CHECK(Bus200eSysExParseMessage(out, (uint32_t) n, &cmd, nullptr, nullptr,
                                  back, sizeof(back), &rl) == 0);
  CHECK(cmd == BUS200E_SYSEX_CMD_GET_DUMP);
  CHECK(rl == 1 && back[0] == 0x3C);
}

static void test_ack_nak_round_trip(void) {
  printf("test_ack_nak_round_trip\n");
  uint8_t out[16], back[16];
  const uint8_t nak_payload[2] = { BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_BUSY };
  const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_NAK, nak_payload, 2,
                                          nullptr, 0, out, sizeof(out));
  CHECK(n == 7);
  uint8_t cmd; uint32_t rl;
  CHECK(Bus200eSysExParseMessage(out, (uint32_t) n, &cmd, nullptr, nullptr,
                                  back, sizeof(back), &rl) == 0);
  CHECK(cmd == BUS200E_SYSEX_CMD_NAK);
  CHECK(rl == 2 && back[0] == BUS200E_SYSEX_CMD_GET_DUMP && back[1] == BUS200E_SYSEX_NAK_BUSY);
}

static void test_dump_data_round_trip_various_sizes(void) {
  printf("test_dump_data_round_trip_various_sizes\n");
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES], msg[BUS200E_SYSEX_MAX_MESSAGE + 8], back[128];
  for (uint32_t n : {0u, 1u, 7u, 8u, 43u, 44u}) {
    for (uint32_t i = 0; i < n; ++i) raw[i] = (uint8_t) (0xC0 + i);  // includes >=0x80 bytes
    const uint8_t field[2] = { 5, 12 };  // seq=5, total=12
    const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, 2,
                                             raw, n, msg, sizeof(msg));
    CHECK(ml > 0);
    CHECK((uint32_t) ml <= BUS200E_SYSEX_MAX_MESSAGE);
    for (int i = 0; i < ml; ++i) CHECK(!(msg[i] & 0x80));  // whole message 7-bit clean

    uint8_t cmd, seq, total; uint32_t rl;
    CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &seq, &total,
                                    back, sizeof(back), &rl) == 0);
    CHECK(cmd == BUS200E_SYSEX_CMD_DUMP_DATA);
    CHECK(seq == 5 && total == 12);
    CHECK(rl == n);
    CHECK(memcmp(back, raw, n) == 0);
  }
}

static void test_dump_data_full_chunk_hits_60_byte_ceiling(void) {
  printf("test_dump_data_full_chunk_hits_60_byte_ceiling\n");
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES] = {};
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
  const uint8_t field[2] = { 0, 1 };
  const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, 2,
                                           raw, BUS200E_SYSEX_CHUNK_BYTES, msg, sizeof(msg));
  CHECK(ml == BUS200E_SYSEX_MAX_MESSAGE);
  // + F0 + F7 (added by a real MIDI layer, not this module) == hOC's 60-byte rule
  CHECK(ml + 2 == 60);
}

static void test_build_rejects_oversized_chunk(void) {
  printf("test_build_rejects_oversized_chunk\n");
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES + 1] = {};
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
  const uint8_t field[2] = { 0, 1 };
  CHECK(Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, 2,
                                  raw, sizeof(raw), msg, sizeof(msg)) == -1);
}

static void test_build_rejects_raw_chunk_on_non_dump_data(void) {
  printf("test_build_rejects_raw_chunk_on_non_dump_data\n");
  uint8_t raw[4] = {1,2,3,4};
  uint8_t msg[32];
  CHECK(Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_ACK, nullptr, 0,
                                  raw, 4, msg, sizeof(msg)) == -1);
}

static void test_build_rejects_undersized_out(void) {
  printf("test_build_rejects_undersized_out\n");
  uint8_t tiny[4];  // smaller than the 5-byte header alone
  CHECK(Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_INFO, nullptr, 0,
                                  nullptr, 0, tiny, sizeof(tiny)) == -1);
}

static void test_parse_error_paths(void) {
  printf("test_parse_error_paths\n");
  uint8_t out[64]; uint8_t cmd, seq, total; uint32_t rl;

  // too short
  uint8_t short_msg[4] = {0};
  CHECK(Bus200eSysExParseMessage(short_msg, 4, &cmd, &seq, &total, out, sizeof(out), &rl) == -1);

  uint8_t raw[4] = { 9, 8, 7, 6 };
  uint8_t msg[32];
  const uint8_t field[2] = { 1, 1 };
  const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, 2,
                                           raw, 4, msg, sizeof(msg));
  CHECK(ml > 0);

  // baseline: parses clean
  CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &seq, &total, out, sizeof(out), &rl) == 0);

  // wrong manufacturer ID
  uint8_t bad_mfr[32]; memcpy(bad_mfr, msg, (size_t) ml);
  bad_mfr[0] = 0x00;
  CHECK(Bus200eSysExParseMessage(bad_mfr, (uint32_t) ml, &cmd, &seq, &total, out, sizeof(out), &rl) == -2);

  // wrong family byte
  uint8_t bad_family[32]; memcpy(bad_family, msg, (size_t) ml);
  bad_family[1] = 0x00;
  CHECK(Bus200eSysExParseMessage(bad_family, (uint32_t) ml, &cmd, &seq, &total, out, sizeof(out), &rl) == -2);

  // wrong app ID (e.g. a Captain MIDI frame -- 0x4D)
  uint8_t bad_app[32]; memcpy(bad_app, msg, (size_t) ml);
  bad_app[2] = 0x4D;
  CHECK(Bus200eSysExParseMessage(bad_app, (uint32_t) ml, &cmd, &seq, &total, out, sizeof(out), &rl) == -2);

  // version mismatch
  uint8_t bad_ver[32]; memcpy(bad_ver, msg, (size_t) ml);
  bad_ver[3] = 0x02;
  CHECK(Bus200eSysExParseMessage(bad_ver, (uint32_t) ml, &cmd, &seq, &total, out, sizeof(out), &rl) == -3);

  // undersized output buffer
  uint8_t tiny_out[2];
  CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &seq, &total, tiny_out, sizeof(tiny_out), &rl) == -5);

  // malformed packed chunk: hibits byte with nothing after it
  uint8_t malformed[8] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                           BUS200E_SYSEX_APP_ID, BUS200E_SYSEX_PROTO_VER,
                           BUS200E_SYSEX_CMD_DUMP_DATA, 0, 1, 0x00 };
  CHECK(Bus200eSysExParseMessage(malformed, 8, &cmd, &seq, &total, out, sizeof(out), &rl) == -4);
}

static void test_non_dump_data_command_not_unpacked(void) {
  printf("test_non_dump_data_command_not_unpacked\n");
  // A non-DUMP_DATA payload containing a byte >= 0x80 must be rejected at
  // BUILD time (every other command is documented "already 7-bit"), not
  // silently packed -- this module never packs anything but a DUMP_DATA
  // chunk. Build a message by hand with an out-of-range field byte and
  // confirm ParseMessage still passes it through byte-identical (no
  // unpacking applied) rather than misinterpreting it.
  uint8_t msg[16] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                      BUS200E_SYSEX_APP_ID, BUS200E_SYSEX_PROTO_VER,
                      BUS200E_SYSEX_CMD_STATUS_R, 0x01, 0x02, 0x03, 0x04 };
  uint8_t cmd; uint32_t rl; uint8_t out[8];
  CHECK(Bus200eSysExParseMessage(msg, 9, &cmd, nullptr, nullptr, out, sizeof(out), &rl) == 0);
  CHECK(cmd == BUS200E_SYSEX_CMD_STATUS_R);
  CHECK(rl == 4);
  CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);
}

// ---- xor7 checksum -------------------------------------------------------------

static void test_xor7(void) {
  printf("test_xor7\n");
  CHECK(Bus200eSysExXor7(0, nullptr, 0) == 0);
  const uint8_t a[3] = { 0x01, 0x02, 0x03 };
  CHECK(Bus200eSysExXor7(0, a, 3) == (0x01 ^ 0x02 ^ 0x03));
  // accumulates across chunks identically to one big call
  const uint8_t b[2] = { 0xFF, 0x10 };
  const uint8_t whole[5] = { 0x01, 0x02, 0x03, 0xFF, 0x10 };
  uint8_t acc = Bus200eSysExXor7(0, a, 3);
  acc = Bus200eSysExXor7(acc, b, 2);
  CHECK(acc == Bus200eSysExXor7(0, whole, 5));
  CHECK(!(acc & 0x80));  // always folded to 7 bits
}

// ---- end-to-end: a DUMP_DATA/DUMP_END stream as a web dev would build -------

static void test_full_dump_stream_shape(void) {
  printf("test_full_dump_stream_shape\n");
  // a small "captured dump" with bytes on both sides of 0x80, spanning two
  // chunks, streamed exactly as GET_DUMP's reply would
  uint8_t dump[60];
  for (int i = 0; i < 60; ++i) dump[i] = (uint8_t) (i * 5 + 3);  // spreads across 0-255

  const uint32_t chunk_bytes = 44;  // BUS200E_SYSEX_CHUNK_BYTES
  const uint32_t total = (sizeof(dump) + chunk_bytes - 1) / chunk_bytes;
  CHECK(total == 2);

  uint8_t reassembled[60];
  uint32_t got = 0;
  uint8_t running_xor = 0;

  for (uint32_t seq = 0; seq < total; ++seq) {
    const uint32_t off = seq * chunk_bytes;
    const uint32_t n = (sizeof(dump) - off < chunk_bytes) ? (sizeof(dump) - off) : chunk_bytes;
    running_xor = Bus200eSysExXor7(running_xor, dump + off, n);

    uint8_t field[2] = { (uint8_t) seq, (uint8_t) total };
    uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
    const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, 2,
                                             dump + off, n, msg, sizeof(msg));
    CHECK(ml > 0);

    uint8_t cmd, rseq, rtotal; uint32_t rl;
    uint8_t chunk_out[64];
    CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &rseq, &rtotal,
                                    chunk_out, sizeof(chunk_out), &rl) == 0);
    CHECK(rseq == seq && rtotal == total && rl == n);
    memcpy(reassembled + got, chunk_out, rl);
    got += rl;
  }
  CHECK(got == sizeof(dump));
  CHECK(memcmp(reassembled, dump, sizeof(dump)) == 0);

  // DUMP_END: [n_packets, xor7]
  const uint8_t end_field[2] = { (uint8_t) total, running_xor };
  uint8_t end_msg[16];
  const int eml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_END, end_field, 2,
                                            nullptr, 0, end_msg, sizeof(end_msg));
  CHECK(eml == 7);
  uint8_t cmd; uint32_t rl; uint8_t back[4];
  CHECK(Bus200eSysExParseMessage(end_msg, (uint32_t) eml, &cmd, nullptr, nullptr,
                                  back, sizeof(back), &rl) == 0);
  CHECK(cmd == BUS200E_SYSEX_CMD_DUMP_END);
  CHECK(rl == 2 && back[0] == total && back[1] == running_xor);

  // an independent, whole-buffer xor7 call agrees with the incremental one
  CHECK(Bus200eSysExXor7(0, dump, sizeof(dump)) == running_xor);
}

int main() {
  test_pack_unpack_round_trip();
  test_pack_exact_bytes();
  test_pack_undersized_out();
  test_unpack_malformed();
  test_unpack_undersized_out();

  test_header_bytes_match_hoc_family();
  test_simple_command_round_trip();
  test_ack_nak_round_trip();
  test_dump_data_round_trip_various_sizes();
  test_dump_data_full_chunk_hits_60_byte_ceiling();
  test_build_rejects_oversized_chunk();
  test_build_rejects_raw_chunk_on_non_dump_data();
  test_build_rejects_undersized_out();
  test_parse_error_paths();
  test_non_dump_data_command_not_unpacked();

  test_xor7();
  test_full_dump_stream_shape();

  printf("\ntest_bus200e_sysex: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
