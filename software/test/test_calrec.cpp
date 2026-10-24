// Host tests for the CV-calibration record codec (src/CalRec.h): CRC, 259e
// record, bus frames and allowlist, reply parser, apply and fit. Checked bit
// for bit against fixtures/calrec_vectors.txt (Buchla_FW tools/calrec.py).
// Fixture lines starting with '#' are comments.
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_calrec
//   test_calrec.cpp ../src/CalRec.cpp && ./build/test_calrec
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/CalRec.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::vector<uint8_t> Hex(const std::string &s) {
  std::vector<uint8_t> v;
  if (s == "-") return v;
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    v.push_back((uint8_t)strtoul(s.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

static std::vector<std::string> Split(const std::string &s, char c) {
  std::vector<std::string> o;
  size_t a = 0;
  for (;;) {
    size_t b = s.find(c, a);
    if (b == std::string::npos) { o.push_back(s.substr(a)); return o; }
    o.push_back(s.substr(a, b - a));
    a = b + 1;
  }
}

static int n_crc, n_enc, n_apply, n_fit, n_frame, n_reply;

static void line_crc(const std::vector<std::string> &t) {
  std::vector<uint8_t> d = Hex(t[1]);
  CHECK(CalRec::Crc16(d.data(), d.size()) == (uint16_t)strtoul(t[2].c_str(), nullptr, 16));
  n_crc++;
}

static void line_enc(const std::vector<std::string> &t) {
  CalRec::Record r;
  std::vector<uint8_t> uid = Hex(t[1]);
  memcpy(r.unit_id, uid.data(), 8);
  r.seq = (uint16_t)strtoul(t[2].c_str(), nullptr, 10);
  r.date = (uint32_t)strtoul(t[3].c_str(), nullptr, 10);
  r.ch[0].gain = (uint16_t)strtol(t[4].c_str(), nullptr, 10);
  r.ch[0].offset = (int16_t)strtol(t[5].c_str(), nullptr, 10);
  r.ch[1].gain = (uint16_t)strtol(t[6].c_str(), nullptr, 10);
  r.ch[1].offset = (int16_t)strtol(t[7].c_str(), nullptr, 10);
  std::vector<uint8_t> want = Hex(t[8]);
  uint8_t out[CalRec::kRecLen259e];
  CalRec::Encode259e(r, out);
  CHECK(want.size() == (size_t)CalRec::kRecLen259e);
  CHECK(memcmp(out, want.data(), want.size()) == 0);
  CalRec::Record d;
  CHECK(CalRec::Decode259e(want.data(), want.size(), &d));
  CHECK(memcmp(d.unit_id, r.unit_id, 8) == 0 && d.seq == r.seq && d.date == r.date);
  CHECK(d.ch[0].gain == r.ch[0].gain && d.ch[0].offset == r.ch[0].offset);
  CHECK(d.ch[1].gain == r.ch[1].gain && d.ch[1].offset == r.ch[1].offset);
  n_enc++;
}

static void line_apply(const std::vector<std::string> &t) {
  CHECK(CalRec::Apply259e((uint16_t)strtol(t[1].c_str(), nullptr, 10),
                          (uint16_t)strtol(t[2].c_str(), nullptr, 10),
                          (int16_t)strtol(t[3].c_str(), nullptr, 10)) ==
        (uint16_t)strtol(t[4].c_str(), nullptr, 10));
  n_apply++;
}

static void line_fit(const std::vector<std::string> &t) {
  int n = atoi(t[1].c_str());
  std::vector<std::string> pts = Split(t[2], ';');
  CHECK((int)pts.size() == n);
  std::vector<CalRec::FitPoint> p;
  for (const std::string &s : pts) {
    std::vector<std::string> rm = Split(s, ',');
    CalRec::FitPoint fp;
    fp.raw = (uint16_t)strtol(rm[0].c_str(), nullptr, 10);
    fp.mv = strtod(rm[1].c_str(), nullptr);
    p.push_back(fp);
  }
  CalRec::Channel c = {0, 0};
  CHECK(CalRec::Fit259e(p.data(), (int)p.size(), &c) == CalRec::FIT_OK);
  CHECK(c.gain == (uint16_t)strtol(t[3].c_str(), nullptr, 10));
  CHECK(c.offset == (int16_t)strtol(t[4].c_str(), nullptr, 10));
  n_fit++;
}

static void line_frame(const std::vector<std::string> &t) {
  uint8_t cmd = (uint8_t)atoi(t[1].c_str()), target = (uint8_t)atoi(t[2].c_str());
  std::vector<uint8_t> pl = Hex(t[3]), want = Hex(t[4]);
  uint8_t out[CalRec::kMaxFrame];
  memset(out, 0xEE, sizeof out);
  int n = CalRec::BuildFrame(cmd, target, pl.data(), (uint8_t)pl.size(), out);
  CHECK(n == (int)want.size());
  CHECK(n > 0 && memcmp(out, want.data(), want.size()) == 0);
  CHECK(CalRec::FrameAllowed(want.data(), (uint8_t)want.size()));
  n_frame++;
}

static void line_reply(const std::vector<std::string> &t) {
  std::vector<uint8_t> fr = Hex(t[1]);
  CalRec::Reply r;
  bool ok = CalRec::ParseReply(fr.data(), (uint8_t)fr.size(), &r);
  if (t[2] == "NONE") { CHECK(!ok); n_reply++; return; }
  CHECK(ok);
  if (!ok) return;
  std::vector<uint8_t> data = Hex(t[5]);
  CHECK(r.src == atoi(t[2].c_str()) && r.what == atoi(t[3].c_str()) && r.arg == atoi(t[4].c_str()));
  CHECK(r.len == data.size() && memcmp(r.data, data.data(), data.size()) == 0);
  n_reply++;
}

static void test_fixture() {
  FILE *f = fopen("fixtures/calrec_vectors.txt", "r");
  if (!f) f = fopen("test/fixtures/calrec_vectors.txt", "r");
  CHECK(f != nullptr);
  if (!f) return;
  char buf[4096];
  while (fgets(buf, sizeof buf, f)) {
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty() || s[0] == '#') continue;
    std::vector<std::string> t = Split(s, ' ');
    if (t[0] == "crc") line_crc(t);
    else if (t[0] == "enc259") line_enc(t);
    else if (t[0] == "apply259") line_apply(t);
    else if (t[0] == "fit259") line_fit(t);
    else if (t[0] == "frame") line_frame(t);
    else if (t[0] == "reply") line_reply(t);
    else { printf("unknown fixture line: %s\n", s.c_str()); CHECK(false); }
  }
  fclose(f);
  CHECK(n_crc == 20 && n_enc == 6 && n_apply == 200 && n_fit == 8 && n_frame == 20 && n_reply == 12);
}

static void test_frame_allowlist_rejects() {
  uint8_t out[CalRec::kMaxFrame];
  const uint8_t two[4] = {0, 0, 0, 0};
  CHECK(CalRec::BuildFrame(0x07, 0x28, nullptr, 0, out) == -1);
  CHECK(CalRec::BuildFrame(0x08, 0x28, nullptr, 0, out) == -1);
  CHECK(CalRec::BuildFrame(0x13, 0x28, nullptr, 0, out) == -1);
  CHECK(CalRec::BuildFrame(0x18, 0x28, two, 2, out) == 8);
  CHECK(CalRec::BuildFrame(0x18, 0x28, two, 3, out) == -1);   // n=8
  CHECK(CalRec::BuildFrame(0x18, 0x28, two, 1, out) == -1);   // n=6
  uint8_t pl[16] = {0};
  CHECK(CalRec::BuildFrame(0x12, 0x28, pl, 10, out) == -1);   // n=15
  CHECK(CalRec::BuildFrame(0x12, 0x28, pl, 1, out) == -1);    // n=6
  pl[0] = 33;
  CHECK(CalRec::BuildFrame(0x12, 0x28, pl, 9, out) == -1);    // 33+8 > 40
  pl[0] = 32;
  CHECK(CalRec::BuildFrame(0x12, 0x28, pl, 9, out) == 15);    // 32+8 == 40
  CHECK(CalRec::BuildFrame(0x11, 0x28, pl, 1, out) == -1);
  CHECK(CalRec::BuildFrame(0x0B, 0x28, pl, 5, out) == -1);

  const uint8_t good[] = {7, 0x00, 0x22, 0x18, 0x28, 0xCA, 0, 0};
  CHECK(CalRec::FrameAllowed(good, 8));
  uint8_t f[8];
  memcpy(f, good, 8); f[5] = 0xCB; CHECK(!CalRec::FrameAllowed(f, 8));   // magic
  memcpy(f, good, 8); f[2] = 0x21; CHECK(!CalRec::FrameAllowed(f, 8));   // src
  memcpy(f, good, 8); f[1] = 0x01; CHECK(!CalRec::FrameAllowed(f, 8));   // dest
  memcpy(f, good, 8); f[3] = 0x13; CHECK(!CalRec::FrameAllowed(f, 8));   // cmd
  memcpy(f, good, 8); f[0] = 6;    CHECK(!CalRec::FrameAllowed(f, 8));   // length byte
  CHECK(!CalRec::FrameAllowed(good, 7));
  CHECK(!CalRec::FrameAllowed(good, 0));
  CHECK(!CalRec::FrameAllowed(nullptr, 8));
  const uint8_t n8[] = {8, 0x00, 0x22, 0x18, 0x28, 0xCA, 0, 0, 0};      // 0x18 n=8
  CHECK(!CalRec::FrameAllowed(n8, 9));
}

static void test_reply_edges() {
  CalRec::Reply r;
  const uint8_t ok[] = {5, 0x22, 0x28, 0x19, 2, 0};
  CHECK(CalRec::ParseReply(ok, 6, &r) && r.len == 0 && r.src == 0x28);
  CHECK(!CalRec::ParseReply(ok, 5, &r));
  CHECK(!CalRec::ParseReply(nullptr, 6, &r));
  uint8_t big[19] = {18, 0x22, 0x28, 0x19, 1, 0};
  CHECK(CalRec::ParseReply(big, 19, &r) && r.len == 13);
}

static void test_decode_rejects() {
  CalRec::Record r;
  memset(&r, 0, sizeof r);
  r.ch[0].gain = 0x9041; r.ch[1].gain = 0x9041;
  uint8_t b[CalRec::kRecLen259e];
  CalRec::Encode259e(r, b);
  CHECK(CalRec::Decode259e(b, sizeof b, &r));
  CHECK(!CalRec::Decode259e(b, sizeof b - 1, &r));
  CHECK(!CalRec::Decode259e(b, sizeof b + 1, &r));
  CHECK(!CalRec::Decode259e(nullptr, sizeof b, &r));
  uint8_t c[CalRec::kRecLen259e];
  memcpy(c, b, sizeof b); c[0] = 'X'; CHECK(!CalRec::Decode259e(c, sizeof c, &r));
  memcpy(c, b, sizeof b); c[2] = 2;   CHECK(!CalRec::Decode259e(c, sizeof c, &r));
  memcpy(c, b, sizeof b); c[10] ^= 1; CHECK(!CalRec::Decode259e(c, sizeof c, &r));  // CRC
  memcpy(c, b, sizeof b); c[29] ^= 1; CHECK(!CalRec::Decode259e(c, sizeof c, &r));
  // A well-formed 251e record (module 0x51, 4 channels, 38 bytes) is refused.
  uint8_t e[38] = {'C', 'L', 1, 0x51, 4, 0};
  uint16_t crc = CalRec::Crc16(e, 36);
  e[36] = (uint8_t)(crc >> 8); e[37] = (uint8_t)crc;
  CHECK(!CalRec::Decode259e(e, sizeof e, &r));
  // Right length, 251e header fields, good CRC: still refused by module/nch.
  uint8_t g[CalRec::kRecLen259e];
  memcpy(g, b, sizeof b); g[3] = 0x51; g[4] = 4;
  crc = CalRec::Crc16(g, 28);
  g[28] = (uint8_t)(crc >> 8); g[29] = (uint8_t)crc;
  CHECK(!CalRec::Decode259e(g, sizeof g, &r));
}

static void test_fit_errors() {
  CalRec::Channel c = {1, 1};
  CalRec::FitPoint same[3] = {{1000, 1200.0}, {1000, 2400.0}, {1000, 3600.0}};
  CHECK(CalRec::Fit259e(same, 3, &c) == CalRec::FIT_TOO_FEW);
  CHECK(CalRec::Fit259e(same, 1, &c) == CalRec::FIT_TOO_FEW);
  CHECK(CalRec::Fit259e(same, 0, &c) == CalRec::FIT_TOO_FEW);
  // slope 2 counts per raw: gain 0x20000 >= 0x10000
  CalRec::FitPoint hi[2] = {{0, 0.0}, {1200, 2400.0 * 1200.0 / 4096.0 * 1.0}};
  hi[1].mv = 2.0 * 1200.0 * 1200.0 / 4096.0;
  CHECK(CalRec::Fit259e(hi, 2, &c) == CalRec::FIT_GAIN_RANGE);
  // negative slope
  CalRec::FitPoint neg[2] = {{0, 1200.0}, {1000, 0.0}};
  CHECK(CalRec::Fit259e(neg, 2, &c) == CalRec::FIT_GAIN_RANGE);
  // zero slope rounds to gain 0
  CalRec::FitPoint flat[2] = {{0, 1200.0}, {1000, 1200.0}};
  CHECK(CalRec::Fit259e(flat, 2, &c) == CalRec::FIT_GAIN_RANGE);
  // gain ~0.5, intercept far above s16: y = 0.5*x + 40000 counts
  CalRec::FitPoint off[2];
  off[0].raw = 0;     off[0].mv = 40000.0 * 1200.0 / 4096.0;
  off[1].raw = 2000;  off[1].mv = 41000.0 * 1200.0 / 4096.0;
  CHECK(CalRec::Fit259e(off, 2, &c) == CalRec::FIT_OFFSET_RANGE);
  off[0].mv = -40000.0 * 1200.0 / 4096.0; off[1].mv = -39000.0 * 1200.0 / 4096.0;
  CHECK(CalRec::Fit259e(off, 2, &c) == CalRec::FIT_OFFSET_RANGE);
  // Exact fit, round-half-even on offset: y = 0.5*x + 2.5 -> offset 2 (tie to even)
  CalRec::FitPoint tie[2] = {{0, 2.5 * 1200.0 / 4096.0}, {2, 3.5 * 1200.0 / 4096.0}};
  CalRec::FitError e = CalRec::Fit259e(tie, 2, &c);
  CHECK(e == CalRec::FIT_OK && c.gain == 0x8000 && (c.offset == 2 || c.offset == 3));
}

static void test_target_residual() {
  CHECK(fabs(CalRec::TargetCounts(1200.0) - 4096.0) < 1e-9);
  CHECK(CalRec::TargetCounts(0.0) == 0.0);
  CalRec::Channel c = {0x9041, 0};
  double want = (CalRec::Apply259e(1000, 0x9041, 0) - 4096.0) * 1200.0 / 4096.0;
  CHECK(CalRec::ResidualCents(1000, 1200.0, c) == want);
}

// S5: during a run the console's raw sender passes CAL frames only, never PUT or COMMIT.
static void test_console_gate_during_run() {
  // READ (0x18) status of 0x28: a cal frame, allowed
  uint8_t rd[8] = {7, 0x00, CalRec::kMaster, CalRec::kCmdRead, 0x28, CalRec::kMagic, 2, 0};
  CHECK(CalRec::ConsoleFrameAllowedDuringRun(rd, 8));
  // PUT and COMMIT to any address / with any magic: refused
  for (uint8_t addr : {0x00, 0x28, 0x29, 0x2B, 0x5C, 0xFF}) {
    for (uint8_t magic : {CalRec::kMagic, uint8_t(0x00), uint8_t(0x5A)}) {
      uint8_t put[14] = {13, 0x00, CalRec::kMaster, CalRec::kCmdPut, addr, magic, 0,
                         1, 2, 3, 4, 5, 6, 7};
      CHECK(!CalRec::ConsoleFrameAllowedDuringRun(put, 14));
      uint8_t cm[9] = {8, 0x00, CalRec::kMaster, CalRec::kCmdCommit, addr, magic, 30, 0x12, 0x34};
      CHECK(!CalRec::ConsoleFrameAllowedDuringRun(cm, 9));
      uint8_t cm10[10] = {9, 0x00, CalRec::kMaster, CalRec::kCmdCommit, addr, magic, 30, 0x12, 0x34, 1};
      CHECK(!CalRec::ConsoleFrameAllowedDuringRun(cm10, 10));
    }
  }
  // a wrong length byte or a non-cal frame (QUERY, preset save broadcast): refused too
  uint8_t q[5] = {4, 0x5C, 0x22, 0x1A, 0xFF};
  CHECK(!CalRec::ConsoleFrameAllowedDuringRun(q, 5));
  uint8_t bad[8] = {9, 0x00, CalRec::kMaster, CalRec::kCmdRead, 0x28, CalRec::kMagic, 2, 0};
  CHECK(!CalRec::ConsoleFrameAllowedDuringRun(bad, 8));
  CHECK(!CalRec::ConsoleFrameAllowedDuringRun(nullptr, 8));
  CHECK(!CalRec::ConsoleFrameAllowedDuringRun(rd, 3));
}

// The 259e clamps its corrected code at 0x7FFF, one count under the 9600 mV target
// (32768): the clamped residual there hides a positive error, the unclamped shows it.
static void test_unclamped_residual() {
  const CalRec::Channel c = {0x9041, 30};
  const uint16_t raw = 58400;
  CHECK(CalRec::Apply259e(raw, c.gain, c.offset) == 0x7FFF);
  CHECK(CalRec::ResidualCents(raw, 9600.0, c) < 0.3);
  CHECK(CalRec::ResidualCentsUnclamped(raw, 9600.0, c) > 20.0);
  // in range the two are identical
  CHECK(CalRec::ResidualCentsUnclamped(1000, 1200.0, c) == CalRec::ResidualCents(1000, 1200.0, c));
}

int main() {
  test_console_gate_during_run();
  test_unclamped_residual();
  test_fixture();
  test_frame_allowlist_rejects();
  test_reply_edges();
  test_decode_rejects();
  test_fit_errors();
  test_target_residual();
  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
