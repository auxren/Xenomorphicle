#ifndef CALREC251E_H_
#define CALREC251E_H_

#include <stddef.h>
#include <stdint.h>

#include "CalRec.h"

// ---------------------------------------------------------------------------
// The 251e's calibration record and fit (design: docs/specs/cal-251e.md; the
// shared contract is Buchla_FW docs/cv-calibration/CONTRACT.md, 251e rows).
// Pure, no Arduino dependency, host-tested by test/test_calrec251e.cpp. The
// framing, CRC-16 and frame allowlist are CalRec's; the 251e uses the same
// commands (OVERRIDE 0x10, RELEASE 0x11, PUT 0x12, COMMIT 0x0B, READ 0x18).
//
// What a channel does (module side): out = clamp(((code * gain) >> 14) + offset,
// 0, 4095); gain is Q2.14 (0x4000 = unity), offset is in DAC codes. An OVERRIDE
// puts a RAW code on the DAC, bypassing that correction, so the run measures
// the analog stage V = a * r + b (r the raw code) and picks gain and offset so
// that stage code c lands on c * kMvPerCode.
//
// ASSUMPTIONS (the only place the physical scale is written down; each is a
// firmware or paper fact, none has been measured on a 251e):
//   * 383.1 DAC codes per volt: the stock tables put 10.000 V at code 3831 and
//     one cent (= 1 mV at 1.2 V per octave) at 0.3831 codes. kStockCodeAtTop and
//     kStockTopMv carry it; kMvPerCode derives from them.
//   * The ANCHORS below are separate literals (stage code -> volts at the jack)
//     and the record check uses them, never kMvPerCode, so a fit whose scale
//     constant is wrong in the CODE cannot also move the check that would catch it
//     (the 259e fit assumed 4096 counts per octave and its own verify agreed).
//     They are predictions from the measured line plus the record, not independent
//     measurements, and they share the premise (383.1 codes per volt) with the fit:
//     a wrong premise is only visible in the measured line itself (ScaleOk, and the
//     logged a and b), which is what the first measuring-only run is for.
// ---------------------------------------------------------------------------

namespace CalRec251e {

constexpr uint8_t kModule251e = 0x51;
constexpr uint8_t kBusAddr = 0x5C;
constexpr int kRecLen = 38, kNch = 4;
constexpr uint16_t kGainOne = 0x4000;
constexpr int kMaxCode = 4095;

constexpr int kStockCodeAtTop = 3831;          // ASSUMPTION: stock table, 10.000 V
constexpr double kStockTopMv = 10000.0;
constexpr double kMvPerCode = kStockTopMv / double(kStockCodeAtTop);  // 2.61028 mV

// Physical sanity limits, in millivolts at the jack (see the spec, section 7).
struct Anchor { int code; double mv; };
// Codes chosen so that 383.1 codes per volt rounds to the code within 0.7 mV (1000 mV -> 383.1,
// 7500 mV -> 2873.25, 10000 mV -> 3831); kAnchorTolMv includes that rounding.
constexpr Anchor kAnchors[3] = {{383, 1000.0}, {2873, 7500.0}, {3831, 10000.0}};  // literals
constexpr double kAnchorTolMv = 8.0;       // the write path's limit
constexpr double kAnchorGrossMv = 50.0;    // a record this far off is refused even in a measuring-only run
constexpr double kScaleLoMv = 9600.0, kScaleHiMv = 10400.0;  // the line at code 3831: +-4 %
constexpr double kZeroMaxMv = 100.0;                         // |b|

struct Record { uint8_t unit_id[8]; uint16_t seq; uint32_t date; CalRec::Channel ch[kNch]; };

void Encode(const Record &r, uint8_t out[kRecLen]);
// False on wrong length, magic, version, module, channel count or CRC.
bool Decode(const uint8_t *in, size_t n, Record *r);

// clamp(((code * gain) >> 14) + offset, 0, 4095), as the module computes it.
uint16_t Apply(uint16_t code, uint16_t gain, int16_t offset);

// The stage code for a stage voltage: round(mv * kStockCodeAtTop / kStockTopMv).
int CodeOfMv(double mv);

// The analog stage as measured: V(mV) = a * r + b, r the raw DAC code.
struct Analog { double a, b; };
double PredictMv(const Analog &an, uint16_t raw_code);

struct FitPoint { uint16_t code; double mv; };  // raw code held, volts measured
enum FitError { FIT_OK, FIT_TOO_FEW, FIT_SLOPE, FIT_GAIN_RANGE, FIT_OFFSET_RANGE };

// The analog line alone: least squares of mv on code (summed in input order, IEEE
// doubles, no FMA). False with fewer than 2 distinct codes.
bool Line(const FitPoint *p, int n, Analog *an);

// Least squares of mv on code (summed in input order, IEEE doubles, no FMA),
// then gain and offset for a target of mv_per_code per stage code, refined over a window of
// integer pairs (gain +-4, offset +-6 around the rounded closed form; every stage code is
// evaluated, ties go to the pair nearest the rounded one). *out, *an are written only on FIT_OK;
// *worst_mv (optional) is the worst predicted |error| over the reachable stage
// codes 0..3831 for the chosen pair.
FitError Fit(const FitPoint *p, int n, double mv_per_code, CalRec::Channel *out, Analog *an,
             double *worst_mv);

// The worst predicted error of one (gain, offset) pair on the line `an`, as Fit() weighs it:
// every stage code 0..3831 the stage can reach. Stops early (returning a value above `cutoff`)
// once the running worst passes cutoff; pass a huge cutoff for the exact value.
double Worst(const Analog &an, uint16_t gain, int16_t offset, double mv_per_code,
             double cutoff = 1.0e300);
// Shape of the fit points about their line, for the first-run report: `bow` is the peak of the
// quadratic component over the span (mV), `inl` the worst residual of a quadratic fit (mV).
bool BowInl(const FitPoint *p, int n, double *bow, double *inl);
// Worst |a * r + b - code * mv_per_code| over the reachable stage codes.
double WorstErrorMv(const Analog &an, const CalRec::Channel &c, double mv_per_code);
// Worst |fit point - line|, mV.
double Nonlinearity(const FitPoint *p, int n, const Analog &an);
// The measured line at code 3831 is within kScaleLoMv..kScaleHiMv, a > 0, |b| <= kZeroMaxMv.
bool ScaleOk(const Analog &an);
// The record's predicted jack voltage at the three anchors is within kAnchorTolMv of
// the literals. *worst_mv (optional) is the largest deviation.
bool AnchorsOk(const Analog &an, const CalRec::Channel &c, double *worst_mv);

}  // namespace CalRec251e

#endif  // CALREC251E_H_
