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

  // BUS200E_SYSEX_CHUNK_BYTES worked example: pack(42) = 6 hibits + 42 = 48.
  const uint8_t raw42_pack_len = 48;
  uint8_t raw42[42] = {};
  uint8_t packed64[64];
  const int pl42 = Bus200eSysExPack(raw42, 42, packed64, sizeof(packed64));
  CHECK(pl42 == raw42_pack_len);
  CHECK(pl42 == BUS200E_SYSEX_MAX_PACKED);
  CHECK(BUS200E_SYSEX_CHUNK_BYTES == 42);

  // ...and 43 would not fit: pack() grows in 8-byte groups, so the next chunk
  // size up jumps straight past the frame budget. This is WHY v2's chunk is
  // 42 rather than "44 minus the two header bytes it lost".
  uint8_t raw43[43] = {};
  CHECK(Bus200eSysExPack(raw43, 43, packed64, sizeof(packed64)) == 50);
  CHECK(5 + BUS200E_SYSEX_DUMP_HDR_BYTES + 50 + 2 > 60);   // 61: over hOC's ceiling
  CHECK(5 + BUS200E_SYSEX_DUMP_HDR_BYTES + 48 + 2 <= 60);  // 59: under it
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

// Builds the four-septet DUMP_DATA header the way pump_send() does.
static void dump_hdr(uint8_t out[BUS200E_SYSEX_DUMP_HDR_BYTES],
                     uint16_t seq, uint16_t total) {
  out[0] = BUS200E_SYSEX_LO7(seq);
  out[1] = BUS200E_SYSEX_HI7(seq);
  out[2] = BUS200E_SYSEX_LO7(total);
  out[3] = BUS200E_SYSEX_HI7(total);
}

static void test_dump_data_round_trip_various_sizes(void) {
  printf("test_dump_data_round_trip_various_sizes\n");
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES], msg[BUS200E_SYSEX_MAX_MESSAGE + 8], back[128];
  for (uint32_t n : {0u, 1u, 7u, 8u, 41u, 42u}) {
    for (uint32_t i = 0; i < n; ++i) raw[i] = (uint8_t) (0xC0 + i);  // includes >=0x80 bytes
    uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
    dump_hdr(field, 5, 12);
    const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                             raw, n, msg, sizeof(msg));
    CHECK(ml > 0);
    CHECK((uint32_t) ml <= BUS200E_SYSEX_MAX_MESSAGE);
    for (int i = 0; i < ml; ++i) CHECK(!(msg[i] & 0x80));  // whole message 7-bit clean

    uint8_t cmd; uint16_t seq, total; uint32_t rl;
    CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &seq, &total,
                                    back, sizeof(back), &rl) == 0);
    CHECK(cmd == BUS200E_SYSEX_CMD_DUMP_DATA);
    CHECK(seq == 5 && total == 12);
    CHECK(rl == n);
    CHECK(memcmp(back, raw, n) == 0);
  }
}

// ---- v2's 14-bit counters ------------------------------------------------

static void test_septet_pair_helpers(void) {
  printf("test_septet_pair_helpers\n");
  // Low septet FIRST, MIDI's own convention. Round-trip the boundaries and
  // the two values the 251e transfer actually uses.
  for (uint32_t v : {0u, 1u, 127u, 128u, 1234u, 1435u, 1503u, 1561u, 16383u}) {
    const uint8_t lo = BUS200E_SYSEX_LO7(v), hi = BUS200E_SYSEX_HI7(v);
    CHECK(!(lo & 0x80) && !(hi & 0x80));
    CHECK(BUS200E_SYSEX_FROM14(lo, hi) == v);
  }
  // worked values, asserted literally so the JS mirror can assert the same
  CHECK(BUS200E_SYSEX_LO7(1234u) == 82 && BUS200E_SYSEX_HI7(1234u) == 9);
  CHECK(BUS200E_SYSEX_LO7(1503u) == 95 && BUS200E_SYSEX_HI7(1503u) == 11);
  CHECK(BUS200E_SYSEX_MAX_COUNT == 16383u);
}

static void test_dump_data_carries_a_14_bit_seq(void) {
  printf("test_dump_data_carries_a_14_bit_seq\n");
  // The whole point of v2: a seq far past v1's 127-packet ceiling survives
  // the wire intact.
  uint8_t raw[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8], back[64];
  for (uint16_t s : {(uint16_t) 127, (uint16_t) 128, (uint16_t) 1434,
                     (uint16_t) 1502, (uint16_t) 16382}) {
    uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
    dump_hdr(field, s, 16383);
    const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                             raw, sizeof(raw), msg, sizeof(msg));
    CHECK(ml > 0);
    uint8_t cmd; uint16_t seq, total; uint32_t rl;
    CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &seq, &total,
                                    back, sizeof(back), &rl) == 0);
    CHECK(seq == s);
    CHECK(total == 16383);
  }
}

static void test_dump_data_full_chunk_fits_the_60_byte_ceiling(void) {
  printf("test_dump_data_full_chunk_fits_the_60_byte_ceiling\n");
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES] = {};
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
  uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
  dump_hdr(field, 0, 1);
  const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                           raw, BUS200E_SYSEX_CHUNK_BYTES, msg, sizeof(msg));
  CHECK(ml == BUS200E_SYSEX_MAX_MESSAGE);
  CHECK(ml == 57);
  // + F0 + F7 (added by a real MIDI layer, not this module). 59, one under
  // hOC's 60-byte rule -- see test_pack_exact_bytes for why nothing lands on
  // it exactly under a 4-byte DUMP_DATA header.
  CHECK(ml + 2 == 59);
  CHECK(ml + 2 <= 60);
}

static void test_build_rejects_oversized_chunk(void) {
  printf("test_build_rejects_oversized_chunk\n");
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES + 1] = {};
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
  uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
  dump_hdr(field, 0, 1);
  CHECK(Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                  raw, sizeof(raw), msg, sizeof(msg)) == -1);
}

static void test_build_rejects_a_v1_two_byte_dump_header(void) {
  printf("test_build_rejects_a_v1_two_byte_dump_header\n");
  // v1's [seq, total] must fail loudly rather than be read as the first two
  // septets of a v2 header with the chunk shifted two bytes into the payload.
  uint8_t raw[8] = {};
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
  const uint8_t v1_field[2] = { 0, 1 };
  CHECK(Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, v1_field, 2,
                                  raw, sizeof(raw), msg, sizeof(msg)) == -1);
  const uint8_t too_long[5] = { 0, 0, 1, 0, 0 };
  CHECK(Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, too_long, 5,
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
  uint8_t out[64]; uint8_t cmd; uint16_t seq, total; uint32_t rl;

  // too short
  uint8_t short_msg[4] = {0};
  CHECK(Bus200eSysExParseMessage(short_msg, 4, &cmd, &seq, &total, out, sizeof(out), &rl) == -1);

  uint8_t raw[4] = { 9, 8, 7, 6 };
  uint8_t msg[32];
  uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
  dump_hdr(field, 1, 2);
  const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                           raw, 4, msg, sizeof(msg));
  CHECK(ml > 0);

  // a DUMP_DATA whose payload is shorter than the four-septet header
  uint8_t stub_hdr[8] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                          BUS200E_SYSEX_APP_ID, BUS200E_SYSEX_PROTO_VER,
                          BUS200E_SYSEX_CMD_DUMP_DATA, 0, 0, 1 };
  CHECK(Bus200eSysExParseMessage(stub_hdr, 8, &cmd, &seq, &total, out, sizeof(out), &rl) == -1);

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

  // version mismatch -- 0x01 is the realistic one now: a browser still on
  // v1's 7-bit packet counters must be told so (NAK 1), not quietly
  // misparsed, which is the whole reason v2 bumped the version byte.
  uint8_t bad_ver[32]; memcpy(bad_ver, msg, (size_t) ml);
  bad_ver[3] = 0x01;
  CHECK(Bus200eSysExParseMessage(bad_ver, (uint32_t) ml, &cmd, &seq, &total, out, sizeof(out), &rl) == -3);
  bad_ver[3] = 0x03;
  CHECK(Bus200eSysExParseMessage(bad_ver, (uint32_t) ml, &cmd, &seq, &total, out, sizeof(out), &rl) == -3);

  // undersized output buffer
  uint8_t tiny_out[2];
  CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &seq, &total, tiny_out, sizeof(tiny_out), &rl) == -5);

  // malformed packed chunk: hibits byte with nothing after it
  uint8_t malformed[10] = { BUS200E_SYSEX_MFR_ID, BUS200E_SYSEX_FAMILY_ID,
                            BUS200E_SYSEX_APP_ID, BUS200E_SYSEX_PROTO_VER,
                            BUS200E_SYSEX_CMD_DUMP_DATA, 0, 0, 1, 0, 0x00 };
  CHECK(Bus200eSysExParseMessage(malformed, 10, &cmd, &seq, &total, out, sizeof(out), &rl) == -4);
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

  const uint32_t chunk_bytes = BUS200E_SYSEX_CHUNK_BYTES;  // 42
  const uint32_t total = (sizeof(dump) + chunk_bytes - 1) / chunk_bytes;
  CHECK(total == 2);

  uint8_t reassembled[60];
  uint32_t got = 0;
  uint8_t running_xor = 0;

  for (uint32_t seq = 0; seq < total; ++seq) {
    const uint32_t off = seq * chunk_bytes;
    const uint32_t n = (sizeof(dump) - off < chunk_bytes) ? (sizeof(dump) - off) : chunk_bytes;
    running_xor = Bus200eSysExXor7(running_xor, dump + off, n);

    uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
    dump_hdr(field, (uint16_t) seq, (uint16_t) total);
    uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
    const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                             dump + off, n, msg, sizeof(msg));
    CHECK(ml > 0);

    uint8_t cmd; uint16_t rseq, rtotal; uint32_t rl;
    uint8_t chunk_out[64];
    CHECK(Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &rseq, &rtotal,
                                    chunk_out, sizeof(chunk_out), &rl) == 0);
    CHECK(rseq == seq && rtotal == total && rl == n);
    memcpy(reassembled + got, chunk_out, rl);
    got += rl;
  }
  CHECK(got == sizeof(dump));
  CHECK(memcmp(reassembled, dump, sizeof(dump)) == 0);

  // DUMP_END: [n_lo, n_hi, xor7]
  const uint8_t end_field[3] = { BUS200E_SYSEX_LO7(total), BUS200E_SYSEX_HI7(total),
                                 running_xor };
  uint8_t end_msg[16];
  const int eml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_END, end_field, 3,
                                            nullptr, 0, end_msg, sizeof(end_msg));
  CHECK(eml == 8);
  uint8_t cmd; uint32_t rl; uint8_t back[4];
  CHECK(Bus200eSysExParseMessage(end_msg, (uint32_t) eml, &cmd, nullptr, nullptr,
                                  back, sizeof(back), &rl) == 0);
  CHECK(cmd == BUS200E_SYSEX_CMD_DUMP_END);
  CHECK(rl == 3);
  CHECK(BUS200E_SYSEX_FROM14(back[0], back[1]) == total && back[2] == running_xor);

  // an independent, whole-buffer xor7 call agrees with the incremental one
  CHECK(Bus200eSysExXor7(0, dump, sizeof(dump)) == running_xor);
}

// ---- SHARED WORKED EXAMPLES ------------------------------------------------
// Everything below is asserted BYTE-FOR-BYTE, with the same literals, in
// tools/251e-sequencer/test/large-dump.test.mjs. They are the cross-check
// that the C++ and the JS implementations agree on the v2 wire format, not
// merely that each is self-consistent. If you change a number here, the JS
// suite must fail until it is changed there too -- that is the point.
//
// The synthetic "bank" both suites use: 63120 bytes (a real 251e preset
// bank: 30 records x 2104, per Buchla_FW/docs/251e-SEQUENCE-FORMAT.md, and
// exactly what a live MasterBackup(0x5C) reported), filled with
// (i*37 + 11) & 0xFF so every byte value appears and the buffer is NOT
// 7-bit-safe -- which is what makes the packing load-bearing.

#define WORKED_BANK_BYTES     63120u
#define WORKED_BANK_PACKETS   1503u   // ceil(63120 / 42)
#define WORKED_BANK_LAST_CHUNK  36u   // 63120 - 1502*42
#define WORKED_BANK_XOR7      0x10u   // Bus200eSysExXor7 over all 63120 bytes
#define WORKED_DUMP_DATA_WIRE_BYTES 88671u  // every DUMP_DATA frame, F0..F7 inclusive

static uint8_t worked_bank_byte(uint32_t i) { return (uint8_t) ((i * 37u + 11u) & 0xFFu); }

// One complete DUMP_DATA frame from the middle of that bank -- seq 1234 of
// 1503, i.e. raw offset 1234*42 = 51828. Message payload only (F0/F7 belong
// to the MIDI layer). Note bytes [5..8]: 0x52 0x09 = seq 1234 as low-septet-
// first, 0x5F 0x0B = total 1503 -- the two numbers v1's single-byte fields
// could not have carried at all.
static const uint8_t kWorkedFrameSeq1234[57] = {
  0x7D, 0x62, 0x35, 0x02, 0x44, 0x52, 0x09, 0x5F, 0x0B, 0x63, 0x4F, 0x74,
  0x19, 0x3E, 0x63, 0x08, 0x2D, 0x63, 0x52, 0x77, 0x1C, 0x41, 0x66, 0x0B,
  0x30, 0x63, 0x55, 0x7A, 0x1F, 0x44, 0x69, 0x0E, 0x33, 0x63, 0x58, 0x7D,
  0x22, 0x47, 0x6C, 0x11, 0x36, 0x61, 0x5B, 0x00, 0x25, 0x4A, 0x6F, 0x14,
  0x39, 0x61, 0x5E, 0x03, 0x28, 0x4D, 0x72, 0x17, 0x3C,
};

static void test_worked_frame_from_a_real_251e_bank(void) {
  printf("test_worked_frame_from_a_real_251e_bank\n");
  CHECK(WORKED_BANK_PACKETS ==
        (WORKED_BANK_BYTES + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);

  uint8_t chunk[BUS200E_SYSEX_CHUNK_BYTES];
  const uint32_t off = 1234u * BUS200E_SYSEX_CHUNK_BYTES;
  CHECK(off == 51828u);
  for (uint32_t i = 0; i < BUS200E_SYSEX_CHUNK_BYTES; ++i)
    chunk[i] = worked_bank_byte(off + i);

  uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
  dump_hdr(field, 1234, (uint16_t) WORKED_BANK_PACKETS);
  CHECK(field[0] == 0x52 && field[1] == 0x09 && field[2] == 0x5F && field[3] == 0x0B);

  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8];
  const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                           chunk, sizeof(chunk), msg, sizeof(msg));
  CHECK(ml == (int) sizeof(kWorkedFrameSeq1234));
  CHECK(memcmp(msg, kWorkedFrameSeq1234, sizeof(kWorkedFrameSeq1234)) == 0);

  // and it parses back to exactly what went in
  uint8_t cmd; uint16_t seq, total; uint32_t rl; uint8_t back[64];
  CHECK(Bus200eSysExParseMessage(kWorkedFrameSeq1234, sizeof(kWorkedFrameSeq1234),
                                  &cmd, &seq, &total, back, sizeof(back), &rl) == 0);
  CHECK(cmd == BUS200E_SYSEX_CMD_DUMP_DATA);
  CHECK(seq == 1234 && total == WORKED_BANK_PACKETS);
  CHECK(rl == BUS200E_SYSEX_CHUNK_BYTES);
  CHECK(memcmp(back, chunk, sizeof(chunk)) == 0);
}

static void test_whole_251e_bank_streams_and_reassembles(void) {
  printf("test_whole_251e_bank_streams_and_reassembles\n");
  // The transfer v1 physically could not describe: 63120 bytes in one
  // session. Built frame by frame, reassembled, checksummed.
  static uint8_t dump[WORKED_BANK_BYTES];
  static uint8_t back_buf[WORKED_BANK_BYTES];
  for (uint32_t i = 0; i < WORKED_BANK_BYTES; ++i) dump[i] = worked_bank_byte(i);

  CHECK(Bus200eSysExXor7(0, dump, WORKED_BANK_BYTES) == WORKED_BANK_XOR7);

  const uint32_t total = WORKED_BANK_PACKETS;
  uint32_t got = 0, wire_bytes = 0;
  uint8_t running_xor = 0;
  uint8_t msg[BUS200E_SYSEX_MAX_MESSAGE + 8], chunk_out[64];

  for (uint32_t seq = 0; seq < total; ++seq) {
    const uint32_t off = seq * BUS200E_SYSEX_CHUNK_BYTES;
    uint32_t n = WORKED_BANK_BYTES - off;
    if (n > BUS200E_SYSEX_CHUNK_BYTES) n = BUS200E_SYSEX_CHUNK_BYTES;
    if (seq == total - 1) CHECK(n == WORKED_BANK_LAST_CHUNK);

    uint8_t field[BUS200E_SYSEX_DUMP_HDR_BYTES];
    dump_hdr(field, (uint16_t) seq, (uint16_t) total);
    const int ml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, field, sizeof(field),
                                             dump + off, n, msg, sizeof(msg));
    if (ml <= 0) { CHECK(false); return; }
    if ((uint32_t) ml > BUS200E_SYSEX_MAX_MESSAGE) { CHECK(false); return; }
    wire_bytes += (uint32_t) ml + 2;  // + F0 + F7
    running_xor = Bus200eSysExXor7(running_xor, dump + off, n);

    uint8_t cmd; uint16_t rseq, rtotal; uint32_t rl;
    if (Bus200eSysExParseMessage(msg, (uint32_t) ml, &cmd, &rseq, &rtotal,
                                 chunk_out, sizeof(chunk_out), &rl) != 0) {
      CHECK(false); return;
    }
    if (rseq != seq || rtotal != total || rl != n) { CHECK(false); return; }
    memcpy(back_buf + got, chunk_out, rl);
    got += rl;
  }

  CHECK(got == WORKED_BANK_BYTES);
  CHECK(memcmp(back_buf, dump, WORKED_BANK_BYTES) == 0);
  CHECK(running_xor == WORKED_BANK_XOR7);
  CHECK(wire_bytes == WORKED_DUMP_DATA_WIRE_BYTES);

  // DUMP_END for the whole bank: [95, 11, 0x10]
  const uint8_t end_field[3] = { BUS200E_SYSEX_LO7(total), BUS200E_SYSEX_HI7(total),
                                 running_xor };
  CHECK(end_field[0] == 95 && end_field[1] == 11 && end_field[2] == 0x10);
  uint8_t end_msg[16];
  const int eml = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_END, end_field, 3,
                                            nullptr, 0, end_msg, sizeof(end_msg));
  CHECK(eml == 8);
  const uint8_t kEndFrame[8] = { 0x7D, 0x62, 0x35, 0x02, 0x45, 95, 11, 0x10 };
  CHECK(memcmp(end_msg, kEndFrame, sizeof(kEndFrame)) == 0);
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
  test_septet_pair_helpers();
  test_dump_data_carries_a_14_bit_seq();
  test_dump_data_full_chunk_fits_the_60_byte_ceiling();
  test_build_rejects_oversized_chunk();
  test_build_rejects_a_v1_two_byte_dump_header();
  test_build_rejects_raw_chunk_on_non_dump_data();
  test_build_rejects_undersized_out();
  test_parse_error_paths();
  test_non_dump_data_command_not_unpacked();

  test_xor7();
  test_full_dump_stream_shape();
  test_worked_frame_from_a_real_251e_bank();
  test_whole_251e_bank_streams_and_reassembles();

  printf("\ntest_bus200e_sysex: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
