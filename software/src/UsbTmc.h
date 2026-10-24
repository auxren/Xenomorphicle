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

}  // namespace UsbTmc

#endif  // USBTMC_H_
