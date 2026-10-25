// A C++ model of the 259e calibration firmware 31.2C4, for host tests. The
// rules are Buchla_FW docs/cv-calibration/CONTRACT.md, "259e behaviour where
// the contract above was open (31.2C4)", plus its frames table and status bits:
//   - acts only if dest 0x00, src 0x22, target and magic 0xCA match
//   - no reply to 0x10 / 0x11 / 0x12 / 0x0B; replies only to READ (0x18)
//   - READ what 0: 8 bytes of the active record from arg; NO data bytes if
//     there is no record or arg > 22; what 1: hi, lo, n (no data if arg > 1);
//     what 2: flags, fmt(=1), seq_hi, seq_lo; an unknown what: no reply
//   - COMMIT: staged bytes 0..len-1 must decode (CalRec::Decode259e) with
//     trailing CRC == crc, else refused (bit3). Byte-equal to the active
//     record: ok (bit2), no write. 8 append-only slots; all full: refused
//     with bits 3+4 unless flags bit0 (compaction: the new record alone in
//     slot 0). COMMIT flushes the receive queue: frames arriving while it
//     rewrites flash (kCommitBusyMs) are lost.
//   - status flags: bit0 record valid, bit2 last commit ok, bit3 refused,
//     bit4 no free slot
// Test knobs: drop_replies, busy_sends (Send returns -1 that many times; -1
// forever when negative), other_addr (another 259e at that address answers
// every READ too, with different data), bad_reply_len, nak, drop_commits (a COMMIT
// frame ACKed on the wire but lost), commit_send_rc (+ commit_latches: a Wire error
// returned for the COMMIT frame, the module having received it or not).
// ADC model (off unless input_mv is set; then the module samples its two
// inputs every sample_ms on the clock, bumping adc_n once per sample):
//   raw = (input_mv(ch) + adc_err_mv[ch]) * adc_k[ch] + noise, clamped to 16 bits,
// noise uniform +-adc_noise counts. kNominalK is the stock correction's inverse:
// a raw word the stock gain 0x9041 maps onto mv * 4096 / 1200.
#pragma once
#include <stdint.h>
#include <string.h>

#include <deque>
#include <functional>
#include <vector>

#include "../src/Cal259eBus.h"
#include "../src/CalRec.h"

struct Sim259e : BusPort {
  static constexpr uint32_t kCommitBusyMs = 1000;

  uint8_t addr = 0x28;
  CalRoutine::Clock *clock = nullptr;

  uint8_t staged[40];
  uint8_t active[30];
  bool has_active = false;
  uint8_t slots_used = 0;
  uint16_t seq = 0;
  bool ok_bit = false, refused_bit = false, noslot_bit = false;
  uint16_t adc_word[2] = {0x1234, 0x2345};
  uint8_t adc_n = 0;
  uint32_t busy_until = 0;
  bool busy_active = false;

  // knobs
  bool drop_replies = false;
  int busy_sends = 0;
  uint8_t other_addr = 0;
  int bad_reply_len = -1;   // >= 0: reply data truncated/padded to this length
  int nak = 0;              // Send returns this (not 0) when non-zero
  int drop_commits = 0;     // the next this-many COMMIT frames are ACKed on the wire and lost
  int commit_send_rc = 0;   // non-zero: a COMMIT frame's Send returns this (a Wire error)...
  bool commit_latches = true;  // ...and the module still received the frame (NAK on its last byte)

  // ADC model
  static constexpr double kNominalK = (4096.0 / 1200.0) / (0x9041 / 65536.0);
  std::function<double(int ch)> input_mv;
  double adc_k[2] = {kNominalK, kNominalK};
  double adc_err_mv[2] = {0.0, 0.0};
  double adc_noise = 0.0;
  uint32_t sample_ms = 2;
  bool sampled = false;
  uint32_t last_sample = 0;
  uint32_t noise_lcg = 7;

  // observation
  int count[256] = {};      // frames received per cmd
  int bad_frames = 0;       // frames that failed CalRec::FrameAllowed
  int lost = 0;             // frames lost to the flash rewrite
  int commits_received = 0; // COMMIT frames the module actually processed
  std::vector<std::vector<uint8_t>> log;
  std::deque<std::vector<uint8_t>> replies;

  Sim259e() { memset(staged, 0xFF, sizeof staged); memset(active, 0xFF, sizeof active); }

  void SampleTick() { ++adc_n; }

  // With the model on: a new sample once sample_ms has passed since the last.
  void ModelSample() {
    if (!input_mv || !clock) return;
    const uint32_t now = clock->Ms();
    if (sampled && now - last_sample < sample_ms) return;
    sampled = true;
    last_sample = now;
    ++adc_n;
    for (int ch = 0; ch < 2; ++ch) {
      noise_lcg = noise_lcg * 1103515245u + 12345u;
      const double u = double((noise_lcg >> 8) & 0xFFFF) / 65535.0 * 2.0 - 1.0;
      double raw = (input_mv(ch) + adc_err_mv[ch]) * adc_k[ch] + u * adc_noise;
      raw = raw < 0.0 ? 0.0 : raw > 65535.0 ? 65535.0 : raw;
      adc_word[ch] = (uint16_t)(raw + 0.5);
    }
  }

  // Test helper: put a record straight into the active slot.
  void Install(const uint8_t rec[30]) {
    memcpy(active, rec, 30); has_active = true; if (slots_used < 8) ++slots_used;
    ok_bit = true; refused_bit = noslot_bit = false;
    CalRec::Record r;
    if (CalRec::Decode259e(rec, 30, &r)) seq = r.seq;
  }

  int Send(const uint8_t *f, uint8_t n) override {
    if (busy_sends != 0) { if (busy_sends > 0) --busy_sends; return -1; }
    if (nak) return nak;
    if (!CalRec::FrameAllowed(f, n)) ++bad_frames;
    log.emplace_back(f, f + n);
    if (n >= 5) ++count[f[3]];
    if (n >= 5 && f[3] == CalRec::kCmdCommit && commit_send_rc) {
      if (commit_latches) Receive(f, n);
      return commit_send_rc;
    }
    if (busy_active && clock->Ms() - busy_until >= 0x80000000u) { ++lost; return 0; }  // before busy_until
    busy_active = false;
    Receive(f, n);
    return 0;
  }

  bool PopReply(uint8_t *f, uint8_t *n) override {
    if (replies.empty()) return false;
    *n = (uint8_t)replies.front().size();
    memcpy(f, replies.front().data(), *n);
    replies.pop_front();
    return true;
  }

  void Receive(const uint8_t *f, uint8_t n) {
    if (n < 6 || f[1] != 0x00 || f[2] != CalRec::kMaster || f[4] != addr || f[5] != CalRec::kMagic)
      return;
    switch (f[3]) {
      case CalRec::kCmdOverride:
      case CalRec::kCmdRelease: return;
      case CalRec::kCmdPut: {
        const uint8_t off = f[6], len = (uint8_t)(n - 7);
        if (off + len <= 40) memcpy(staged + off, f + 7, len);
        return;
      }
      case CalRec::kCmdCommit:
        if (drop_commits > 0) { --drop_commits; return; }  // never reached the module
        Commit(f, n);
        return;
      case CalRec::kCmdRead: Read(f[6], f[7]); return;
      default: return;
    }
  }

  void Commit(const uint8_t *f, uint8_t n) {
    ++commits_received;
    const uint8_t len = f[6];
    const uint16_t crc = (uint16_t)((f[7] << 8) | f[8]);
    const uint8_t flags = n >= 10 ? f[9] : 0;
    busy_active = true;
    busy_until = clock->Ms() + kCommitBusyMs;  // the queue behind it is lost
    ok_bit = refused_bit = noslot_bit = false;
    CalRec::Record r;
    if (len != 30 || !CalRec::Decode259e(staged, len, &r) ||
        crc != (uint16_t)((staged[28] << 8) | staged[29])) {
      refused_bit = true;
      return;
    }
    if (has_active && memcmp(active, staged, 30) == 0) { ok_bit = true; return; }
    if (slots_used >= 8) {
      if (!(flags & 1)) { refused_bit = noslot_bit = true; return; }
      slots_used = 0;  // compaction: the new record alone in slot 0
    }
    memcpy(active, staged, 30);
    has_active = true;
    ++slots_used;
    seq = r.seq;
    ok_bit = true;
  }

  // Firmware 31.2C4 (as built by make_259e_rc4.py): the reply frame buffer REPLY=0x067A
  // overlaps the stock send routine's scratch. 0x8D22 clears XDATA 0x0684 (= reply byte
  // 10 = data[5]) before it sends, and 0x8815 uses 0x0686 (= reply byte 12 = data[7])
  // as its byte-index counter, so while byte 12 goes out it holds 12. Any reply with
  // 8 data bytes therefore arrives with data[5] = 0x00 and data[7] = 0x0C. (Status and
  // ADC replies are too short to reach them.) Set false to model a fixed module.
  bool stock_scratch_overlap = true;
  uint8_t corrupt_read_arg = 0xFF;  // test knob: READ what 0 at this arg returns data[1] flipped
  bool status_hides_valid = false;   // test knob: the status shows bit0 (record valid) clear

  void Reply(uint8_t src, uint8_t what, uint8_t arg, const uint8_t *d0, uint8_t dn) {
    if (drop_replies) return;
    uint8_t dm[16];
    memcpy(dm, d0, dn);
    if (stock_scratch_overlap && src == addr) {
      if (dn > 5) dm[5] = 0x00;
      if (dn > 7) dm[7] = 0x0C;
    }
    const uint8_t *d = dm;
    std::vector<uint8_t> fr;
    fr.push_back(0);
    fr.push_back(CalRec::kMaster);
    fr.push_back(src);
    fr.push_back(CalRec::kCmdReply);
    fr.push_back(what);
    fr.push_back(arg);
    if (src == addr && bad_reply_len >= 0) {
      for (int i = 0; i < bad_reply_len; ++i) fr.push_back(i < dn ? d[i] : 0);
    } else {
      fr.insert(fr.end(), d, d + dn);
    }
    fr[0] = (uint8_t)(fr.size() - 1);
    replies.push_back(fr);
  }

  void Read(uint8_t what, uint8_t arg) {
    if (other_addr) {  // a second 259e answers first, with other data
      const uint8_t d[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
      Reply(other_addr, what, arg, d, what == 0 ? 8 : what == 1 ? 3 : 4);
      Reply(addr, (uint8_t)(what ^ 1), arg, d, 3);  // right module, wrong request
    }
    uint8_t d[8];
    switch (what) {
      case 0:
        if (!has_active || arg > 22) { Reply(addr, 0, arg, d, 0); return; }
        { uint8_t pad[32]; memset(pad, 0xFF, sizeof pad); memcpy(pad, active, 30);
          // test knob: the read at this arg returns record byte arg+1 corrupted
          if (arg == corrupt_read_arg) pad[arg + 1] ^= 0x40;
          Reply(addr, 0, arg, pad + arg, 8); }
        return;
      case 1:
        if (arg > 1) { Reply(addr, 1, arg, d, 0); return; }
        ModelSample();
        d[0] = (uint8_t)(adc_word[arg] >> 8); d[1] = (uint8_t)adc_word[arg]; d[2] = adc_n;
        Reply(addr, 1, arg, d, 3);
        return;
      case 2:
        d[0] = (uint8_t)((has_active && !status_hides_valid ? 1 : 0) | (ok_bit ? 4 : 0) | (refused_bit ? 8 : 0) |
                         (noslot_bit ? 16 : 0));
        d[1] = 1; d[2] = (uint8_t)(seq >> 8); d[3] = (uint8_t)seq;
        Reply(addr, 2, arg, d, 4);
        return;
      default: return;  // unknown what: no reply
    }
  }
};
