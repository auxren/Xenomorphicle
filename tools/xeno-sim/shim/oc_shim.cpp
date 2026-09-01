// Definitions for the host shim declared in oc_shim.h: the virtual clock, the
// four fake MIDI ports, the framebuffer-backed `graphics` object and
// gfxHeader(). See oc_shim.h for what is real and what is faked.
#include "oc_shim.h"

// --- clock -----------------------------------------------------------------
static uint32_t sim_now_ms = 0;

uint32_t SimNowMs() { return sim_now_ms; }
void SimAdvanceMs(uint32_t dt) { sim_now_ms += dt; }

// --- MIDI ------------------------------------------------------------------
void SimMidiPort::Push(uint8_t type, uint8_t d1, uint8_t d2) {
  const int next = (head_ + 1) % kCap;
  if (next == tail_) return;   // full: drop, exactly as a real ring would
  q_[head_][0] = type;
  q_[head_][1] = d1;
  q_[head_][2] = d2;
  head_ = next;
}

bool SimMidiPort::read() {
  if (tail_ == head_) return false;
  type_ = q_[tail_][0];
  d1_ = q_[tail_][1];
  d2_ = q_[tail_][2];
  tail_ = (tail_ + 1) % kCap;
  return true;
}

SimMidiPort usbMIDI;
SimMidiPort usbHostMIDI[2];
SimMidiPort MIDI1;

// --- graphics --------------------------------------------------------------
// The real 128x64 vertically-packed framebuffer the SH1106 driver would be
// handed on target. weegfx::Graphics writes into it unmodified.
static uint8_t sim_frame[weegfx::Graphics::kWidth * weegfx::Graphics::kHeight / 8];

weegfx::Graphics graphics;

uint8_t *SimFrameBuffer() { return sim_frame; }

// Mirrors HSUtils.cpp's gfxHeader() call for call (gfxPrint = setPrintPos +
// print, gfxLine = drawLine). Reproduced rather than linked because HSUtils.cpp
// is a 900-line file wired into the whole o_C runtime.
void gfxHeader(const char *str, const uint8_t *icon) {
  int x = 1;
  if (icon) {
    graphics.drawBitmap8(x, 1, 8, icon);
    x += 8;
  }
  graphics.setPrintPos(x, 1);
  graphics.print(str);
  graphics.drawLine(0, 10, 127, 10);
}
