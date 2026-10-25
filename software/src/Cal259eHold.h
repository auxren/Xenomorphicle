#ifndef CAL259EHOLD_H_
#define CAL259EHOLD_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// The 259e write confirm's hold-to-write gate. A write to a 259e uses one of its
// 8 commits for good, and on the first real run the B press that wrote was made by
// mistake / by habit. So B must be HELD for kHoldMs, solo, on the confirm screen:
//   * nothing counts in the first kDeadMs after the screen appeared (a press that
//     was already in flight, or a double tap);
//   * the hold starts on the DOWN edge of B alone (no other button in the mask);
//   * releasing B early, a second key or encoder (any other event), or the app being
//     switched away (Reset) ends it: nothing is written and the screen says "hold B";
//   * it fires exactly once, when kHoldMs have passed with B still down.
// BSP-free (the clock and the pin come in as arguments), host-tested in
// test/test_cal259e_hold.cpp; apps/CalRoutine.h feeds it from the UI events and reads
// the raw pin (Ui::read_immediate) each Loop().
// ---------------------------------------------------------------------------
class Cal259eHold {
 public:
  static constexpr uint32_t kDeadMs = 350;
  static constexpr uint32_t kHoldMs = 2000;
  static constexpr uint32_t kBounceMs = 40;  // the raw pin must read released this long to count

  // The confirm screen appeared.
  void Arm(uint32_t now) { armed_ = now; Clear(); hint_ = false; }
  // Anything that ends the screen's life (stage change, app switch, cancel).
  void Reset() { Clear(); hint_ = false; }

  // B went down. `solo`: nothing else was down with it.
  void BDown(uint32_t now, bool solo) {
    if (held_) return;  // a repeated DOWN while one is open
    hint_ = false;
    if (!solo || now - armed_ < kDeadMs) { fired_ = false; return; }
    held_ = true;
    fired_ = false;
    t0_ = now;
    low_since_ = 0;
  }
  // B went up (released).
  void BUp() {
    if (held_) { held_ = false; hint_ = true; }
    fired_ = false;
  }
  // Any other control: a second key, an encoder turn.
  void Other() {
    if (held_) { held_ = false; hint_ = true; }
  }
  // Call every loop. `b_pin`: the raw B pin (true = down). True exactly once, when the
  // hold has lasted kHoldMs.
  bool Tick(uint32_t now, bool b_pin) {
    if (!held_) return false;
    if (b_pin) low_since_ = 0;
    else {
      if (!low_since_) low_since_ = now ? now : 1;
      if (now - low_since_ >= kBounceMs) { held_ = false; hint_ = true; return false; }
    }
    if (now - t0_ >= kHoldMs) {
      held_ = false;
      fired_ = true;
      return true;
    }
    return false;
  }

  bool holding() const { return held_; }
  bool fired() const { return fired_; }
  // After an aborted hold: the screen shows "hold B to write" until the next B down.
  bool hint() const { return hint_; }
  // 0..1000 of the hold done at `now` (0 when not holding).
  uint32_t permille(uint32_t now) const {
    if (!held_) return 0;
    const uint32_t d = now - t0_;
    return d >= kHoldMs ? 1000u : d * 1000u / kHoldMs;
  }

 private:
  void Clear() { held_ = false; fired_ = false; low_since_ = 0; }
  uint32_t armed_ = 0, t0_ = 0, low_since_ = 0;
  bool held_ = false, fired_ = false, hint_ = false;
};

#endif  // CAL259EHOLD_H_
