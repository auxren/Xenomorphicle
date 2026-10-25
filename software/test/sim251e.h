// A C++ model of the 251e's calibration firmware (30.7C4, and 30.7C5 with fw_c5) and of its analog
// output stage, for host tests. It follows the shared bus contract in the private firmware
// repository, restated here in our own words:
//   - it reacts only to a whole frame addressed to it (master source, its own address, the
//     calibration magic) and ignores everything else; each command has one exact frame length,
//     and a frame of any other length changes nothing;
//   - only READ is ever answered; OVERRIDE, RELEASE, PUT and COMMIT get no reply;
//   - OVERRIDE holds a channel (0-3, code up to 4095) at a RAW DAC code, bypassing the correction;
//     channel 0xFF lets all go; every accepted OVERRIDE restarts one timer (override_ms, 5 s) for
//     all channels;
//   - COMMIT checks the staged record (38 bytes, CRC matching the frame's) and refuses it
//     otherwise; one record page is rewritten by every accepted COMMIT, and committing the same
//     record again just writes it again;
//   - READ of the record returns 8 bytes from an offset (nothing if no record or the offset is past
//     30); READ of the status returns flags (bit0 record valid, bit1 an override is active, bit2
//     last commit ok, bit3 last commit refused), the format (1) and the record's sequence number
//     (0 with none); any other READ is not answered.
// Firmware versions. C4 (the default): the commit-result bits keep their old value until the COMMIT
// finishes, so a stale ok or refused bit can be read. C5 (fw_c5): an accepted COMMIT clears both
// bits at once and sets them when it finishes. commit_ms is how long a COMMIT takes to finish;
// stuck_commit makes it never finish (the event never runs).
// Analog stage, per channel: V(mV) = scale * K * (1 + gain_err[ch]) * raw + zero_mv[ch]
//   + bow_mv[ch] * sin(pi raw / 4095) + inl_mv[ch] * sin(8 pi raw / 4095)
// where K = 10000 / 3831 mV per code (the stock table's 383.1 codes per volt). `scale` is the knob
// that injects a WRONG-SCALE stage (e.g. 3831/4096 for hardware with 4096 codes per 10 V).
// The sequencer (no override) drives channel ch at stage code seq_code[ch] through the record
// (Apply) when one is valid.
// Test knobs (as Sim259e): drop_replies, busy_sends, nak, drop_commits, commit_send_rc (+
// commit_latches), bad_reply_len, corrupt_read_arg, status_hides_valid, tear_commit (the COMMIT
// leaves a torn page: no valid record afterwards), ignore_override (firmware without the
// calibration code: it takes no overrides and never sets the override bit).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <deque>
#include <vector>

#include "../src/Cal251eBus.h"
#include "../src/CalRec.h"
#include "../src/CalRec251e.h"

static const double kPi251 = 3.14159265358979323846;

struct Sim251e : BusPort {

  uint32_t override_ms = 5000;       // the module's auto-release time (the contract says 5 s)
  uint8_t addr = 0x5C;
  CalRoutine::Clock *clock = nullptr;

  uint8_t staged[40];
  uint8_t active[38];
  bool has_active = false;
  uint16_t seq = 0;
  bool ok_bit = false, refused_bit = false;
  int ovr[4] = {-1, -1, -1, -1};
  uint32_t ovr_t0 = 0;
  int seq_code[4] = {1000, 1500, 2000, 2500};

  // analog stage
  double scale = 1.0;
  double gain_err[4] = {0.012, -0.008, 0.021, -0.015};
  double zero_mv[4] = {6.0, -4.0, 9.0, 2.0};
  double bow_mv[4] = {1.2, 0.8, 1.5, 1.0};
  double inl_mv[4] = {0.5, 0.4, 0.6, 0.3};
  int spike_lo = 0, spike_hi = -1;   // raw codes in [lo, hi] get spike_mv extra (a DAC glitch)
  double spike_mv = 0.0;
  double drift_mv_per_s = 0.0;       // thermal drift of the whole stage, from the clock
  double extra_mv = 0.0;             // a step added to every output (set mid-run by a test)

  // knobs
  bool drop_replies = false;
  int busy_sends = 0;
  int bad_reply_len = -1;
  int nak = 0;
  int drop_commits = 0;
  int commit_send_rc = 0;
  bool commit_latches = true;
  uint8_t corrupt_read_arg = 0xFF;
  bool status_hides_valid = false;
  bool tear_commit = false;
  bool ignore_override = false;
  bool fw_c5 = false;
  uint32_t commit_ms = 0;
  bool stuck_commit = false;
  // a COMMIT accepted and not yet finished
  bool pend = false;
  uint32_t pend_t0 = 0;
  uint8_t pend_stage[40];
  uint8_t pend_len = 0;
  uint16_t pend_crc = 0;
  uint8_t status_fmt = 1;

  // observation
  int count[256] = {};
  int bad_frames = 0;
  int commits_received = 0;
  int overrides = 0, releases = 0;
  int lapses = 0;                    // overrides that timed out (5 s without a refresh)
  uint32_t drift_t0 = 0;
  std::vector<std::vector<uint8_t>> log;
  std::deque<std::vector<uint8_t>> replies;

  Sim251e() { memset(staged, 0xFF, sizeof staged); memset(active, 0xFF, sizeof active); }

  static double K() { return CalRec251e::kMvPerCode; }

  double AnalogMv(int ch, int raw) const {
    const double x = double(raw) / 4095.0;
    double v = scale * K() * (1.0 + gain_err[ch]) * double(raw) + zero_mv[ch] +
               bow_mv[ch] * sin(kPi251 * x) + inl_mv[ch] * sin(8.0 * kPi251 * x);
    if (raw >= spike_lo && raw <= spike_hi) v += spike_mv;
    v += extra_mv;
    if (drift_mv_per_s != 0.0 && clock) v += drift_mv_per_s * double(clock->Ms() - drift_t0) / 1000.0;
    return v;
  }

  bool OverrideActive() {
    bool any = false;
    for (int c = 0; c < 4; ++c) any = any || ovr[c] >= 0;
    if (any && clock && clock->Ms() - ovr_t0 >= override_ms) {
      for (int c = 0; c < 4; ++c) ovr[c] = -1;
      any = false;
      ++lapses;
    }
    return any;
  }

  // The DAC code the module is sending on a channel right now.
  int RawCode(int ch) {
    Settle();
    OverrideActive();
    if (ovr[ch] >= 0) return ovr[ch];
    if (!has_active) return seq_code[ch];
    CalRec251e::Record r;
    if (!CalRec251e::Decode(active, 38, &r)) return seq_code[ch];
    return CalRec251e::Apply((uint16_t)seq_code[ch], r.ch[ch].gain, r.ch[ch].offset);
  }
  double OutMv(int ch) { return AnalogMv(ch, RawCode(ch)); }

  void Install(const uint8_t rec[38]) {
    memcpy(active, rec, 38);
    has_active = true;
    ok_bit = true;
    refused_bit = false;
    CalRec251e::Record r;
    if (CalRec251e::Decode(rec, 38, &r)) seq = r.seq;
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
    if (n < 6 || f[0] != n - 1 || f[1] != 0x00 || f[2] != CalRec::kMaster || f[4] != addr ||
        f[5] != CalRec::kMagic)
      return;
    switch (f[3]) {
      case CalRec::kCmdOverride:
        if (f[0] != 8 || ignore_override) return;
        if (f[6] == 0xFF) { for (int c = 0; c < 4; ++c) ovr[c] = -1; ++releases; return; }
        { const int code = (f[7] << 8) | f[8];
          if (f[6] > 3 || code > 4095) return;
          ovr[f[6]] = code; ovr_t0 = clock ? clock->Ms() : 0; ++overrides; }
        return;
      case CalRec::kCmdRelease:
        if (f[0] != 5) return;
        for (int c = 0; c < 4; ++c) ovr[c] = -1;
        ++releases;
        return;
      case CalRec::kCmdPut: {
        if (f[0] < 7 || f[0] > 14) return;
        const uint8_t off = f[6], len = (uint8_t)(n - 7);
        if (off + len <= 40) memcpy(staged + off, f + 7, len);
        return;
      }
      case CalRec::kCmdCommit:
        if (f[0] != 8 && f[0] != 9) return;
        if (drop_commits > 0) { --drop_commits; return; }
        Commit(f);
        return;
      case CalRec::kCmdRead:
        if (f[0] != 7) return;
        Read(f[6], f[7]);
        return;
      default: return;
    }
  }

  void Commit(const uint8_t *f) {
    ++commits_received;
    if (fw_c5) ok_bit = refused_bit = false;   // C5: cleared the moment the COMMIT is accepted
    pend = true;
    pend_t0 = clock ? clock->Ms() : 0;
    pend_len = f[6];
    pend_crc = (uint16_t)((f[7] << 8) | f[8]);
    memcpy(pend_stage, staged, sizeof staged);
    Settle();
  }

  // Finishes an accepted COMMIT once commit_ms has passed (and it is not stuck).
  void Settle() {
    if (!pend || stuck_commit) return;
    if (clock && clock->Ms() - pend_t0 < commit_ms) return;
    pend = false;
    ok_bit = refused_bit = false;
    CalRec251e::Record r;
    if (pend_len != 38 || !CalRec251e::Decode(pend_stage, pend_len, &r) ||
        pend_crc != (uint16_t)((pend_stage[36] << 8) | pend_stage[37])) {
      refused_bit = true;
      return;
    }
    if (tear_commit) { has_active = false; seq = 0; return; }  // torn page: stock path, no verdict
    memcpy(active, pend_stage, 38);
    has_active = true;
    seq = r.seq;
    ok_bit = true;
  }

  void Reply(uint8_t what, uint8_t arg, const uint8_t *d, uint8_t dn) {
    if (drop_replies) return;
    std::vector<uint8_t> fr;
    fr.push_back(0);
    fr.push_back(CalRec::kMaster);
    fr.push_back(addr);
    fr.push_back(CalRec::kCmdReply);
    fr.push_back(what);
    fr.push_back(arg);
    if (bad_reply_len >= 0) {
      for (int i = 0; i < bad_reply_len; ++i) fr.push_back(i < dn ? d[i] : 0);
    } else {
      fr.insert(fr.end(), d, d + dn);
    }
    fr[0] = (uint8_t)(fr.size() - 1);
    replies.push_back(fr);
  }

  void Read(uint8_t what, uint8_t arg) {
    Settle();
    uint8_t d[8];
    switch (what) {
      case 0:
        if (!has_active || arg > 30) { Reply(0, arg, d, 0); return; }
        { uint8_t pad[48]; memset(pad, 0xFF, sizeof pad); memcpy(pad, active, 38);
          if (arg == corrupt_read_arg) pad[arg + 1] ^= 0x40;
          Reply(0, arg, pad + arg, 8); }
        return;
      case 2:
        d[0] = (uint8_t)((has_active && !status_hides_valid ? 1 : 0) | (OverrideActive() ? 2 : 0) |
                         (ok_bit ? 4 : 0) | (refused_bit ? 8 : 0));
        d[1] = status_fmt;
        d[2] = (uint8_t)((has_active ? seq : 0) >> 8);
        d[3] = (uint8_t)(has_active ? seq : 0);
        Reply(2, arg, d, 4);
        return;
      default: return;
    }
  }
};
