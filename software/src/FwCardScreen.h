#ifndef FWCARDSCREEN_H_
#define FWCARDSCREEN_H_
// ---------------------------------------------------------------------------
// On-screen status of a firmware-card load (the module reading FWCARD.BIN from
// the Xenomorpher's emulated 0x50 card). Pure logic: no bus access, no heap, no
// file access, no graphics, so it runs on the host (test_fwcard_screen.cpp) and
// Main.cpp only feeds it the PresetBus counters and draws the rows it returns.
//
// States
//   IDLE     not serving a card (and nothing finished this boot): draw nothing
//   READY    serving, the module has not read a byte yet
//   READING  the module is reading: bar, percent, bytes, seconds
//   DONE     the whole image was served and the reads then went quiet
//
// DONE is a HEURISTIC and cannot be anything else. The Xeno sees only the
// module's reads of the card; it never hears from the module that the new
// firmware is programmed. "Every byte served, then nothing for kDoneQuietS"
// looks exactly the same as "every byte served, module paused (erasing,
// programming, waiting on the operator)". A 251e reads the image TWICE (served
// bytes go to ~2x the image length), so on the first pass's end the bus can sit
// quiet long enough to trip DONE; the next byte moves the screen back to
// READING with the pass count at 2. The text therefore says "Update complete"
// only after kDoneQuietS of silence, and takes it back if the reads resume.
//
// The latch: PresetBus disarms the card (and leaves FwCardMode()) 30 s after a
// full read, and FwCardServedBytes() reads 0 from then on. DONE is remembered
// here so the screen keeps saying "switch the case OFF, then ON" for the rest
// of this boot (static state, set by Update() while the card was still served).
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace FwCardScreen {

// Quiet time, after served >= image length, before the screen calls it done.
static constexpr uint32_t kDoneQuietS = 8;

static constexpr uint32_t kNever = 0xFFFFFFFFu;   // FwCardFirstReadAgeS(): no read yet
static constexpr int kCols = 21;                  // the OLED text width
static constexpr int kRows = 5;                   // text rows under the title

enum State : uint8_t { IDLE, READY, READING, DONE };

class Tracker {
 public:
  // Call once per frame. mode = FwCardMode(); served/age_s/len are the PresetBus
  // counters (len 0 = not known yet). now_ms is millis(). Returns the state.
  State Update(uint32_t now_ms, bool mode, uint32_t served, uint32_t age_s, uint32_t len) {
    if (!mode) {
      // Card gone (disarmed after the full read, `k`, reboot-less leave): the
      // counters are 0 now. Keep the last view only if it was DONE.
      if (state_ != DONE) state_ = IDLE;
      return state_;
    }
    len_ = len;
    if (age_s == kNever) {          // serving, first read still to come
      state_ = READY;
      return state_;
    }
    if (!seen_ || served != served_) {
      seen_ = true;
      served_ = served;
      moved_ms_ = now_ms;
    }
    age_s_ = age_s;
    const bool full = len_ && served_ >= len_;
    state_ = (full && (now_ms - moved_ms_) >= kDoneQuietS * 1000u) ? DONE : READING;
    return state_;
  }

  State state() const { return state_; }
  bool Active() const { return state_ != IDLE; }
  uint32_t served() const { return served_; }
  uint32_t len() const { return len_; }
  uint32_t age_s() const { return age_s_; }
  // Bar fill, clamped: a second pass takes served past the image length.
  uint32_t Percent() const {
    if (!len_) return 0;
    if (served_ >= len_) return 100;
    return (uint32_t)((uint64_t)served_ * 100u / len_);
  }
  // 1 for the first read of the image, 2 once served exceeds it, and so on.
  uint32_t Pass() const { return (len_ && served_) ? (served_ - 1) / len_ + 1 : 1; }

  // The text under the title (rows[i] is "" when unused). *bar is true when the
  // progress bar belongs between the first and second row; *pct is its fill.
  void Rows(char rows[kRows][kCols + 1], bool *bar, uint32_t *pct) const {
    for (int i = 0; i < kRows; ++i) rows[i][0] = 0;
    *bar = false;
    *pct = Percent();
    switch (state_) {
      case READY:
        Put(rows[0], "Firmware card ready");
        Put(rows[1], "Hold the module's");
        Put(rows[2], "Remote, power the");
        Put(rows[3], "case on");
        break;
      case READING:
        Put(rows[0], "Module is reading");
        *bar = true;
        if (Pass() > 1) snprintf(rows[1], kCols + 1, "%lu%% pass %lu", (unsigned long)*pct, (unsigned long)Pass());
        else            snprintf(rows[1], kCols + 1, "%lu%%", (unsigned long)*pct);
        snprintf(rows[2], kCols + 1, "%lu/%lu B", (unsigned long)served_, (unsigned long)len_);
        snprintf(rows[3], kCols + 1, "%lu s", (unsigned long)age_s_);
        break;
      case DONE:
        Put(rows[0], "Update complete");
        Put(rows[1], "Switch the case OFF,");
        Put(rows[2], "then ON");
        snprintf(rows[3], kCols + 1, "%lu B in %lu s", (unsigned long)served_, (unsigned long)age_s_);
        break;
      case IDLE:
      default:
        break;
    }
  }

 private:
  static void Put(char *dst, const char *s) { snprintf(dst, kCols + 1, "%s", s); }

  State state_ = IDLE;
  bool seen_ = false;        // a read was seen this boot
  uint32_t served_ = 0;
  uint32_t len_ = 0;
  uint32_t age_s_ = 0;
  uint32_t moved_ms_ = 0;    // millis() when served_ last changed
};

}  // namespace FwCardScreen

#endif  // FWCARDSCREEN_H_
