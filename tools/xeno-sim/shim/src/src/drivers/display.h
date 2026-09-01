#ifndef XENOSIM_DISPLAY_H_
#define XENOSIM_DISPLAY_H_
// ---------------------------------------------------------------------------
// The real display header, with ONE macro redefined.
//
// GRAPHICS_BEGIN_FRAME(true) busy-waits for a free frame in the double buffer.
// On hardware that wait is fine: the core ISR keeps running underneath it,
// sends the pending frame page by page and frees the buffer. The simulator has
// no ISR of its own -- it calls the firmware's ISR from the same loop as
// everything else -- so a blocking firmware loop would spin forever after two
// frames, and every screen that draws from inside one would be unreachable:
// the splash screen, the EEPROM-reset confirmation, the calibration wizard,
// the "Saving..." bar.
//
// So the wait calls display::SimPump(), which runs exactly what the ISR would
// have run in that time (one core-ISR period of DAC/display/ADC/digital-input
// work, plus the UI poll on its own 1 kHz schedule) and advances the virtual
// clock by that period. The effect is the hardware's: time passes, the display
// drains, buttons are still polled, and the loop makes progress.
//
// Everything else -- the FrameBuffer, the paged driver, GRAPHICS_END_FRAME,
// the graphics object -- comes from the real header, included below.
// ---------------------------------------------------------------------------

#include <real/src/drivers/display.h>

namespace display {
// One core-ISR period of background work, on the virtual clock.
void SimPump();
}

#undef GRAPHICS_BEGIN_FRAME
#define GRAPHICS_BEGIN_FRAME(wait) \
do { \
  uint8_t *frame = NULL; \
  do { \
    if (display::frame_buffer.writeable()) \
      frame = display::frame_buffer.writeable_frame(); \
    else if (wait) \
      display::SimPump(); \
  } while (!frame && wait); \
  if (frame) { \
    graphics.Begin(frame, weegfx::CLEAR_FRAME_ENABLE); \
    do {} while(0)

#endif  // XENOSIM_DISPLAY_H_
