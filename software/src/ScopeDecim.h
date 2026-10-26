#ifndef SCOPEDECIM_H_
#define SCOPEDECIM_H_
// ---------------------------------------------------------------------------
// The spectrum view's zoom: a streaming decimator, 2 to 64 times, that fills a
// small ring with the low-passed, down-sampled input, so a 1024-point FFT of the
// ring has 4 to 64 times finer bins than the capture rate allows. Pure integer
// code, host-tested in test/test_scope_decim.cpp; fed from the capture path
// (ScopeCapture.cpp) one input sample at a time, read from loop context.
//
// A cascade of halfband /2 FIR stages (DC gain exactly 1, int16 in, int16 out,
// Q15 coefficients, 32-bit accumulation). Every stage but the last has a short
// filter (19 taps; at least -74 dB from 0.42 of its input rate and a null at 0.4,
// passband flat to 0.1): the band that is finally shown is small against its rate,
// but a near-full-scale tone anywhere above 0.4 of the input rate folds into it, and
// the plot reaches -90 dB (the first version's 11-tap filter, -42 dB there, drew a
// 19.5 kHz tone at 21 px). The last stage has the long one (55 taps, -81 dB from 0.3 of
// its input rate, flat to 0.2), because the shown band is 0.8 of the final Nyquist (0.2 of
// the last stage's input rate) and everything that would fold into it comes from 0.3 and up.
// ---------------------------------------------------------------------------

#include <stdint.h>

namespace ScopeDecim {

constexpr int kMaxStages = 6;           // /64
constexpr int kRingLen = 1024;          // = the FFT length
// One slot more than the FFT needs: the ISR writes the slot for sample n BEFORE it bumps the
// count, and with exactly kRingLen slots that slot is the oldest sample of the window being
// read. With a spare slot a push in progress never touches the window.
constexpr int kReadAttempts = 4;
constexpr int kRingCap = kRingLen + 1;   // (the index wraps once per 2^32 decimated samples: at /4 about every 4 days, one snapshot refused for a few ms, then it is right again; nothing stays wrong)

inline int16_t Clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

class Cascade {
public:
  // stages 1..kMaxStages (0 = pass-through). Clears all state.
  void Reset(int stages) {
    n_ = stages < 0 ? 0 : stages > kMaxStages ? kMaxStages : (uint8_t)stages;
    for (auto &b : sbuf_) for (auto &v : b) v = 0;
    for (auto &v : lbuf_) v = 0;
    for (auto &p : spos_) p = 0;
    lpos_ = 0;
    for (auto &p : phase_) p = 0;
  }
  int stages() const { return n_; }

  // One input sample; true when an output sample was produced (every 2^stages-th).
  bool Push(int16_t x, int16_t *y) {
    int16_t v = x;
    for (int s = 0; s < n_; ++s) {
      const bool last = s == n_ - 1;
      if (last) { lbuf_[lpos_] = v; lpos_ = (uint8_t)((lpos_ + 1) & 63); }
      else { sbuf_[s][spos_[s]] = v; spos_[s] = (uint8_t)((spos_[s] + 1) & 31); }
      phase_[s] ^= 1;
      if (phase_[s]) return false;
      v = last ? Long() : Short(s);
    }
    *y = v;
    return true;
  }

private:
  // x_k is the sample k pushes old in a power-of-two ring whose next write position is p.
  static inline __attribute__((always_inline)) int32_t Tap(const int16_t *b, unsigned p, unsigned mask, int k) {
    return b[(p - 1u - (unsigned)k) & mask];
  }
  // Both filters are written out tap by tap with literal coefficients: a loop over a table was
  // measured at ~14 instructions per iteration (not unrolled, table reloaded from the stack).
#define SCOPEDECIM_PAIR(b, p, mask, centre, c, d) \
  acc += (c) * (Tap((b), (p), (mask), (centre) - (d)) + Tap((b), (p), (mask), (centre) + (d)))

  // 19-tap Kaiser (beta 7) halfband, Q15: centre 16386, odd taps below (the sum is 32768).
  // Flat to 0.1 of its input rate, -74 dB or better from 0.42 (a -122 dB null at 0.4).
  __attribute__((always_inline)) inline int16_t Short(int s) const {
    const int16_t *b = sbuf_[s];
    const unsigned p = spos_[s];
    int32_t acc = 16386 * Tap(b, p, 31, 9);
    SCOPEDECIM_PAIR(b, p, 31, 9, 10022, 1);
    SCOPEDECIM_PAIR(b, p, 31, 9, -2403, 3);
    SCOPEDECIM_PAIR(b, p, 31, 9, 706, 5);
    SCOPEDECIM_PAIR(b, p, 31, 9, -141, 7);
    SCOPEDECIM_PAIR(b, p, 31, 9, 7, 9);
    return Clamp16((acc + 16384) >> 15);
  }
  // 55-tap Kaiser (beta 9) halfband, Q15: centre 16384, odd taps below (the sum is 32768).
  // Flat to 0.2 of its input rate (within 0.001 dB), at least -81 dB from 0.30: the shown band
  // is 0.8 of the final Nyquist = 0.2 of this stage's input rate, and a tone at 0.30 folds to
  // exactly its top edge (the previous 47-tap filter was only -69 dB between 0.300 and 0.305).
  __attribute__((always_inline)) inline int16_t Long() const {
    const unsigned p = lpos_;
    int32_t acc = 16384 * Tap(lbuf_, p, 63, 27);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 10378, 1);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -3322, 3);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 1837, 5);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -1160, 7);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 763, 9);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -504, 11);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 327, 13);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -206, 15);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 124, 17);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -70, 19);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 37, 21);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -17, 23);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 7, 25);
    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -2, 27);
    return Clamp16((acc + 16384) >> 15);
  }
#undef SCOPEDECIM_PAIR

  int16_t sbuf_[kMaxStages - 1][32] = {};
  int16_t lbuf_[64] = {};
  uint8_t spos_[kMaxStages - 1] = {};
  uint8_t lpos_ = 0;
  uint8_t phase_[kMaxStages] = {};
  uint8_t n_ = 0;
};

// The cascade plus the ring the FFT reads: the last kRingLen decimated samples.
class Feed {
public:
  void Reset(int stages) {
    casc_.Reset(stages);
    count_ = 0;
  }
  int stages() const { return casc_.stages(); }
  void Push(int16_t x) {
    int16_t y;
    if (casc_.Push(x, &y)) {
      WriteSlot(y);
      Publish();
    }
  }
  // The two halves of storing a decimated sample, apart so a test can stop between them the way
  // an interrupt can stop a reader: the slot is written first, the count bumped after.
  void WriteSlot(int16_t y) { ring_[count_ % kRingCap] = y; }
  // (the fences are free on the Cortex-M7, where reader and ISR share one core; they matter on a
  // host test with two real threads and on any weakly ordered machine)
  void Publish() {
    __atomic_thread_fence(__ATOMIC_RELEASE);   // the slot is visible before the count that covers it
    count_ = count_ + 1;
  }
  void Push(const int16_t *x, int n) { for (int i = 0; i < n; ++i) Push(x[i]); }
  // Decimated samples produced so far; the FFT needs kRingLen of them.
  uint32_t Count() const { return count_; }
  bool Full() const { return count_ >= (uint32_t)kRingLen; }
  // Copies the newest kRingLen RAW samples, oldest first, into dst (kRingLen int16). The producer
  // is an ISR that may push at any moment, so a copy during which the count moved is thrown
  // away and retried (kReadAttempts times; false if it never came out clean, or if the ring is not
  // yet full). A push in progress cannot tear it (the spare slot). The copy is two plain loops
  // on purpose (a few instructions a sample in this header, about 9 in the caller's flash-resident
  // build): do any conversion AFTER it, on dst, or the
  // window for a push grows from microseconds to over a hundred (every attempt would lose).
  // `between(attempt)` runs after each copy and before the check: a test's stand-in for the ISR.
  template <class H> bool Snapshot(int16_t *dst, H between) const {
    for (int attempt = 0; attempt < kReadAttempts; ++attempt) {
      const uint32_t c0 = count_;
      if (c0 < (uint32_t)kRingLen) return false;
      const uint32_t start = (c0 - (uint32_t)kRingLen) % kRingCap;   // slot of the oldest sample
      const int first = (int)(kRingCap - start) < kRingLen ? (int)(kRingCap - start) : kRingLen;
      for (int i = 0; i < first; ++i) dst[i] = ring_[start + (uint32_t)i];
      for (int i = first; i < kRingLen; ++i) dst[i] = ring_[i - first];
      between(attempt);
      __atomic_thread_fence(__ATOMIC_ACQUIRE);   // every slot above is read before the count below
      if (count_ == c0) return true;
    }
    return false;
  }
  bool Snapshot(int16_t *dst) const { return Snapshot(dst, [](int) {}); }

private:
  Cascade casc_;
  volatile int16_t ring_[kRingCap] = {};
  volatile uint32_t count_ = 0;
};

}  // namespace ScopeDecim

#endif  // SCOPEDECIM_H_
