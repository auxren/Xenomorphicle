// The Cal Routine's meter rule (query with retries, one measurement of a held
// output), moved out of CalRoutineFlow.cpp unchanged. See CalRoutineMeasure.h.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define CALR_CODE FLASHMEM
#else
#define CALR_CODE
#endif

#include "CalRoutineMeasure.h"

#include <stdio.h>

#include "CvMeterMath.h"
#include "UsbTmc.h"

namespace CM = CvMeterMath;
namespace CR = CalRoutineMath;
namespace L = CalRoutineLines;

// ---- the meter rule's lines ----------------------------------------------------
// Moved from CalRoutineFlow.cpp with the same output (CALR,RETRY and
// CALR,REREAD in test/fixtures/calr_lines.txt): fields appended with
// CvMeterMath::Append, which truncates cleanly at n.

namespace CalRoutineLines {

CALR_CODE void MeasField(char *buf, size_t n, size_t *pos, const char *s) {
  CM::Append(buf, n, pos, ",");
  CM::Append(buf, n, pos, s);
}

CALR_CODE int Retry(char *buf, size_t n, const char *scpi, int attempt) {
  if (!n) return 0;
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, "CALR,RETRY");
  MeasField(buf, n, &pos, scpi);
  char t[16];
  snprintf(t, sizeof(t), "%ld", long(attempt));
  MeasField(buf, n, &pos, t);
  return int(pos);
}

CALR_CODE int Reread(char *buf, size_t n, int out, int32_t target_mv, uint16_t code,
                     double spread_mv) {
  if (!n) return 0;
  size_t pos = 0;
  buf[0] = 0;
  CM::Append(buf, n, &pos, "CALR,REREAD");
  char t[32];
  snprintf(t, sizeof(t), "%ld", out >= 0 && out < 8 ? long(out + 1) : 0L);  // panel label, 0 = none
  MeasField(buf, n, &pos, t);
  snprintf(t, sizeof(t), "%ld", long(target_mv));
  MeasField(buf, n, &pos, t);
  snprintf(t, sizeof(t), "%lu", (unsigned long)code);
  MeasField(buf, n, &pos, t);
  CM::FormatFixed(t, sizeof(t), CM::ScaleRound(spread_mv, 3), 3);
  MeasField(buf, n, &pos, t);
  return int(pos);
}

}  // namespace CalRoutineLines

namespace CalRoutine {

const char *const kMeterSetup[6] = {
  "*CLS", "CONF:VOLT:DC 20", "VOLT:DC:NPLC 10", "VOLT:DC:AZ ON", "TRIG:SOUR IMM", "SYST:ERR?",
};
const char *const kErrorQuery = "SYST:ERR?";

#define CALR_LOG(fn, ...) do { \
    char line_[L::kMaxLine]; \
    L::fn(line_, sizeof(line_), __VA_ARGS__); \
    Log(line_); \
  } while (0)

CALR_CODE void MeterQuery::Log(const char *line) {
  if (log_) log_(line);
}

// ---- one meter query, retried ------------------------------------------------

CALR_CODE void MeterQuery::Start(const char *scpi, uint32_t timeout_ms, uint32_t now) {
  scpi_ = scpi;
  timeout_ = timeout_ms;
  fails_ = 0;
  phase_ = BEGIN;
  begin_ms_ = now;
}

CALR_CODE MeterQuery::Result MeterQuery::Failure(uint32_t now) {
  if (!meter_->Connected()) {
    phase_ = NONE;
    return LOST;
  }
  if (fails_ >= kQueryRetries) {
    phase_ = NONE;
    return FAIL;
  }
  ++fails_;
  CALR_LOG(Retry, scpi_, fails_);
  phase_ = WAIT;
  t0_ = now;
  return BUSY;
}

CALR_CODE MeterQuery::Result MeterQuery::Step(uint32_t now) {
  switch (phase_) {
    case NONE:
      return FAIL;
    case WAIT:  // after a failed attempt
      if (now - t0_ < kRetryWaitMs) return BUSY;
      begin_ms_ = now;
      phase_ = BEGIN;
      return BUSY;
    case BEGIN_WAIT:  // Begin() refused: the driver is busy or resynchronising
      if (now - t0_ < kRetryWaitMs) return BUSY;
      phase_ = BEGIN;
      return BUSY;
    case BEGIN:
      if (!meter_->Connected()) {
        phase_ = NONE;
        return LOST;
      }
      if (meter_->Begin(scpi_, timeout_)) {
        phase_ = BUSY_PHASE;
        t0_ = now;
        return BUSY;
      }
      if (now - begin_ms_ >= kBeginGiveUpMs) return Failure(now);
      phase_ = BEGIN_WAIT;
      t0_ = now;
      return BUSY;
    case BUSY_PHASE: {
      const char *reply = nullptr;
      const int r = meter_->Poll(&reply);
      if (r > 0) {
        phase_ = NONE;
        reply_ = reply ? reply : "";
        return DONE;
      }
      if (r == 0 && now - t0_ < timeout_ + kWatchdogMs) return BUSY;
      return Failure(now);
    }
  }
  return FAIL;
}

// ---- one measurement: settle, throwaway READ?, readings, median and spread -----

CALR_CODE void MeterMeasure::Log(const char *line) {
  if (log_) log_(line);
}

CALR_CODE void MeterMeasure::Bind(MeterPort &meter, OutputPort &out, LogFn log) {
  out_ = &out;
  log_ = log;
  query_.Bind(meter, log);
}

CALR_CODE void MeterMeasure::Start(int out, uint16_t code, int32_t target_mv, uint32_t now,
                                   int readings) {
  out_idx_ = out;
  out_->Hold(out, code);
  code_ = code;
  target_ = target_mv;
  t0_ = now;
  phase_ = SETTLE;
  n_ = 0;
  want_ = readings;
  throw_ = true;
  reread_ = false;
}

CALR_CODE MeterMeasure::Result MeterMeasure::Step(uint32_t now) {
  if (phase_ == SETTLE) {
    if (now - t0_ < kOutSettleMs) return BUSY;
    phase_ = READ;
    query_.Start("READ?", kReadTimeoutMs, now);
    return BUSY;
  }
  if (phase_ != READ) return FAIL;
  MeterQuery::Result r = query_.Step(now);
  if (r == MeterQuery::DONE) {
    double volts = 0.0;
    if (!UsbTmc::ParseReading(query_.reply(), &volts)) {
      r = query_.Failure(now);  // garbled: ask again, within the same retry budget
    } else {
      if (throw_) {  // the conversion in progress at the code change: never used
        throw_ = false;
        query_.Start("READ?", kReadTimeoutMs, now);
        return BUSY;
      }
      mv_[n_++] = volts * 1000.0;
      if (n_ < want_) {
        query_.Start("READ?", kReadTimeoutMs, now);
        return BUSY;
      }
      reading_ = CR::Summarize(mv_, n_);
      if (CR::ReadingTrusted(reading_)) {
        phase_ = NONE;
        return DONE;
      }
      if (!reread_) {  // one re-read; the untrusted readings are dropped
        reread_ = true;
        CALR_LOG(Reread, out_idx_, target_, code_, reading_.spread_mv);
        n_ = 0;
        query_.Start("READ?", kReadTimeoutMs, now);
        return BUSY;
      }
      phase_ = NONE;
      return NOISY;
    }
  }
  if (r == MeterQuery::BUSY) return BUSY;
  phase_ = NONE;
  return r == MeterQuery::LOST ? LOST : FAIL;
}

}  // namespace CalRoutine
