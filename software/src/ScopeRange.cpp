// The Scope's timebase/volts tables and auto-ranging. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define SCOPE_CODE FLASHMEM
#else
#define SCOPE_CODE
#endif

#include "ScopeRange.h"

#include <math.h>
#include <stdio.h>

namespace ScopeRange {

const uint32_t kTimeDivUs[kTimeCount] = {100,    200,    500,    1000,   2000,
                                         5000,   10000,  20000,  50000,  100000,
                                         200000, 500000, 1000000, 2000000};
const uint16_t kVoltDivMv[kVoltCount] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000};

namespace {

constexpr uint32_t kHoldMs = 1000;

int TimeTarget(float period_s) {
  for (int i = 0; i < kTimeCount; ++i)
    if (kDivsX * (kTimeDivUs[i] * 1e-6f) >= 2.5f * period_s) return i;
  return kTimeCount - 1;
}

int VoltTarget(int32_t span_mv) {
  for (int i = 0; i < kVoltCount; ++i)
    if ((int32_t)kVoltDivMv[i] * kDivsY * 4 >= span_mv * 5) return i;  // 8 div >= 1.25 x span
  return kVoltCount - 1;
}

}  // namespace

SCOPE_CODE int ClampTime(int idx) { return idx < 0 ? 0 : idx >= kTimeCount ? kTimeCount - 1 : idx; }
SCOPE_CODE int ClampVolt(int idx) { return idx < 0 ? 0 : idx >= kVoltCount ? kVoltCount - 1 : idx; }

SCOPE_CODE uint16_t DecimFor(int time_idx, float rate_hz) {
  const double bucket_s = kTimeDivUs[ClampTime(time_idx)] * 1e-6 / kColsPerDiv;
  const long d = lround(bucket_s * rate_hz);
  if (d < 1) return 1;
  if (d > 65535) return 65535;
  return (uint16_t)d;
}

SCOPE_CODE float ColsPerBucket(int time_idx, float rate_hz) {
  if (DecimFor(time_idx, rate_hz) > 1 || rate_hz <= 0) return 1.f;
  const float col_s = kTimeDivUs[ClampTime(time_idx)] * 1e-6f / kColsPerDiv;
  const float c = (1.f / rate_hz) / col_s;
  return c > 1.f ? c : 1.f;
}

SCOPE_CODE void FormatTime(int idx, char *buf, size_t n) {
  if (!buf || !n) return;
  const uint32_t us = kTimeDivUs[ClampTime(idx)];
  if (us < 1000) snprintf(buf, n, "%luus", (unsigned long)us);
  else if (us < 1000000) snprintf(buf, n, "%lums", (unsigned long)(us / 1000));
  else snprintf(buf, n, "%lus", (unsigned long)(us / 1000000));
}

SCOPE_CODE void FormatVolt(int idx, char *buf, size_t n) {
  if (!buf || !n) return;
  const unsigned mv = kVoltDivMv[ClampVolt(idx)];
  if (mv < 1000) snprintf(buf, n, "%umV", mv);
  else snprintf(buf, n, "%uV", mv / 1000);
}

SCOPE_CODE void AutoTime::Reset(int idx) {
  idx_ = ClampTime(idx);
  pending_ = false;
}

SCOPE_CODE int AutoTime::Update(float period_s, uint32_t now_ms) {
  int target;
  if (period_s > 0) target = TimeTarget(period_s);
  else target = idx_ < kAutoSlowestNoPeriod ? idx_ + 1 : idx_;
  if (target == idx_) {
    pending_ = false;
    return idx_;
  }
  if (!pending_) {
    pending_ = true;
    pending_since_ = now_ms;
  } else if (now_ms - pending_since_ >= kHoldMs) {
    idx_ += target > idx_ ? 1 : -1;
    pending_since_ = now_ms;  // the next step waits another full second
  }
  return idx_;
}

SCOPE_CODE void AutoVolt::Reset(int idx) {
  idx_ = ClampVolt(idx);
  center_mv_ = 0;
  pending_ = false;
  center_pending_ = false;
}

SCOPE_CODE int AutoVolt::Update(int32_t lo_mv, int32_t hi_mv, uint32_t now_ms) {
  if (hi_mv < lo_mv) { const int32_t t = lo_mv; lo_mv = hi_mv; hi_mv = t; }
  const int32_t span = hi_mv - lo_mv;
  if (span < 1) {  // flat: nothing to range on, hold everything
    pending_ = false;
    center_pending_ = false;
    return idx_;
  }
  const int32_t mid = lo_mv + span / 2;
  const int32_t half = (int32_t)kVoltDivMv[idx_] * kDivsY / 2;
  const int target = VoltTarget(span);

  // Fast attack: anything outside the plot coarsens and re-centres now.
  if (hi_mv > center_mv_ + half || lo_mv < center_mv_ - half) {
    if (target > idx_) idx_ = target;
    center_mv_ = mid;
    pending_ = false;
    pending_since_ = now_ms;
    center_pending_ = false;
    return idx_;
  }

  // Slow release: a finer range must be wanted for a full second.
  if (target < idx_) {
    if (!pending_) {
      pending_ = true;
      pending_since_ = now_ms;
    } else if (now_ms - pending_since_ >= kHoldMs) {
      idx_ = target;
      center_mv_ = mid;
      pending_ = false;
      pending_since_ = now_ms;
      center_pending_ = false;
    }
  } else {
    pending_ = false;
  }

  // The centre follows a drifted signal on the same one-second hold.
  const int32_t off = mid - center_mv_;
  if ((off < 0 ? -off : off) > (int32_t)kVoltDivMv[idx_]) {
    if (!center_pending_) {
      center_pending_ = true;
      center_since_ = now_ms;
    } else if (now_ms - center_since_ >= kHoldMs) {
      center_mv_ = mid;
      center_pending_ = false;
    }
  } else {
    center_pending_ = false;
  }
  return idx_;
}

}  // namespace ScopeRange
