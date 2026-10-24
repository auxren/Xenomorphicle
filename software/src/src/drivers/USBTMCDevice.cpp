// USBTMC host driver: see USBTMCDevice.h. Structure follows USBHost_t36's
// midi.cpp (claim, new_Pipe, bulk callbacks) and hub.cpp (pools).

#include "USBTMCDevice.h"

#if defined(ARDUINO_TEENSY41)

#include <string.h>

namespace {

// USB host interrupt masked for the duration of a scope. queue_Data_Transfer
// saves and restores this same NVIC bit itself, so nesting is safe; the point
// is that disconnect() (an ISR) cannot null a pipe between our check and the
// queue call, nor a callback land between two flag updates.
struct UsbIrqGuard {
  bool was;
  UsbIrqGuard() : was(NVIC_IS_ENABLED(IRQ_USBHS)) { NVIC_DISABLE_IRQ(IRQ_USBHS); }
  ~UsbIrqGuard() { if (was) NVIC_ENABLE_IRQ(IRQ_USBHS); }
};

}  // namespace

void USBTMCDevice::init() {
  contribute_Pipes(mypipes, sizeof(mypipes) / sizeof(Pipe_t));
  contribute_Transfers(mytransfers, sizeof(mytransfers) / sizeof(Transfer_t));
  rxpipe_ = nullptr;
  txpipe_ = nullptr;
  state_ = QueryState::Idle;
  phase_ = kNone;
  driver_ready_for_device(this);
}

FLASHMEM bool USBTMCDevice::connected() const {
  Device_t *dev = *(Device_t *const volatile *)&device;
  return dev != nullptr && rxpipe_ != nullptr && txpipe_ != nullptr;
}

FLASHMEM uint16_t USBTMCDevice::idVendor() const {
  Device_t *dev = *(Device_t *const volatile *)&device;
  return dev ? dev->idVendor : 0;
}

FLASHMEM uint16_t USBTMCDevice::idProduct() const {
  Device_t *dev = *(Device_t *const volatile *)&device;
  return dev ? dev->idProduct : 0;
}

// Interface level only. descriptors starts at the interface descriptor and
// runs to the end of the configuration; stop at the next interface.
FLASHMEM bool USBTMCDevice::claim(Device_t *dev, int type, const uint8_t *descriptors,
                                  uint32_t len) {
  if (type != 1) return false;
  const uint8_t *p = descriptors;
  const uint8_t *end = p + len;
  if (len < 9 || p[0] < 9 || p[1] != 4) return false;
  if (p[5] != 0xFE || p[6] != 0x03) return false;  // Application / USBTMC
  const uint32_t max_packet = (dev->speed == 2) ? 512 : 64;
  const uint8_t iface = p[2];
  uint8_t in_ep = 0, out_ep = 0;
  uint16_t in_size = 0, out_size = 0;
  p += p[0];
  while (p + 2 <= end) {
    const uint32_t dlen = p[0];
    if (dlen < 2 || p + dlen > end) return false;
    const uint8_t dtype = p[1];
    if (dtype == 4 || dtype == 11) break;  // next interface or IAD
    if (dtype == 5 && dlen >= 7 && (p[3] & 0x03) == 2) {  // bulk endpoint
      const uint8_t addr = p[2];
      const uint16_t size = (uint16_t)((p[4] | (p[5] << 8)) & 0x07FF);
      if (addr & 0x80) {
        if (!in_ep) { in_ep = addr & 0x0F; in_size = size; }
      } else {
        if (!out_ep) { out_ep = addr & 0x0F; out_size = size; }
      }
    }
    // The interrupt IN endpoint (SRQ notifications) is deliberately unused.
    p += dlen;
  }
  if (!in_ep || !out_ep) return false;
  if (!in_size || in_size > max_packet || !out_size || out_size > max_packet) return false;

  Pipe_t *rx = new_Pipe(dev, 2, in_ep, 1, in_size);
  if (!rx) return false;
  Pipe_t *tx = new_Pipe(dev, 2, out_ep, 0, out_size);
  if (!tx) return false;  // rx is freed with the device's other pipes
  rx->callback_function = rx_callback;
  tx->callback_function = tx_callback;
  // USBHost_t36 reports a failed qTD (STALL, babble, 3 transaction errors)
  // ONLY through these; the normal callback never runs for it.
  rx->error_callback_function = rx_error_callback;
  tx->error_callback_function = tx_error_callback;
  rx_size_ = in_size;
  tx_size_ = out_size;
  in_addr_ = (uint8_t)(0x80 | in_ep);
  out_addr_ = out_ep;
  iface_ = iface;
  rx_done_ = false;
  in_queued_ = false;
  tx_busy_ = false;
  ctl_busy_ = false;
  ctl_done_ = false;
  ctl_abandoned_ = false;
  halt_in_ = false;
  halt_out_ = false;
  recover_req_ = false;
  rec_ = rNone;
  rx_len_ = 0;
  phase_ = kNone;
  state_ = QueryState::Idle;
  err_ = "";
  rxpipe_ = rx;
  txpipe_ = tx;
  return true;
}

// ISR context. The pipes and any queued transfers (control ones included)
// are freed by the host layer right after this returns; no callback follows.
void USBTMCDevice::disconnect() {
  rxpipe_ = nullptr;
  txpipe_ = nullptr;
  rx_done_ = false;
  in_queued_ = false;
  tx_busy_ = false;
  ctl_busy_ = false;
  ctl_done_ = false;
  ctl_abandoned_ = false;  // the host layer frees the queued control transfer
  halt_in_ = false;
  halt_out_ = false;
  recover_req_ = false;
  rx_len_ = 0;
  if (state_ == QueryState::Busy || state_ == QueryState::Recovering) {
    err_ = "DISCONNECT";
    state_ = QueryState::Error;
  }
  // phase_ and rec_ are loop-owned; Poll() drops a recovery whose device has
  // gone, and acts on phase_ only while Busy.
}

void USBTMCDevice::rx_callback(const Transfer_t *transfer) {
  if (transfer->driver) ((USBTMCDevice *)(transfer->driver))->rx_data(transfer);
}

void USBTMCDevice::tx_callback(const Transfer_t *transfer) {
  if (transfer->driver) ((USBTMCDevice *)(transfer->driver))->tx_data(transfer);
}

void USBTMCDevice::rx_error_callback(const Transfer_t *transfer) {
  if (transfer->driver) ((USBTMCDevice *)(transfer->driver))->XferError(true);
}

void USBTMCDevice::tx_error_callback(const Transfer_t *transfer) {
  if (transfer->driver) ((USBTMCDevice *)(transfer->driver))->XferError(false);
}

// ISR context: the transfer is gone (freed by followup_Error, which also
// reset the pipe's overlay, data toggle included, to DATA0). Release the
// slot, end a running query, and ask for the halt to be cleared on the
// device side too before anything else is sent.
void USBTMCDevice::XferError(bool in) {
  if (in) {
    in_queued_ = false;
    halt_in_ = true;
  } else {
    tx_busy_ = false;
    halt_out_ = true;
  }
  recover_req_ = true;
  if (state_ == QueryState::Busy) {
    err_ = in ? "XFER_ERR_IN" : "XFER_ERR_OUT";
    state_ = QueryState::Error;
  }
}

// ISR context: one of our control requests completed (enumeration.cpp calls
// this only for transfers whose driver is us). Its data, if any, is already
// in ctl_buf_. A request the device STALLs never lands here (USBHost_t36
// routes control-pipe errors to its enumeration retry); RecoverStep's step
// deadline covers that.
void USBTMCDevice::control(const Transfer_t *) {
  ctl_busy_ = false;
  // A late completion of a request RecoverFinish gave up on: its qTDs are
  // back in the pool, so a new resync may start (RecoverStep). Its data is
  // stale and the step it belonged to is over, so it does not count as done.
  if (ctl_abandoned_) {
    ctl_abandoned_ = false;
    return;
  }
  ctl_done_ = true;
}

// ISR context: copy what arrived, set the flag. Parsing is Poll()'s job.
void USBTMCDevice::rx_data(const Transfer_t *transfer) {
  uint32_t n = transfer->length - ((transfer->qtd.token >> 16) & 0x7FFF);
  if (n > kRxDmaLen) n = kRxDmaLen;
  // Anything longer than rx_copy_ holds cannot be a valid reply within the
  // 255-char cap; keep the true length so Poll() rejects it, copy what fits.
  memcpy(rx_copy_, rx_dma_, n < kRxCopyLen ? n : kRxCopyLen);
  rx_len_ = n;
  in_queued_ = false;
  rx_done_ = true;
}

void USBTMCDevice::tx_data(const Transfer_t *) { tx_busy_ = false; }

FLASHMEM void USBTMCDevice::Fail(QueryState s, const char *why) {
  err_ = why;
  phase_ = kNone;
  state_ = s;
  // Anything that ends a query mid-exchange leaves the meter out of step.
  if (s == QueryState::Timeout || s == QueryState::Error) recover_req_ = true;
}

FLASHMEM bool USBTMCDevice::QueueOut(uint32_t len) {
  UsbIrqGuard g;
  if (!txpipe_ || tx_busy_) return false;
  tx_busy_ = true;
  if (!queue_Data_Transfer(txpipe_, tx_buf_, len, this)) {
    tx_busy_ = false;
    return false;
  }
  return true;
}

FLASHMEM bool USBTMCDevice::SendRequest() {
  tag_ = UsbTmc::NextTag(tag_);
  req_tag_ = tag_;
  UsbTmc::BuildRequestIn(tag_, (uint32_t)(kMaxReply - reply_len_), tx_buf_);
  if (!QueueOut(UsbTmc::kHeaderLen)) return false;
  phase_ = kOutReq;
  return true;
}

// At most one IN transfer is ever in the controller. One left over from a
// query that timed out is reused rather than doubled up.
FLASHMEM bool USBTMCDevice::EnsureInQueued() {
  UsbIrqGuard g;
  if (!rxpipe_) return false;
  if (in_queued_ || rx_done_) return true;
  in_queued_ = true;
  if (!queue_Data_Transfer(rxpipe_, rx_dma_, kRxDmaLen, this)) {
    in_queued_ = false;
    return false;
  }
  return true;
}

// One control request on the device's control pipe. The setup bytes are the
// host-tested UsbTmc framing, copied as-is (setup_t is the wire layout).
FLASHMEM bool USBTMCDevice::StartControl(const UsbTmc::Setup &s) {
  UsbIrqGuard g;
  Device_t *dev = device;
  if (!dev || ctl_busy_) return false;
  uint8_t raw[8];
  UsbTmc::PackSetup(s, raw);
  memcpy(&ctl_setup_, raw, sizeof(raw));
  memset(ctl_buf_, 0, sizeof(ctl_buf_));
  ctl_done_ = false;
  ctl_busy_ = true;
  if (!queue_Control_Transfer(dev, &ctl_setup_, s.wLength ? ctl_buf_ : nullptr, this)) {
    ctl_busy_ = false;
    return false;
  }
  step_ms_ = millis();
  return true;
}

FLASHMEM void USBTMCDevice::RecoverFinish(bool ok) {
  rec_ = rNone;
  // A request we gave up on (its step deadline, or the whole resync's, passed)
  // may still be queued in the controller, holding its qTDs from the pool the
  // host shares with MIDI. Do not queue another behind it: ctl_busy_ stays
  // set, and no new resync starts (RecoverStep) until it completes (control()),
  // the device goes (disconnect()), or the controller drops it on an error
  // (RecoverStep sees the control pipe inactive). A meter that never answers
  // then holds one control transfer's qTDs, not one set per resync.
  if (ctl_busy_) ctl_abandoned_ = true;
  if (state_ == QueryState::Recovering) {
    if (ok) {
      state_ = QueryState::Idle;
      err_ = "";
    } else {
      state_ = QueryState::Error;
      err_ = "RECOVER_FAILED";
    }
  } else if (!ok) {
    err_ = "RECOVER_FAILED";
  }
}

// One non-blocking step of the resync (see the header). Runs with the host
// interrupt masked, from Poll().
FLASHMEM void USBTMCDevice::RecoverStep() {
  if (!connected()) {
    rec_ = rNone;
    recover_req_ = false;
    return;
  }
  const uint32_t now = millis();
  if (rec_ == rNone) {
    if (!recover_req_) return;
    // A control request from the last resync is still queued: wait for it
    // (recover_req_ stays set, so queries report Recovering meanwhile).
    if (ctl_abandoned_) {
      // ...unless the controller is no longer working on it. A request the
      // device STALLs is freed by the host layer's error follow-up and never
      // reaches control() (see there), which leaves the control pipe's
      // overlay inactive; a request still NAKed keeps it Active.
      Device_t *dev = device;
      if (dev && dev->control_pipe && (dev->control_pipe->qh.token & 0x80u)) return;
      ctl_abandoned_ = false;
      ctl_busy_ = false;
    }
    recover_req_ = false;
    rec_ = rNext;
    rec_start_ms_ = now;
  }
  if (now - rec_start_ms_ >= kRecoverMs) { RecoverFinish(false); return; }
  // Bulk-IN data arriving during the resync is the meter flushing what it
  // had queued: drop it.
  if (rx_done_) rx_done_ = false;
  const bool waiting = rec_ == rHaltIn || rec_ == rHaltOut || rec_ == rInitClear ||
                       rec_ == rCheck || rec_ == rFinalOutWait;
  if (waiting && !ctl_done_) {
    if (now - step_ms_ >= kCtlStepMs) RecoverFinish(false);
    return;
  }
  uint8_t st = 0;
  bool more = false;
  switch (rec_) {
    case rHaltIn:
      halt_in_ = false;
      rec_ = rNext;
      break;
    case rHaltOut:
      halt_out_ = false;
      rec_ = rNext;
      break;
    case rInitClear:
      if (!UsbTmc::ParseInitiateClearReply(ctl_buf_, 1, &st) || st != UsbTmc::kStatusSuccess) {
        // Not accepted (e.g. nothing to clear): the halts are cleared, which
        // is as far as this meter lets us go.
        RecoverFinish(true);
        return;
      }
      rec_ = rCheckGap;
      step_ms_ = now;
      return;
    case rCheck:
      UsbTmc::ParseCheckClearReply(ctl_buf_, 2, &st, &more);
      if (st == UsbTmc::kStatusPending) {
        if (more) EnsureInQueued();  // the meter needs Bulk-IN read to finish
        rec_ = rCheckGap;
        step_ms_ = now;
        return;
      }
      if (st != UsbTmc::kStatusSuccess) { RecoverFinish(true); return; }
      rec_ = rFinalOut;
      break;
    case rFinalOutWait:
      // CLEAR_FEATURE reset the meter's Bulk-OUT toggle to DATA0; do the same
      // on our side. Safe here: no OUT transfer is queued (rFinalOut waited
      // for that), so the controller is not using this QH's overlay.
      if (txpipe_ && !tx_busy_) txpipe_->qh.token &= ~0x80000000u;
      RecoverFinish(true);
      return;
    default:
      break;
  }
  switch (rec_) {
    case rNext:
      if (halt_in_) {
        rec_ = StartControl(UsbTmc::ClearEndpointHalt(in_addr_)) ? rHaltIn : rNext;
      } else if (halt_out_) {
        rec_ = StartControl(UsbTmc::ClearEndpointHalt(out_addr_)) ? rHaltOut : rNext;
      } else {
        rec_ = StartControl(UsbTmc::InitiateClear(iface_)) ? rInitClear : rNext;
      }
      break;
    case rCheckGap:
      if (now - step_ms_ < kCheckGapMs) break;
      if (StartControl(UsbTmc::CheckClearStatus(iface_))) rec_ = rCheck;
      break;
    case rFinalOut:
      if (tx_busy_) break;  // a stuck OUT must leave the controller first
      if (StartControl(UsbTmc::ClearEndpointHalt(out_addr_))) rec_ = rFinalOutWait;
      break;
    default:
      break;
  }
}

FLASHMEM bool USBTMCDevice::BeginQuery(const char *scpi, uint32_t timeout_ms) {
  if (state_ == QueryState::Busy) return false;
  reply_[0] = 0;
  reply_len_ = 0;
  err_ = "";
  // Build the exact line to frame first, then check THAT against the
  // allowlist: a newline is appended when missing (ScpiAllowed accepts one).
  // Anything that does not fit line_ is far past ScpiAllowed's 64-char cap.
  size_t n = scpi ? strlen(scpi) : 0;
  const bool add_nl = n == 0 || scpi[n - 1] != '\n';
  if (!scpi || n + (add_nl ? 1 : 0) + 1 > sizeof(line_)) {
    Fail(QueryState::Refused, "");
    return false;
  }
  memcpy(line_, scpi, n);
  if (add_nl) line_[n++] = '\n';
  line_[n] = 0;
  if (!UsbTmc::ScpiAllowed(line_)) { Fail(QueryState::Refused, ""); return false; }
  expect_reply_ = n >= 2 && line_[n - 2] == '?';

  // From here to Busy, the host interrupt must not interleave: a disconnect
  // between the checks and the queue call would leave a query on a dead pipe.
  UsbIrqGuard g;
  if (!connected()) { Fail(QueryState::Error, "NOT_CONNECTED"); return false; }
  if (rec_ != rNone || recover_req_) {
    err_ = "RECOVERING";
    state_ = QueryState::Recovering;
    return false;
  }
  if (tx_busy_) { Fail(QueryState::Error, "OUT_STUCK"); return false; }
  rx_done_ = false;  // a completed-but-unread IN from an abandoned query is stale
  stale_drained_ = 0;
  tag_ = UsbTmc::NextTag(tag_);
  const size_t len = UsbTmc::BuildMsgOut(tag_, line_, tx_buf_, sizeof(tx_buf_));
  if (!len) { Fail(QueryState::Error, "FRAME"); return false; }
  start_ms_ = millis();
  timeout_ms_ = timeout_ms;
  if (!QueueOut((uint32_t)len)) { Fail(QueryState::Error, "QUEUE"); return false; }
  phase_ = kOutCmd;
  state_ = QueryState::Busy;
  return true;
}

FLASHMEM USBTMCDevice::QueryState USBTMCDevice::Poll() {
  // The whole step runs with the host interrupt masked (a few microseconds,
  // never a wait): the ISR side clears the flags read below, and must not do
  // so between a read and the decision taken on it.
  UsbIrqGuard g;
  if (state_ != QueryState::Busy) {
    if (rec_ != rNone || recover_req_) RecoverStep();
    return state_;
  }
  switch (phase_) {
    case kOutCmd:
      if (tx_busy_) break;
      if (!expect_reply_) {
        phase_ = kNone;
        state_ = QueryState::Done;
        return state_;
      }
      if (!SendRequest()) { Fail(QueryState::Error, "QUEUE"); return state_; }
      break;
    case kOutReq:
      if (tx_busy_) break;
      if (!EnsureInQueued()) { Fail(QueryState::Error, "QUEUE"); return state_; }
      phase_ = kIn;
      break;
    case kIn: {
      if (!rx_done_) break;
      const uint32_t got = rx_len_;
      rx_done_ = false;
      if (got <= kRxCopyLen && UsbTmc::IsStaleMsgIn(rx_copy_, got, req_tag_)) {
        // A late answer to an abandoned request: drop it and keep waiting
        // for ours, which the meter sends next.
        if (++stale_drained_ > kMaxStaleDrain) {
          Fail(QueryState::Error, "BAD_REPLY");
          return state_;
        }
        if (!EnsureInQueued()) { Fail(QueryState::Error, "QUEUE"); return state_; }
        break;
      }
      if (got > kRxCopyLen) { Fail(QueryState::Error, "OVERFLOW"); return state_; }
      const uint8_t *payload = nullptr;
      uint32_t plen = 0;
      bool eom = false;
      if (!UsbTmc::ParseMsgIn(rx_copy_, got, req_tag_, &payload, &plen, &eom)) {
        Fail(QueryState::Error, "BAD_REPLY");
        return state_;
      }
      if (plen > kMaxReply - reply_len_) { Fail(QueryState::Error, "OVERFLOW"); return state_; }
      memcpy(reply_ + reply_len_, payload, plen);
      reply_len_ += plen;
      reply_[reply_len_] = 0;
      if (eom) {
        phase_ = kNone;
        state_ = QueryState::Done;
        return state_;
      }
      // EOM clear: the meter has more. Ask again and append, within the cap.
      if (plen == 0) { Fail(QueryState::Error, "NO_PROGRESS"); return state_; }
      if (reply_len_ >= kMaxReply) { Fail(QueryState::Error, "OVERFLOW"); return state_; }
      if (!SendRequest()) { Fail(QueryState::Error, "QUEUE"); return state_; }
      break;
    }
    case kNone:
      Fail(QueryState::Error, "STATE");
      return state_;
  }
  if (state_ == QueryState::Busy && millis() - start_ms_ >= timeout_ms_) {
    Fail(QueryState::Timeout, "");
  }
  return state_;
}

#endif  // ARDUINO_TEENSY41
