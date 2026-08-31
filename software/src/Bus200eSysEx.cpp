// SysEx framing for the browser <-> Xenomorpher preset-dump bridge. Pure
// logic -- no USB/MIDI includes; see Bus200eSysEx.h for the wire contract.
#include <string.h>

// On target, keep this cold code out of ITCM; host builds compile it bare.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define SYSEX_CODE FLASHMEM
#else
#define SYSEX_CODE
#endif

#include "Bus200eSysEx.h"

// ---- 7-bit packing -----------------------------------------------------------

SYSEX_CODE int Bus200eSysExPack(const uint8_t *raw, uint32_t raw_len,
                                 uint8_t *out, uint32_t out_cap) {
  if (!raw && raw_len) return -1;
  uint32_t oi = 0;
  for (uint32_t ri = 0; ri < raw_len; ri += 7) {
    const uint32_t cnt = (raw_len - ri < 7) ? (raw_len - ri) : 7;
    if (oi + 1 + cnt > out_cap) return -1;
    uint8_t hibits = 0;
    for (uint32_t j = 0; j < cnt; ++j)
      if (raw[ri + j] & 0x80) hibits |= (uint8_t) (1u << j);
    out[oi++] = hibits;
    for (uint32_t j = 0; j < cnt; ++j)
      out[oi++] = raw[ri + j] & 0x7F;
  }
  return (int) oi;
}

SYSEX_CODE int Bus200eSysExUnpack(const uint8_t *packed, uint32_t packed_len,
                                   uint8_t *out, uint32_t out_cap) {
  if (!packed && packed_len) return -1;
  uint32_t pi = 0, oi = 0;
  while (pi < packed_len) {
    const uint8_t hibits = packed[pi++];
    const uint32_t remaining = packed_len - pi;
    const uint32_t cnt = (remaining < 7) ? remaining : 7;
    if (cnt == 0) return -1;  // a hibits byte with no data bytes following
    if (oi + cnt > out_cap) return -1;
    for (uint32_t j = 0; j < cnt; ++j) {
      uint8_t b = packed[pi++] & 0x7F;
      if (hibits & (1u << j)) b |= 0x80;
      out[oi++] = b;
    }
  }
  return (int) oi;
}

// Structural scan of a pack()-shaped buffer: no output, just the raw length
// it would unpack to (or -1 if the group structure is malformed -- a hibits
// byte with no data byte following it). Lets ParseMessage tell "your buffer
// is too small" (-5) apart from "the message itself is corrupt" (-4)
// without a wasted full unpack attempt.
SYSEX_CODE static int scan_unpacked_len(const uint8_t *packed, uint32_t packed_len) {
  uint32_t pi = 0, count = 0;
  while (pi < packed_len) {
    pi++;  // hibits byte
    const uint32_t remaining = packed_len - pi;
    const uint32_t cnt = (remaining < 7) ? remaining : 7;
    if (cnt == 0) return -1;
    pi += cnt;
    count += cnt;
  }
  return (int) count;
}

// ---- message build/parse ------------------------------------------------------

SYSEX_CODE int Bus200eSysExBuildMessage(uint8_t cmd,
                                         const uint8_t *field_payload, uint32_t field_len,
                                         const uint8_t *raw_chunk, uint32_t raw_chunk_len,
                                         uint8_t *out, uint32_t out_cap) {
  if (out_cap < 5) return -1;
  if (raw_chunk_len && cmd != BUS200E_SYSEX_CMD_DUMP_DATA) return -1;
  if (raw_chunk_len > BUS200E_SYSEX_CHUNK_BYTES) return -1;
  if (field_len && !field_payload) return -1;
  if (raw_chunk_len && !raw_chunk) return -1;
  // A DUMP_DATA frame's header is a fixed four septets (seq lo/hi, total
  // lo/hi) -- v2 widened these, and a caller still passing v1's two bytes
  // must fail loudly here rather than have its chunk silently read as a
  // header. (A zero-length chunk is legal; a wrong-size header is not.)
  if (cmd == BUS200E_SYSEX_CMD_DUMP_DATA && field_len != BUS200E_SYSEX_DUMP_HDR_BYTES)
    return -1;

  out[0] = BUS200E_SYSEX_MFR_ID;
  out[1] = BUS200E_SYSEX_FAMILY_ID;
  out[2] = BUS200E_SYSEX_APP_ID;
  out[3] = BUS200E_SYSEX_PROTO_VER;
  out[4] = cmd;
  uint32_t oi = 5;

  if (field_len) {
    if (oi + field_len > out_cap) return -1;
    memcpy(out + oi, field_payload, field_len);
    oi += field_len;
  }

  if (raw_chunk_len) {
    const int pl = Bus200eSysExPack(raw_chunk, raw_chunk_len, out + oi, out_cap - oi);
    if (pl < 0) return -1;
    oi += (uint32_t) pl;
  }

  return (int) oi;
}

SYSEX_CODE int Bus200eSysExParseMessage(const uint8_t *in, uint32_t in_len,
                                         uint8_t *cmd_out, uint16_t *out_seq, uint16_t *out_total,
                                         uint8_t *out_raw, uint32_t out_raw_cap,
                                         uint32_t *out_raw_len) {
  if (in_len < 5) return -1;
  if (in[0] != BUS200E_SYSEX_MFR_ID || in[1] != BUS200E_SYSEX_FAMILY_ID ||
      in[2] != BUS200E_SYSEX_APP_ID)
    return -2;
  if (in[3] != BUS200E_SYSEX_PROTO_VER) return -3;

  const uint8_t cmd = in[4];
  const uint8_t *payload = in + 5;
  const uint32_t payload_len = in_len - 5;

  if (cmd == BUS200E_SYSEX_CMD_DUMP_DATA) {
    if (payload_len < BUS200E_SYSEX_DUMP_HDR_BYTES) return -1;
    const uint16_t seq = BUS200E_SYSEX_FROM14(payload[0], payload[1]);
    const uint16_t total = BUS200E_SYSEX_FROM14(payload[2], payload[3]);
    const uint8_t *packed = payload + BUS200E_SYSEX_DUMP_HDR_BYTES;
    const uint32_t packed_len = payload_len - BUS200E_SYSEX_DUMP_HDR_BYTES;

    const int need = scan_unpacked_len(packed, packed_len);
    if (need < 0) return -4;
    if ((uint32_t) need > out_raw_cap) return -5;

    const int raw_len = Bus200eSysExUnpack(packed, packed_len, out_raw, out_raw_cap);
    if (raw_len < 0) return -4;  // unreachable given the scan above; fail closed

    if (cmd_out) *cmd_out = cmd;
    if (out_seq) *out_seq = seq;
    if (out_total) *out_total = total;
    if (out_raw_len) *out_raw_len = (uint32_t) raw_len;
    return 0;
  }

  // every other command: payload is already 7-bit, passed through verbatim
  if (payload_len > out_raw_cap) return -5;
  if (payload_len && !out_raw) return -5;
  if (payload_len) memcpy(out_raw, payload, payload_len);
  if (cmd_out) *cmd_out = cmd;
  if (out_raw_len) *out_raw_len = payload_len;
  return 0;
}

SYSEX_CODE uint8_t Bus200eSysExXor7(uint8_t acc, const uint8_t *raw, uint32_t len) {
  uint8_t x = acc & 0x7F;
  for (uint32_t i = 0; i < len; ++i) x ^= raw[i];
  return x & 0x7F;
}
