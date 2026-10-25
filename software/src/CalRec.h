#ifndef CALREC_H_
#define CALREC_H_

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Pure codec for the CV-calibration contract (Buchla_FW docs/cv-calibration/
// CONTRACT.md): CRC-16, the 259e calibration record, 200e bus frames with the
// allowlist of what the Xenomorpher may ever send, the cmd-0x19 reply parser,
// and the 259e apply/fit maths. Bit-exact with Buchla_FW tools/calrec.py;
// host-tested against test/fixtures/calrec_vectors.txt by test/test_calrec.cpp.
// No Arduino dependency. All multi-byte wire fields are big-endian.
//
// The fit uses double throughout (FitPoint::mv is double, not float) because
// the golden vectors are doubles and must be reproduced exactly.
// ---------------------------------------------------------------------------

namespace CalRec {

constexpr uint8_t kMaster = 0x22, kMagic = 0xCA, kModule259e = 0x59;
constexpr uint8_t kCmdOverride = 0x10, kCmdRelease = 0x11, kCmdPut = 0x12,
                  kCmdCommit = 0x0B, kCmdRead = 0x18, kCmdReply = 0x19;
constexpr int kRecLen259e = 30, kMaxFrame = 18;
constexpr int kMaxReplyData = 13;  // reply payload (what, arg, data) is at most 15

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.
uint16_t Crc16(const uint8_t *d, size_t n);

struct Channel { uint16_t gain; int16_t offset; };
struct Record { uint8_t unit_id[8]; uint16_t seq; uint32_t date; Channel ch[2]; };

void Encode259e(const Record &r, uint8_t out[kRecLen259e]);
// False on wrong length, magic, version, module, channel count or CRC.
bool Decode259e(const uint8_t *in, size_t n, Record *r);

// [n][0x00][0x22][cmd][target][0xCA] + payload, n = bytes after the length
// byte. Returns the total length, or -1 if FrameAllowed would refuse it.
int BuildFrame(uint8_t cmd, uint8_t target, const uint8_t *payload, uint8_t plen,
               uint8_t out[kMaxFrame]);

// The send allowlist. n is the array length (so f[0] must equal n - 1).
// Checks dest 0x00, src 0x22, magic 0xCA and, per cmd, the length byte:
// 0x18 7; 0x0B 8..9; 0x12 7..14 with off + datalen <= 40; 0x10 8; 0x11 5.
bool FrameAllowed(const uint8_t *f, uint8_t n);

// The console's raw-frame sender ('W') while a Cal Routine run is open (or a
// cancelled 259e run's COMMIT verdict is pending): only CAL frames the allowlist
// knows (FrameAllowed), and of those never PUT (0x12) or COMMIT (0x0B): those two
// are refused here whatever the address or magic, as is every frame FrameAllowed
// does not accept. Outside a run 'W' is unrestricted, as before.
bool ConsoleFrameAllowedDuringRun(const uint8_t *f, uint8_t n);

// Single-letter console commands that must not run while a run is open (or a cancelled
// run's COMMIT verdict is pending): the ones that change the resident card image or card
// serving, 'w' (patch a byte of the image: it could alter the run's backup), 'k' (toggle
// card serving), 'm' (master backup) and 'x' (master restore).
bool ConsoleCommandBlockedDuringRun(char c);

// [n][0x22][src][0x19][what][arg][data...]; n is the array length.
struct Reply { uint8_t src, what, arg, len; uint8_t data[kMaxReplyData]; };
bool ParseReply(const uint8_t *f, uint8_t n, Reply *r);

// clamp(((x * gain) >> 16) + offset, 0, 0x7FFF), exactly as the 259e computes it.
uint16_t Apply259e(uint16_t x, uint16_t gain, int16_t offset);

struct FitPoint { uint16_t raw; double mv; };
enum FitError { FIT_OK, FIT_TOO_FEW, FIT_GAIN_RANGE, FIT_OFFSET_RANGE };
// Least squares of mv * 4096 / 1200 against raw; the arithmetic order is pinned
// in the contract. *out is written only on FIT_OK.
FitError Fit259e(const FitPoint *p, int n, Channel *out);
// Ideal corrected code for mv millivolts: mv * 4096.0 / 1200.0.
double TargetCounts(double mv);
// (Apply259e(raw) - TargetCounts(mv)) * 1200 / 4096, in cents.
double ResidualCents(uint16_t raw, double mv, const Channel &c);
// The same without the 0..0x7FFF clamp. The 259e clamps its corrected code at
// 0x7FFF = 32767, one count under the 9600 mV target (32768), so the clamped
// residual at 9600 mV hides a positive error there; this one shows it.
double ResidualCentsUnclamped(uint16_t raw, double mv, const Channel &c);

}  // namespace CalRec

#endif  // CALREC_H_
