#pragma once
// ---------------------------------------------------------------------------
// A 4-deep FIFO of raw 0x19 calibration-reply frames, between the 200e bus
// parser's cal_reply hook and whoever runs the calibration
// (OC::PresetBus::PopCalReply). Header-only and BSP-free so the host suite
// can test it; PresetBus.cpp owns the one instance.
//
// SINGLE CONTEXT ONLY: the parser runs from Bus200eFeedEvent(), which is
// called from loop context (Task() / SendRawFrame()'s drain), never from the
// ISR, and PopCalReply is a loop-context call too -- so there is no
// producer/consumer race and no volatile here. Do not push from an ISR.
//
// Full ring: the NEWEST frame is refused (Push returns false; the caller
// counts it). The oldest replies are the ones a calibration step is waiting
// on, so they are never overwritten.
// ---------------------------------------------------------------------------
#include <stdint.h>
#include <string.h>

struct CalReplyRing {
  static constexpr uint8_t kDepth = 4;
  static constexpr uint8_t kFrameMax = 18;   // = BUS200E_CAL_REPLY_FRAME_MAX

  uint8_t buf[kDepth][kFrameMax];
  uint8_t len[kDepth];
  uint8_t head;    // index of the oldest frame
  uint8_t count;   // frames held, 0..kDepth

  void Clear() { head = 0; count = 0; }

  // Copies n bytes in. false = ring full (the newest -- this one -- is
  // dropped) or n is 0 / larger than kFrameMax.
  bool Push(const uint8_t *f, uint8_t n) {
    if (n == 0 || n > kFrameMax || count >= kDepth) return false;
    const uint8_t slot = (uint8_t) ((head + count) % kDepth);
    memcpy(buf[slot], f, n);
    len[slot] = n;
    count++;
    return true;
  }

  // Oldest first. `f` must hold kFrameMax bytes. false = empty.
  bool Pop(uint8_t *f, uint8_t *n) {
    if (!count) return false;
    *n = len[head];
    memcpy(f, buf[head], len[head]);
    head = (uint8_t) ((head + 1) % kDepth);
    count--;
    return true;
  }
};
