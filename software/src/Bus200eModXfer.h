#ifndef BUS200EMODXFER_H_
#define BUS200EMODXFER_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// MOD_XFER (SysEx cmd 0x28, reply 0x68, bridge level 2): send ONE allowlisted
// 200e bus frame to a 259e and, where the frame asks for it, return the
// module's answer. Pure logic -- no Arduino/Teensy/USB dependency, host-tested
// in test_bus200e_modxfer.cpp against fixtures/mod_xfer_vectors.json,
// the same vectors a host-side client checks. The wire format is "Bridge level
// 2: MOD_XFER" in docs/bus200e-sysex-fw.md.
//
// This is NOT CalRec::FrameAllowed (a different allowlist, for the Cal
// Routine); neither calls the other.
// ---------------------------------------------------------------------------

#define BUS200E_MOD_XFER_FRAME_MAX 17  // request frame: 7..17 raw bytes
#define BUS200E_MOD_XFER_REPLY_MAX 18  // reply frame: 0..18 raw bytes

// MOD_XFER_R result byte.
enum {
  BUS200E_MOD_XFER_SENT      = 0,  // sent, no reply asked for
  BUS200E_MOD_XFER_REPLY     = 1,  // matched reply follows, packed
  BUS200E_MOD_XFER_NO_REPLY  = 2,  // no matching reply after the retries
  BUS200E_MOD_XFER_BUS_BUSY  = 3,  // bus busy for 1 s: the frame was NOT sent
  BUS200E_MOD_XFER_BUS_ERROR = 4,  // no acknowledge seen; the module may still have acted
};

// True only for the 259e table commands: frame[0] == n-1, dest 0x00, src 0x22,
// target frame[4] in 0x28..0x2B, magic frame[5] == 0xCA, 7 <= n <= 17, and
//  - frame[3] == 0x18 (read): n == 8, want_reply == 1;
//  - frame[3] == 0x0C (write): want_reply == 0 and by op frame[6]:
//    0 -> 10..17 bytes, 1 -> 10, 2 -> 11, 3 -> 7, 4 -> 7; any other op refused.
// Anything else (including a null frame or want_reply > 1) is refused.
bool Bus200eModXferAllowed(const uint8_t *frame, uint8_t n, uint8_t want_reply);

// True when `reply` (reply_n raw bytes, 6..18) answers `request`: a cmd 0x19
// frame from the request's target (request[4]) with the request's what/arg
// (request[6], request[7]). `request` must be an allowed read frame, i.e. at
// least 8 bytes.
bool Bus200eModXferReplyMatches(const uint8_t *request, const uint8_t *reply, uint8_t reply_n);

// payload = the bytes after the command byte: want_reply, then the 7-in-8
// packed frame. Returns 0 and fills *want_reply, frame[] and *n (7..17), or
// <0 when malformed (bad want_reply, a non-7-bit byte, bad packing, or a frame
// shorter than 7 or longer than 17 bytes). Never reads or writes out of range.
int Bus200eModXferParse(const uint8_t *payload, uint32_t len, uint8_t *want_reply,
                        uint8_t frame[BUS200E_MOD_XFER_FRAME_MAX], uint8_t *n);

// Builds the MOD_XFER_R message exactly as Bus200eSysExBuildMessage and the
// other Bus200eSysExBuild* helpers do: the bytes between F0 and F7, starting
// at the manufacturer byte (7D 62 35 02 68 <result> <packed reply>), which is
// what the bridge's send_message takes. Nothing follows `result` when
// reply_n == 0. Returns the length, or <0 on a null/undersized buffer,
// reply_n > 18, a null reply with reply_n > 0, or result > 4.
int Bus200eModXferBuildReply(uint8_t result, const uint8_t *reply, uint8_t reply_n,
                             uint8_t *out, uint32_t cap);

#endif  // BUS200EMODXFER_H_
