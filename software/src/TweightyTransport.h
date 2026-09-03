#ifndef TWEIGHTYTRANSPORT_H_
#define TWEIGHTYTRANSPORT_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// TIME-mode transport for the Tweighty app -- a TWO-state machine, not
// three. WRITE continuously records the panel input into the loop buffer;
// RECIRC gates the fresh input off and lets the captured window recirculate
// through feedback instead. "Loop" is what RECIRC's playback does once a
// window is captured, not a state of its own -- this mirrors the 288r's own
// transport.h/transport.c (XP_WRITE=1, XP_RECIRC=2), which is genuinely
// two-state despite the three-word vocabulary (WRITE/RECIRC/LOOP) that shows
// up in casual descriptions of the hardware.
//
// PITCH mode (dual-head windowed reader) is NOT implemented -- 288r's own
// docs mark it as having an unresolved buffer-wrap glitch. TIME mode only
// for v1.
//
// Pure state, no Arduino/Audio.h: host-tested by
// test/test_tweighty_transport.cpp. AudioTweightyF32 drives the write
// gate from transport_should_write() and polls transport_consume_resync_edge
// once per audio block to know when to (re)start its equal-power crossfade;
// TweightyApp drives transport_request_toggle() from the digital input and
// button A (resolution: both are a toggle, not a 3-way advance).
// ---------------------------------------------------------------------------

enum transport_mode_t : uint8_t {
  XP_WRITE = 1,
  XP_RECIRC = 2,
};

struct TweightyTransportState {
  transport_mode_t mode = XP_WRITE;
  // One-shot: something happened this block that the audio engine has not
  // yet reacted to -- a WRITE<->RECIRC edge, or a RECIRC loop-wrap. Both
  // want the same response (retrigger the crossfade), so they share one
  // flag rather than the engine polling two separate queries every block.
  bool resync_edge = false;
};

void transport_begin_write(TweightyTransportState &s);
void transport_begin_recirc(TweightyTransportState &s);
bool transport_should_write(const TweightyTransportState &s);

// Rising-edge WRITE<->RECIRC toggle -- what the digital input and button A
// both drive. Idempotent while held: only a rising edge should reach this at
// all, but calling it on an already-current mode is a no-op regardless
// (transport_begin_write/_recirc below already refuse to re-fire the edge).
void transport_request_toggle(TweightyTransportState &s);

// Consumes and clears the resync flag; returns whether it was set. Call once
// per audio block from update().
bool transport_consume_resync_edge(TweightyTransportState &s);

// The engine calls this when RECIRC's read position wraps the captured
// window. 288r's own hardware crossfader doesn't need this signal -- it is
// continuous by construction -- but this app's block-rate equal-power
// crossfade does, so a wrap is folded into the same edge a mode change
// raises rather than needing its own separate handling in the engine.
void transport_signal_loop_wrap(TweightyTransportState &s);

// Rising-edge helper for driving transport_request_toggle from a raw digital
// input or button read polled every block: the caller keeps `prev_level`
// across calls (one instance per source -- the digital input and button A
// each need their own, since either can rise while the other is idle).
inline bool transport_rising_edge(bool level, bool &prev_level) {
  const bool edge = level && !prev_level;
  prev_level = level;
  return edge;
}

#endif  // TWEIGHTYTRANSPORT_H_
