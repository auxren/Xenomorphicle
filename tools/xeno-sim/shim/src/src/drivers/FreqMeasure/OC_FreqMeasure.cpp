// ---------------------------------------------------------------------------
// The frequency-measurement timer, replaced. The real driver captures edges
// with a FlexPWM or QuadTimer peripheral; there is no such thing on a host.
//
// available() is always 0, so anything that reads an incoming clock rate --
// the Tuner, a BPM display, a clock-follow -- reads nothing and stays still.
// The header is the real one, so begin()/end() are still called in the real
// places (the app switcher calls FreqMeasure.end() on every app change).
// ---------------------------------------------------------------------------
#include "OC_FreqMeasure.h"

FreqMeasureClass FreqMeasure;

/*static*/ FreqMeasureClass *FreqMeasureClass::pin_inst[4] = {nullptr, nullptr, nullptr, nullptr};

void FreqMeasureClass::begin(uint8_t) { running = true; }
uint8_t FreqMeasureClass::available(void) { return 0; }
uint32_t FreqMeasureClass::read(void) { return 0; }
void FreqMeasureClass::end(void) { running = false; }
void FreqMeasureClass::isr() {}
