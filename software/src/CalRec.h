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

// ---- the target scale T (counts per octave) ---------------------------------------
// kCountsPerOct is THE one constant the live flow fits to, checks against and derives its
// identity threshold from. UNPROVEN: H1 says the 259e engine is 4096 counts per octave (the
// record stored by the first bench run is right, the stock gain 0x9041 is ~8 % sharp); H2
// says it is ~4431.5 (stock tracks, that record is ~90 cents flat per octave). Only a
// frequency measurement settles it (bench gates G1/G2). Change this value ONLY after those
// measurements, and keep every other implementation of this record format in step with it.
constexpr double kCountsPerOct = 4096.0;
// The T of the 3-argument Fit259e / 1-argument TargetCounts and ResidualCents: pinned, so the
// golden vectors (fixtures/calrec_vectors.txt) never move when kCountsPerOct does.
constexpr double kCountsPerOctGolden = 4096.0;

struct FitPoint { uint16_t raw; double mv; };
enum FitError { FIT_OK, FIT_TOO_FEW, FIT_GAIN_RANGE, FIT_OFFSET_RANGE };
// Least squares of mv * T / 1200 against raw; the arithmetic order is pinned in the
// contract. *out is written only on FIT_OK. The 3-argument form is T = 4096 (golden).
FitError Fit259e(const FitPoint *p, int n, double counts_per_oct, Channel *out);
FitError Fit259e(const FitPoint *p, int n, Channel *out);
// Ideal corrected code for mv millivolts: mv * T / 1200.0 (1-argument form: T = 4096).
double TargetCounts(double mv, double counts_per_oct);
double TargetCounts(double mv);

// ---- ADC span / offset / channel check of a fitted channel (host-side; no bus, no writing) ----
// The 259e's input ADC spans 10 V over 65536 raw counts (nominal; one bench unit measured
// -0.01 % from it), so one 1.2 V octave is 7864.32 raw counts and a correct gain is
// T * 65536 / 7864.32 whatever the meter says. A gain far from that is a wrong raw span,
// a wrong channel or a wrong T. NOTE: the fit's gain is ~8.3333 * T by construction, so a
// check against the SAME T cannot tell a wrong T (it cannot detect a wrong counts-per-octave
// constant); that needs the believed truth passed in, or the octave-ratio proof below.
constexpr double kRawPerOct = 65536.0 * 1.2 / 10.0;  // 7864.32
constexpr double kGainTol = 0.015;                   // |gain / g_exp - 1| <= 1.5 %
constexpr double kChannelGainTol = 0.01;             // the two channels agree within 1 % of g_exp
// The fitted offset is -(raw at 0 V) * slope: the input's zero error times ~3.4 counts/mV
// (about 3.7 at T 4431). The bench unit read +38/+42 (zero about -11 mV); the window allows
// a zero from about -44 mV to +15 mV, so a unit with a zero at or above 0 V is not rejected,
// and a bent input that drags the fitted offset down by ~90 counts still fits.
constexpr int kOffsetMin = -50;
constexpr int kOffsetMax = 150;
enum Sanity { SANITY_OK, SANITY_GAIN_LOW, SANITY_GAIN_HIGH, SANITY_OFFSET_RANGE, SANITY_CH_MISMATCH };
double ExpectedGain(double counts_per_oct);
Sanity Sanity259e(const Channel &c, double counts_per_oct);
// Both channels: the first channel's own verdict, then the second's, then the pair.
Sanity Sanity259e(const Channel &c0, const Channel &c1, double counts_per_oct);
const char *SanityName(Sanity s);

// ---- post-write proof (frequency counter, bench gate G5) ------------------------------
// |log2(f_hi / f_lo) / n_oct - 1| <= tol: n_oct octaves of CV must give n_oct octaves of
// frequency. 0.0015 = 1.8 cents per octave. 2.000 passes; 1.898 (T 4096 on a 4431.5
// engine) and 2.117 (4431.5 on 4096) fail. Not wired into the flow yet: the frequency
// counter feeds it later.
constexpr double kOctaveRatioTol = 0.0015;
bool OctaveRatioOk(double f_lo, double f_hi, double n_oct, double tol = kOctaveRatioTol);

// (Apply259e(raw) - TargetCounts(mv, T)) * 1200 / T, in cents (1-argument forms: T = 4096).
double ResidualCents(uint16_t raw, double mv, const Channel &c, double counts_per_oct);
double ResidualCents(uint16_t raw, double mv, const Channel &c);
// The same without the 0..0x7FFF clamp. The 259e clamps its corrected code at
// 0x7FFF = 32767, one count under the 9600 mV target (32768), so the clamped
// residual at 9600 mV hides a positive error there; this one shows it.
double ResidualCentsUnclamped(uint16_t raw, double mv, const Channel &c, double counts_per_oct);
double ResidualCentsUnclamped(uint16_t raw, double mv, const Channel &c);

}  // namespace CalRec

#endif  // CALREC_H_
