// Pure WRITE/RECIRC transport logic for the Tweighty app. See the header
// for why this is its own file and why it is a two-state machine.
//
// Deliberately NOT FLASHMEM: every one of these functions is reached from
// either the audio-rate update() (AudioTweightyF32.h) or the 16.666kHz
// Controller() (TweightyApp.h) -- both audio-ISR-hot paths this codebase's
// own convention keeps off flash (see Bus200eApp.h's tail comment on the
// LTO/FLASHMEM trap, and TweightyApp.h's file-tail comment for why
// Controller() itself is exempt from the FLASHMEM-everything-else rule that
// applies elsewhere in that file). An earlier version of this file wrapped
// every function in a FLASHMEM macro by copy-paste from the pure-logic
// modules elsewhere in this codebase that genuinely are cold -- confirmed via
// linker veneers in the linked ELF that this put six ISR-hot calls a flash
// fetch away from ITCM on every audio block and every Controller() pass.

#include "TweightyTransport.h"

void transport_begin_write(TweightyTransportState &s) {
  if (s.mode == XP_WRITE) return;
  s.mode = XP_WRITE;
  s.resync_edge = true;
}

void transport_begin_recirc(TweightyTransportState &s) {
  if (s.mode == XP_RECIRC) return;
  s.mode = XP_RECIRC;
  s.resync_edge = true;
}

bool transport_should_write(const TweightyTransportState &s) {
  return s.mode == XP_WRITE;
}

void transport_request_toggle(TweightyTransportState &s) {
  if (s.mode == XP_WRITE) transport_begin_recirc(s);
  else transport_begin_write(s);
}

bool transport_consume_resync_edge(TweightyTransportState &s) {
  const bool fired = s.resync_edge;
  s.resync_edge = false;
  return fired;
}

void transport_signal_loop_wrap(TweightyTransportState &s) {
  s.resync_edge = true;
}
