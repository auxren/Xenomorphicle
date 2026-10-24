#ifndef SCOPERANGE_H_
#define SCOPERANGE_H_
// ---------------------------------------------------------------------------
// The Scope's timebase and volts tables, and the damped auto-ranging that
// picks from them. Pure; host-tested in test/test_scope_range.cpp.
//
// The screen is 8 divisions of 16 columns across and 8 divisions down the
// plot. One ring bucket (ScopeRing.h) is one column, so the timebase sets
// each source's decimation.
//
// Damping is the point (spec: "no hunting"). A change needs its target to
// have held for a full second, and then moves one step for time or straight
// to the target for volts. The one exception is clipping: a signal that no
// longer fits coarsens volts on the very next update.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace ScopeRange {

constexpr int kColsPerDiv = 16;
constexpr int kDivsX = 8;
constexpr int kDivsY = 8;

constexpr int kTimeCount = 14;
extern const uint32_t kTimeDivUs[kTimeCount];
constexpr int kVoltCount = 9;
extern const uint16_t kVoltDivMv[kVoltCount];

constexpr int kDefaultTimeIdx = 3;  // 1 ms/div
constexpr int kDefaultVoltIdx = 6;  // 1 V/div
constexpr int kAutoSlowestNoPeriod = 10;  // 200 ms/div

int ClampTime(int idx);
int ClampVolt(int idx);

// Samples per bucket at `rate_hz`; at least 1.
uint16_t DecimFor(int time_idx, float rate_hz);
// Columns one sample spans: above 1 only when a sample outlasts a column,
// and the trace is then stretched.
float ColsPerBucket(int time_idx, float rate_hz);

void FormatTime(int idx, char *buf, size_t n);  // "100us", "1ms", "2s"
void FormatVolt(int idx, char *buf, size_t n);  // "10mV", "1V"

class AutoTime {
public:
  void Reset(int idx);
  // period_s <= 0: no period found.
  int Update(float period_s, uint32_t now_ms);
  int Idx() const { return idx_; }

private:
  int idx_ = kDefaultTimeIdx;
  bool pending_ = false;
  uint32_t pending_since_ = 0;
};

class AutoVolt {
public:
  void Reset(int idx);
  int Update(int32_t lo_mv, int32_t hi_mv, uint32_t now_ms);
  int Idx() const { return idx_; }
  int32_t CenterMv() const { return center_mv_; }

private:
  int idx_ = kDefaultVoltIdx;
  int32_t center_mv_ = 0;
  bool pending_ = false;
  uint32_t pending_since_ = 0;
  bool center_pending_ = false;
  uint32_t center_since_ = 0;
};

}  // namespace ScopeRange

#endif  // SCOPERANGE_H_
