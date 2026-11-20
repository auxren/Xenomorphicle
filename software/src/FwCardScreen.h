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
//   READY    card armed and served, no module has read a byte yet
//   READING  first read arrived: "MODULE UPDATING", bar, percent, bytes, seconds
//   STALLED  reads started, then stopped short of the image length for kStallS
//   DONE     the whole image was served and the reads then went quiet
//
// Which modules read how often: a 251e reads the image TWICE (served bytes go to
// ~2x the image length; the pass count on screen shows it), a 267e and a 257e
// read it once, about 45 s. Do not assume one pass.
//
// DONE is a HEURISTIC and cannot be anything else. The Xeno sees only the
// module's reads of the card; it never hears from the module that the new
// firmware is programmed. "Every byte served, then nothing for kDoneQuietS"
// looks exactly the same as "every byte served, module paused (erasing,
// programming, waiting on the operator)". On a 251e the gap between its two
// passes can be long enough to trip DONE; the next byte moves the screen back to
// READING with the pass count at 2. So "UPDATE COMPLETE" is only said after the
// served count reached the image length AND kDoneQuietS of silence followed, and
// it is taken back if the reads resume. STALLED is the same guess from the other
// side: reads stopped short of the image for kStallS may be a failed read or a
// slow module; it clears by itself if the reads resume.
//
// The latch: PresetBus disarms the card (and leaves FwCardMode()) 30 s after a
// full read, and FwCardServedBytes() reads 0 from then on. DONE is remembered
// here so the screen keeps saying "switch the case OFF, then ON" until the next
// boot or until the user dismisses it with a button press.
//
// Dismiss: a button press while READY, STALLED or DONE hides the screen (so the
// app, and its own "READ FAIL: fw card" message, is reachable). It stays hidden
// until the state changes. While READING every event is swallowed instead and
// nothing is dismissed: the app cannot be driven blind under the bar.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace FwCardScreen {

// Quiet time, after served >= image length, before the screen calls it done.
static constexpr uint32_t kDoneQuietS = 12;
// Quiet time, with served < image length, before a started read is called stalled.
static constexpr uint32_t kStallS = 20;

static constexpr uint32_t kNever = 0xFFFFFFFFu;   // FwCardFirstReadAgeS(): no read yet
static constexpr int kCols = 21;                  // the OLED text width
static constexpr int kRows = 5;                   // text rows under the title

enum State : uint8_t { IDLE, READY, READING, STALLED, DONE };

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
    } else {
      if (!seen_) {                 // the first read of the image
        seen_ = true;
        started_ = true;
        served_ = served;
        moved_ms_ = now_ms;
      } else if (served != served_) {
        served_ = served;
        moved_ms_ = now_ms;
      }
      age_s_ = age_s;
      const uint32_t quiet_ms = now_ms - moved_ms_;
      if (len_ && served_ >= len_)
        state_ = (quiet_ms >= kDoneQuietS * 1000u) ? DONE : READING;
      else
        state_ = (len_ && quiet_ms >= kStallS * 1000u) ? STALLED : READING;
    }
    if (dismissed_ && state_ != dismissed_state_) dismissed_ = false;
    return state_;
  }

  // True once, on the first frame after the module's first read arrived (for the
  // Serial log line).
  bool TakeStarted() { const bool s = started_; started_ = false; return s; }

  // A UI event arrived while the screen may be up. Returns true if the screen
  // took it (the caller drops the event). press = a button went down.
  bool Swallow(bool press) {
    if (!Active()) return false;
    if (press && state_ != READING) { dismissed_ = true; dismissed_state_ = state_; }
    return true;
  }

  State state() const { return state_; }
  bool Active() const { return state_ != IDLE && !(dismissed_ && dismissed_state_ == state_); }
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
        Put(rows[4], "Any button: hide");
        break;
      case READING:
        Put(rows[0], "MODULE UPDATING");
        *bar = true;
        if (Pass() > 1) snprintf(rows[1], kCols + 1, "%lu%% pass %lu", (unsigned long)*pct, (unsigned long)Pass());
        else            snprintf(rows[1], kCols + 1, "%lu%%", (unsigned long)*pct);
        snprintf(rows[2], kCols + 1, "%lu/%lu B", (unsigned long)served_, (unsigned long)len_);
        snprintf(rows[3], kCols + 1, "%lu s", (unsigned long)age_s_);
        break;
      case STALLED:
        snprintf(rows[0], kCols + 1, "READ STALLED %lu%%", (unsigned long)*pct);
        Put(rows[1], "Check the bus, retry");
        Put(rows[2], "the module's Remote");
        Put(rows[3], "hold");
        Put(rows[4], "Any button: hide");
        break;
      case DONE:
        Put(rows[0], "UPDATE COMPLETE");
        Put(rows[1], "Switch the case OFF,");
        Put(rows[2], "then ON");
        snprintf(rows[3], kCols + 1, "%lu B in %lu s", (unsigned long)served_, (unsigned long)age_s_);
        Put(rows[4], "Any button: hide");
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
  bool started_ = false;     // first read seen, not yet reported
  bool dismissed_ = false;
  State dismissed_state_ = IDLE;
  uint32_t served_ = 0;
  uint32_t len_ = 0;
  uint32_t age_s_ = 0;
  uint32_t moved_ms_ = 0;    // millis() when served_ last changed
};

}  // namespace FwCardScreen

#endif  // FWCARDSCREEN_H_
