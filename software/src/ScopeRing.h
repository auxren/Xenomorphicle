#ifndef SCOPERING_H_
#define SCOPERING_H_
// ---------------------------------------------------------------------------
// The Scope's capture rings. Header-only and ISR-safe: each ring has exactly
// one writer (the ISR that owns its source -- audio update(), the ADC frame
// hook, the core tick or a TR pin interrupt) and loop-context readers that
// tolerate a torn bucket, since a display frame is redrawn a moment later.
// No FLASHMEM, no allocation, bounded work per call.
//
// ScopeRing decimates: every `decim` pushes become one committed bucket
// holding their min and max, so one bucket is one screen column at the
// chosen timebase and a 2 s/div view costs the same memory as 1 ms/div.
// Buckets are addressed by absolute number (0 = first after Reset); the
// newest kLen stay readable.
// ---------------------------------------------------------------------------

#include <stdint.h>

struct ScopeBucket {
  int16_t lo, hi;
};

class ScopeRing {
public:
  static constexpr int kLen = 1024;  // power of two
  static_assert((kLen & (kLen - 1)) == 0, "kLen must be a power of two");

  void Reset(uint16_t decim) {
    decim_ = decim ? decim : 1;
    open_n_ = 0;
    open_lo_ = INT16_MAX;
    open_hi_ = INT16_MIN;
    committed_ = 0;
    commit_cyc = 0;
  }

  void Push(int16_t s) {
    if (s < open_lo_) open_lo_ = s;
    if (s > open_hi_) open_hi_ = s;
    if (++open_n_ < decim_) return;
    const uint32_t c = committed_;
    buf_[c & (kLen - 1)].lo = open_lo_;
    buf_[c & (kLen - 1)].hi = open_hi_;
    committed_ = c + 1;
    open_n_ = 0;
    open_lo_ = INT16_MAX;
    open_hi_ = INT16_MIN;
  }

  uint32_t Committed() const { return committed_; }
  ScopeBucket At(uint32_t bucket_no) const {
    const volatile ScopeBucket &b = buf_[bucket_no & (kLen - 1)];
    return ScopeBucket{b.lo, b.hi};
  }
  uint16_t Decim() const { return decim_; }

  // ARM_DWT_CYCCNT of the newest sample of the newest committed bucket, set
  // by the writer; lets the reader place buckets in time across sources.
  volatile uint32_t commit_cyc = 0;

private:
  volatile ScopeBucket buf_[kLen] = {};
  uint16_t decim_ = 1;
  uint16_t open_n_ = 0;
  int16_t open_lo_ = INT16_MAX;
  int16_t open_hi_ = INT16_MIN;
  volatile uint32_t committed_ = 0;
};

// Rising-edge timestamps (ARM_DWT_CYCCNT) for one TR jack, written from its
// pin interrupt. Edges closer than min_gap_cyc to the previous stored one are
// dropped, which bounds what an audio-rate clock patched into TR can cost.
class ScopeEdgeRing {
public:
  static constexpr int kLen = 64;  // power of two

  void Reset() {
    count_ = 0;
    last_ = 0;
  }

  void Note(uint32_t cyc, uint32_t min_gap_cyc) {
    const uint32_t n = count_;
    if (n && (uint32_t)(cyc - last_) < min_gap_cyc) return;
    buf_[n & (kLen - 1)] = cyc;
    last_ = cyc;
    count_ = n + 1;
  }

  uint32_t Count() const { return count_; }
  uint32_t At(uint32_t n) const { return buf_[n & (kLen - 1)]; }

private:
  volatile uint32_t buf_[kLen] = {};
  volatile uint32_t count_ = 0;
  uint32_t last_ = 0;
};

#endif  // SCOPERING_H_
