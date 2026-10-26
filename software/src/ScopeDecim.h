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
// filter (11 taps; -42 dB at 0.4 of its input rate, passband flat to 0.1): the
// band that is finally shown is small against its rate. The last stage has the
// long one (47 taps, -80 dB from 0.3 of its input rate, flat to 0.2), because the
// shown band is 0.8 of the final Nyquist (0.2 of the last stage's input rate) and
// everything that would fold into it comes from 0.3 and up. Cost per input
// sample, all stages together: about 8 multiply-accumulates.
// ---------------------------------------------------------------------------

#include <stdint.h>

namespace ScopeDecim {

constexpr int kMaxStages = 6;           // /64
constexpr int kRingLen = 1024;          // = the FFT length
// One slot more than the FFT needs: the ISR writes the slot for sample n BEFORE it bumps the
// count, and with exactly kRingLen slots that slot is the oldest sample of the window being
// read. With a spare slot a push in progress never touches the window.
constexpr int kRingCap = kRingLen + 1;   // (the index wraps once per 2^32 decimated samples: one bad frame in 4 days at /4)

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
      else { sbuf_[s][spos_[s]] = v; spos_[s] = (uint8_t)((spos_[s] + 1) & 15); }
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
  // 11-tap halfband {3,0,-25,0,150,256,150,0,-25,0,3}/512 in Q15.
  __attribute__((always_inline)) inline int16_t Short(int s) const {
    const int16_t *b = sbuf_[s];
    const unsigned p = spos_[s];
    const int32_t acc = 192 * (Tap(b, p, 15, 0) + Tap(b, p, 15, 10)) - 1600 * (Tap(b, p, 15, 2) + Tap(b, p, 15, 8)) +
                        9600 * (Tap(b, p, 15, 4) + Tap(b, p, 15, 6)) + 16384 * Tap(b, p, 15, 5);
    return Clamp16((acc + 16384) >> 15);
  }
  // 47-tap Kaiser (beta 7) halfband, Q15: centre 16382, odd taps below (sum is 32768).
  __attribute__((always_inline)) inline int16_t Long() const {
    static const int16_t kC[12] = {10367, -3290, 1787, -1096, 693, -433, 261, -148, 77, -35, 13, -3};
    const unsigned p = lpos_;
    int32_t acc = 16382 * Tap(lbuf_, p, 63, 23);
    for (int j = 0; j < 12; ++j) {
      const int d = 2 * j + 1;
      acc += kC[j] * (Tap(lbuf_, p, 63, 23 - d) + Tap(lbuf_, p, 63, 23 + d));
    }
    return Clamp16((acc + 16384) >> 15);
  }

  int16_t sbuf_[kMaxStages - 1][16] = {};
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
  void Publish() { count_ = count_ + 1; }
  void Push(const int16_t *x, int n) { for (int i = 0; i < n; ++i) Push(x[i]); }
  // Decimated samples produced so far; the FFT needs kRingLen of them.
  uint32_t Count() const { return count_; }
  bool Full() const { return count_ >= (uint32_t)kRingLen; }
  // Copies the newest kRingLen samples, oldest first, through f(i, sample). The producer is an
  // ISR that may push at any moment, so a copy that overlapped a
  // push (the count moved) is thrown away and retried (at most 4 times; false if it never came
  // out clean, or if the ring is not yet full). A push in progress cannot tear it (spare slot). f must be idempotent: a retry calls it again for every i.
  template <class F> bool Read(F f) const {
    for (int attempt = 0; attempt < 4; ++attempt) {
      const uint32_t c0 = count_;
      if (c0 < (uint32_t)kRingLen) return false;
      for (int i = 0; i < kRingLen; ++i) f(i, (int16_t)ring_[(c0 - (uint32_t)kRingLen + (uint32_t)i) % kRingCap]);
      if (count_ == c0) return true;
    }
    return false;
  }

private:
  Cascade casc_;
  volatile int16_t ring_[kRingCap] = {};
  volatile uint32_t count_ = 0;
};

}  // namespace ScopeDecim

#endif  // SCOPEDECIM_H_
