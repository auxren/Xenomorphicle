// SysEx framing for the browser <-> Xenomorpher preset-dump bridge. Pure
// logic -- no USB/MIDI includes; see Bus200eSysEx.h for the wire contract.
#include <string.h>

// On target, keep this cold code out of ITCM; host builds compile it bare.
#if defined(__IMXRT1062__)
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
  const bool chunked = (cmd == BUS200E_SYSEX_CMD_DUMP_DATA || cmd == BUS200E_SYSEX_CMD_FW_DATA);
  if (raw_chunk_len && !chunked) return -1;
  if (raw_chunk_len > BUS200E_SYSEX_CHUNK_BYTES) return -1;
  if (field_len && !field_payload) return -1;
  if (raw_chunk_len && !raw_chunk) return -1;
  // A DUMP_DATA frame's header is a fixed four septets (seq lo/hi, total
  // lo/hi) -- v2 widened these, and a caller still passing v1's two bytes
  // must fail loudly here rather than have its chunk silently read as a
  // header. (A zero-length chunk is legal; a wrong-size header is not.)
  if (chunked && field_len != BUS200E_SYSEX_DUMP_HDR_BYTES)
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

  if (cmd == BUS200E_SYSEX_CMD_DUMP_DATA || cmd == BUS200E_SYSEX_CMD_FW_DATA) {
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

// ---- firmware-card / scan / bus-status codec -----------------------------------

SYSEX_CODE uint32_t Bus200eSysExSeptets(const uint8_t *p, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n && i < 5; ++i) v |= (uint32_t) (p[i] & 0x7F) << (7 * i);
  return v;
}

SYSEX_CODE void Bus200eSysExToSeptets(uint32_t v, uint8_t *out, int n) {
  for (int i = 0; i < n; ++i) out[i] = (i < 5) ? (uint8_t) ((v >> (7 * i)) & 0x7F) : 0;
}

SYSEX_CODE static int begin_message(uint8_t cmd, uint32_t fields, uint8_t *out, uint32_t cap) {
  if (!out || cap < 5 + fields) return -1;
  out[0] = BUS200E_SYSEX_MFR_ID;
  out[1] = BUS200E_SYSEX_FAMILY_ID;
  out[2] = BUS200E_SYSEX_APP_ID;
  out[3] = BUS200E_SYSEX_PROTO_VER;
  out[4] = cmd;
  return 5;
}

SYSEX_CODE static bool all7(const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) if (p[i] & 0x80) return false;
  return true;
}

SYSEX_CODE int Bus200eSysExBuildFwBegin(uint8_t kind, uint32_t len, uint32_t crc32,
                                         uint8_t *out, uint32_t cap) {
  if ((kind & 0x80) || len > 0x1FFFFFu) return -1;
  const int o = begin_message(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_FW_BEGIN_FIELDS, out, cap);
  if (o < 0) return -1;
  out[o] = kind;
  Bus200eSysExToSeptets(len, out + o + 1, 3);
  Bus200eSysExToSeptets(crc32, out + o + 4, 5);
  return o + BUS200E_SYSEX_FW_BEGIN_FIELDS;
}

SYSEX_CODE int Bus200eSysExParseFwBegin(const uint8_t *payload, uint32_t len,
                                         uint8_t *kind, uint32_t *image_len, uint32_t *crc32) {
  if (!payload || len != BUS200E_SYSEX_FW_BEGIN_FIELDS || !all7(payload, len)) return -1;
  if (payload[8] > 0x0F) return -1;  // the CRC is 32 bits: the fifth septet is 0..15
  if (kind) *kind = payload[0];
  if (image_len) *image_len = Bus200eSysExSeptets(payload + 1, 3);
  if (crc32) *crc32 = Bus200eSysExSeptets(payload + 4, 5);
  return 0;
}

SYSEX_CODE int Bus200eSysExBuildFwStatusR(const Bus200eFwStatus *s, uint8_t *out, uint32_t cap) {
  if (!s) return -1;
  if (s->ver_gen > 0x3FFF) return -1;
  if (s->served > 0x1FFFFFu || s->image_len > 0x1FFFFFu || s->first_read_age_s > 0x3FFF) return -1;
  if ((s->armed | s->serving | s->kind | s->ver_addr) & 0x80) return -1;
  const int o = begin_message(BUS200E_SYSEX_CMD_FW_STATUS_R, BUS200E_SYSEX_FW_STATUS_R_FIELDS, out, cap);
  if (o < 0) return -1;
  uint8_t *p = out + o;
  p[0] = s->armed;
  p[1] = s->serving;
  p[2] = s->kind;
  Bus200eSysExToSeptets(s->served, p + 3, 3);
  Bus200eSysExToSeptets(s->first_read_age_s, p + 6, 2);
  Bus200eSysExToSeptets(s->image_len, p + 8, 3);
  p[11] = s->ver_addr;
  p[12] = s->ver_valid ? 1 : 0;
  for (int i = 0; i < 7; ++i) p[13 + i] = s->ver_valid ? (uint8_t) (s->ver[i] & 0x7F) : 0x20;
  Bus200eSysExToSeptets(s->ver_gen, p + 20, 2);
  return o + BUS200E_SYSEX_FW_STATUS_R_FIELDS;
}

SYSEX_CODE int Bus200eSysExParseFwStatusR(const uint8_t *payload, uint32_t len, Bus200eFwStatus *s) {
  if (!payload || !s || len != BUS200E_SYSEX_FW_STATUS_R_FIELDS || !all7(payload, len)) return -1;
  s->armed = payload[0];
  s->serving = payload[1];
  s->kind = payload[2];
  s->served = Bus200eSysExSeptets(payload + 3, 3);
  s->first_read_age_s = (uint16_t) Bus200eSysExSeptets(payload + 6, 2);
  s->image_len = Bus200eSysExSeptets(payload + 8, 3);
  s->ver_addr = payload[11];
  s->ver_valid = payload[12];
  memcpy(s->ver, payload + 13, 7);
  s->ver_gen = (uint16_t) Bus200eSysExSeptets(payload + 20, 2);
  return 0;
}

SYSEX_CODE int Bus200eSysExBuildScanR(uint8_t idx, uint8_t count, uint8_t addr, uint8_t ver_valid,
                                       const uint8_t ver[7], uint8_t *out, uint32_t cap) {
  if ((idx | count | addr) & 0x80) return -1;
  if (ver_valid && !ver) return -1;
  const int o = begin_message(BUS200E_SYSEX_CMD_SCAN_R, 11, out, cap);
  if (o < 0) return -1;
  out[o] = idx;
  out[o + 1] = count;
  out[o + 2] = addr;
  out[o + 3] = ver_valid ? 1 : 0;
  for (int i = 0; i < 7; ++i) out[o + 4 + i] = ver_valid ? (uint8_t) (ver[i] & 0x7F) : 0x20;
  return o + 11;
}

SYSEX_CODE int Bus200eSysExBuildBusStatusR(uint8_t wpm, uint8_t quiet, uint8_t dialect, uint8_t err,
                                            uint8_t *out, uint32_t cap) {
  if ((wpm | quiet | dialect | err) & 0x80) return -1;
  const int o = begin_message(BUS200E_SYSEX_CMD_BUS_STATUS_R, 4, out, cap);
  if (o < 0) return -1;
  out[o] = wpm;
  out[o + 1] = quiet;
  out[o + 2] = dialect;
  out[o + 3] = err;
  return o + 4;
}
