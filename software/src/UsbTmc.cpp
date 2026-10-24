#include "UsbTmc.h"

#include <stdlib.h>
#include <string.h>

namespace UsbTmc {

namespace {

void PutHeader(uint8_t *h, uint8_t id, uint8_t tag, uint32_t size) {
  h[0] = id;
  h[1] = tag;
  h[2] = (uint8_t)~tag;
  h[3] = 0;
  h[4] = (uint8_t)size;
  h[5] = (uint8_t)(size >> 8);
  h[6] = (uint8_t)(size >> 16);
  h[7] = (uint8_t)(size >> 24);
  h[8] = h[9] = h[10] = h[11] = 0;
}

char Up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

// Case-insensitive match of [s, s+n) against an uppercase literal.
bool EqN(const char *s, size_t n, const char *lit) {
  if (strlen(lit) != n) return false;
  for (size_t i = 0; i < n; i++)
    if (Up(s[i]) != lit[i]) return false;
  return true;
}

// Strictly [+-]digits[.digits][E[+-]digits] over [s, s+n), at least one
// mantissa digit. No hex, nan, inf or stray characters for strtod to accept.
bool IsNumber(const char *s, size_t n) {
  size_t i = 0;
  if (i < n && (s[i] == '+' || s[i] == '-')) i++;
  size_t digits = 0;
  while (i < n && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
  if (i < n && s[i] == '.') {
    i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
  }
  if (!digits) return false;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    size_t ed = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { i++; ed++; }
    if (!ed) return false;
  }
  return i == n;
}

// One allowlist entry: colon-separated keywords, each "SHORT" or
// "SHORT|LONG" (uppercase literals), optional '?', then an argument kind.
enum Arg { kNone, kNumberOrKeyword, kNumber, kAz, kImm };

struct Entry {
  const char *path;  // e.g. "CONF:VOLT:DC"; keywords written as SHORT/LONG
  Arg arg;
};

// Keyword pair "CONF/CONFIGURE": short form is the uppercase letters of the
// SCPI long form.
const Entry kEntries[] = {
  {"*IDN?", kNone},
  {"*CLS", kNone},
  {"CONF/CONFIGURE:VOLT/VOLTAGE:DC", kNumberOrKeyword},
  {"VOLT/VOLTAGE:DC:NPLC/NPLCYCLES", kNumber},
  {"VOLT/VOLTAGE:DC:AZ", kAz},
  {"TRIG/TRIGGER:SOUR/SOURCE", kImm},
  {"READ?", kNone},
  {"SYST/SYSTEM:ERR/ERROR?", kNone},
};

// Does head [h, h+hn) match the entry's path (keywords short or long)?
bool HeadMatches(const char *h, size_t hn, const char *path) {
  const char *p = path;
  size_t i = 0;
  for (;;) {
    // Next keyword in the entry: up to ':' or '?' or end.
    const char *kw = p;
    while (*p && *p != ':' && *p != '?') p++;
    const char *slash = (const char *)memchr(kw, '/', (size_t)(p - kw));
    size_t kwe = (size_t)(p - kw);
    // Next keyword in the line.
    size_t j = i;
    while (j < hn && h[j] != ':' && h[j] != '?') j++;
    bool ok;
    if (slash) {
      size_t sn = (size_t)(slash - kw);
      char sbuf[16], lbuf[24];
      size_t ln = kwe - sn - 1;
      if (sn >= sizeof sbuf || ln >= sizeof lbuf) return false;
      memcpy(sbuf, kw, sn); sbuf[sn] = 0;
      memcpy(lbuf, slash + 1, ln); lbuf[ln] = 0;
      ok = EqN(h + i, j - i, sbuf) || EqN(h + i, j - i, lbuf);
    } else {
      char kbuf[24];
      if (kwe >= sizeof kbuf) return false;
      memcpy(kbuf, kw, kwe); kbuf[kwe] = 0;
      ok = EqN(h + i, j - i, kbuf);
    }
    if (!ok) return false;
    i = j;
    if (*p == ':') {
      if (i >= hn || h[i] != ':') return false;
      p++; i++;
      continue;
    }
    // Entry ends here, optionally with '?'.
    if (*p == '?') {
      if (i >= hn || h[i] != '?') return false;
      p++; i++;
    }
    return *p == 0 && i == hn;
  }
}

bool ArgOk(Arg a, const char *s, size_t n) {
  switch (a) {
    case kNumberOrKeyword:
      return IsNumber(s, n) || EqN(s, n, "AUTO") || EqN(s, n, "MIN") ||
             EqN(s, n, "MAX") || EqN(s, n, "DEF");
    case kNumber: return IsNumber(s, n);
    case kAz: return EqN(s, n, "ON") || EqN(s, n, "OFF") || EqN(s, n, "ONCE");
    case kImm: return EqN(s, n, "IMM") || EqN(s, n, "IMMEDIATE");
    case kNone: return false;
  }
  return false;
}

}  // namespace

size_t BuildMsgOut(uint8_t tag, const char *scpi, uint8_t *out, size_t cap) {
  if (!scpi || !out) return 0;
  size_t n = strlen(scpi);
  if (n > 0xFFFFFFFFu - 3 - kHeaderLen) return 0;
  size_t total = (kHeaderLen + n + 3) & ~(size_t)3;
  if (total > cap) return 0;
  PutHeader(out, kDevDepMsgOut, tag, (uint32_t)n);
  out[8] = 0x01;  // EOM: the whole command is in this one message
  memcpy(out + kHeaderLen, scpi, n);
  memset(out + kHeaderLen + n, 0, total - kHeaderLen - n);
  return total;
}

size_t BuildRequestIn(uint8_t tag, uint32_t max_len, uint8_t *out) {
  PutHeader(out, kRequestDevDepMsgIn, tag, max_len);
  return kHeaderLen;
}

bool ParseMsgIn(const uint8_t *buf, size_t len, uint8_t expect_tag,
                const uint8_t **payload, uint32_t *payload_len, bool *eom) {
  if (!buf || len < kHeaderLen) return false;
  if (buf[0] != kRequestDevDepMsgIn) return false;
  if (buf[1] != expect_tag || buf[2] != (uint8_t)~expect_tag) return false;
  uint32_t size = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                  ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
  if (size > len - kHeaderLen) return false;
  if (payload) *payload = buf + kHeaderLen;
  if (payload_len) *payload_len = size;
  if (eom) *eom = (buf[8] & 0x01) != 0;
  return true;
}

uint8_t NextTag(uint8_t tag) { return tag >= 255 ? 1 : (uint8_t)(tag + 1); }

bool ScpiAllowed(const char *line) {
  if (!line) return false;
  size_t n = strlen(line);
  if (n && line[n - 1] == '\n') n--;
  if (n == 0 || n > 64) return false;
  // Printable ASCII only: no ';' compounds, no control characters, no tabs.
  for (size_t i = 0; i < n; i++)
    if (line[i] < 0x20 || line[i] > 0x7E || line[i] == ';') return false;
  // Split at the first run of spaces: header, then optional argument.
  size_t h = 0;
  while (h < n && line[h] != ' ') h++;
  size_t a = h;
  while (a < n && line[a] == ' ') a++;
  bool has_arg = a < n;
  if (h < n && !has_arg) return false;  // trailing space with no argument
  for (const Entry &e : kEntries) {
    if (!HeadMatches(line, h, e.path)) continue;
    if (e.arg == kNone) return !has_arg && h == n;
    return has_arg && ArgOk(e.arg, line + a, n - a);
  }
  return false;
}

bool ParseReading(const char *reply, double *volts) {
  if (!reply || !volts) return false;
  size_t n = strlen(reply);
  while (n && (reply[n - 1] == '\n' || reply[n - 1] == '\r')) n--;
  if (!IsNumber(reply, n) || n >= 48) return false;
  char buf[48];
  memcpy(buf, reply, n);
  buf[n] = 0;
  double v = strtod(buf, nullptr);
  // strtod gives +/-HUGE_VAL on 1e999; both that and the meter's 9.9E37
  // overload marker are >= the limit.
  if (!(v < 9.9e37 && v > -9.9e37)) return false;
  *volts = v;
  return true;
}

}  // namespace UsbTmc
