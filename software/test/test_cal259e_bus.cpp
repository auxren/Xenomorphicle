// Host tests for the 259e calibration request/reply client (src/Cal259eBus.h)
// against Sim259e (sim259e.h), a model of firmware 31.2C4.
//   g++ -std=c++17 -Wall -Wsign-compare -Werror -O2 -o build/test_cal259e_bus
//   test_cal259e_bus.cpp ../src/Cal259eBus.cpp ../src/CalRec.cpp && ./build/test_cal259e_bus
#include <cstdio>
#include <cstring>

#include "sim259e.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
  printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct FakeClock : CalRoutine::Clock {
  uint32_t t = 0;
  uint32_t Ms() const override { return t; }
};

struct Rig {
  FakeClock clk;
  Sim259e sim;
  Cal259eBus bus;
  Rig(uint32_t start = 1000) {
    clk.t = start;
    sim.clock = &clk;
    bus.Begin(sim, clk, 0x28);
  }
  // Ticks, 1 ms per tick, until the operation stops; 0 if it never does.
  int Run(uint32_t max_ms = 20000) {
    for (uint32_t i = 0; i < max_ms; ++i) {
      const int r = bus.Tick();
      if (r != 0) return r;
      ++clk.t;
    }
    return 0;
  }
};

static void MakeRecord(uint8_t out[30], uint16_t seq, uint16_t g0 = 0x9041) {
  CalRec::Record r;
  memset(&r, 0, sizeof r);
  for (int i = 0; i < 8; ++i) r.unit_id[i] = (uint8_t)(0x10 + i);
  r.seq = seq;
  r.date = 0x01020304;
  r.ch[0].gain = g0; r.ch[0].offset = -5;
  r.ch[1].gain = 0x9100; r.ch[1].offset = 7;
  CalRec::Encode259e(r, out);
}

static uint16_t RecCrc(const uint8_t rec[30]) { return (uint16_t)((rec[28] << 8) | rec[29]); }

static void put_commit_roundtrip() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 7);
  CHECK(g.bus.PutRecord(rec));
  CHECK(!g.bus.ReadStatus());  // in flight
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdPut] == 4);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 0);
  CHECK(g.bus.Commit(RecCrc(rec), 0));
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  CHECK(g.bus.status().flags & 0x04);
  CHECK(g.bus.status().flags & 0x01);
  CHECK(g.bus.status().fmt == 1);
  CHECK(g.bus.status().seq == 7);
  CHECK(g.bus.ReadRecord());
  CHECK(g.Run() == 1);
  CHECK(g.bus.has_record());
  CHECK(memcmp(g.bus.record(), rec, 30) == 0);
  // PUT offsets and lengths: 0/8, 8/8, 16/8, 24/6
  int k = 0;
  const int off[4] = {0, 8, 16, 24}, len[4] = {8, 8, 8, 6};
  for (auto &f : g.sim.log)
    if (f[3] == CalRec::kCmdPut) {
      CHECK(f[6] == off[k] && f.size() == (size_t)(7 + len[k]));
      ++k;
    }
  CHECK(g.sim.bad_frames == 0);
}

static void no_record_is_distinct_outcome() {
  Rig g;
  CHECK(g.bus.ReadRecord());
  CHECK(g.Run() == 1);
  CHECK(!g.bus.has_record());
  CHECK(g.sim.count[CalRec::kCmdRead] == 1);  // stopped after the first reply
  CHECK(g.bus.ReadStatus());
  CHECK(g.Run() == 1);
  CHECK((g.bus.status().flags & 1) == 0);
}

static void commit_never_resent() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 3);
  g.bus.PutRecord(rec);
  CHECK(g.Run() == 1);
  g.sim.drop_replies = true;
  CHECK(g.bus.Commit(RecCrc(rec), 0));
  CHECK(g.Run() == E_COMMIT_UNKNOWN);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  CHECK(g.sim.count[CalRec::kCmdRead] >= 4);   // it kept asking within the window
  // 1500 ms wait + 3000 ms window
  CHECK(g.clk.t - 1000 >= 4500);
  CHECK(g.bus.Tick() == E_COMMIT_UNKNOWN);     // the result stays
}

static void commit_waits_for_the_flash() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 3);
  g.bus.PutRecord(rec);
  g.Run();
  const uint32_t t0 = g.clk.t;
  g.bus.Commit(RecCrc(rec), 0);
  CHECK(g.Run() == 1);
  CHECK(g.clk.t - t0 >= Cal259eBus::kCommitWaitMs);
  CHECK(g.sim.lost == 0);  // nothing was sent into the module's blind window
}

static void commit_refused() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 3);
  g.bus.PutRecord(rec);
  g.Run();
  CHECK(g.bus.Commit((uint16_t)(RecCrc(rec) ^ 1), 0));  // wrong crc
  CHECK(g.Run() == E_COMMIT_REFUSED);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
}

static void retried_commit_is_deduplicated() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 3);
  g.bus.PutRecord(rec);
  g.Run();
  g.bus.Commit(RecCrc(rec), 0);
  CHECK(g.Run() == 1);
  CHECK(g.sim.slots_used == 1);
  g.bus.Commit(RecCrc(rec), 0);   // byte-equal: ok, no new slot
  CHECK(g.Run() == 1);
  CHECK(g.sim.slots_used == 1);
}

static void slots_full_reports_no_slot() {
  Rig g;
  for (int i = 0; i < 8; ++i) {
    uint8_t rec[30];
    MakeRecord(rec, (uint16_t)(i + 1));
    g.bus.PutRecord(rec);
    CHECK(g.Run() == 1);
    g.bus.Commit(RecCrc(rec), 0);
    CHECK(g.Run() == 1);
  }
  CHECK(g.sim.slots_used == 8);
  uint8_t rec[30];
  MakeRecord(rec, 99);
  g.bus.PutRecord(rec);
  g.Run();
  CHECK(g.bus.Commit(RecCrc(rec), 0));
  CHECK(g.Run() == E_NO_SLOT);
  CHECK((g.bus.status().flags & 0x18) == 0x18);
  // flags 1: compaction
  g.bus.PutRecord(rec);
  g.Run();
  CHECK(g.bus.Commit(RecCrc(rec), 1));
  CHECK(g.Run() == 1);
  CHECK(g.sim.slots_used == 1);
  CHECK(g.bus.status().seq == 99);
  CHECK(g.sim.bad_frames == 0);
}

static void reply_from_other_address_ignored() {
  Rig g;
  g.sim.other_addr = 0x29;
  CHECK(g.bus.ReadAdc(0));
  CHECK(g.Run() == 1);
  CHECK(g.bus.adc() == 0x1234);
  CHECK(g.sim.count[CalRec::kCmdRead] == 1);
  g.bus.ReadStatus();
  CHECK(g.Run() == 1);
  CHECK(g.bus.status().fmt == 1);
  // only the other module answers: no reply for us
  Rig h;
  h.sim.other_addr = 0x29;
  h.sim.drop_replies = false;
  h.bus.Begin(h.sim, h.clk, 0x2A);   // nobody at 0x2A: the sim ignores the frame
  h.bus.ReadStatus();
  CHECK(h.Run() == E_NO_REPLY);
}

static void stale_adc_counter_rejected() {
  Rig g;
  g.bus.ReadAdc(0);
  CHECK(g.Run() == 1);
  const uint8_t n1 = g.bus.adc_n();
  g.bus.ReadAdc(0);
  CHECK(g.Run() == 1);
  CHECK(g.bus.adc_n() == n1);   // no sample tick: the caller sees the same n
  g.sim.SampleTick();
  g.bus.ReadAdc(1);
  CHECK(g.Run() == 1);
  CHECK(g.bus.adc_n() == (uint8_t)(n1 + 1));
  CHECK(g.bus.adc() == 0x2345);
  CHECK(!g.bus.ReadAdc(2));
}

static void busy_bus_retries_then_fails() {
  Rig g;
  g.sim.busy_sends = 3;
  g.bus.ReadStatus();
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdRead] == 1);
  Rig h;
  h.sim.busy_sends = -1;
  h.bus.ReadStatus();
  const uint32_t t0 = h.clk.t;
  CHECK(h.Run() == E_BUS_BUSY);
  CHECK(h.clk.t - t0 >= Cal259eBus::kBusyGiveUpMs);
  CHECK(h.clk.t - t0 <= Cal259eBus::kBusyGiveUpMs + 10);
  CHECK(h.sim.log.empty());
  // a busy bus before COMMIT: nothing went out, so retrying is safe
  Rig c;
  uint8_t rec[30];
  MakeRecord(rec, 1);
  c.bus.PutRecord(rec);
  c.Run();
  c.sim.busy_sends = 5;
  c.bus.Commit(RecCrc(rec), 0);
  CHECK(c.Run() == 1);
  CHECK(c.sim.count[CalRec::kCmdCommit] == 1);
}

static void send_error_is_bus_error() {
  Rig g;
  g.sim.nak = 2;
  g.bus.ReadStatus();
  CHECK(g.Run() == E_BUS_ERROR);
  Rig h;
  uint8_t rec[30];
  MakeRecord(rec, 1);
  h.bus.PutRecord(rec);
  h.Run();
  h.sim.nak = 4;
  h.bus.Commit(RecCrc(rec), 0);
  // A Wire error on the COMMIT is not "nothing went out" (S4): it is polled for
  // its status, and nothing says it landed here.
  CHECK(h.Run() == E_COMMIT_UNKNOWN);
  CHECK(h.sim.count[CalRec::kCmdCommit] == 0);  // and not resent
}

// S2: bits 2/3 of the status describe the last commit since power-up. A COMMIT
// frame lost after an earlier OK commit must not be reported OK from the stale bit.
static void stale_ok_bit_is_not_a_verdict() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 7);
  g.bus.PutRecord(rec); g.Run();
  g.bus.Commit(RecCrc(rec), 0);
  CHECK(g.Run() == 1);                       // the earlier commit: ok, seq 7
  CHECK(g.sim.ok_bit && g.sim.seq == 7);
  uint8_t rec2[30];
  MakeRecord(rec2, 8);
  g.bus.PutRecord(rec2); g.Run();
  g.sim.drop_commits = 1;                    // the next COMMIT frame never reaches the module
  g.bus.Commit(RecCrc(rec2), 0);
  CHECK(g.Run() == E_COMMIT_UNKNOWN);        // not 1 from the stale bit2
  CHECK(g.sim.commits_received == 1);        // only the earlier one landed
  CHECK(g.sim.count[CalRec::kCmdCommit] == 2);  // one earlier + exactly one, not resent
  CHECK(g.bus.status().seq == 7);
  CHECK(g.clk.t >= 1000 + Cal259eBus::kCommitWaitMs + Cal259eBus::kCommitVerdictMs);
}

// L5 (S2's bit0 term): bit2 ok + the right seq are not enough, the module must also say it
// holds a valid record (bit0). Without that term this ends 1.
static void ok_bit_without_a_valid_record_is_not_a_verdict() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 7);
  g.bus.PutRecord(rec); g.Run();
  g.sim.status_hides_valid = true;
  g.bus.Commit(RecCrc(rec), 0);
  CHECK(g.Run() == E_COMMIT_UNKNOWN);
  CHECK(g.sim.ok_bit && g.sim.seq == 7);     // bit2 and the seq were both right
  CHECK((g.bus.status().flags & 0x05) == 0x04);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  Rig h;                                       // and with bit0 shown it is a verdict
  h.bus.PutRecord(rec); h.Run();
  h.bus.Commit(RecCrc(rec), 0);
  CHECK(h.Run() == 1);
}

// S2: the same rig with the frame delivered reports OK at the new seq.
static void second_commit_ok_needs_the_new_seq() {
  Rig g;
  uint8_t rec[30], rec2[30];
  MakeRecord(rec, 7);
  MakeRecord(rec2, 8);
  g.bus.PutRecord(rec); g.Run();
  g.bus.Commit(RecCrc(rec), 0); g.Run();
  g.bus.PutRecord(rec2); g.Run();
  g.bus.Commit(RecCrc(rec2), 0);
  CHECK(g.Run() == 1);
  CHECK(g.bus.status().seq == 8);
}

// S4: a Wire error on the COMMIT. The module may have latched the frame (a NAK on
// the last byte): poll, and exactly one COMMIT goes out whatever the outcome.
static void commit_wire_error_latched_is_polled() {
  for (int rc : {2, 3, 4, 5}) {
    Rig g;
    uint8_t rec[30];
    MakeRecord(rec, 5);
    g.bus.PutRecord(rec); g.Run();
    g.sim.commit_send_rc = rc;
    g.sim.commit_latches = true;
    CHECK(g.bus.Commit(RecCrc(rec), 0));
    CHECK(g.Run() == 1);                       // the status shows the record at seq 5
    CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
    CHECK(g.sim.commits_received == 1);
    CHECK(g.bus.sends() >= 1);  // the COMMIT counts as sent, plus the status reads
    CHECK(g.bus.last_send_error() == rc);
    CHECK(g.clk.t - 1000 >= Cal259eBus::kCommitWaitMs);  // it waited out the flash rewrite first
  }
}

static void commit_wire_error_not_latched_is_unknown() {
  for (int rc : {2, 3, 4, 5}) {
    Rig g;
    uint8_t rec[30];
    MakeRecord(rec, 5);
    g.bus.PutRecord(rec); g.Run();
    g.sim.commit_send_rc = rc;
    g.sim.commit_latches = false;
    g.bus.Commit(RecCrc(rec), 0);
    CHECK(g.Run() == E_COMMIT_UNKNOWN);
    CHECK(g.sim.count[CalRec::kCmdCommit] == 1);   // never resent
    CHECK(g.sim.commits_received == 0);
    CHECK(g.bus.sends() >= 1);  // the COMMIT counts as sent, plus the status reads
  }
}

// The one send error that IS safe to retry: -1, the tx gate closed.
static void commit_gate_closed_still_retries() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 5);
  g.bus.PutRecord(rec); g.Run();
  g.sim.busy_sends = 3;
  g.bus.Commit(RecCrc(rec), 0);
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
}

static void read_timeout_retries_then_fails() {
  Rig g;
  g.sim.drop_replies = true;
  g.bus.ReadStatus();
  const uint32_t t0 = g.clk.t;
  CHECK(g.Run() == E_NO_REPLY);
  CHECK(g.sim.count[CalRec::kCmdRead] == 4);   // 1 + kReadRetries
  CHECK(g.bus.sends() == 4);
  CHECK(g.clk.t - t0 >= 4 * Cal259eBus::kReplyTimeoutMs);
  // a reply that arrives on the last retry is accepted
  Rig h;
  h.sim.drop_replies = true;
  h.bus.ReadStatus();
  while (h.sim.count[CalRec::kCmdRead] < 3) { h.bus.Tick(); ++h.clk.t; }
  h.sim.drop_replies = false;
  CHECK(h.Run() == 1);
}

static void bad_reply_length() {
  const int lens[3] = {5, 2, 3};   // what 0: 5; what 1: 2; what 2: 3
  Rig a; a.sim.bad_reply_len = lens[0];
  uint8_t rec[30]; MakeRecord(rec, 1); a.sim.Install(rec);
  a.bus.ReadRecord();
  CHECK(a.Run() == E_BAD_REPLY);
  Rig b; b.sim.bad_reply_len = lens[1];
  b.bus.ReadAdc(0);
  CHECK(b.Run() == E_BAD_REPLY);
  Rig c; c.sim.bad_reply_len = lens[2];
  c.bus.ReadStatus();
  CHECK(c.Run() == E_BAD_REPLY);
}

static void record_read_uses_args_the_module_answers() {
  // The module (31.2C4) corrupts data bytes 5 and 7 of every 8-byte reply (sim259e.h), so
  // the record is assembled from bytes 0..4 and 6 of overlapping reads, and the unreadable
  // last byte (record offset 29) is the low CRC byte, inferred. Args stay <= 22.
  for (int fixed = 0; fixed < 2; ++fixed) {
    Rig g;
    g.sim.stock_scratch_overlap = !fixed;
    uint8_t rec[30];
    MakeRecord(rec, 5);
    g.sim.Install(rec);
    g.bus.ReadRecord();
    CHECK(g.Run() == 1);
    CHECK(g.bus.has_record() && memcmp(g.bus.record(), rec, 30) == 0);
    int k = 0;
    const int args[7] = {0, 5, 10, 15, 20, 21, 22};
    for (auto &f : g.sim.log) { CHECK(k < 7 && f[7] == args[k]); ++k; }
    CHECK(k == 7);
  }
}

// A record whose bytes sit exactly at the positions the module corrupts (5, 7, 13, 15, ...)
// must read back exactly; a stored byte that differs must show.
static void record_read_back_detects_a_wrong_stored_byte() {
  for (int pos = 0; pos < 29; ++pos) {
    Rig g;
    uint8_t rec[30], bad[30];
    MakeRecord(rec, 9);
    memcpy(bad, rec, 30);
    bad[pos] ^= 0x55;                      // a record the module holds that is not rec
    g.sim.Install(bad);
    g.bus.ReadRecord();
    CHECK(g.Run() == 1);
    CHECK(memcmp(g.bus.record(), rec, 30) != 0);
  }
}

static void every_sent_frame_allowed() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 2);
  g.bus.ReadStatus(); g.Run();
  g.bus.ReadAdc(0); g.Run();
  g.bus.ReadRecord(); g.Run();
  g.bus.PutRecord(rec); g.Run();
  g.bus.Commit(RecCrc(rec), 0); g.Run();
  g.bus.PutRecord(rec); g.Run();
  g.bus.Commit(RecCrc(rec), 1); g.Run();
  CHECK(g.sim.log.size() > 10);
  for (auto &f : g.sim.log) CHECK(CalRec::FrameAllowed(f.data(), (uint8_t)f.size()));
  CHECK(g.sim.bad_frames == 0);
  CHECK(g.sim.count[CalRec::kCmdOverride] == 0 && g.sim.count[CalRec::kCmdRelease] == 0);
}

static void stale_replies_are_dropped() {
  Rig g;
  // an old reply sitting in the ring must not satisfy a new request
  const uint8_t old[] = {9, 0x22, 0x28, 0x19, 1, 0, 0xAA, 0xBB, 0x00};
  g.sim.replies.emplace_back(old, old + sizeof old);
  g.bus.ReadAdc(0);
  CHECK(g.Run() == 1);
  CHECK(g.bus.adc() == 0x1234);
}

static void AllFramesIntact(Rig &g) {
  CHECK(g.sim.bad_frames == 0);
  for (auto &f : g.sim.log) CHECK(CalRec::FrameAllowed(f.data(), (uint8_t)f.size()));
}

static void refused_commit_during_put_sends_nothing() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 1);
  g.sim.busy_sends = -1;
  CHECK(g.bus.PutRecord(rec));
  for (int i = 0; i < 40; ++i) { g.bus.Tick(); ++g.clk.t; }
  CHECK(!g.bus.Commit(RecCrc(rec), 0));
  CHECK(!g.bus.Commit(RecCrc(rec), 1));
  g.sim.busy_sends = 0;
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 0);
  CHECK(g.sim.count[CalRec::kCmdPut] == 4);
  AllFramesIntact(g);
}

static void refused_commit_during_read_sends_nothing() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 1);
  g.sim.drop_replies = true;
  CHECK(g.bus.ReadStatus());
  while (g.sim.count[CalRec::kCmdRead] < 2) { g.bus.Tick(); ++g.clk.t; }
  CHECK(!g.bus.Commit(RecCrc(rec), 1));
  g.sim.drop_replies = false;
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 0);
  AllFramesIntact(g);
  for (auto &f : g.sim.log) CHECK(f[3] == CalRec::kCmdRead && f.size() == 8);
}

static void refused_put_during_read_sends_nothing() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 1);
  g.sim.drop_replies = true;
  CHECK(g.bus.ReadStatus());
  while (g.sim.count[CalRec::kCmdRead] < 2) { g.bus.Tick(); ++g.clk.t; }
  CHECK(!g.bus.PutRecord(rec));
  CHECK(!g.bus.ReadAdc(0));
  CHECK(!g.bus.ReadRecord());
  g.sim.drop_replies = false;
  CHECK(g.Run() == 1);
  CHECK(g.sim.count[CalRec::kCmdPut] == 0 && g.sim.count[CalRec::kCmdCommit] == 0);
  for (auto &f : g.sim.log) CHECK(f[3] == CalRec::kCmdRead && f[6] == 2);
  AllFramesIntact(g);
}

static void post_commit_send_error_is_unknown() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 1);
  g.bus.PutRecord(rec); g.Run();
  g.bus.Commit(RecCrc(rec), 0);
  while (g.sim.count[CalRec::kCmdCommit] < 1) { g.bus.Tick(); ++g.clk.t; }
  g.sim.nak = 2;   // the COMMIT was ACKed; every later send fails
  CHECK(g.Run() == E_COMMIT_UNKNOWN);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  // ... and if the bus recovers inside the window, the verdict still arrives
  Rig h;
  h.bus.PutRecord(rec); h.Run();
  h.bus.Commit(RecCrc(rec), 0);
  while (h.sim.count[CalRec::kCmdCommit] < 1) { h.bus.Tick(); ++h.clk.t; }
  h.sim.nak = 2;
  for (int i = 0; i < 2500; ++i) { h.bus.Tick(); ++h.clk.t; }
  h.sim.nak = 0;
  CHECK(h.Run() == 1);
  CHECK(h.sim.count[CalRec::kCmdCommit] == 1);
}

static void post_commit_malformed_status_is_unknown() {
  Rig g;
  uint8_t rec[30];
  MakeRecord(rec, 1);
  g.bus.PutRecord(rec); g.Run();
  g.bus.Commit(RecCrc(rec), 0);
  while (g.sim.count[CalRec::kCmdCommit] < 1) { g.bus.Tick(); ++g.clk.t; }
  g.sim.bad_reply_len = 3;
  CHECK(g.Run() == E_COMMIT_UNKNOWN);
  CHECK(g.sim.count[CalRec::kCmdCommit] == 1);
  Rig h;
  h.bus.PutRecord(rec); h.Run();
  h.bus.Commit(RecCrc(rec), 0);
  while (h.sim.count[CalRec::kCmdCommit] < 1) { h.bus.Tick(); ++h.clk.t; }
  h.sim.bad_reply_len = 3;
  for (int i = 0; i < 2500; ++i) { h.bus.Tick(); ++h.clk.t; }
  h.sim.bad_reply_len = -1;
  CHECK(h.Run() == 1);
  CHECK(h.sim.count[CalRec::kCmdCommit] == 1);
}

int main() {
  refused_commit_during_put_sends_nothing();
  refused_commit_during_read_sends_nothing();
  refused_put_during_read_sends_nothing();
  post_commit_send_error_is_unknown();
  post_commit_malformed_status_is_unknown();
  put_commit_roundtrip();
  no_record_is_distinct_outcome();
  commit_never_resent();
  commit_waits_for_the_flash();
  commit_refused();
  retried_commit_is_deduplicated();
  slots_full_reports_no_slot();
  reply_from_other_address_ignored();
  stale_adc_counter_rejected();
  busy_bus_retries_then_fails();
  send_error_is_bus_error();
  stale_ok_bit_is_not_a_verdict();
  ok_bit_without_a_valid_record_is_not_a_verdict();
  second_commit_ok_needs_the_new_seq();
  commit_wire_error_latched_is_polled();
  commit_wire_error_not_latched_is_unknown();
  commit_gate_closed_still_retries();
  read_timeout_retries_then_fails();
  bad_reply_length();
  record_read_uses_args_the_module_answers();
  record_read_back_detects_a_wrong_stored_byte();
  every_sent_frame_allowed();
  stale_replies_are_dropped();
  printf("%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
