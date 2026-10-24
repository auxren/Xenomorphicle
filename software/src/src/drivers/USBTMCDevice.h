#ifndef USBTMCDEVICE_H_
#define USBTMCDEVICE_H_

// ---------------------------------------------------------------------------
// USBTMC host driver for a bench DMM on the module's USB host port (verified
// against a Siglent SDM3065X: VID F4EC, PID 1203, high speed, one interface
// FE/03/01 with bulk OUT, bulk IN and interrupt IN). Claims any interface of
// class 0xFE subclass 0x03, by class and never by VID/PID, and opens the two
// bulk pipes only.
//
// One query at a time, never blocking:
//   BeginQuery("READ?", 3000)    frames one allowlisted SCPI line
//   Poll() from loop()           OUT -> REQUEST_DEV_DEP_MSG_IN -> IN, repeated
//                                while the meter answers with EOM clear
//   Reply()                      the NUL-terminated answer once Done
//
// The framing, the allowlist and the reading parser are the host-tested pure
// logic in UsbTmc.h. BeginQuery runs UsbTmc::ScpiAllowed on exactly the bytes
// it frames, so nothing outside the allowlist can reach the meter (in
// particular nothing that touches its own calibration).
//
// Memory: the object's member arrays are EHCI DMA buffers and qTD/QH pools.
// USBHost_t36 does no cache maintenance, so the instance MUST live in DTCM
// (a plain global, like usbHostMIDI in Main.cpp), never DMAMEM/RAM2/EXTMEM.
//
// Context: the transfer callbacks and disconnect() run in the USB host
// interrupt. They only copy bytes and set flags; every decision is made in
// Poll(), from loop().
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <USBHost_t36.h>

#include "../../UsbTmc.h"

class USBTMCDevice : public USBDriver {
public:
  enum class QueryState { Idle, Busy, Done, Refused, Timeout, Error };

  static constexpr size_t kMaxReply = 255;   // reply cap, chars
  static constexpr size_t kMaxLine = 64;     // ScpiAllowed's line cap

  explicit USBTMCDevice(USBHost &) { init(); }

  bool connected() const;
  uint16_t idVendor() const;
  uint16_t idProduct() const;

  // False, with state Refused, when the line is not on the allowlist; false,
  // with state Error, when no meter is attached or an earlier OUT transfer is
  // still stuck in the controller; false, state unchanged, while Busy. A line
  // ending in '?' expects a reply; any other completes after its OUT
  // transfer. A trailing '\n' is added if the line has none.
  bool BeginQuery(const char *scpi, uint32_t timeout_ms);

  // Advances the query; cheap when idle. Call every loop() pass.
  QueryState Poll();

  // The reply once Done (the meter's bytes, trailing "\n" included), up to
  // kMaxReply chars. Empty for a command that expects none.
  const char *Reply() const { return reply_; }

  // Why the last query ended in Error (or was not started): a short
  // upper-case token, "" when there is none.
  const char *ErrorName() const { return err_; }

protected:
  bool claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) override;
  void disconnect() override;

private:
  enum Phase : uint8_t { kNone, kOutCmd, kOutReq, kIn };

  void init();
  void Fail(QueryState s, const char *why);
  bool QueueOut(uint32_t len);      // tx_buf_[0, len)
  bool SendRequest();               // REQUEST_DEV_DEP_MSG_IN, next tag
  bool EnsureInQueued();
  static void rx_callback(const Transfer_t *transfer);
  static void tx_callback(const Transfer_t *transfer);
  void rx_data(const Transfer_t *transfer);
  void tx_data(const Transfer_t *transfer);

  // EHCI pools (hub.cpp / midi.cpp convention: 32-byte aligned).
  Pipe_t mypipes[2] __attribute__((aligned(32)));
  Transfer_t mytransfers[4] __attribute__((aligned(32)));

  // DMA buffers. The IN transfer is always one full high-speed packet long
  // (a multiple of the 64-byte full-speed packet too), so a device can never
  // babble past it.
  static constexpr size_t kRxDmaLen = 512;
  static constexpr size_t kTxLen = (UsbTmc::kHeaderLen + kMaxLine + 1 + 3) & ~size_t(3);
  static constexpr size_t kRxCopyLen = (UsbTmc::kHeaderLen + kMaxReply + 1 + 3) & ~size_t(3);
  uint8_t rx_dma_[kRxDmaLen] __attribute__((aligned(32)));
  uint8_t tx_buf_[kTxLen] __attribute__((aligned(32)));

  // Filled by rx_data (ISR), read by Poll().
  uint8_t rx_copy_[kRxCopyLen];
  volatile uint32_t rx_len_ = 0;        // bytes the device sent
  volatile bool rx_done_ = false;       // an IN transfer completed
  volatile bool in_queued_ = false;     // an IN transfer is in the controller
  volatile bool tx_busy_ = false;       // an OUT transfer is in the controller

  Pipe_t *volatile rxpipe_ = nullptr;
  Pipe_t *volatile txpipe_ = nullptr;
  uint16_t rx_size_ = 0;
  uint16_t tx_size_ = 0;

  volatile QueryState state_ = QueryState::Idle;
  Phase phase_ = kNone;
  bool expect_reply_ = false;
  uint8_t tag_ = 0;                     // last bTag used (1..255)
  uint8_t req_tag_ = 0;                 // bTag of the outstanding REQUEST
  uint32_t start_ms_ = 0;
  uint32_t timeout_ms_ = 0;
  char line_[kMaxLine + 2] = {};        // the framed line, '\n' included
  char reply_[kMaxReply + 1] = {};
  size_t reply_len_ = 0;
  const char *volatile err_ = "";
};

#if defined(ARDUINO_TEENSY41)
extern USBTMCDevice usbMeter;   // Main.cpp, in DTCM beside usbHostMIDI
#endif

#endif  // USBTMCDEVICE_H_
