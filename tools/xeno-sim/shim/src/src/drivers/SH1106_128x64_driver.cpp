// ---------------------------------------------------------------------------
// The OLED panel, replaced. Everything ABOVE it is the firmware's own:
// GRAPHICS_BEGIN_FRAME, the double-buffered FrameBuffer, PagedDisplayDriver's
// eight-page walk and display.cpp are all compiled from software/src/, so the
// simulator's frame is assembled and handed over page by page exactly as the
// module's is. This file is only the SPI transfer at the end of that, and the
// panel's own command set.
//
// SendPage() copies the page into a buffer the simulator can show, and always
// succeeds -- there is no DMA, so a page never has to be retried. That means
// the simulator cannot show you a torn frame, a display that falls behind the
// renderer, or a Flush() that has to wait: every timing question about the
// display is unanswerable here.
//
// SetInverted() is honoured: it flips the pixels on the way out, which is what
// the panel command does, so the Setup app's invert-display toggle is visible.
// AdjustOffset/SetFlipMode/SetContrast/ChangeSpeed are recorded and otherwise
// inert -- contrast and column offset have no meaning for a browser canvas.
// ---------------------------------------------------------------------------

#include "SH1106_128x64_driver.h"

#include <string.h>

#include "sim_host.h"   // SimPanelFrameComplete: the deferred-capture hook

namespace {
uint8_t g_panel[SH1106_128x64_Driver::kFrameSize];
bool g_inverted = false;
bool g_flipped = false;
uint8_t g_contrast = 0;
uint8_t g_offset = SH1106_128x64_Driver::kDefaultOffset;
}  // namespace

// What the simulator draws. Page-packed, 128x64, bit (y & 7) of buf[(y >> 3) *
// 128 + x] -- the same bytes the panel is sent, in the same order.
const uint8_t *SimPanelBytes() { return g_panel; }
bool SimPanelInverted() { return g_inverted; }
bool SimPanelFlipped() { return g_flipped; }
uint8_t SimPanelContrast() { return g_contrast; }
uint8_t SimPanelOffset() { return g_offset; }

void SH1106_128x64_Driver::Init() {
  memset(g_panel, 0, sizeof(g_panel));
}

void SH1106_128x64_Driver::Clear() {
  memset(g_panel, 0, sizeof(g_panel));
}

void SH1106_128x64_Driver::Flush() {}

bool SH1106_128x64_Driver::SendPage(uint_fast8_t index, const uint8_t *data) {
  if (index < kNumPages)
    memcpy(g_panel + index * kPageSize, data, kPageSize);
  // The last page of the eight completes a frame. That is the ONLY instant at
  // which g_panel holds one whole picture rather than a blend of two, so it is
  // where a deferred capture has to happen -- see SimSnapArm(). Capturing on a
  // timer alone would sample mid-walk and produce a torn frame that differs
  // run to run, which in a determinism-first simulator is worse than no
  // capture at all.
  if (index == kNumPages - 1) SimPanelFrameComplete();
  return true;
}

void SH1106_128x64_Driver::SPI_send(void *, size_t) {}

void SH1106_128x64_Driver::AdjustOffset(uint8_t offset) { g_offset = offset; }
void SH1106_128x64_Driver::ChangeSpeed(uint32_t) {}
void SH1106_128x64_Driver::SetFlipMode(bool flip180) { g_flipped = flip180; }
void SH1106_128x64_Driver::SetContrast(uint8_t contrast) { g_contrast = contrast; }
void SH1106_128x64_Driver::SetInverted(bool inverted) { g_inverted = inverted; }
