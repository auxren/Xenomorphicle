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
  rx_size_ = in_size;
  tx_size_ = out_size;
  rx_done_ = false;
  in_queued_ = false;
  tx_busy_ = false;
  rx_len_ = 0;
  phase_ = kNone;
  state_ = QueryState::Idle;
  err_ = "";
  rxpipe_ = rx;
  txpipe_ = tx;
  return true;
}

// ISR context. The pipes and any queued transfers are freed by the host
// layer right after this returns; no callback will follow for them.
void USBTMCDevice::disconnect() {
  rxpipe_ = nullptr;
  txpipe_ = nullptr;
  rx_done_ = false;
  in_queued_ = false;
  tx_busy_ = false;
  rx_len_ = 0;
  if (state_ == QueryState::Busy) {
    err_ = "DISCONNECT";
    state_ = QueryState::Error;
  }
  // phase_ is loop-owned; Poll() never acts on it outside Busy.
}

void USBTMCDevice::rx_callback(const Transfer_t *transfer) {
  if (transfer->driver) ((USBTMCDevice *)(transfer->driver))->rx_data(transfer);
}

void USBTMCDevice::tx_callback(const Transfer_t *transfer) {
  if (transfer->driver) ((USBTMCDevice *)(transfer->driver))->tx_data(transfer);
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
  if (tx_busy_) { Fail(QueryState::Error, "OUT_STUCK"); return false; }
  rx_done_ = false;  // a completed-but-unread IN from an abandoned query is stale
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
  if (state_ != QueryState::Busy) return state_;  // includes disconnect -> Error
  // The whole step runs with the host interrupt masked (a few microseconds,
  // never a wait): disconnect() clears the flags read below, and must not do
  // so between a read and the decision taken on it.
  UsbIrqGuard g;
  if (state_ != QueryState::Busy) return state_;
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
