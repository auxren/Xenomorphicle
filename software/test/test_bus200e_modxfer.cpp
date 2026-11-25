// Host tests for the MOD_XFER codec and 259e table-command allowlist
// (src/Bus200eModXfer.cpp). The fixture fixtures/mod_xfer_vectors.json is a
// byte copy of the Buccaneer app's app/fixtures/sysex/mod_xfer_vectors.json:
// both sides check the same bytes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/Bus200eSysEx.h"
#include "../src/Bus200eModXfer.h"

static int checks = 0, fails = 0;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static std::string g_fixture;

static void load_fixture() {
  char path[512];
  snprintf(path, sizeof(path), "%s", __FILE__);
  char *slash = strrchr(path, '/');
  if (slash) *(slash + 1) = 0; else path[0] = 0;
  { const size_t pl = strlen(path); snprintf(path + pl, sizeof(path) - pl, "%s", "fixtures/mod_xfer_vectors.json"); }
  FILE *f = fopen(path, "r");
  if (!f) f = fopen("fixtures/mod_xfer_vectors.json", "r");  // cwd fallback
  CHECK(f != nullptr);
  if (!f) return;
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) g_fixture.append(buf, got);
  fclose(f);
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Hex string -> bytes. Returns the length, or -1 on a bad digit.
static int unhex(const std::string &s, uint8_t *out, size_t cap) {
  if (s.size() % 2) return -1;
  size_t n = 0;
  for (size_t i = 0; i < s.size(); i += 2) {
    const int a = hexval(s[i]), b = hexval(s[i + 1]);
    if (a < 0 || b < 0 || n >= cap) return -1;
    out[n++] = (uint8_t) (a * 16 + b);
  }
  return (int) n;
}

// One fixture row: the text between "{" and the matching "}" of the Nth entry
// of the array opened by `"<array>": [`.
static bool nth_row(const char *array, int idx, std::string *row) {
  const std::string key = std::string("\"") + array + "\": [";
  size_t p = g_fixture.find(key);
  if (p == std::string::npos) return false;
  p += key.size();
  const size_t arr_end = g_fixture.find(']', p);  // no row contains a ']'
  if (arr_end == std::string::npos) return false;
  for (int i = 0;; ++i) {
    p = g_fixture.find('{', p);
    if (p == std::string::npos || p > arr_end) return false;
    const size_t e = g_fixture.find('}', p);
    if (e == std::string::npos) return false;
    if (i == idx) { *row = g_fixture.substr(p, e - p + 1); return true; }
    p = e + 1;
  }
}

static bool str_field(const std::string &row, const char *key, std::string *v) {
  const std::string k = std::string("\"") + key + "\": \"";
  size_t p = row.find(k);
  if (p == std::string::npos) return false;
  p += k.size();
  const size_t e = row.find('"', p);
  if (e == std::string::npos) return false;
  *v = row.substr(p, e - p);
  return true;
}

static bool int_field(const std::string &row, const char *key, int *v) {
  const std::string k = std::string("\"") + key + "\": ";
  const size_t p = row.find(k);
  if (p == std::string::npos) return false;
  *v = atoi(row.c_str() + p + k.size());
  return true;
}

// sysex = F0 <body> F7; returns the body (what send_message takes).
static int body_of(const std::string &sysex, uint8_t *out, size_t cap) {
  uint8_t full[128];
  const int n = unhex(sysex, full, sizeof(full));
  if (n < 2 || full[0] != 0xF0 || full[n - 1] != 0xF7) return -1;
  if ((size_t) (n - 2) > cap) return -1;
  memcpy(out, full + 1, (size_t) (n - 2));
  return n - 2;
}

static void test_allowlist_matches_fixture() {
  printf("test_allowlist_matches_fixture\n");
  int rows = 0, accepted = 0, refused = 0;
  std::string row;
  for (int i = 0; nth_row("allow", i, &row); ++i) {
    std::string name, hex;
    int want = -1;
    const bool okf = str_field(row, "name", &name) && str_field(row, "frame", &hex) &&
                     int_field(row, "want_reply", &want);
    CHECK(okf);
    if (!okf) continue;
    const bool ok = row.find("\"ok\": true") != std::string::npos;
    uint8_t frame[64];
    const int n = unhex(hex, frame, sizeof(frame));
    CHECK(n >= 0);
    if (n < 0) continue;
    const bool got = Bus200eModXferAllowed(frame, (uint8_t) n, (uint8_t) want);
    if (got != ok) printf("  FAIL allow row %s: got %d want %d\n", name.c_str(), got, ok);
    CHECK(got == ok);
    ++rows;
    ok ? ++accepted : ++refused;
  }
  CHECK(rows == 38 && accepted == 14 && refused == 24);
  // A null frame is refused, not dereferenced.
  CHECK(!Bus200eModXferAllowed(nullptr, 7, 0));
  // want_reply values other than 0/1 are refused.
  uint8_t save[7] = { 0x06, 0x00, 0x22, 0x0C, 0x28, 0xCA, 0x03 };
  CHECK(Bus200eModXferAllowed(save, 7, 0));
  CHECK(!Bus200eModXferAllowed(save, 7, 2));
}

static void test_parse_matches_fixture() {
  printf("test_parse_matches_fixture\n");
  int seen = 0;
  std::string row;
  for (int i = 0; nth_row("encode", i, &row); ++i) {
    std::string msg;
    if (!str_field(row, "msg", &msg) || msg != "mod_xfer") continue;
    std::string fhex, shex;
    int want = -1;
    CHECK(str_field(row, "frame", &fhex) && str_field(row, "sysex", &shex) &&
          int_field(row, "want_reply", &want));
    uint8_t body[64], frame_want[32];
    const int bn = body_of(shex, body, sizeof(body));
    const int fn = unhex(fhex, frame_want, sizeof(frame_want));
    CHECK(bn > 5 && fn > 0);
    if (bn <= 5 || fn <= 0) continue;
    CHECK(body[0] == BUS200E_SYSEX_MFR_ID && body[4] == BUS200E_SYSEX_CMD_MOD_XFER);
    uint8_t wr = 9, frame[BUS200E_MOD_XFER_FRAME_MAX], n = 0;
    const int rc = Bus200eModXferParse(body + 5, (uint32_t) (bn - 5), &wr, frame, &n);
    CHECK(rc == 0);
    CHECK(wr == want);
    CHECK(n == fn && memcmp(frame, frame_want, (size_t) fn) == 0);
    CHECK(Bus200eModXferAllowed(frame, n, wr));
    ++seen;
  }
  CHECK(seen == 7);
}

static void test_build_reply_matches_fixture() {
  printf("test_build_reply_matches_fixture\n");
  int seen = 0;
  std::string row;
  for (int i = 0; nth_row("encode", i, &row); ++i) {
    std::string msg;
    if (!str_field(row, "msg", &msg) || msg != "mod_xfer_r") continue;
    std::string rhex, shex;
    int result = -1;
    CHECK(str_field(row, "reply", &rhex) && str_field(row, "sysex", &shex) &&
          int_field(row, "result", &result));
    uint8_t body[64], reply[BUS200E_MOD_XFER_REPLY_MAX];
    const int bn = body_of(shex, body, sizeof(body));
    const int rn = unhex(rhex, reply, sizeof(reply));
    CHECK(bn > 5 && rn >= 0);
    if (bn <= 5 || rn < 0) continue;
    uint8_t out[BUS200E_SYSEX_MAX_MESSAGE + 8];
    const int n = Bus200eModXferBuildReply((uint8_t) result, reply, (uint8_t) rn, out, sizeof(out));
    if (n != bn || memcmp(out, body, (size_t) bn) != 0) printf("  FAIL build row %d\n", i);
    CHECK(n == bn && memcmp(out, body, (size_t) bn) == 0);
    // Tight capacity: exact fit works, one short refuses.
    CHECK(Bus200eModXferBuildReply((uint8_t) result, reply, (uint8_t) rn, out, (uint32_t) bn) == bn);
    CHECK(Bus200eModXferBuildReply((uint8_t) result, reply, (uint8_t) rn, out, (uint32_t) bn - 1) < 0);
    ++seen;
  }
  CHECK(seen == 7);
  // Refusals: reply too long, null reply with a length, bad result, null out.
  uint8_t out[64], r19[19] = { 0 };
  CHECK(Bus200eModXferBuildReply(1, r19, 19, out, sizeof(out)) < 0);
  CHECK(Bus200eModXferBuildReply(1, nullptr, 3, out, sizeof(out)) < 0);
  CHECK(Bus200eModXferBuildReply(0x80, nullptr, 0, out, sizeof(out)) < 0);
  CHECK(Bus200eModXferBuildReply(0, nullptr, 0, nullptr, 64) < 0);
  CHECK(Bus200eModXferBuildReply(0, nullptr, 0, out, 5) < 0);
}

static void test_parse_rejects_truncated_packing() {
  printf("test_parse_rejects_truncated_packing\n");
  const uint8_t frame[7] = { 0x06, 0x00, 0x22, 0x0C, 0x28, 0xCA, 0x03 };
  uint8_t packed[16];
  const int pn = Bus200eSysExPack(frame, 7, packed, sizeof(packed));
  CHECK(pn == 8);
  uint8_t pl[32];
  pl[0] = 0;
  memcpy(pl + 1, packed, (size_t) pn);
  uint8_t wr, out[BUS200E_MOD_XFER_FRAME_MAX], n;
  CHECK(Bus200eModXferParse(pl, 9, &wr, out, &n) == 0 && n == 7);
  // Every shorter payload is malformed or too short for 7 frame bytes.
  for (uint32_t len = 0; len < 9; ++len) {
    CHECK(Bus200eModXferParse(pl, len, &wr, out, &n) < 0);
  }
  // A group header with nothing after it (2 + 7 data bytes + stray header).
  uint8_t stray[32];
  memcpy(stray, pl, 9);
  stray[9] = 0;
  CHECK(Bus200eModXferParse(stray, 10, &wr, out, &n) < 0);
  // want_reply out of range.
  pl[0] = 2;
  CHECK(Bus200eModXferParse(pl, 9, &wr, out, &n) < 0);
  pl[0] = 0;
  // A non-7-bit payload byte.
  pl[3] = 0x80;
  CHECK(Bus200eModXferParse(pl, 9, &wr, out, &n) < 0);
  // Null pointers.
  pl[3] = 0x00;
  CHECK(Bus200eModXferParse(nullptr, 9, &wr, out, &n) < 0);
  CHECK(Bus200eModXferParse(pl, 9, nullptr, out, &n) < 0);
  CHECK(Bus200eModXferParse(pl, 9, &wr, nullptr, &n) < 0);
  CHECK(Bus200eModXferParse(pl, 9, &wr, out, nullptr) < 0);
}

static void test_parse_rejects_frame_over_17() {
  printf("test_parse_rejects_frame_over_17\n");
  uint8_t raw[40], packed[64], pl[80], wr, out[BUS200E_MOD_XFER_FRAME_MAX], n;
  for (int i = 0; i < 40; ++i) raw[i] = (uint8_t) i;
  for (int len = 0; len <= 40; ++len) {
    const int pn = len ? Bus200eSysExPack(raw, (uint32_t) len, packed, sizeof(packed)) : 0;
    CHECK(pn >= 0);
    pl[0] = 0;
    if (pn > 0) memcpy(pl + 1, packed, (size_t) pn);
    const int rc = Bus200eModXferParse(pl, 1 + (uint32_t) pn, &wr, out, &n);
    if (len >= 7 && len <= 17) { CHECK(rc == 0 && n == len && memcmp(out, raw, (size_t) len) == 0); }
    else CHECK(rc < 0);
  }
  CHECK(Bus200eModXferParse(pl, 0, &wr, out, &n) < 0);
}

static void test_reply_matches_only_same_target_what_arg() {
  printf("test_reply_matches_only_same_target_what_arg\n");
  const uint8_t req[8] = { 0x07, 0x00, 0x22, 0x18, 0x2B, 0xCA, 0x10, 0x00 };
  uint8_t rep[18] = { 0x0A, 0x22, 0x2B, 0x19, 0x10, 0x00, 1, 2, 3, 4, 5, 0, 0, 0, 0, 0, 0, 0 };
  rep[0] = 0x0A;
  CHECK(Bus200eModXferReplyMatches(req, rep, 11));
  // exactly 6 and exactly 18 bytes are in range
  { uint8_t r6[6] = { 0x05, 0x22, 0x2B, 0x19, 0x10, 0x00 }; CHECK(Bus200eModXferReplyMatches(req, r6, 6)); }
  { uint8_t r18[18] = { 0x11, 0x22, 0x2B, 0x19, 0x10, 0x00 }; CHECK(Bus200eModXferReplyMatches(req, r18, 18)); }
  // length 5 and 19
  { uint8_t r5[5] = { 0x04, 0x22, 0x2B, 0x19, 0x10 }; CHECK(!Bus200eModXferReplyMatches(req, r5, 5)); }
  { uint8_t r19[19] = { 0x12, 0x22, 0x2B, 0x19, 0x10, 0x00 }; CHECK(!Bus200eModXferReplyMatches(req, r19, 19)); }
  // length byte disagrees with reply_n
  rep[0] = 0x09; CHECK(!Bus200eModXferReplyMatches(req, rep, 11));
  rep[0] = 0x0A;
  uint8_t t[18];
  memcpy(t, rep, sizeof(t)); t[1] = 0x21; CHECK(!Bus200eModXferReplyMatches(req, t, 11));  // wrong src
  memcpy(t, rep, sizeof(t)); t[2] = 0x28; CHECK(!Bus200eModXferReplyMatches(req, t, 11));  // wrong target
  memcpy(t, rep, sizeof(t)); t[3] = 0x13; CHECK(!Bus200eModXferReplyMatches(req, t, 11));  // cmd 0x13
  memcpy(t, rep, sizeof(t)); t[4] = 0x11; CHECK(!Bus200eModXferReplyMatches(req, t, 11));  // wrong what
  memcpy(t, rep, sizeof(t)); t[5] = 0x01; CHECK(!Bus200eModXferReplyMatches(req, t, 11));  // wrong arg
  CHECK(!Bus200eModXferReplyMatches(nullptr, rep, 11));
  CHECK(!Bus200eModXferReplyMatches(req, nullptr, 11));
}

int main() {
  load_fixture();
  test_allowlist_matches_fixture();
  test_parse_matches_fixture();
  test_build_reply_matches_fixture();
  test_parse_rejects_truncated_packing();
  test_parse_rejects_frame_over_17();
  test_reply_matches_only_same_target_what_arg();
  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
