#pragma once
// Arpeggiator note-order engine.
//
// Pure, allocation-free and clock-agnostic: it turns a set of held notes plus
// a parameter block into a pitch sequence, then hands out one step at a time
// when something else decides a step is due. That separation is deliberate --
// the app drives it from an internal tempo, incoming MIDI clock or a trigger
// jack, and none of that belongs in the note logic. It also makes the whole
// thing host-testable (test/test_arp_engine.cpp).
//
// Everything stochastic goes through a seeded xorshift so a pattern can be
// reproduced exactly, on the bench and in a test.

#include <stdint.h>

namespace arp {

// Matches NoteBuffer::kCapacity (HSMIDITypes.h): the MIDI layer tracks at
// most this many held notes per channel, so a longer chord cannot reach us.
static constexpr uint8_t kMaxNotes = 8;
static constexpr uint8_t kMaxOct = 4;
// Worst case: an inclusive up-down doubles the note count (16), and an
// up-down octave traversal of 4 octaves visits 6 octave positions. 16 * 6.
static constexpr uint8_t kMaxSeq = 96;

enum Order : uint8_t {
  ORDER_UP, ORDER_DOWN,
  ORDER_UPDOWN, ORDER_DOWNUP,            // turning points played once
  ORDER_UPDOWN_INC, ORDER_DOWNUP_INC,    // turning points repeated
  ORDER_CONVERGE, ORDER_DIVERGE,
  ORDER_AS_PLAYED, ORDER_RANDOM,
  ORDER_COUNT
};

enum OctMode : uint8_t {
  OCT_UP,      // whole figure, then the whole figure an octave up
  OCT_DOWN,    // highest octave first
  OCT_UPDOWN,  // up through the octaves and back down
  OCT_ALT,     // each note through every octave before the next note
  OCT_COUNT
};

struct Note { uint8_t pitch; uint8_t velocity; };

struct Params {
  uint8_t order = ORDER_UP;
  uint8_t octaves = 1;        // 1..kMaxOct
  uint8_t oct_mode = OCT_UP;
  int8_t  transpose = 0;      // semitones
  uint8_t skip_pct = 0;       // chance a step is a rest
  uint8_t ratchet_max = 1;    // upper bound on repeats, 1..4
  uint8_t ratchet_pct = 0;    // chance a step ratchets at all
  uint8_t vel_fixed = 0;      // 0 = inherit the played note's velocity
};

struct Step {
  bool    rest;
  uint8_t pitch;
  uint8_t velocity;
  uint8_t repeats;            // 1 normally, >1 when the step ratchets
};

namespace detail {

inline void SortAscending(Note* n, uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {     // insertion sort: count <= 8
    Note k = n[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && n[j].pitch > k.pitch) { n[j + 1] = n[j]; --j; }
    n[j + 1] = k;
  }
}

// The base figure as indices into the (already ordered) note array. Returned
// as indices rather than notes so the octave expansion below can replay the
// same figure at each octave without re-deriving it.
inline uint8_t BaseFigure(uint8_t order, uint8_t n, uint8_t* idx, uint8_t cap) {
  uint8_t len = 0;
  auto put = [&](uint8_t v) { if (len < cap) idx[len++] = v; };
  if (n == 0) return 0;
  switch (order) {
    case ORDER_DOWN:
      for (int8_t i = (int8_t)n - 1; i >= 0; --i) put((uint8_t)i);
      break;
    case ORDER_UPDOWN:
      for (uint8_t i = 0; i < n; ++i) put(i);
      // Turning points played once: stop one short at each end. With a single
      // note there is nothing to turn around, so this adds nothing.
      for (int8_t i = (int8_t)n - 2; i >= 1; --i) put((uint8_t)i);
      break;
    case ORDER_UPDOWN_INC:
      for (uint8_t i = 0; i < n; ++i) put(i);
      for (int8_t i = (int8_t)n - 1; i >= 0; --i) put((uint8_t)i);
      break;
    case ORDER_DOWNUP:
      for (int8_t i = (int8_t)n - 1; i >= 0; --i) put((uint8_t)i);
      for (uint8_t i = 1; i + 1 < n; ++i) put(i);
      break;
    case ORDER_DOWNUP_INC:
      for (int8_t i = (int8_t)n - 1; i >= 0; --i) put((uint8_t)i);
      for (uint8_t i = 0; i < n; ++i) put(i);
      break;
    case ORDER_CONVERGE: {                  // outside in: lowest, highest, ...
      uint8_t lo = 0, hi = n - 1;
      while (lo <= hi) {
        put(lo);
        if (lo != hi) put(hi);
        ++lo; if (hi == 0) break; --hi;
      }
      break;
    }
    case ORDER_DIVERGE: {                   // inside out: converge, reversed
      uint8_t tmp[kMaxNotes * 2];
      uint8_t m = BaseFigure(ORDER_CONVERGE, n, tmp, (uint8_t)(kMaxNotes * 2));
      for (int8_t i = (int8_t)m - 1; i >= 0; --i) put(tmp[i]);
      break;
    }
    default:                                // UP, AS_PLAYED, RANDOM
      for (uint8_t i = 0; i < n; ++i) put(i);
      break;
  }
  return len;
}

inline uint8_t OctaveOrder(uint8_t mode, uint8_t octaves, uint8_t* out) {
  if (octaves < 1) octaves = 1;
  if (octaves > kMaxOct) octaves = kMaxOct;
  uint8_t len = 0;
  switch (mode) {
    case OCT_DOWN:
      for (int8_t o = (int8_t)octaves - 1; o >= 0; --o) out[len++] = (uint8_t)o;
      break;
    case OCT_UPDOWN:
      for (uint8_t o = 0; o < octaves; ++o) out[len++] = o;
      for (int8_t o = (int8_t)octaves - 2; o >= 1; --o) out[len++] = (uint8_t)o;
      break;
    default:                                 // OCT_UP and OCT_ALT
      for (uint8_t o = 0; o < octaves; ++o) out[len++] = o;
      break;
  }
  return len;
}

inline uint8_t ClampPitch(int v) { return (uint8_t)(v < 0 ? 0 : (v > 127 ? 127 : v)); }

}  // namespace detail

// Build the full pitch sequence. Returns its length, 0 when nothing is held.
// Never writes past out_cap.
inline uint8_t BuildSequence(const Note* notes, uint8_t count, const Params& p,
                             Note* out, uint8_t out_cap) {
  if (!notes || count == 0 || out_cap == 0) return 0;
  if (count > kMaxNotes) count = kMaxNotes;

  Note ordered[kMaxNotes];
  for (uint8_t i = 0; i < count; ++i) ordered[i] = notes[i];
  // As-played is the one mode that must NOT sort: the arrival order is the
  // musical information. Random picks at step time, so its base order is
  // irrelevant, and sorting keeps it consistent with the rest.
  if (p.order != ORDER_AS_PLAYED) detail::SortAscending(ordered, count);

  uint8_t figure[kMaxNotes * 2];
  const uint8_t flen = detail::BaseFigure(p.order, count, figure,
                                          (uint8_t)(kMaxNotes * 2));
  uint8_t octs[kMaxOct * 2];
  const uint8_t olen = detail::OctaveOrder(p.oct_mode, p.octaves, octs);

  uint8_t len = 0;
  auto emit = [&](uint8_t note_idx, uint8_t oct) {
    if (len >= out_cap) return;
    const Note& src = ordered[note_idx];
    out[len].pitch = detail::ClampPitch((int)src.pitch + 12 * (int)oct + p.transpose);
    out[len].velocity = src.velocity;
    ++len;
  };

  if (p.oct_mode == OCT_ALT) {
    // Note-major: each note through every octave before moving on. This is
    // musically a different figure, not a re-spelling of octave-major.
    for (uint8_t f = 0; f < flen; ++f)
      for (uint8_t o = 0; o < olen; ++o) emit(figure[f], octs[o]);
  } else {
    for (uint8_t o = 0; o < olen; ++o)
      for (uint8_t f = 0; f < flen; ++f) emit(figure[f], octs[o]);
  }
  return len;
}

class Engine {
 public:
  void Reset(uint32_t seed = 0x2545F491u) {
    pos_ = 0;
    rng_ = seed ? seed : 1u;
  }

  // One step. The held set is passed in every time rather than cached, so a
  // chord change takes effect immediately and a shrinking chord cannot leave
  // the position pointing past the end.
  Step Next(const Note* notes, uint8_t count, const Params& p) {
    Note seq[kMaxSeq];
    const uint8_t len = BuildSequence(notes, count, p, seq, kMaxSeq);
    if (len == 0) { pos_ = 0; return Step{true, 0, 0, 1}; }

    if (pos_ >= len) pos_ %= len;
    const uint8_t at = (p.order == ORDER_RANDOM) ? (uint8_t)(Rand() % len) : pos_;

    Step s;
    s.rest = (p.skip_pct > 0) && ((Rand() % 100u) < p.skip_pct);
    s.pitch = seq[at].pitch;
    s.velocity = p.vel_fixed ? p.vel_fixed : seq[at].velocity;
    s.repeats = 1;
    if (p.ratchet_pct > 0 && p.ratchet_max > 1 && (Rand() % 100u) < p.ratchet_pct) {
      s.repeats = (uint8_t)(1 + Rand() % p.ratchet_max);
    }

    // A rest advances the pattern like any other step: skipping should punch
    // a hole in the figure, not stall it.
    pos_ = (uint8_t)((pos_ + 1) % len);
    return s;
  }

  uint8_t position() const { return pos_; }

 private:
  uint32_t Rand() {                          // xorshift32
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return rng_;
  }
  uint8_t  pos_ = 0;
  uint32_t rng_ = 0x2545F491u;
};

}  // namespace arp
