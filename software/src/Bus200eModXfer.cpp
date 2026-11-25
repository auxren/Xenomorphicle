// MOD_XFER allowlist, reply matcher and payload codec. Pure logic: no
// Arduino/Teensy headers, no dynamic allocation. See Bus200eModXfer.h.
#include <string.h>

#include "Bus200eModXfer.h"
#include "Bus200eSysEx.h"

bool Bus200eModXferAllowed(const uint8_t *frame, uint8_t n, uint8_t want_reply) {
  if (!frame || want_reply > 1) return false;
  if (n < 7 || n > BUS200E_MOD_XFER_FRAME_MAX) return false;
  if (frame[0] != (uint8_t) (n - 1) || frame[1] != 0x00 || frame[2] != 0x22) return false;
  if (frame[4] < 0x28 || frame[4] > 0x2B || frame[5] != 0xCA) return false;
  if (frame[3] == 0x18) return n == 8 && want_reply == 1;
  if (frame[3] != 0x0C || want_reply != 0) return false;
  switch (frame[6]) {
    case 0: return n >= 10;  // n <= 17 already checked
    case 1: return n == 10;
    case 2: return n == 11;
    case 3:
    case 4: return n == 7;
    default: return false;
  }
}

bool Bus200eModXferReplyMatches(const uint8_t *request, const uint8_t *reply, uint8_t reply_n) {
  if (!request || !reply) return false;
  if (reply_n < 6 || reply_n > BUS200E_MOD_XFER_REPLY_MAX) return false;
  return reply[0] == (uint8_t) (reply_n - 1) && reply[1] == 0x22 && reply[2] == request[4] &&
         reply[3] == 0x19 && reply[4] == request[6] && reply[5] == request[7];
}

int Bus200eModXferParse(const uint8_t *payload, uint32_t len, uint8_t *want_reply,
                        uint8_t frame[BUS200E_MOD_XFER_FRAME_MAX], uint8_t *n) {
  if (!payload || !want_reply || !frame || !n) return -1;
  if (len < 2) return -1;  // want_reply plus at least one packed group
  if (payload[0] > 1) return -1;
  for (uint32_t i = 1; i < len; ++i)
    if (payload[i] & 0x80) return -1;
  // Unpack refuses anything that would overrun the 17-byte buffer.
  const int got = Bus200eSysExUnpack(payload + 1, len - 1, frame, BUS200E_MOD_XFER_FRAME_MAX);
  if (got < 7 || got > BUS200E_MOD_XFER_FRAME_MAX) return -1;
  *want_reply = payload[0];
  *n = (uint8_t) got;
  return 0;
}

int Bus200eModXferBuildReply(uint8_t result, const uint8_t *reply, uint8_t reply_n,
                             uint8_t *out, uint32_t cap) {
  if (!out || result > BUS200E_MOD_XFER_BUS_ERROR) return -1;
  if (reply_n > BUS200E_MOD_XFER_REPLY_MAX || (reply_n && !reply)) return -1;
  if (cap < 6) return -1;
  out[0] = BUS200E_SYSEX_MFR_ID;
  out[1] = BUS200E_SYSEX_FAMILY_ID;
  out[2] = BUS200E_SYSEX_APP_ID;
  out[3] = BUS200E_SYSEX_PROTO_VER;
  out[4] = BUS200E_SYSEX_CMD_MOD_XFER_R;
  out[5] = result;
  if (!reply_n) return 6;
  const int pn = Bus200eSysExPack(reply, reply_n, out + 6, cap - 6);
  if (pn < 0) return -1;
  return 6 + pn;
}
