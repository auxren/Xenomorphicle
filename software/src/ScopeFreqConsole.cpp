// The console's frequency counter commands. See the header.
#if defined(__IMXRT1062__)
#include <Arduino.h>
#define FREQ_CON_CODE FLASHMEM
#else
#define FREQ_CON_CODE
#endif

// No jump tables: on the Teensy build the linker folds .rodata into RAM1 data,
// and a few switch tables here would push the next 4 KB alignment boundary.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-jump-tables")
#endif

#include "ScopeFreqConsole.h"

#include <stdio.h>

namespace ScopeFreqConsole {

namespace {
Action Make(Kind k, int ppm10 = 0) {
  Action a;
  a.kind = k;
  a.ppm10 = ppm10;
  return a;
}
}  // namespace

FREQ_CON_CODE Action Parser::Feed(int c, uint32_t now_ms) {
  if (state_ != IDLE && (uint32_t)(now_ms - last_ms_) > kTimeoutMs) state_ = IDLE;
  if (state_ == DRAIN) {
    if ((uint32_t)(now_ms - last_ms_) > kDrainMs) state_ = IDLE;
    else { last_ms_ = now_ms; return Make(Kind::PENDING); }
  }
  if (state_ == IDLE) {
    if (c != kPrefix) return Action();
    state_ = PREFIX;
    last_ms_ = now_ms;
    return Make(Kind::PENDING);
  }
  last_ms_ = now_ms;
  switch (state_) {
    case PREFIX:
      if (c == 'r') { state_ = IDLE; return Make(Kind::READ); }
      if (c == 'c') { state_ = CLOCK_SIGN; return Make(Kind::PENDING); }
      if (c == 'g') { state_ = GATE; return Make(Kind::PENDING); }
      break;
    case GATE:
      if (c == 'f') { state_ = IDLE; return Make(Kind::GATE_FAST); }
      if (c == 's') { state_ = IDLE; return Make(Kind::GATE_SLOW); }
      break;
    case CLOCK_SIGN:
      if (c == 'x') { state_ = IDLE; return Make(Kind::CLOCK_CLEAR); }
      if (c == '+' || c == '-') {
        neg_ = c == '-';
        n_ = 0;
        value_ = 0;
        state_ = CLOCK_DIGITS;
        return Make(Kind::PENDING);
      }
      break;
    case CLOCK_DIGITS:
      // "dddd.d": positions 0-3 digits, 4 the point, 5 the tenth
      if (n_ == 4) {
        if (c != '.') break;
        ++n_;
        return Make(Kind::PENDING);
      }
      if (c < '0' || c > '9') break;
      value_ = value_ * 10 + (c - '0');
      if (n_ < 4) {
        ++n_;
        return Make(Kind::PENDING);
      }
      // the tenth: done
      state_ = IDLE;
      if (value_ > kMaxPpm10) return Make(Kind::RANGE);
      return Make(Kind::CLOCK_SET, neg_ ? -value_ : value_);
    default:
      break;
  }
  state_ = DRAIN;
  return Make(Kind::CANCEL);
}

FREQ_CON_CODE int FormatClockReply(char *buf, size_t n, int ppm10, bool calibrated) {
  if (!buf || n == 0) return 0;
  const int a = ppm10 < 0 ? -ppm10 : ppm10;
  return snprintf(buf, n, "SFREQ,CLOCK,%c%d.%d,%s,unsaved", ppm10 < 0 ? '-' : '+', a / 10, a % 10,
                  calibrated ? "SET" : "UNCAL");
}

FREQ_CON_CODE const char *CancelLine() { return "SFREQ,ERR,cancelled"; }
FREQ_CON_CODE const char *RangeLine() { return "SFREQ,ERR,RANGE"; }

FREQ_CON_CODE int FormatGateReply(char *buf, size_t n, bool fast) {
  if (!buf || n == 0) return 0;
  return snprintf(buf, n, "SFREQ,GATE,%s", fast ? "FAST" : "SLOW");
}

}  // namespace ScopeFreqConsole
