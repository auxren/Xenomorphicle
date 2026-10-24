#ifndef USBTMC_H_
#define USBTMC_H_

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Pure logic for talking to a USB bench DMM (Siglent SDM3065X) over USBTMC:
// the 12-byte Bulk-OUT/IN message framing (USB-IF USBTMC 1.0), the allowlist
// of SCPI lines the firmware may ever send to the meter, and the parser for
// its readings. No Arduino or USB-host dependency -- host-tested by
// test/test_usbtmc.cpp, same split as CvMeterMath.h/.cpp. The USB host driver
// that moves these bytes lives elsewhere.
// ---------------------------------------------------------------------------

namespace UsbTmc {

constexpr uint8_t kDevDepMsgOut = 1;       // MsgID of a command to the meter
constexpr uint8_t kRequestDevDepMsgIn = 2; // MsgID of a read request and of the reply
constexpr size_t kHeaderLen = 12;

// Header + payload (EOM set), zero-padded to a multiple of 4. Returns the
// total length, or 0 if scpi/out is null or it does not fit in cap.
size_t BuildMsgOut(uint8_t tag, const char *scpi, uint8_t *out, size_t cap);

// The 12-byte REQUEST_DEV_DEP_MSG_IN header, TermCharEnabled off. out must
// hold 12 bytes. Returns 12.
size_t BuildRequestIn(uint8_t tag, uint32_t max_len, uint8_t *out);

// Validates a DEV_DEP_MSG_IN reply (MsgID, tag, ~tag, TransferSize within the
// buffer) and points payload at the data inside buf.
bool ParseMsgIn(const uint8_t *buf, size_t len, uint8_t expect_tag,
                const uint8_t **payload, uint32_t *payload_len, bool *eom);

// bTag runs 1..255; 0 is not a legal tag.
uint8_t NextTag(uint8_t tag);

// True only for the exact lines the calibration routine needs: *IDN?, *CLS,
// CONF:VOLT:DC <range>, VOLT:DC:NPLC <n>, VOLT:DC:AZ <ON|OFF|ONCE>,
// TRIG:SOUR IMM, READ?, SYST:ERR?. Short or long keyword forms, any case, one
// optional trailing '\n'. Everything else (every CAL..., other SYST:..., any
// ';' compound) is refused, so a bug elsewhere cannot make the meter change
// its own calibration.
bool ScpiAllowed(const char *line);

// SCPI NR3 reading (+1.23456789E+00). Rejects garbage, nan/inf and the
// meter's 9.9E37 overload marker (|v| >= 9.9E37).
bool ParseReading(const char *reply, double *volts);

// A SYST:ERR? reply that says the error queue is empty: the error number is 0
// (SCPI "0,\"No error\"", also "+0,..."; the SDM3065X answers
// 0,"No error"), after optional leading blanks. Anything else -- another
// number, no number, an empty or missing reply -- is not "no error".
bool NoScpiError(const char *reply);

// ---------------------------------------------------------------------------
// Control requests: the USBTMC 1.0 class requests (section 4.2.1) and the
// standard CLEAR_FEATURE(ENDPOINT_HALT), as setup-packet fields, plus the
// parsers for their short status replies. The driver sends these on the
// control pipe to resynchronise the meter after a timeout or a bulk error.
// ---------------------------------------------------------------------------

constexpr uint8_t kInitiateAbortBulkOut = 1;
constexpr uint8_t kCheckAbortBulkOutStatus = 2;
constexpr uint8_t kInitiateAbortBulkIn = 3;
constexpr uint8_t kCheckAbortBulkInStatus = 4;
constexpr uint8_t kInitiateClear = 5;
constexpr uint8_t kCheckClearStatus = 6;

// USBTMC_status values (Table 16).
constexpr uint8_t kStatusSuccess = 0x01;
constexpr uint8_t kStatusPending = 0x02;
constexpr uint8_t kStatusFailed = 0x80;
constexpr uint8_t kStatusTransferNotInProgress = 0x81;
constexpr uint8_t kStatusSplitNotInProgress = 0x82;
constexpr uint8_t kStatusSplitInProgress = 0x83;

struct Setup {
  uint8_t bmRequestType;
  uint8_t bRequest;
  uint16_t wValue;
  uint16_t wIndex;
  uint16_t wLength;
};

// ep_out / ep_in are endpoint numbers or addresses; the direction bit is
// forced (0x00 OUT, 0x80 IN) in wIndex. iface is bInterfaceNumber.
Setup InitiateAbortBulkOut(uint8_t tag, uint8_t ep_out);  // A2 01, wLength 2
Setup CheckAbortBulkOutStatus(uint8_t ep_out);            // A2 02, wLength 8
Setup InitiateAbortBulkIn(uint8_t tag, uint8_t ep_in);    // A2 03, wLength 2
Setup CheckAbortBulkInStatus(uint8_t ep_in);              // A2 04, wLength 8
Setup InitiateClear(uint8_t iface);                       // A1 05, wLength 1
Setup CheckClearStatus(uint8_t iface);                    // A1 06, wLength 2
// Standard request: bmRequestType 02 (endpoint), bRequest 01 CLEAR_FEATURE,
// wValue 0 ENDPOINT_HALT, wIndex = the full endpoint address.
Setup ClearEndpointHalt(uint8_t ep_addr);

// The 8 bytes on the wire, little-endian (USB 2.0 9.3).
void PackSetup(const Setup &s, uint8_t out[8]);

// INITIATE_ABORT_BULK_OUT/IN reply: [status, bTag]. False if len < 2.
bool ParseInitiateAbortReply(const uint8_t *buf, size_t len, uint8_t *status, uint8_t *tag);
// CHECK_ABORT_BULK_OUT/IN_STATUS reply: [status, bmAbortBulkIn, 0, 0,
// NBYTES (4, LE)]. more_in = bmAbortBulkIn D0 (only meaningful for IN: the
// host must read Bulk-IN again). False if len < 8.
bool ParseCheckAbortReply(const uint8_t *buf, size_t len, uint8_t *status, bool *more_in,
                          uint32_t *nbytes);
// INITIATE_CLEAR reply: [status]. False if len < 1.
bool ParseInitiateClearReply(const uint8_t *buf, size_t len, uint8_t *status);
// CHECK_CLEAR_STATUS reply: [status, bmClear]. more_in = bmClear D0 (the
// device has Bulk-IN data the host must read). False if len < 2.
bool ParseCheckClearReply(const uint8_t *buf, size_t len, uint8_t *status, bool *more_in);

// True when buf is a well-formed DEV_DEP_MSG_IN header (MsgID, tag/~tag pair)
// whose tag is NOT expect_tag: a late answer to an abandoned request, to be
// discarded while waiting for the current one.
bool IsStaleMsgIn(const uint8_t *buf, size_t len, uint8_t expect_tag);

}  // namespace UsbTmc

#endif  // USBTMC_H_
