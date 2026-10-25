// Host tests for the 251e calibration request/reply client (src/Cal251eBus.h) against
// Sim251e (sim251e.h), a model of firmware 30.7C4.
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_cal251e_bus
//   test_cal251e_bus.cpp ../src/Cal251eBus.cpp ../src/CalRec.cpp ../src/CalRec251e.cpp
#include <cstdio>
#include <cstring>

#include "sim251e.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct FakeClock : CalRoutine::Clock {
  uint32_t t = 0;
  uint32_t Ms() const override { return t; }
};

struct Rig {
  FakeClock clk;
  Sim251e sim;
  Cal251eBus bus;
  explicit Rig(uint32_t start = 1000) {
    clk.t = start;
    sim.clock = &clk;
    bus.Begin(sim, clk, 0x5C);
  }
  int Run(uint32_t max_ms = 20000) {
    for (uint32_t i = 0; i < max_ms; ++i) {
      const int r = bus.Tick();
      if (r != 0) return r;
      ++clk.t;
    }
    return 0;
  }
};

static void MakeRecord(uint8_t out[38], uint16_t seq, uint16_t g0 = 0x4012) {
  CalRec251e::Record r;
  memset(&r, 0, sizeof r);
  for (int i = 0; i < 8; ++i) r.unit_id[i] = (uint8_t)(0x10 + i);
  r.seq = seq;
  r.date = 0x01020304;
  r.ch[0] = {g0, -7};
  r.ch[1] = {0x3FF0, 3};
  r.ch[2] = {0x41A0, -12};
  r.ch[3] = {0x4000, 0};
  CalRec251e::Encode(r, out);
}
static uint16_t RecCrc(const uint8_t rec[38]) { return (uint16_t)((rec[36] << 8) | rec[37]); }

static void put_commit_roundtrip() {
  Rig g;
  uint8_t rec[38];
  MakeRecord(rec, 7);
  CHECK(g.bus.PutRecord(rec));
  CHECK(!g.bus.ReadStatus());  // in flight
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdPut] == 5);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 0);
  CHECK(g.bus.Commit(RecCrc(rec)));
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  CHECK((g.bus.status().flags & 0x05) == 0x05);
  CHECK(g.bus.status().fmt == 1 && g.bus.status().seq == 7);
  CHECK(g.bus.ReadRecord());
  CHECK(g.Run() == 1);
  CHECK(g.bus.has_record());
  CHECK(memcmp(g.bus.record(), rec, 38) == 0);
  CHECK(g.sim.count[CalRec::kCmdRead] == 1 + 5);   // the status poll after the COMMIT + 5 chunks
  int k = 0;
  const int off[5] = {0, 8, 16, 24, 32}, len[5] = {8, 8, 8, 8, 6};
  for (auto &f : g.sim.log)
    if (f[3] == CalRec::kCmdPut) { CHECK(f[6] == off[k] && f.size() == (size_t)(7 + len[k])); ++k; }
  // The COMMIT frame: length 38, the CRC, no flags byte (n = 8).
  for (auto &f : g.sim.log)
    if (f[3] == CalRec::kCmdCommit) {
      CHECK(f.size() == 9 && f[0] == 8 && f[6] == 38 && f[7] == (RecCrc(rec) >> 8) && f[8] == (RecCrc(rec) & 0xFF));
    }
  // READ args: 0, 8, 16, 24, 30 (arg > 30 returns nothing).
  const uint8_t args[5] = {0, 8, 16, 24, 30};
  k = 0;
  for (auto &f : g.sim.log)
    if (f[3] == CalRec::kCmdRead && f[6] == 0) { CHECK(f[7] == args[k]); ++k; }
  CHECK(k == 5);
  CHECK(g.sim.bad_frames == 0);
}

static void no_record_is_distinct_outcome() {
  Rig g;
  CHECK(g.bus.ReadRecord());
  CHECK(g.Run() == 1);
  CHECK(!g.bus.has_record());
  CHECK(g.sim.count[CalRec::kCmdRead] == 1);
  CHECK(g.bus.ReadStatus());
  CHECK(g.Run() == 1);
  CHECK((g.bus.status().flags & 1) == 0 && g.bus.status().seq == 0);
}

// Bytes 30 and 31 are read twice (chunks at arg 24 and 30). A flipped byte at 31 (arg 30's data[1])
// disagrees with the first read; a flipped byte read once is only caught by the record's CRC.
static void overlap_disagreement_fails() {
  uint8_t rec[38];
  MakeRecord(rec, 5);
  {
    Rig g;
    g.sim.Install(rec);
    g.sim.corrupt_read_arg = 30;
    CHECK(g.bus.ReadRecord());
    CHECK(g.Run() == E_INCONSISTENT && !g.bus.has_record());
  }
  {
    Rig g;
    g.sim.Install(rec);
    g.sim.corrupt_read_arg = 8;   // offset 9, read once
    CHECK(g.bus.ReadRecord());
    CHECK(g.Run() == 1 && g.bus.has_record());
    CalRec251e::Record r;
    CHECK(!CalRec251e::Decode(g.bus.record(), 38, &r));  // the CRC is the check
  }
  Rig ok;
  ok.sim.Install(rec);
  CHECK(ok.bus.ReadRecord() && ok.Run() == 1 && ok.bus.has_record());
}

static void override_and_release() {
  Rig g;
  CHECK(!g.bus.Override(4, 100));      // only channels 0..3
  CHECK(!g.bus.Override(0, 4096));     // only 12-bit codes
  CHECK(g.bus.Override(2, 1234));
  CHECK(!g.bus.Override(1, 5));        // one operation at a time
  CHECK(g.Run() == 1);                 // done when the frame is ACKed: no reply exists
  CHECK(g.sim.replies.empty());
  CHECK(g.sim.ovr[2] == 1234 && g.sim.ovr[0] == -1);
  const uint8_t want[9] = {8, 0x00, 0x22, 0x10, 0x5C, 0xCA, 2, 0x04, 0xD2};
  CHECK(g.sim.log.size() == 1 && g.sim.log[0].size() == 9 && memcmp(g.sim.log[0].data(), want, 9) == 0);
  CHECK(g.bus.ReadStatus() && g.Run() == 1);
  CHECK(g.bus.status().flags & Cal251eBus::kStatusOverride);   // bit1 shows it
  CHECK(g.bus.Release());
  CHECK(g.Run() == 1);
  const uint8_t rel[6] = {5, 0x00, 0x22, 0x11, 0x5C, 0xCA};
  CHECK(g.sim.log.back().size() == 6 && memcmp(g.sim.log.back().data(), rel, 6) == 0);
  CHECK(g.sim.ovr[2] == -1);
  CHECK(g.bus.ReadStatus() && g.Run() == 1);
  CHECK(!(g.bus.status().flags & Cal251eBus::kStatusOverride));
  CHECK(g.sim.bad_frames == 0);
  // The module drops overrides 5 s after the last one (a model check the flow relies on).
  Rig h;
  h.bus.Override(1, 700);
  h.Run();
  h.clk.t += 4900;
  CHECK(h.sim.ovr[1] == 700 && h.sim.OverrideActive());
  h.clk.t += 200;
  CHECK(!h.sim.OverrideActive());
}

static void override_send_failures() {
  Rig g;
  g.sim.nak = 2;
  CHECK(g.bus.Override(0, 10));
  CHECK(g.Run() == E_BUS_ERROR);
  Rig h;
  h.sim.busy_sends = 3;
  CHECK(h.bus.Override(0, 10));
  CHECK(h.Run() == 1);
  CHECK(h.sim.ovr[0] == 10);
  Rig b;
  b.sim.busy_sends = -1;
  CHECK(b.bus.Release());
  CHECK(b.Run() == E_BUS_BUSY);
}

static void commit_never_resent() {
  Rig g;
  uint8_t rec[38];
  MakeRecord(rec, 3);
  g.bus.PutRecord(rec);
  CHECK(g.Run() == 1);
  g.sim.drop_replies = true;
  CHECK(g.bus.Commit(RecCrc(rec)));
  CHECK(g.Run() == E_COMMIT_UNKNOWN);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  CHECK(g.sim.count[CalRec::kCmdRead] >= 4);
  CHECK(g.clk.t - 1000 >= Cal251eBus::kCommitWaitMs + Cal251eBus::kCommitVerdictMs);
  CHECK(g.bus.Tick() == E_COMMIT_UNKNOWN);
}

static void commit_waits_then_ok() {
  Rig g;
  uint8_t rec[38];
  MakeRecord(rec, 3);
  g.bus.PutRecord(rec);
  g.Run();
  const uint32_t t0 = g.clk.t;
  g.bus.Commit(RecCrc(rec));
  CHECK(g.Run() == 1);
  CHECK(g.clk.t - t0 >= Cal251eBus::kCommitWaitMs);
}

static void commit_refused_and_torn() {
  Rig g;
  uint8_t rec[38];
  MakeRecord(rec, 3);
  g.bus.PutRecord(rec);
  g.Run();
  CHECK(g.bus.ReadStatus() && g.Run() == 1);     // the baseline the refused bit is judged against
  CHECK(g.bus.Commit((uint16_t)(RecCrc(rec) ^ 1)));
  CHECK(g.Run() == E_COMMIT_REFUSED);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  Rig t;                       // a torn page: no verdict, and the module has no valid record
  t.bus.PutRecord(rec);
  t.Run();
  t.sim.tear_commit = true;
  t.bus.Commit(RecCrc(rec));
  CHECK(t.Run() == E_COMMIT_UNKNOWN);
  CHECK(!t.sim.has_active);
}

// One page, rewritten every time: a COMMIT of the same record again is ok and does not fail
// with a "no slot" outcome; there is no such outcome for the 251e.
static void repeated_commit_rewrites() {
  Rig g;
  uint8_t rec[38];
  MakeRecord(rec, 3);
  for (int i = 0; i < 12; ++i) {
    g.bus.PutRecord(rec);
    CHECK(g.Run() == 1);
    g.bus.Commit(RecCrc(rec));
    CHECK(g.Run() == 1);
  }
  CHECK(g.sim.commits_received == 12);
  CHECK(g.sim.bad_frames == 0);
}

static void commit_wire_error_is_polled_not_resent() {
  uint8_t rec[38];
  MakeRecord(rec, 3);
  {
    Rig g;                      // the frame was latched: the status shows it
    g.bus.PutRecord(rec); g.Run();
    g.sim.commit_send_rc = 4;
    g.sim.commit_latches = true;
    g.bus.Commit(RecCrc(rec));
    CHECK(g.Run() == 1);
    CHECK(g.sim.count[CalRec::kCmdCommit] == 1 && g.sim.commits_received == 1);
  }
  {
    Rig g;                      // not latched: unknown, still not resent
    g.bus.PutRecord(rec); g.Run();
    g.sim.commit_send_rc = 4;
    g.sim.commit_latches = false;
    g.bus.Commit(RecCrc(rec));
    CHECK(g.Run() == E_COMMIT_UNKNOWN);
    CHECK(g.sim.count[CalRec::kCmdCommit] == 1 && g.sim.commits_received == 0);
  }
  {
    Rig g;                      // the bus gate shut (-1): nothing went out, so it retries safely
    g.bus.PutRecord(rec); g.Run();
    g.sim.busy_sends = 5;
    g.bus.Commit(RecCrc(rec));
    CHECK(g.Run() == 1);
    CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  }
}

static void stale_ok_bit_is_not_a_verdict() {
  Rig g;
  uint8_t rec[38], rec2[38];
  MakeRecord(rec, 7);
  g.bus.PutRecord(rec); g.Run();
  g.bus.Commit(RecCrc(rec));
  CHECK(g.Run() == 1);
  MakeRecord(rec2, 8);
  g.bus.PutRecord(rec2); g.Run();
  g.sim.drop_commits = 1;          // the next COMMIT frame never reaches the module
  g.bus.Commit(RecCrc(rec2));
  CHECK(g.Run() == E_COMMIT_UNKNOWN);   // not 1 from the stale bit2
  CHECK(g.sim.commits_received == 1 && g.bus.status().seq == 7);
}

static void ok_bit_needs_valid_and_seq() {
  uint8_t rec[38];
  MakeRecord(rec, 7);
  Rig g;
  g.bus.PutRecord(rec); g.Run();
  g.sim.status_hides_valid = true;
  g.bus.Commit(RecCrc(rec));
  CHECK(g.Run() == E_COMMIT_UNKNOWN);
  CHECK(g.sim.ok_bit && g.sim.seq == 7);
}

static void replies_and_lengths() {
  Rig g;
  g.sim.bad_reply_len = 3;           // a status reply of the wrong length
  g.bus.ReadStatus();
  CHECK(g.Run() == E_BAD_REPLY);
  Rig h;
  h.sim.drop_replies = true;
  h.bus.ReadStatus();
  CHECK(h.Run() == E_NO_REPLY);
  CHECK(h.sim.count[CalRec::kCmdRead] == 1 + Cal251eBus::kReadRetries);   // 3 retries
  Rig w;                              // a module at another address does not answer us
  w.bus.Begin(w.sim, w.clk, 0x5D);
  w.bus.ReadStatus();
  CHECK(w.Run() == E_NO_REPLY);
}

// The module ignores a whole frame that is the wrong length (strict lengths).
static void module_strict_lengths() {
  Sim251e s;
  FakeClock c;
  s.clock = &c;
  const uint8_t long_ovr[10] = {9, 0, 0x22, 0x10, 0x5C, 0xCA, 1, 0, 5, 0};
  s.Send(long_ovr, 10);
  CHECK(s.ovr[1] == -1 && s.bad_frames == 1);
  const uint8_t big[9] = {8, 0, 0x22, 0x10, 0x5C, 0xCA, 1, 0x10, 0x00};   // code 4096
  s.Send(big, 9);
  CHECK(s.ovr[1] == -1);
  const uint8_t four[9] = {8, 0, 0x22, 0x10, 0x5C, 0xCA, 4, 0, 5};          // channel 4 (a pulse output)
  s.Send(four, 9);
  CHECK(s.overrides == 0);
}

// Firmware 30.7C4 and 30.7C5 differ in what the commit-result bits show while a COMMIT runs (C4:
// the last value; C5: cleared at once). The client must be right for both, never resend, and say
// "unknown" rather than believe a bit that may be stale.
static void Staged(Rig &g, uint16_t seq, uint8_t rec[38]) {
  MakeRecord(rec, seq);
  g.bus.PutRecord(rec);
  g.Run();
}
static void commit_on_c4_and_c5() {
  uint8_t rec[38];
  for (int c5 = 0; c5 < 2; ++c5) {
    {   // a slow COMMIT: the poll goes through "in progress" (C5: bits 2/3 read 0) to ok
      Rig g;
      g.sim.fw_c5 = c5;
      g.sim.commit_ms = 1200;
      Staged(g, 4, rec);
      g.bus.ReadStatus(); g.Run();
      g.bus.Commit(RecCrc(rec));
      CHECK(g.Run() == 1);
      CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
      CHECK(g.sim.count[CalRec::kCmdRead] >= 3);          // it polled more than once
      CHECK(g.bus.status().seq == 4 && (g.bus.status().flags & 5) == 5);
    }
    {   // the COMMIT is accepted and never finishes: unknown, never resent
      Rig g;
      g.sim.fw_c5 = c5;
      g.sim.stuck_commit = true;
      Staged(g, 4, rec);
      g.bus.ReadStatus(); g.Run();
      g.bus.Commit(RecCrc(rec));
      CHECK(g.Run() == E_COMMIT_UNKNOWN);
      CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
      CHECK(!g.sim.has_active);
    }
    {   // a refusal after a clean baseline is a verdict
      Rig g;
      g.sim.fw_c5 = c5;
      g.sim.commit_ms = 600;
      Staged(g, 4, rec);
      g.bus.ReadStatus(); g.Run();
      g.bus.Commit((uint16_t)(RecCrc(rec) ^ 1));
      CHECK(g.Run() == E_COMMIT_REFUSED);
    }
    {   // a refused bit already showing before the COMMIT proves nothing about a lost frame
      Rig g;
      g.sim.fw_c5 = c5;
      Staged(g, 4, rec);
      g.bus.ReadStatus(); g.Run();
      g.bus.Commit((uint16_t)(RecCrc(rec) ^ 1));
      CHECK(g.Run() == E_COMMIT_REFUSED);          // sets the module's refused bit
      g.bus.PutRecord(rec); g.Run();
      g.bus.ReadStatus(); g.Run();
      CHECK(g.bus.status().flags & 8);             // the baseline now has it set
      g.sim.drop_commits = 1;                      // ... and the next COMMIT frame is lost
      g.bus.Commit(RecCrc(rec));
      CHECK(g.Run() == E_COMMIT_UNKNOWN);          // not REFUSED from the stale bit
      CHECK(g.sim.count[CalRec::kCmdCommit] == 2 && g.sim.commits_received == 1);
    }
    {   // no baseline read: a refused bit is not believed either
      Rig g;
      g.sim.fw_c5 = c5;
      Staged(g, 4, rec);
      g.bus.Commit((uint16_t)(RecCrc(rec) ^ 1));
      CHECK(g.Run() == E_COMMIT_UNKNOWN);
    }
  }
  {   // C5 only: the accepted COMMIT clears an old ok bit at once, so a lost one cannot hide behind it
    Rig g;
    g.sim.fw_c5 = true;
    uint8_t r1[38];
    Staged(g, 7, r1);
    g.bus.Commit(RecCrc(r1)); CHECK(g.Run() == 1);
    uint8_t r2[38];
    Staged(g, 8, r2);
    g.bus.ReadStatus(); g.Run();
    g.sim.stuck_commit = true;                     // accepted, unfinished: C5 shows no result
    g.bus.Commit(RecCrc(r2));
    CHECK(g.Run() == E_COMMIT_UNKNOWN);
    CHECK((g.bus.status().flags & 0x0C) == 0);
  }
}

int main() {
  commit_on_c4_and_c5();
  put_commit_roundtrip();
  no_record_is_distinct_outcome();
  overlap_disagreement_fails();
  override_and_release();
  override_send_failures();
  commit_never_resent();
  commit_waits_then_ok();
  commit_refused_and_torn();
  repeated_commit_rewrites();
  commit_wire_error_is_polled_not_resent();
  stale_ok_bit_is_not_a_verdict();
  ok_bit_needs_valid_and_seq();
  replies_and_lengths();
  module_strict_lengths();
  printf("test_cal251e_bus: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
