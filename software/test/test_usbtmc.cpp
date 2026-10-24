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

int main() {
  test_msg_out();
  test_request_in();
  test_parse_in();
  test_next_tag();
  test_allowlist();
  test_reading();
  printf("test_usbtmc: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
