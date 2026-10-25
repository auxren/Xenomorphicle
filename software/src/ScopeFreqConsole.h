#ifndef SCOPEFREQCONSOLE_H_
#define SCOPEFREQCONSOLE_H_
// ---------------------------------------------------------------------------
// The bench console's frequency counter commands. Pure byte parser, host-tested
// in test/test_scope_freq_console.cpp; Main.cpp feeds it every console byte and
// prints the answers.
//
// Every bare letter of the console is taken (some of them dangerous on an
// unlocked console), so these hang off one punctuation prefix that no other
// command uses:
//
//   %r            read:   SFREQ,<src>,<hz>,<sigma_cents>,<status>,<gate_ms>,<flags>,<seq>,<cal>
//   %c+0012.3     set the crystal correction, ppm of the sample rate above
//   %c-0012.3     nominal: sign, FOUR digits, '.', ONE digit, nothing after it
//   %cx           forget the correction (uncalibrated)
//   %gf / %gs     gate: fast (0.1 s) or slow (1 s)
//
// No Enter anywhere: a command ends with its last character. Any byte that
// does not belong cancels the command AND is swallowed, so a stray '%' followed
// by a real command letter never runs that command; and the bytes that follow it
// within 50 ms (the rest of what was typed or sent in one burst: '.' is a
// broadcast recall, '(' a save) are swallowed too. A pending prefix expires
// after 3 s, and the next byte is then an ordinary command byte again.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace ScopeFreqConsole {

constexpr char kPrefix = '%';
constexpr uint32_t kTimeoutMs = 3000;
constexpr uint32_t kDrainMs = 50;   // after a cancel: bytes closer together than this are swallowed
constexpr int kMaxPpm10 = 10000;   // +-1000.0 ppm

enum class Kind : uint8_t {
  NONE,         // not ours: the caller handles the byte
  PENDING,      // consumed, command not complete
  READ,
  CLOCK_SET,    // ppm10 valid
  CLOCK_CLEAR,
  GATE_FAST,
  GATE_SLOW,
  CANCEL,       // consumed; the command was malformed
  RANGE,        // consumed; well formed but outside +-1000.0 ppm
};

struct Action {
  Kind kind = Kind::NONE;
  int ppm10 = 0;
  bool consumed() const { return kind != Kind::NONE; }
};

class Parser {
public:
  // Every console byte, with the time. NONE means "not part of a command".
  Action Feed(int c, uint32_t now_ms);
  bool Pending() const { return state_ != IDLE && state_ != DRAIN; }   // a command is half typed
  bool Draining() const { return state_ == DRAIN; }
  void Reset() { state_ = IDLE; }

private:
  enum State : uint8_t { IDLE, PREFIX, CLOCK_SIGN, CLOCK_DIGITS, GATE, DRAIN };
  State state_ = IDLE;
  uint32_t last_ms_ = 0;
  bool neg_ = false;
  uint8_t n_ = 0;     // digits taken
  int value_ = 0;     // tenths
};

// The lines the commands answer with (no line ending).
int FormatClockReply(char *buf, size_t n, int ppm10, bool calibrated);   // SFREQ,CLOCK,+12.3,SET|UNCAL,unsaved
const char *CancelLine();   // SFREQ,ERR,cancelled
const char *RangeLine();    // SFREQ,ERR,RANGE
int FormatGateReply(char *buf, size_t n, bool fast);   // SFREQ,GATE,FAST|SLOW

}  // namespace ScopeFreqConsole

#endif  // SCOPEFREQCONSOLE_H_
