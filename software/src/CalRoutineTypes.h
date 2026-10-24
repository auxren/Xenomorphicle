#ifndef CALROUTINETYPES_H_
#define CALROUTINETYPES_H_

#include <stddef.h>
#include <stdint.h>

#include "CalRoutineMath.h"

// ---------------------------------------------------------------------------
// What every Cal Routine target shares: the hardware ports (the USB DMM, the
// CV outputs, the clock), the log sink, and the meter rule's timings and
// limits. Split out of CalRoutineFlow.h so that the meter rule
// (CalRoutineMeasure) and other targets (Cal259eFlow) build and link without
// the Self/Outputs/Inputs flow. CalRoutineFlow.h includes this; the names are
// unchanged.
// ---------------------------------------------------------------------------

namespace CalRoutine {

// The USB DMM. Implemented over USBTMCDevice (src/drivers/USBTMCDevice.h):
// Begin = BeginQuery, Poll maps Done -> 1 (reply = Reply()), Busy -> 0,
// Refused / Timeout / Error -> < 0, Recovering -> 0 while a query was
// started, Connected = connected(), Identity = the *IDN? reply the glue read.
struct MeterPort {
  // False when the query could not be started (no meter, busy, the driver
  // resynchronising after an error): the flow waits and tries again.
  virtual bool Begin(const char *scpi, uint32_t timeout_ms) = 0;
  // 0 busy, 1 done (*reply valid until the next Begin), < 0 error, timeout or refused.
  virtual int Poll(const char **reply) = 0;
  virtual bool Connected() const = 0;
  virtual const char *Identity() const = 0;
};

// The 8 CV outputs. The output stages hold only the output under test; the
// input stages hold all 8 (each at its own code). ReleaseAll lets go of
// everything this port holds (the glue may track its own holds
// so that it does not release outputs held by someone else).
struct OutputPort {
  virtual void Hold(int out, uint16_t code) = 0;
  virtual void ReleaseAll() = 0;
  // The stored calibration table's entry k (k * 1200 mV), the iteration's start.
  virtual uint16_t TableCode(int out, int k) const = 0;
  // The DAC code for mv through the normal pitch-to-code path, but on `table`
  // (the 11 entries being verified) instead of the stored one.
  virtual uint16_t CodeForMv(int out, const uint16_t table[CalRoutineMath::kNodes],
                             int32_t mv) const = 0;
};

struct Clock {
  virtual uint32_t Ms() const = 0;  // wraps; the flow only ever subtracts
};

typedef void (*LogFn)(const char *line);  // one line, no line ending

// ---- timings and limits of the meter rule ------------------------------------

constexpr uint32_t kOutSettleMs = 150;     // after every DAC code change
// The measurement rule (design 9.1), one place for every target that measures
// a held output with the meter (MeterMeasure): settle kOutSettleMs after the
// code change, one throwaway READ? (the conversion in progress at the change
// spans the step and is never used), then kReadings READ? -> Summarize
// (median, spread, trusted <= 0.5 mV; one re-read set, then NOISY). A node's
// final value takes kFinalReadings instead.
constexpr int kReadings = 3;               // READ? per measurement -> Summarize
constexpr int kFinalReadings = 10;         // a node's final value, after the iteration settles
// One READ? at NPLC 10 with autozero ON is a real conversion and takes ~0.53 s
// on the SDM3065X; 3 s is only the give-up point.
constexpr uint32_t kReadTimeoutMs = 3000;
constexpr uint32_t kWatchdogMs = 500;      // past the timeout a busy Poll counts as failed
constexpr int kQueryRetries = 3;           // a failed query is retried this often
constexpr uint32_t kRetryWaitMs = 200;     // ... after this wait
constexpr uint32_t kBeginGiveUpMs = 5000;  // Begin() refused for this long: failed

// The meter setup, in order (all on UsbTmc::ScpiAllowed's list): DC volts on
// the fixed 20 V range, NPLC 10 with autozero ON (below NPLC 10 the SDM3065X
// returns a smoothed, stale stream; bench 2026-10-24), immediate trigger, and
// last kErrorQuery. Every target that measures with the meter sends it first
// (and CalRoutineFlow again after every reconnect).
//
// None of the setup lines has a reply, so a meter that rejected one would
// say so only in its error queue: the last line reads it (*CLS emptied it
// first), and the run fails unless it is empty (UsbTmc::NoScpiError). With
// autozero on, every reading zeroes itself: there is no per-output re-zero
// and no per-output error query. Defined in CalRoutineMeasure.cpp.
constexpr uint32_t kConfigTimeoutMs = 1000;
extern const char *const kMeterSetup[6];
extern const char *const kErrorQuery;  // "SYST:ERR?"

}  // namespace CalRoutine

#endif  // CALROUTINETYPES_H_
