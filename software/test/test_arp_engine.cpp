// Host tests for the arpeggiator's note-order engine (src/ArpEngine.h).
//
// The engine is deliberately clock-agnostic and allocation-free: it turns a
// set of held notes plus a parameter block into a pitch sequence, and then
// hands out one step at a time. Everything stochastic goes through a seeded
// PRNG so a test can pin it.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <set>

#include "../src/ArpEngine.h"

using namespace arp;

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

// Held notes as played, deliberately NOT in pitch order: the ordered modes
// must sort for themselves, and As-Played must not.
static std::vector<Note> played3() { return {{64,100},{60,90},{67,110}}; }

static std::vector<uint8_t> seq(const std::vector<Note>& n, Params p) {
  Note out[kMaxSeq];
  uint8_t len = BuildSequence(n.data(), (uint8_t)n.size(), p, out, kMaxSeq);
  std::vector<uint8_t> v;
  for (uint8_t i = 0; i < len; ++i) v.push_back(out[i].pitch);
  return v;
}

static void test_up_sorts_ascending() {
  Params p; p.order = ORDER_UP;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{60,64,67}));
}

static void test_down_sorts_descending() {
  Params p; p.order = ORDER_DOWN;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{67,64,60}));
}

// As-Played must preserve the order the notes arrived in. This is the one
// mode where sorting would be a bug.
static void test_as_played_preserves_arrival_order() {
  Params p; p.order = ORDER_AS_PLAYED;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{64,60,67}));
}

// Exclusive up-down does not repeat the turning points: 1 2 3 2.
static void test_updown_exclusive_does_not_repeat_endpoints() {
  Params p; p.order = ORDER_UPDOWN;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{60,64,67,64}));
}

// Inclusive repeats them: 1 2 3 3 2 1.
static void test_updown_inclusive_repeats_endpoints() {
  Params p; p.order = ORDER_UPDOWN_INC;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{60,64,67,67,64,60}));
}

static void test_downup_exclusive() {
  Params p; p.order = ORDER_DOWNUP;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{67,64,60,64}));
}

// Converge walks outside-in; diverge inside-out. Four notes makes the
// pattern unambiguous.
static void test_converge_and_diverge() {
  std::vector<Note> n{{60,100},{64,100},{67,100},{72,100}};
  Params p; p.order = ORDER_CONVERGE;
  CHECK((seq(n, p) == std::vector<uint8_t>{60,72,64,67}));
  p.order = ORDER_DIVERGE;
  CHECK((seq(n, p) == std::vector<uint8_t>{67,64,72,60}));
}

// Octaves are octave-major by default: the whole figure, then again up 12.
static void test_octaves_are_octave_major() {
  Params p; p.order = ORDER_UP; p.octaves = 2;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{60,64,67,72,76,79}));
}

// Note-major plays each note through every octave before moving on. This is
// the musically different one, not a re-spelling of the same thing.
static void test_octave_alt_is_note_major() {
  Params p; p.order = ORDER_UP; p.octaves = 2; p.oct_mode = OCT_ALT;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{60,72,64,76,67,79}));
}

static void test_octave_down_starts_high() {
  Params p; p.order = ORDER_UP; p.octaves = 2; p.oct_mode = OCT_DOWN;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{72,76,79,60,64,67}));
}

static void test_transpose_shifts_every_note() {
  Params p; p.order = ORDER_UP; p.transpose = -12;
  CHECK((seq(played3(), p) == std::vector<uint8_t>{48,52,55}));
}

// Octaves plus transpose can run off the top of MIDI. Clamping, not
// wrapping: a wrapped note is a wrong note in a different octave.
static void test_pitch_is_clamped_not_wrapped() {
  std::vector<Note> n{{120,100}};
  Params p; p.order = ORDER_UP; p.octaves = 4; p.transpose = 12;
  for (uint8_t v : seq(n, p)) CHECK(v <= 127);
  Params q; q.order = ORDER_UP; q.transpose = -127;
  for (uint8_t v : seq({{5,100}}, q)) CHECK(v <= 127);  // unsigned: never wraps high
}

static void test_no_notes_yields_empty_sequence() {
  Params p;
  CHECK(seq({}, p).empty());
}

static void test_single_note_modes_do_not_duplicate_absurdly() {
  std::vector<Note> n{{60,100}};
  Params p; p.order = ORDER_UPDOWN;
  CHECK((seq(n, p) == std::vector<uint8_t>{60}));      // nothing to turn around
  p.order = ORDER_UPDOWN_INC;
  CHECK((seq(n, p) == std::vector<uint8_t>{60,60}));   // inclusive still doubles
}

static void test_sequence_never_overflows_its_buffer() {
  std::vector<Note> n;
  for (int i = 0; i < kMaxNotes; ++i) n.push_back({(uint8_t)(48+i),100});
  for (uint8_t o = 0; o < ORDER_COUNT; ++o) {
    Params p; p.order = o; p.octaves = kMaxOct; p.oct_mode = OCT_UPDOWN;
    Note out[kMaxSeq];
    uint8_t len = BuildSequence(n.data(), (uint8_t)n.size(), p, out, kMaxSeq);
    CHECK(len <= kMaxSeq);
  }
}

// --- stepping, probability, ratchets ---

static void test_steps_walk_the_sequence_and_wrap() {
  Engine e; e.Reset(1);
  Params p; p.order = ORDER_UP;
  auto n = played3();
  std::vector<uint8_t> got;
  for (int i = 0; i < 7; ++i) got.push_back(e.Next(n.data(), 3, p).pitch);
  CHECK((got == std::vector<uint8_t>{60,64,67,60,64,67,60}));
}

static void test_skip_zero_never_rests() {
  Engine e; e.Reset(7);
  Params p; p.order = ORDER_UP; p.skip_pct = 0;
  auto n = played3();
  for (int i = 0; i < 200; ++i) CHECK(!e.Next(n.data(), 3, p).rest);
}

static void test_skip_hundred_always_rests() {
  Engine e; e.Reset(7);
  Params p; p.order = ORDER_UP; p.skip_pct = 100;
  auto n = played3();
  for (int i = 0; i < 200; ++i) CHECK(e.Next(n.data(), 3, p).rest);
}

// A rest must still advance the position, or a skipped step would stall the
// pattern instead of leaving a hole in it.
static void test_a_rest_still_advances_the_pattern() {
  Engine e; e.Reset(3);
  Params p; p.order = ORDER_UP; p.skip_pct = 100;
  auto n = played3();
  e.Next(n.data(), 3, p); e.Next(n.data(), 3, p);
  p.skip_pct = 0;
  CHECK(e.Next(n.data(), 3, p).pitch == 67);   // third note, not the first
}

static void test_skip_is_roughly_the_requested_rate() {
  Engine e; e.Reset(12345);
  Params p; p.order = ORDER_UP; p.skip_pct = 50;
  auto n = played3();
  int rests = 0;
  for (int i = 0; i < 4000; ++i) if (e.Next(n.data(), 3, p).rest) rests++;
  CHECK(rests > 1700 && rests < 2300);
}

static void test_ratchets_are_bounded_and_off_by_default() {
  Engine e; e.Reset(5);
  Params p; p.order = ORDER_UP;
  auto n = played3();
  for (int i = 0; i < 100; ++i) CHECK(e.Next(n.data(), 3, p).repeats == 1);
  Engine f; f.Reset(5);
  Params q; q.order = ORDER_UP; q.ratchet_pct = 100; q.ratchet_max = 4;
  bool saw_more = false;
  for (int i = 0; i < 200; ++i) {
    uint8_t r = f.Next(n.data(), 3, q).repeats;
    CHECK(r >= 1 && r <= 4);
    if (r > 1) saw_more = true;
  }
  CHECK(saw_more);
}

static void test_velocity_follows_the_played_note_unless_fixed() {
  Engine e; e.Reset(1);
  Params p; p.order = ORDER_UP;
  auto n = played3();                 // 60->90, 64->100, 67->110
  CHECK(e.Next(n.data(), 3, p).velocity == 90);
  CHECK(e.Next(n.data(), 3, p).velocity == 100);
  Engine f; f.Reset(1);
  Params q; q.order = ORDER_UP; q.vel_fixed = 77;
  CHECK(f.Next(n.data(), 3, q).velocity == 77);
}

// Changing the held chord mid-pattern must not index past the new, shorter
// sequence -- the classic arpeggiator crash.
static void test_shrinking_the_chord_mid_pattern_is_safe() {
  Engine e; e.Reset(1);
  Params p; p.order = ORDER_UP; p.octaves = 4;
  std::vector<Note> big;
  for (int i = 0; i < 8; ++i) big.push_back({(uint8_t)(48+i),100});
  for (int i = 0; i < 20; ++i) e.Next(big.data(), 8, p);
  std::vector<Note> small{{60,100}};
  for (int i = 0; i < 5; ++i) {
    Step s = e.Next(small.data(), 1, p);
    CHECK(s.pitch >= 60 && s.pitch <= 60 + 36);
  }
}

static void test_random_order_stays_in_the_held_set() {
  Engine e; e.Reset(99);
  Params p; p.order = ORDER_RANDOM;
  auto n = played3();
  std::set<uint8_t> allowed{60,64,67};
  for (int i = 0; i < 300; ++i) {
    Step s = e.Next(n.data(), 3, p);
    CHECK(allowed.count(s.pitch) == 1);
  }
}

static void test_reset_makes_the_sequence_reproducible() {
  Params p; p.order = ORDER_UP; p.skip_pct = 40; p.ratchet_pct = 40;
  auto n = played3();
  std::vector<int> a, b;
  Engine e; e.Reset(2024);
  for (int i = 0; i < 50; ++i) { Step s = e.Next(n.data(),3,p); a.push_back(s.rest?-1:s.pitch*10+s.repeats); }
  Engine f; f.Reset(2024);
  for (int i = 0; i < 50; ++i) { Step s = f.Next(n.data(),3,p); b.push_back(s.rest?-1:s.pitch*10+s.repeats); }
  CHECK(a == b);
}

int main() {
  test_up_sorts_ascending();
  test_down_sorts_descending();
  test_as_played_preserves_arrival_order();
  test_updown_exclusive_does_not_repeat_endpoints();
  test_updown_inclusive_repeats_endpoints();
  test_downup_exclusive();
  test_converge_and_diverge();
  test_octaves_are_octave_major();
  test_octave_alt_is_note_major();
  test_octave_down_starts_high();
  test_transpose_shifts_every_note();
  test_pitch_is_clamped_not_wrapped();
  test_no_notes_yields_empty_sequence();
  test_single_note_modes_do_not_duplicate_absurdly();
  test_sequence_never_overflows_its_buffer();
  test_steps_walk_the_sequence_and_wrap();
  test_skip_zero_never_rests();
  test_skip_hundred_always_rests();
  test_a_rest_still_advances_the_pattern();
  test_skip_is_roughly_the_requested_rate();
  test_ratchets_are_bounded_and_off_by_default();
  test_velocity_follows_the_played_note_unless_fixed();
  test_shrinking_the_chord_mid_pattern_is_safe();
  test_random_order_stays_in_the_held_set();
  test_reset_makes_the_sequence_reproducible();
  printf("test_arp_engine: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
