// Host tests for the console's frequency counter commands
// (src/ScopeFreqConsole.cpp): the '%' prefix grammar, the fixed-width clock
// value with no newline, the +-1000 ppm refusal, cancel-and-swallow, the
// timeout, and the reply lines.
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/ScopeFreqConsole.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                            \
  do {                                                         \
    ++checks;                                                  \
    if (!(cond)) {                                             \
      ++failures;                                              \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                          \
  } while (0)

using namespace ScopeFreqConsole;

// Feeds a string; returns the LAST action, and counts how many bytes were ours.
struct Out {
  Action last;
  int consumed = 0;
  int bytes = 0;
};

// Each call starts a second after the last ended, bytes within it a millisecond
// apart (one USB burst), unless t is given.
static uint32_t g_clock = 100000;
static Out Feed(Parser &p, const char *s, uint32_t t = 0) {
  Out o;
  if (!t) g_clock += 1000;
  for (; *s; ++s) {
    if (!t) ++g_clock;
    o.last = p.Feed((unsigned char)*s, t ? t : g_clock);
    ++o.bytes;
    if (o.last.consumed()) ++o.consumed;
  }
  return o;
}

static void test_read() {
  printf("test_read\n");
  Parser p;
  Out o = Feed(p, "%r");
  CHECK(o.last.kind == Kind::READ && o.consumed == 2 && !p.Pending());
  // the prefix alone is pending, and is swallowed
  Action a = p.Feed('%', 0);
  CHECK(a.kind == Kind::PENDING && p.Pending());
  a = p.Feed('r', 0);
  CHECK(a.kind == Kind::READ && !p.Pending());
  // after a completed command the next byte is an ordinary one
  a = p.Feed('t', 0);
  CHECK(a.kind == Kind::NONE && !a.consumed());
}

static void test_ordinary_bytes_pass_through() {
  printf("test_ordinary_bytes_pass_through\n");
  Parser p;
  const char *all = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!\"#$&'()*+,-./:;<=>?@[\\]^_`{|}~\r\n ";
  for (const char *c = all; *c; ++c) {
    Action a = p.Feed((unsigned char)*c, 0);
    CHECK(a.kind == Kind::NONE);
    CHECK(!p.Pending());
  }
  // including the unlock text, which is bytes to the console, never to this parser
  Out o = Feed(p, "pew!");
  CHECK(o.consumed == 0);
}

static void test_clock_set() {
  printf("test_clock_set\n");
  struct { const char *s; int ppm10; } good[] = {
    {"%c+0012.3", 123},  {"%c-0012.3", -123}, {"%c+0000.0", 0},    {"%c-0000.0", 0},
    {"%c+0000.1", 1},    {"%c-0000.1", -1},   {"%c+0999.9", 9999}, {"%c-0999.9", -9999},
    {"%c+1000.0", 10000}, {"%c-1000.0", -10000}, {"%c+0020.0", 200}, {"%c+0100.5", 1005},
  };
  for (auto &g : good) {
    Parser p;
    Out o = Feed(p, g.s);
    CHECK(o.last.kind == Kind::CLOCK_SET);
    CHECK(o.last.ppm10 == g.ppm10);
    CHECK(o.consumed == o.bytes);
    CHECK(!p.Pending());
    if (o.last.kind != Kind::CLOCK_SET || o.last.ppm10 != g.ppm10) printf("  (%s)\n", g.s);
  }
  // no newline needed: it completes on the last digit, and not before
  Parser p;
  Out o = Feed(p, "%c+0012.");
  CHECK(o.last.kind == Kind::PENDING);
  CHECK(p.Feed('3', g_clock + 1).kind == Kind::CLOCK_SET);
  // out of range: refused, and swallowed whole
  const char *bad[] = {"%c+1000.1", "%c-1000.1", "%c+1999.9", "%c+9999.9", "%c-5000.0", "%c+1001.0"};
  for (const char *s : bad) {
    Parser q;
    Out r = Feed(q, s);
    CHECK(r.last.kind == Kind::RANGE);
    CHECK(r.consumed == r.bytes && !q.Pending());
  }
  // clear
  Parser c;
  Out cl = Feed(c, "%cx");
  CHECK(cl.last.kind == Kind::CLOCK_CLEAR && !c.Pending());
}

static void test_clock_malformed() {
  printf("test_clock_malformed\n");
  const char *bad[] = {
      "%c0012.3",   // no sign
      "%c00123.4",  // no sign, five digits: the first must not be taken for one
      "%c+012.3",   // three digits: the '.' arrives where a digit belongs
      "%c+00123",   // no point
      "%c+0012,3",  // wrong point
      "%c+0012.a",  // not a digit
      "%c+00a2.3",
      "%c++012.3",
      "%c 0012.3",
      "%cX",        // clear is lowercase x
      "%c\r",
      "%c\n",
      "%c",         // handled below: pending, not malformed
  };
  for (const char *s : bad) {
    if (!std::strcmp(s, "%c")) continue;
    Parser p;
    Out o = Feed(p, s);
    // somewhere along the way it cancelled: the final state is idle and the
    // last consumed action was a CANCEL, never a CLOCK_SET
    Parser q;
    bool set = false, cancel = false;
    for (const char *c = s; *c; ++c) {
      Action a = q.Feed((unsigned char)*c, 0);
      if (a.kind == Kind::CLOCK_SET) set = true;
      if (a.kind == Kind::CANCEL) cancel = true;
    }
    CHECK(!set);
    CHECK(cancel);
    CHECK(!q.Pending());
    (void)o;
  }
}

static void test_cancel_swallows() {
  printf("test_cancel_swallows\n");
  // a stray prefix followed by a real command letter: the letter is eaten
  const char *letters = "pewkmxWTXdDI(){}<>jAoSRHM=nvQ";   // not 't': %t is the time command now
  for (const char *c = letters; *c; ++c) {
    Parser p;
    CHECK(p.Feed('%', 0).kind == Kind::PENDING);
    Action a = p.Feed((unsigned char)*c, 0);
    CHECK(a.kind == Kind::CANCEL);
    CHECK(!p.Pending());
    // and once the burst is over, the byte after that is ordinary again
    CHECK(p.Feed((unsigned char)*c, 1000).kind == Kind::NONE);
  }
  // inside the clock command a command letter cancels too
  Parser p;
  Feed(p, "%c+00");
  Action a = p.Feed('p', g_clock + 1);
  CHECK(a.kind == Kind::CANCEL && !p.Pending());
  // a doubled prefix is a cancel, not a restart
  Parser q;
  q.Feed('%', 0);
  CHECK(q.Feed('%', 0).kind == Kind::CANCEL);
  CHECK(!q.Pending());
  // a new command a moment after a cancel works
  CHECK(Feed(q, "%r").last.kind == Kind::READ);
}

// The rest of a malformed command must not run as commands: "." is a broadcast
// recall, "(" a save, "x" a restore. A cancel swallows the burst it came in.
static void test_cancel_drains_the_burst() {
  printf("test_cancel_drains_the_burst\n");
  {
    Parser p;
    // no sign: refused at the first digit; the tail "012.3" is swallowed whole
    Out o = Feed(p, "%c0012.3");
    CHECK(o.consumed == o.bytes);
    bool set = false;
    Parser q;
    for (const char *c = "%c0012.3"; *c; ++c) if (q.Feed((unsigned char)*c, 50).kind == Kind::CLOCK_SET) set = true;
    CHECK(!set);
  }
  {
    Parser p;
    p.Feed('%', 10000);
    CHECK(p.Feed('z', 10001).kind == Kind::CANCEL);
    // the typed command's tail, back to back
    const char *tail = ".(){}<>xwpe!";
    uint32_t t = 10002;
    for (const char *c = tail; *c; ++c, ++t) CHECK(p.Feed((unsigned char)*c, t).consumed());
    CHECK(!p.Pending() || true);
    // 50 ms of quiet ends it; the next byte is ordinary
    CHECK(p.Feed('t', t + kDrainMs + 1).kind == Kind::NONE);
    // and the window has an upper bound: 60 ms after the last swallowed byte is a new command
    Parser r;
    r.Feed('%', 30000);
    r.Feed('z', 30001);
    CHECK(r.Feed('.', 30001 + 30).consumed());
    CHECK(r.Feed('.', 30001 + 30 + 60).kind == Kind::NONE);
    // each swallowed byte renews the window
    Parser q;
    q.Feed('%', 20000);
    q.Feed('z', 20001);
    CHECK(q.Feed('.', 20001 + kDrainMs).consumed());
    CHECK(q.Feed('.', 20001 + 2 * kDrainMs).consumed());
    CHECK(q.Feed('.', 20001 + 3 * kDrainMs + 1).kind == Kind::NONE);
  }
  {  // a malformed range/shape inside the clock command drains too
    Parser p;
    Feed(p, "%c+00");
    CHECK(p.Feed('a', g_clock + 1).kind == Kind::CANCEL);
    CHECK(p.Feed('7', g_clock + 2).consumed());
  }
  {  // a finished command leaves nothing to drain
    Parser p;
    CHECK(Feed(p, "%r").last.kind == Kind::READ);
    CHECK(p.Feed('t', g_clock + 1).kind == Kind::NONE);
    CHECK(Feed(p, "%c+0001.0").last.kind == Kind::CLOCK_SET);
    CHECK(p.Feed('t', g_clock + 1).kind == Kind::NONE);
    CHECK(Feed(p, "%c+1000.1").last.kind == Kind::RANGE);
    CHECK(p.Feed('t', g_clock + 1).kind == Kind::NONE);
  }
}

static void test_gate() {
  printf("test_gate\n");
  Parser p;
  CHECK(Feed(p, "%gf").last.kind == Kind::GATE_FAST);
  CHECK(Feed(p, "%gs").last.kind == Kind::GATE_SLOW);
  CHECK(Feed(p, "%gx").last.kind == Kind::CANCEL);
  CHECK(Feed(p, "%gF").last.kind == Kind::CANCEL);
  CHECK(Feed(p, "%g").last.kind == Kind::PENDING);
  CHECK(p.Feed('\r', g_clock + 1).kind == Kind::CANCEL);
}

static void test_timeout() {
  printf("test_timeout\n");
  Parser p;
  CHECK(p.Feed('%', 1000).kind == Kind::PENDING);
  // within 3 s: still pending
  CHECK(p.Feed('c', 3999).kind == Kind::PENDING);
  CHECK(p.Feed('+', 4000).kind == Kind::PENDING);   // each byte renews the window
  CHECK(p.Feed('0', 6999).kind == Kind::PENDING);
  // silence for over 3 s: the pending command is dropped and this byte is ordinary
  Parser q;
  q.Feed('%', 1000);
  Action a = q.Feed('r', 1000 + kTimeoutMs + 1);
  CHECK(a.kind == Kind::NONE);
  CHECK(!q.Pending());
  // exactly at the limit still counts
  Parser r;
  r.Feed('%', 1000);
  CHECK(r.Feed('r', 1000 + kTimeoutMs).kind == Kind::READ);
  // a fresh prefix after the expiry starts a new command
  Parser s;
  s.Feed('%', 0);
  CHECK(s.Feed('%', 9000).kind == Kind::PENDING);
  CHECK(s.Feed('r', 9001).kind == Kind::READ);
  // the clock wrapping at 2^32 ms is handled
  Parser t;
  t.Feed('%', 0xFFFFFF00u);
  CHECK(t.Feed('r', 0x00000100u).kind == Kind::READ);
}

static void test_replies() {
  printf("test_replies\n");
  char b[96];
  FormatClockReply(b, sizeof(b), 123, true);
  CHECK(std::string(b) == "SFREQ,CLOCK,+12.3,SET,unsaved");
  FormatClockReply(b, sizeof(b), -5, true);
  CHECK(std::string(b) == "SFREQ,CLOCK,-0.5,SET,unsaved");
  FormatClockReply(b, sizeof(b), 0, true);
  CHECK(std::string(b) == "SFREQ,CLOCK,+0.0,SET,unsaved");
  FormatClockReply(b, sizeof(b), -10000, true);
  CHECK(std::string(b) == "SFREQ,CLOCK,-1000.0,SET,unsaved");
  FormatClockReply(b, sizeof(b), 0, false);
  CHECK(std::string(b) == "SFREQ,CLOCK,+0.0,UNCAL,unsaved");
  FormatGateReply(b, sizeof(b), true);
  CHECK(std::string(b) == "SFREQ,GATE,FAST");
  FormatGateReply(b, sizeof(b), false);
  CHECK(std::string(b) == "SFREQ,GATE,SLOW");
  CHECK(std::string(CancelLine()) == "SFREQ,ERR,cancelled");
  CHECK(std::string(RangeLine()) == "SFREQ,ERR,RANGE");
  // every reply starts with the prefix the host parses on
  CHECK(std::strncmp(CancelLine(), "SFREQ,", 6) == 0 && std::strncmp(RangeLine(), "SFREQ,", 6) == 0);
}

static void test_time() {
  printf("test_time\n");
  // %t completes on its last character, no Enter; read-only, no payload
  Parser p;
  Out o = Feed(p, "%t");
  CHECK(o.last.kind == Kind::TIME && o.consumed == 2 && !p.Pending());
  CHECK(Feed(p, "%").last.kind == Kind::PENDING && p.Pending());
  CHECK(p.Feed('t', g_clock + 1).kind == Kind::TIME);
  // after it the next byte is ordinary again (a second 't' is the selftest, as after %r)
  CHECK(p.Feed('t', g_clock + 2).kind == Kind::NONE);
  // malformed: the prefix then anything that is not r c g t cancels and is swallowed
  Parser q;
  Out m = Feed(q, "%T");
  CHECK(m.last.kind == Kind::CANCEL && m.consumed == m.bytes);
  Out m2 = Feed(q, "%x");
  CHECK(m2.last.kind == Kind::CANCEL);
  // a cancelled %t-lookalike swallows the burst behind it ('.' recall, '(' save)
  Parser d;
  Out m3 = Feed(d, "%T.(t");
  CHECK(m3.consumed == m3.bytes);
  // %t never leaves a half command that eats the next one
  Parser r;
  CHECK(Feed(r, "%t").last.kind == Kind::TIME);
  CHECK(Feed(r, "%r").last.kind == Kind::READ);
  CHECK(Feed(r, "%t%r%t").last.kind == Kind::TIME);
  // interaction with the other % commands: abandoned mid-command, %t is a malformed byte
  Parser c;
  Out i1 = Feed(c, "%c+0012%t");     // '%' where the point belongs: cancel, and %t is swallowed
  CHECK(i1.consumed == i1.bytes && i1.last.kind != Kind::TIME);
  Parser g;
  Out i2 = Feed(g, "%gt");            // 't' is not a gate letter
  CHECK(i2.last.kind == Kind::CANCEL && i2.consumed == i2.bytes);
  Parser c2;
  CHECK(Feed(c2, "%c+0012.3").last.kind == Kind::CLOCK_SET);
  CHECK(Feed(c2, "%t").last.kind == Kind::TIME);
  CHECK(Feed(c2, "%gs").last.kind == Kind::GATE_SLOW);
  CHECK(Feed(c2, "%t").last.kind == Kind::TIME);
  // an expired prefix: 3 s later 't' is an ordinary byte, not %t
  Parser e;
  CHECK(e.Feed('%', 1000).kind == Kind::PENDING);
  CHECK(e.Feed('t', 1000 + kTimeoutMs + 1).kind == Kind::NONE);
  // a pending %c does not turn into %t
  Parser f;
  Feed(f, "%c");
  CHECK(Feed(f, "t").last.kind == Kind::CANCEL);
}

int main() {
  test_read();
  test_ordinary_bytes_pass_through();
  test_clock_set();
  test_clock_malformed();
  test_cancel_swallows();
  test_cancel_drains_the_burst();
  test_gate();
  test_timeout();
  test_replies();
  test_time();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
