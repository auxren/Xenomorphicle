// Host tests for the USBTMC framing, SCPI allowlist and reading parser
// (src/UsbTmc.h). Standalone plain asserts, same style as test_cvmeter_math.
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_usbtmc
//   test_usbtmc.cpp ../src/UsbTmc.cpp && ./build/test_usbtmc
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/UsbTmc.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void test_msg_out() {
  uint8_t b[64];
  const uint8_t want[20] = {0x01, 0x01, 0xFE, 0x00, 0x06, 0x00, 0x00, 0x00,
                            0x01, 0x00, 0x00, 0x00, '*', 'I', 'D', 'N', '?',
                            '\n', 0x00, 0x00};
  memset(b, 0xAA, sizeof b);
  CHECK(UsbTmc::BuildMsgOut(1, "*IDN?\n", b, sizeof b) == 20);
  CHECK(memcmp(b, want, 20) == 0);
  CHECK(b[20] == 0xAA);  // wrote nothing past the padded length
  // Exact fit and one short.
  CHECK(UsbTmc::BuildMsgOut(1, "*IDN?\n", b, 20) == 20);
  CHECK(UsbTmc::BuildMsgOut(1, "*IDN?\n", b, 19) == 0);
  // Already a multiple of 4: no padding.
  CHECK(UsbTmc::BuildMsgOut(2, "READ?\n\n\n", b, sizeof b) == 20);
  // Empty payload is just the header.
  CHECK(UsbTmc::BuildMsgOut(3, "", b, sizeof b) == 12);
  CHECK(UsbTmc::BuildMsgOut(3, nullptr, b, sizeof b) == 0);
  CHECK(UsbTmc::BuildMsgOut(3, "x", nullptr, 64) == 0);
}

static void test_request_in() {
  uint8_t b[12];
  const uint8_t want[12] = {0x02, 0x07, 0xF8, 0x00, 0x00, 0x01, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00};
  memset(b, 0xAA, sizeof b);
  CHECK(UsbTmc::BuildRequestIn(7, 256, b) == 12);
  CHECK(memcmp(b, want, 12) == 0);
}

static void test_parse_in() {
  // Reply: tag 5, 4 bytes "+1\n" + pad, EOM.
  uint8_t r[16] = {0x02, 0x05, 0xFA, 0x00, 0x03, 0x00, 0x00, 0x00,
                   0x01, 0x00, 0x00, 0x00, '+', '1', '\n', 0x00};
  const uint8_t *p = nullptr;
  uint32_t n = 0;
  bool eom = false;
  CHECK(UsbTmc::ParseMsgIn(r, 16, 5, &p, &n, &eom));
  CHECK(p == r + 12 && n == 3 && eom);
  r[8] = 0;
  CHECK(UsbTmc::ParseMsgIn(r, 16, 5, &p, &n, &eom) && !eom);
  r[8] = 1;
  CHECK(!UsbTmc::ParseMsgIn(r, 16, 6, &p, &n, &eom));   // wrong tag
  r[2] = 0xFB;
  CHECK(!UsbTmc::ParseMsgIn(r, 16, 5, &p, &n, &eom));   // wrong ~tag
  r[2] = 0xFA;
  r[4] = 5;                                             // 5 > 16-12
  CHECK(!UsbTmc::ParseMsgIn(r, 16, 5, &p, &n, &eom));
  r[4] = 4;
  CHECK(UsbTmc::ParseMsgIn(r, 16, 5, &p, &n, &eom) && n == 4);
  r[7] = 1;                                             // huge size
  CHECK(!UsbTmc::ParseMsgIn(r, 16, 5, &p, &n, &eom));
  r[7] = 0;
  r[0] = 1;                                             // MsgID != 2
  CHECK(!UsbTmc::ParseMsgIn(r, 16, 5, &p, &n, &eom));
  r[0] = 2;
  CHECK(!UsbTmc::ParseMsgIn(r, 11, 5, &p, &n, &eom));   // short header
  CHECK(!UsbTmc::ParseMsgIn(nullptr, 16, 5, &p, &n, &eom));
}

static void test_next_tag() {
  CHECK(UsbTmc::NextTag(255) == 1);
  CHECK(UsbTmc::NextTag(1) == 2);
  CHECK(UsbTmc::NextTag(0) == 1);
  CHECK(UsbTmc::NextTag(254) == 255);
}

static void test_allowlist() {
  using UsbTmc::ScpiAllowed;
  const char *yes[] = {
    "READ?", "READ?\n", "read?", "*IDN?", "*idn?\n", "*CLS", "*cls",
    "conf:volt:dc 20", "CONFigure:VOLTage:DC 20\n", "CONF:VOLT:DC 0.2",
    "CONF:VOLT:DC AUTO", "conf:volt:dc max",
    "VOLTage:DC:NPLC 10", "VOLT:DC:NPLCycles 10", "VOLT:DC:NPLC 0.5",
    "VOLT:DC:AZ ON", "volt:dc:az off", "VOLTage:DC:AZ ONCE",
    "TRIG:SOUR IMM", "TRIGger:SOURce IMMediate", "trig:sour imm\n",
    "SYST:ERR?", "SYSTem:ERRor?", "syst:err?\n",
  };
  for (const char *s : yes) {
    bool ok = ScpiAllowed(s);
    if (!ok) printf("  should allow: %s\n", s);
    CHECK(ok);
  }
  const char *no[] = {
    "CAL:VAL 1", "CALibration:SECure:STATe OFF", "CAL:SEC:STAT OFF,1234",
    "SYST:PRES", "SYST:LOC", "SYST:ERR", "*RST", "*IDN", "MEAS:VOLT:DC?",
    "", "\n", "READ?;CAL:STOR", "*CLS;*RST", "READ? ", "READ?\n\n",
    "READ?x", "CONF:VOLT:DC", "CONF:VOLT:DC 20 ", "CONF:VOLT:DC abc",
    "CONF:VOLT:DC 1e", "CONF:VOLT:DC nan", "CONF:VOLT:DC 20;CAL:STOR",
    "VOLT:DC:AZ MAYBE", "VOLT:DC:AZ", "VOLT:DC:NPLC", "TRIG:SOUR BUS",
    "TRIG:SOUR IMMX", "CONF:CURR:DC 1", "CONF:VOLT:AC 1", "VOLT:DC:NPL 10",
    "CONFI:VOLT:DC 1", ":READ?", "CONF:VOLT:DC\t20", "CONF:VOLT:DC 20\r",
    "VOLT:DC:AZ ON,CAL", "SYST:ERR?:CAL", nullptr,
  };
  for (const char *s : no) {
    bool ok = ScpiAllowed(s);
    if (ok) printf("  should refuse: %s\n", s ? s : "(null)");
    CHECK(!ok);
  }
}

static void test_reading() {
  double v = 0;
  CHECK(UsbTmc::ParseReading("+1.19998765E+00\n", &v));
  CHECK(fabs(v - 1.19998765) < 1e-12);
  CHECK(UsbTmc::ParseReading("-3.5E-03", &v) && fabs(v + 0.0035) < 1e-15);
  CHECK(UsbTmc::ParseReading("0", &v) && v == 0.0);
  CHECK(UsbTmc::ParseReading("1.5\r\n", &v) && v == 1.5);
  CHECK(!UsbTmc::ParseReading("9.9E37", &v));
  CHECK(!UsbTmc::ParseReading("+9.91000000E+37\n", &v));
  CHECK(!UsbTmc::ParseReading("-9.9E37", &v));
  CHECK(!UsbTmc::ParseReading("", &v));
  CHECK(!UsbTmc::ParseReading("\n", &v));
  CHECK(!UsbTmc::ParseReading("abc", &v));
  CHECK(!UsbTmc::ParseReading("1.0x", &v));
  CHECK(!UsbTmc::ParseReading("nan", &v));
  CHECK(!UsbTmc::ParseReading("inf", &v));
  CHECK(!UsbTmc::ParseReading("1e999", &v));
  CHECK(!UsbTmc::ParseReading("1.0,2.0", &v));
  CHECK(!UsbTmc::ParseReading(nullptr, &v));
  CHECK(!UsbTmc::ParseReading("1.0", nullptr));
}

static bool SetupIs(const UsbTmc::Setup &s, const uint8_t (&want)[8]) {
  uint8_t b[8];
  UsbTmc::PackSetup(s, b);
  return memcmp(b, want, 8) == 0;
}

static void test_control_setup() {
  // USBTMC 1.0 Tables 18, 20, 22, 24, 28, 30; wIndex is the endpoint address
  // (direction bit forced) or the interface number.
  static const uint8_t ab_out[8] = {0xA2, 0x01, 0x07, 0x00, 0x02, 0x00, 0x02, 0x00};
  CHECK(SetupIs(UsbTmc::InitiateAbortBulkOut(7, 0x02), ab_out));
  CHECK(SetupIs(UsbTmc::InitiateAbortBulkOut(7, 0x82), ab_out));  // dir bit cleared
  static const uint8_t ck_out[8] = {0xA2, 0x02, 0x00, 0x00, 0x02, 0x00, 0x08, 0x00};
  CHECK(SetupIs(UsbTmc::CheckAbortBulkOutStatus(2), ck_out));
  static const uint8_t ab_in[8] = {0xA2, 0x03, 0xFF, 0x00, 0x81, 0x00, 0x02, 0x00};
  CHECK(SetupIs(UsbTmc::InitiateAbortBulkIn(255, 0x01), ab_in));
  CHECK(SetupIs(UsbTmc::InitiateAbortBulkIn(255, 0x81), ab_in));
  static const uint8_t ck_in[8] = {0xA2, 0x04, 0x00, 0x00, 0x81, 0x00, 0x08, 0x00};
  CHECK(SetupIs(UsbTmc::CheckAbortBulkInStatus(1), ck_in));
  static const uint8_t clr[8] = {0xA1, 0x05, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00};
  CHECK(SetupIs(UsbTmc::InitiateClear(3), clr));
  static const uint8_t ck_clr[8] = {0xA1, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00};
  CHECK(SetupIs(UsbTmc::CheckClearStatus(0), ck_clr));
  // USB 2.0 9.4.1: CLEAR_FEATURE(ENDPOINT_HALT), recipient endpoint.
  static const uint8_t halt_in[8] = {0x02, 0x01, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00};
  CHECK(SetupIs(UsbTmc::ClearEndpointHalt(0x81), halt_in));
  static const uint8_t halt_out[8] = {0x02, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
  CHECK(SetupIs(UsbTmc::ClearEndpointHalt(0x02), halt_out));
  // Fields too, not only bytes.
  const UsbTmc::Setup s = UsbTmc::InitiateAbortBulkIn(9, 3);
  CHECK(s.bmRequestType == 0xA2 && s.bRequest == UsbTmc::kInitiateAbortBulkIn &&
        s.wValue == 9 && s.wIndex == 0x83 && s.wLength == 2);
}

static void test_control_replies() {
  uint8_t st = 0, tag = 0;
  bool more = true;
  uint32_t nb = 0;
  static const uint8_t ia[2] = {UsbTmc::kStatusSuccess, 0x2A};
  CHECK(UsbTmc::ParseInitiateAbortReply(ia, 2, &st, &tag) && st == 0x01 && tag == 0x2A);
  CHECK(!UsbTmc::ParseInitiateAbortReply(ia, 1, &st, &tag));
  CHECK(!UsbTmc::ParseInitiateAbortReply(nullptr, 2, &st, &tag));
  static const uint8_t ca[8] = {UsbTmc::kStatusPending, 0x01, 0, 0, 0x34, 0x12, 0x00, 0x01};
  CHECK(UsbTmc::ParseCheckAbortReply(ca, 8, &st, &more, &nb) && st == 0x02 && more &&
        nb == 0x01001234u);
  static const uint8_t ca2[8] = {UsbTmc::kStatusSuccess, 0xFE, 0, 0, 0, 0, 0, 0};
  CHECK(UsbTmc::ParseCheckAbortReply(ca2, 8, &st, &more, &nb) && st == 0x01 && !more && nb == 0);
  CHECK(!UsbTmc::ParseCheckAbortReply(ca, 7, &st, &more, &nb));
  static const uint8_t ic[1] = {UsbTmc::kStatusFailed};
  CHECK(UsbTmc::ParseInitiateClearReply(ic, 1, &st) && st == 0x80);
  CHECK(!UsbTmc::ParseInitiateClearReply(ic, 0, &st));
  static const uint8_t cc[2] = {UsbTmc::kStatusPending, 0x01};
  CHECK(UsbTmc::ParseCheckClearReply(cc, 2, &st, &more) && st == 0x02 && more);
  static const uint8_t cc2[2] = {UsbTmc::kStatusSuccess, 0x00};
  CHECK(UsbTmc::ParseCheckClearReply(cc2, 2, &st, &more) && st == 0x01 && !more);
  CHECK(!UsbTmc::ParseCheckClearReply(cc, 1, &st, &more));
  // Null outputs are allowed.
  CHECK(UsbTmc::ParseCheckClearReply(cc, 2, nullptr, nullptr));
}

static void test_stale_msg_in() {
  uint8_t r[16] = {0x02, 0x05, 0xFA, 0x00, 0x04, 0x00, 0x00, 0x00,
                   0x01, 0x00, 0x00, 0x00, '+', '1', '\n', 0x00};
  CHECK(UsbTmc::IsStaleMsgIn(r, 16, 6));    // reply to tag 5, waiting for 6
  CHECK(!UsbTmc::IsStaleMsgIn(r, 16, 5));   // the current one, not stale
  CHECK(!UsbTmc::IsStaleMsgIn(r, 11, 6));   // short: garbage, not stale
  r[2] = 0x00;
  CHECK(!UsbTmc::IsStaleMsgIn(r, 16, 6));   // broken ~tag: garbage
  r[2] = 0xFA;
  r[0] = 0x01;
  CHECK(!UsbTmc::IsStaleMsgIn(r, 16, 6));   // wrong MsgID: garbage
  CHECK(!UsbTmc::IsStaleMsgIn(nullptr, 16, 6));
}

int main() {
  test_control_setup();
  test_control_replies();
  test_stale_msg_in();
  test_msg_out();
  test_request_in();
  test_parse_in();
  test_next_tag();
  test_allowlist();
  test_reading();
  printf("test_usbtmc: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
