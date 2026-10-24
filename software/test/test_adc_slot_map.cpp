// Host tests for the ADC33131D frame-slot map (src/AdcSlotMap.h).
//   g++ -std=c++17 -Wall -Werror -O2 -o build/test_adc_slot_map \
//     test_adc_slot_map.cpp ../src/CvMeterMath.cpp ../src/InputCurveStore.cpp
//
// The map cannot be read off the datasheet (not in the tree), and the FlexIO
// reference leaves one detail open: whether a trigger that is already
// asserted when FlexIO is enabled counts as an edge. So this models the
// capture start event by event, for every combination of the unknowns, keeps
// the combinations that reproduce the bench (the old init gave a different
// slot depending on the trigger level at FLEXEN), fits the one free constant
// to the bench pair, and checks that the new start gives AdcSlotForPanelInput
// under every one of them.
#include <cstdio>
#include <cstring>

#include "../src/AdcSlotMap.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---- model of the capture start -------------------------------------------
//
// Trigger edges alternate. The mux shifters step on RISING edges (mux timer,
// trigger active high); a reading starts on each FALLING edge (baud timer,
// trigger active low) and is deposited in buffer order from slot 0.
struct Semantics {
  bool load_on_enable;  // the mux shifter outputs SHIFTBUF bit 0 at timer enable
                        // (else its first rising edge loads it without stepping)
  bool spurious_read;   // an asserted active-low trigger at FLEXEN starts a reading
  bool spurious_step;   // an asserted active-high trigger at FLEXEN counts as a rising edge
};

enum Start { OLD_TRIGGER_LOW, OLD_TRIGGER_HIGH, NEW_START };

// Mux steps taken before reading k, minus k, in steady state (k large): the
// constant c in "reading k is taken with address index k + c".
static int phase(const Semantics &m, Start st) {
  int steps = 0;          // address index
  bool loaded = m.load_on_enable;
  auto rising = [&]() { if (loaded) ++steps; else loaded = true; };
  int k = 0;              // readings deposited so far
  int c = 0;
  const bool high = (st == OLD_TRIGGER_HIGH);
  // At FLEXEN.
  if (high && m.spurious_step) rising();
  if (!high && m.spurious_read && st == OLD_TRIGGER_LOW) ++k;  // NEW_START discards it
  // Then free-running edges, starting with the one opposite to the level.
  bool next_rising = !high;
  for (int e = 0; e < 64; ++e, next_rising = !next_rising) {
    if (next_rising) { rising(); continue; }
    c = steps - k;        // this reading is index k, taken at address index `steps`
    ++k;
  }
  return c;
}

// Panel n's mux address is n + W, and the ADC returns a conversion L readings
// late: slot = n + W + L - c. W + L is one unknown constant, D.
static int slot(int n, int D, int c) { return ((n + D - c) % 8 + 8) % 8; }

static void new_start_matches_bench_under_every_consistent_model() {
  int consistent = 0;
  for (int bits = 0; bits < 8; ++bits) {
    const Semantics m = {bool(bits & 1), bool(bits & 2), bool(bits & 4)};
    const int c_low = phase(m, OLD_TRIGGER_LOW), c_high = phase(m, OLD_TRIGGER_HIGH);
    // The bench: two images, two slots one apart. A model in which the old
    // init does not depend on the trigger level cannot explain it.
    if (c_low == c_high) continue;
    CHECK(c_low - c_high == 1 || c_high - c_low == 1);
    ++consistent;
    // Fit D: {slot under low, slot under high} = {(n+4)&7, (n+5)&7} for n = 0.
    int D = -1;
    for (int d = 0; d < 8; ++d) {
      const int a = slot(0, d, c_low), b = slot(0, d, c_high);
      if ((a == 4 && b == 5) || (a == 5 && b == 4)) D = d;
    }
    CHECK(D >= 0);
    const int c_new = phase(m, NEW_START);
    for (int n = 0; n < 8; ++n) CHECK(slot(n, D, c_new) == AdcSlotForPanelInput(n));
  }
  CHECK(consistent > 0);
}

// The old init really was level-dependent in the models above, by one slot,
// and in the direction the bench saw: one of the two levels reproduces image
// 88d35b7d (panel 1 -> ADC_CHANNEL_2 under the old table) and the other
// 5ebcde9f (panel 1 -> ADC_CHANNEL_8 under the +1 table).
static void old_start_reproduces_both_images() {
  for (int bits = 0; bits < 8; ++bits) {
    const Semantics m = {bool(bits & 1), bool(bits & 2), bool(bits & 4)};
    const int c_low = phase(m, OLD_TRIGGER_LOW), c_high = phase(m, OLD_TRIGGER_HIGH);
    if (c_low == c_high) continue;
    const int c_new = phase(m, NEW_START);
    // NEW_START equals whichever old case put panel n in (n+4)&7.
    CHECK(c_new == c_low || c_new == c_high);
    // The other case is the one slot later: image 88d35b7d.
    const int other = (c_new == c_low) ? c_high : c_low;
    CHECK(c_new - other == 1);
  }
}

static void map_is_a_permutation_top_row_on_4_to_7() {
  int seen = 0;
  for (int n = 0; n < 8; ++n) seen |= 1 << AdcSlotForPanelInput(n);
  CHECK(seen == 0xFF);
  const int expect[8] = {4, 5, 6, 7, 0, 1, 2, 3};
  for (int n = 0; n < 8; ++n) CHECK(AdcSlotForPanelInput(n) == expect[n]);
}

// ---- the page flags -----------------------------------------------------------

static void flags_mark_pages_from_before_the_fix() {
  CvMeterMath::InputCalibration c;
  CvMeterMath::InputCalReset(c);
  CHECK(!IcaPredatesFix(c));            // nothing fitted: nothing to re-run
  c.valid_mask = 0x02;
  CHECK(IcaPredatesFix(c));             // older firmware left the byte 0
  IcaMapFlag(c) = kInputMapV1;
  CHECK(!IcaPredatesFix(c) && c.reserved0[0] == kInputMapV1);

  InputCurveRecord r;
  memset(&r, 0, sizeof r);
  r.valid_mask = 0x01;
  CHECK(CurvesPredateFix(r));
  CurveMapFlag(r) = kInputMapV1;
  CHECK(!CurvesPredateFix(r) && r.reserved2[0] == kInputMapV1);
}

int main() {
  new_start_matches_bench_under_every_consistent_model();
  old_start_reproduces_both_images();
  map_is_a_permutation_top_row_on_4_to_7();
  flags_mark_pages_from_before_the_fix();
  if (g_fail) {
    printf("test_adc_slot_map: %d FAILED\n", g_fail);
    return 1;
  }
  printf("test_adc_slot_map: all passed\n");
  return 0;
}
