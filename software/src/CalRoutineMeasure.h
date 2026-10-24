#ifndef CALROUTINEMEASURE_H_
#define CALROUTINEMEASURE_H_

#include <stddef.h>
#include <stdint.h>

#include "CalRoutineMath.h"

// ---------------------------------------------------------------------------
// The Cal Routine's meter rule, moved out of CalRoutineFlow unchanged so that
// every target that measures a held output with the USB DMM uses the same one:
//   MeterQuery    one SCPI query on the meter, retried (kQueryRetries after
//                 kRetryWaitMs, Begin() refused up to kBeginGiveUpMs, a
//                 kWatchdogMs grace past the query's own timeout).
//   MeterMeasure  one measurement of a held output: hold the code, settle
//                 kOutSettleMs, one throwaway READ?, then `readings` READ? ->
//                 CalRoutineMath::Summarize (median, spread, trusted <= 0.5
//                 mV); one re-read set when untrusted, then NOISY.
// Both are driven by the caller's Tick: time is passed in (the caller's one
// per-tick sample of its clock), nothing here reads a clock or blocks.
// The ports (MeterPort, OutputPort) are declared in CalRoutineFlow.h.
// ---------------------------------------------------------------------------

namespace CalRoutine {

struct MeterPort;
struct OutputPort;

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

// ---- one meter query, retried ------------------------------------------------

class MeterQuery {
 public:
  enum Result : uint8_t { BUSY, DONE, FAIL, LOST };

  void Bind(MeterPort &meter, LogFn log) { meter_ = &meter; log_ = log; }
  void SetLog(LogFn log) { log_ = log; }
  void Start(const char *scpi, uint32_t timeout_ms, uint32_t now);
  Result Step(uint32_t now);
  // Counts a failed attempt (garbled reply): retried within the same budget,
  // BUSY while retries are left. MeterMeasure uses it for an unparsable READ?.
  Result Failure(uint32_t now);
  void Reset() { phase_ = NONE; }
  const char *scpi() const { return scpi_; }    // the query last started
  const char *reply() const { return reply_; }  // valid after DONE

 private:
  enum Phase : uint8_t { NONE, BEGIN, BEGIN_WAIT, WAIT, BUSY_PHASE };
  void Log(const char *line);

  MeterPort *meter_ = nullptr;
  LogFn log_ = nullptr;
  const char *scpi_ = nullptr;
  uint32_t timeout_ = 0;
  uint32_t t0_ = 0;        // phase start
  uint32_t begin_ms_ = 0;  // first Begin attempt of this try
  int fails_ = 0;
  Phase phase_ = NONE;
  const char *reply_ = nullptr;
};

// ---- one measurement: settle, throwaway READ?, readings, median and spread -----

class MeterMeasure {
 public:
  // MeterMeasure::Result maps 1:1 onto CalRoutineFlow's old MResult.
  enum Result : uint8_t { BUSY, DONE, NOISY, FAIL, LOST };

  void Bind(MeterPort &meter, OutputPort &out, LogFn log);
  void SetLog(LogFn log) { log_ = log; query_.SetLog(log); }
  // Holds `code` on output `out` (the port holds; ReleaseAll stays the caller's)
  // and starts a measurement of it; `target_mv` is only for the re-read log line.
  void Start(int out, uint16_t code, int32_t target_mv, uint32_t now,
             int readings = kReadings);
  Result Step(uint32_t now);
  const CalRoutineMath::Reading &reading() const { return reading_; }  // valid after DONE
  void Reset() { phase_ = NONE; query_.Reset(); }

 private:
  enum Phase : uint8_t { NONE, SETTLE, READ };  // READ: the throwaway, then the readings
  void Log(const char *line);

  OutputPort *out_ = nullptr;
  LogFn log_ = nullptr;
  MeterQuery query_;
  Phase phase_ = NONE;
  uint32_t t0_ = 0;
  int n_ = 0;
  int want_ = kReadings;
  bool throw_ = false;      // the throwaway READ? is in flight
  bool reread_ = false;
  int out_idx_ = 0;
  uint16_t code_ = 0;
  int32_t target_ = 0;
  double mv_[kFinalReadings];
  CalRoutineMath::Reading reading_ = {0.0, 0.0, 0};
};

}  // namespace CalRoutine

#endif  // CALROUTINEMEASURE_H_
