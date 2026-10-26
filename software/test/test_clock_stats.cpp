// Host tests for the free-running counters behind the console's %t
// (src/ClockStats.cpp): the 64-bit extension and its wrap handling, and the
// integer-only STIME line with large 64-bit values.
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/ClockStats.h"

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

using namespace ClockStats;

static constexpr uint32_t kCpm = 600000;   // cycles per millisecond

// A fake chip: true 64-bit time in cycles; the registers are its low 32 bits.
struct Chip {
  uint64_t cyc = 0;
  uint32_t blocks = 0, frames = 0;
  void AdvanceMs(uint32_t ms) { cyc += (uint64_t)ms * kCpm; }
  Raw Read(uint32_t ms_jitter_cycles = 0) const {
    Raw r;
    const uint64_t c = cyc + ms_jitter_cycles;
    r.cyc = (uint32_t)c;
    r.ms = (uint32_t)(cyc / kCpm);
    r.blocks = blocks;
    r.frames = frames;
    return r;
  }
};

static void test_dec() {
  printf("test_dec\n");
  char b[24];
  CHECK(U64ToDec(b, 0) == 1 && std::string(b) == "0");
  CHECK(U64ToDec(b, 9) == 1 && std::string(b) == "9");
  CHECK(U64ToDec(b, 10) == 2 && std::string(b) == "10");
  CHECK(std::string((U64ToDec(b, 4294967295ull), b)) == "4294967295");
  CHECK(std::string((U64ToDec(b, 4294967296ull), b)) == "4294967296");
  CHECK(U64ToDec(b, 18446744073709551615ull) == 20 && std::string(b) == "18446744073709551615");
  CHECK(std::string((U64ToDec(b, 10000000000000000000ull), b)) == "10000000000000000000");
  CHECK(std::string((U64ToDec(b, 9223372036854775808ull), b)) == "9223372036854775808");
}

static void test_format() {
  printf("test_format\n");
  Stamp s;
  s.cycles = 18446744073709551615ull;
  s.audio = 9223372036854775808ull;
  s.cv = 4294967296ull;
  s.seq = 4294967295u;
  char b[128];
  int n = FormatStime(b, sizeof(b), s);
  CHECK(std::string(b) == "STIME,18446744073709551615,9223372036854775808,4294967296,4294967295");
  CHECK(n == (int)std::strlen(b));
  Stamp z;
  FormatStime(b, sizeof(b), z);
  CHECK(std::string(b) == "STIME,0,0,0,0");
  // longest possible line fits the console's 112-byte buffer
  CHECK(n < 112);
  // truncation: NUL-terminated, returns the full length like snprintf
  char t[10];
  int full = FormatStime(t, sizeof(t), s);
  CHECK(full == n && std::strlen(t) == 9 && std::string(t) == "STIME,184");
  char one[1] = {'x'};
  CHECK(FormatStime(one, 1, s) == n && one[0] == 0);
  CHECK(FormatStime(nullptr, 0, s) == n);   // size query
  // realistic: 90 minutes at 600 MHz
  Stamp r;
  r.cycles = 3240000000000ull; r.audio = 259200000ull; r.cv = 201693229ull; r.seq = 1800;
  FormatStime(b, sizeof(b), r);
  CHECK(std::string(b) == "STIME,3240000000000,259200000,201693229,1800");
}

static void test_cycle_wrap() {
  printf("test_cycle_wrap\n");
  Chip chip;
  Clock clk(kCpm);
  // sampled every 2 s for 120 s: crosses 16 wraps of the 32-bit counter
  uint64_t prev = 0;
  for (int i = 0; i < 60; ++i) {
    chip.AdvanceMs(2000);
    Stamp s = clk.Sample(chip.Read());
    CHECK(s.cycles == chip.cyc);
    CHECK(s.cycles >= prev);
    prev = s.cycles;
  }
  // the exact wrap: one read just below 2^32, the next just above
  Chip c3;
  Clock k3(kCpm);
  c3.cyc = 4294967296ull - 500ull * kCpm;
  Stamp a = k3.Sample(c3.Read());
  c3.AdvanceMs(1000);
  Stamp bb = k3.Sample(c3.Read());
  CHECK(bb.cycles - a.cycles == 1000ull * kCpm);   // 600e6 cycles across the wrap
}

static void test_long_gaps() {
  printf("test_long_gaps\n");
  // gaps between two reads from 7 s to 4 h (stalled loop, slow host); the code's limit is the
  // 49.7 days of millis(), checked below by the millis wrap case
  const uint32_t gaps_ms[] = {7000, 7200, 20000, 45000, 60000, 3600000, 4u * 3600000u};
  for (uint32_t g : gaps_ms) {
    Chip chip;
    Clock clk(kCpm);
    clk.Sample(chip.Read());
    chip.AdvanceMs(g);
    Stamp s = clk.Sample(chip.Read());
    CHECK(s.cycles == chip.cyc);
    if (s.cycles != chip.cyc) printf("  gap %u ms\n", g);
  }
  // clock skew between millis() and the cycle counter (up to 2 ms either way) must not
  // move the wrap count, including right at a wrap boundary
  for (uint32_t gap : {7158u, 7159u}) for (int skew = -2; skew <= 2; ++skew) {
    Chip chip;
    Clock clk(kCpm);
    chip.cyc = 4294967296ull - 3ull * kCpm;
    chip.cyc -= chip.cyc % kCpm;
    clk.Sample(chip.Read());
    chip.AdvanceMs(gap);   // 7158 ms: just short of a wrap; 7159 ms: just past it
    Raw r = chip.Read();
    r.ms += (uint32_t)skew;   // millis() reads 2 ms off
    Stamp s = clk.Sample(r);
    CHECK(s.cycles == chip.cyc);
  }
  // millis() itself wraps (49 days): the delta is modular
  Clock clk(kCpm);
  Raw r0; r0.cyc = 100; r0.ms = 0xFFFFFF00u;
  clk.Sample(r0);
  Raw r1; r1.cyc = 100 + 2 * kCpm; r1.ms = 0xFFFFFF00u + 2;   // wraps to 0xFFFFFF02: no wrap yet
  CHECK(clk.Sample(r1).cycles - 0 >= 2ull * kCpm);
  Raw r2; r2.cyc = r1.cyc + 5 * kCpm; r2.ms = r1.ms + 5;       // ms crosses zero here
  Stamp s2 = clk.Sample(r2);
  CHECK(s2.cycles - 0 >= 7ull * kCpm);
}

static void test_counters() {
  printf("test_counters\n");
  Chip chip;
  Clock clk(kCpm);
  Stamp s = clk.Sample(chip.Read());
  CHECK(s.audio == 0 && s.cv == 0 && s.seq == 0);
  chip.blocks += 375;     // one second of audio
  chip.frames += 37350;
  chip.AdvanceMs(1000);
  s = clk.Sample(chip.Read());
  CHECK(s.audio == 375ull * 128 && s.cv == 37350 && s.seq == 1);
  // 32-bit wrap of the ISR words
  chip.blocks = 0xFFFFFFF0u;
  chip.frames = 0xFFFFFF00u;
  chip.AdvanceMs(1000);
  clk.Sample(chip.Read());
  const Stamp before = clk.Sample(chip.Read());
  chip.blocks += 0x20;
  chip.frames += 0x200;
  chip.AdvanceMs(1000);
  s = clk.Sample(chip.Read());
  CHECK(s.audio - before.audio == 0x20ull * 128);
  CHECK(s.cv - before.cv == 0x200);
  CHECK(s.audio > 0xFFFFFFFFull);
  // seq counts replies and wraps at 2^32 (checked through the formatter)
  CHECK(clk.seq() == s.seq + 1);
}

static void test_monotonic_long_run() {
  printf("test_monotonic_long_run\n");
  // 90 minutes, one read every 2-5 s (a deterministic mix), fixed rates: the 64-bit values
  // grow exactly with the true counts
  Chip chip;
  Clock clk(kCpm);
  uint64_t blocks64 = 0, frames64 = 0;
  uint32_t lcg = 12345;
  uint64_t elapsed = 0;
  Stamp last;
  int n = 0;
  while (elapsed < 90ull * 60 * 1000) {
    lcg = lcg * 1664525u + 1013904223u;
    const uint32_t gap = 2000 + (lcg >> 16) % 3001;
    chip.AdvanceMs(gap);
    elapsed += gap;
    const uint32_t db = gap * 375 / 1000, df = gap * 37350 / 1000;
    chip.blocks += db; chip.frames += df;
    blocks64 += db; frames64 += df;
    last = clk.Sample(chip.Read());
    ++n;
    if (last.cycles != chip.cyc || last.audio != blocks64 * 128 || last.cv != frames64) {
      CHECK(false);
      break;
    }
  }
  CHECK(n > 1000 && last.seq == (uint32_t)(n - 1));
  CHECK(last.cycles > 3000000000000ull);   // far beyond 2^32
}

int main() {
  test_dec();
  test_format();
  test_cycle_wrap();
  test_long_gaps();
  test_counters();
  test_monotonic_long_run();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
