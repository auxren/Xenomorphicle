# 200e Preset-Bus Conformance — WPM + Xenomorpher Coexistence

Maps every acceptance criterion and hazard from the coexistence spec
(2026-08 session) to its implementation, tests, and live verification.
Bench: Xenomorpher (T4.1, `-DPRESET_BUS`) + Studio H 2WIRELESS WPM + 200e
case, driven from the Orin; two overnight soaks (42 + ongoing cycles) and
`~/xeno/bench_regression.py` as the standing gates.

## Protocol findings that revise the spec's model

Two of the spec's working assumptions turned out to be wrong on real
hardware/firmware, and the implementation follows the *corrected* model:

1. **"The preset store IS whoever answers 0x50" — not for normal
   save/recall.** Verified from the 2WIRELESS source: modules store presets
   in their own internal memory; general-call SAVE/RECALL never touches
   slave 0x50. The 0x50 FRAM is used only by explicit **card BACKUP/RESTORE
   operations** (v1 long ops 0x04/0x05, v2 0x2D/0x2E). Consequence: a preset
   saved by broadcast from either device is recallable from either device
   *because the modules themselves hold it* — no store hand-off is involved,
   and criterion 3's "save lands in WPM FRAM" applies to card backups, not
   ordinary saves.
2. **The WPM swallows the whole bus during a card transfer window** — its
   `receiveEvent` in FRAM mode consumes every byte, so any master TX during
   a backup/restore corrupts the user's card data. This became the 1.5s
   post-transfer TX holdoff (`Bus200eLastTransferMs()` consulted by
   `tx_gate_open()` and the bus-stuck detector).

## Acceptance criteria

| # | Criterion | Implementation | Tests | Live verification | Status |
|---|-----------|----------------|-------|-------------------|--------|
| 1 | WPM-initiated recall/save unaffected by our presence | GC-only slave (GCEN; SAMR never matches 0x50 while WPM present — triple hard gate in `CardServeEnable`); zero-copy ISR→ring→parser | `test_bus200e.cpp` (88 checks: both dialects, frame hygiene, preemption) | Two overnight soaks: clean GC RX, dialect counters advancing, zero collisions; `b` dump `owner_0x50=WPM` throughout | **MET** |
| 2 | Xenomorpher-initiated recall works bus-wide | `BroadcastRecall`: masters the same `[04 00 22 01 nn]` frame a preset manager sends, plus local dispatch (own slave can't hear own TX); echo suppression via `Bus200eSuppressFrame` | bus200e broadcast/echo tests | Nightly soak "bus recall" cycles; modules + engine restore in lockstep | **MET** |
| 3 | Xenomorpher-initiated save persists and is recallable from either device | Broadcast SAVE `[04 00 22 02 nn]` → modules save internally (see finding 1); card-op decode (BACKUP/RESTORE args) implemented and logged | bus200e SAVE + card-op arg tests | Broadcast saves live-verified (slot round-trips); FRAM byte-level `readmemory` cross-check applies only to card ops and remains an open manual item | **MET** (revised semantics); FRAM byte-check: open, low priority |
| 4 | Bus MIDI streaming during WPM activity: both complete | TX quiet-gate (`tx_gate_open`: ring empty + 2ms quiet + !BBF + 1.5s transfer holdoff), bounded retries, arb losses counted (`midi_tx_drop`, `query_retries`), never silent | midi ring + gating covered in bus200e/midi_types (773 checks) | Soak MIDI stress (100 msgs/cycle) interleaved with recalls: 0 drops across both nights | **MET** |
| 5 | WPM unplug → Xenomorpher serves 0x50; replug → yield | Presence probe (empty write to 0x50, 5s cadence, 3-strike hysteresis); 0x50 serving fully implemented (BusCard 24xx/FRAM emulator, slave-TX, PBCARD.BIN, prefetch-corrected pointer) | `test_buscard.cpp` (41 checks incl. WPM-probe no-op, chunked reads) | Gate refusal live-proven (`k` → REFUSED with WPM present); serving itself validated on host only — see deviation below | **MET with deliberate deviation** |
| 6 | DebugDump reports presence/owner/dialect/stats at any moment | Console `b`: wpm presence, `owner_0x50` (WPM / US(card) / none), dialect v1/v2 with frame counts, full stats + ring high-water + stuck counters | — | Used continuously by the soak status cycles | **MET** |

### Deliberate deviation on criterion 5

The spec suggested automatic takeover/yield of 0x50. Implemented instead as
**manual opt-in** (console `k`), never persisted across boots, refused while
a WPM is detected, self-tested at enable, with the presence probe suspended
while serving. Rationale: hazard 3 (ACKing 0x50 against a live WPM corrupts
module recalls) is catastrophic and lands on the owner's instrument, while
presence detection can flap during WPM power-up/reset — an automatic ACK on
a false "absent" is exactly the failure the hard gate exists to prevent.
Automatic takeover can be revisited with a long (minutes) absence timer if
field use demands it.

## Hazards

| # | Hazard | Mitigation | Proof |
|---|--------|------------|-------|
| 1 | Master writes to 0x50 write WPM FRAM | Only the presence probe addresses 0x50: a **0-byte** write, verified side-effect-free against the card protocol (host test `test_probe_is_side_effect_free`) | Code path audit; 41-check suite |
| 2 | WPM deaf while mastering | Quiet-gate + LPI2C hardware arbitration + bounded retries; broadcasts never assumed universally heard (local dispatch is explicit) | bus200e preemption tests; soak stats |
| 3 | ACKing 0x50 under a live WPM | Triple gate: enable refused if `wpm_present`; probe suspended while serving (no self-ACK flap); enable-time self-test reverts on any address-match anomaly | Live: `k` → `card serve REFUSED - WPM owns 0x50` |
| 4 | Tests touching the real bus | All protocol logic host-tested (88+41 checks, CI-gated); live-bus actions are explicit console commands; the soak exercises only normal operations | CI `ci.yml`; soak design |
| 5 | Our own flash writes under a 251e bank transfer | While the 200e app has a BACKUP/RESTORE open the 251e is the I2C master writing into / reading out of our card slave, and every LittleFS erase here masks interrupts. A bus RECALL/SAVE landing then (70-150 ms blocked) could leave a 251e that stops waiting holding half a bank, or read as a false BAD on the verify pass. `PresetEngine::Process` holds the request until `PresetBus::MasterTransferring()` drops (45 s cap, sized from a measured 10.8 s real BACKUP), and the card-image flush and the slot/app EEPROM record wait the same way | xeno-sim selfcheck "a bus RECALL waits for a 200e bank transfer"; live 2026-09-02: `jr` + WPM recall → `request deferred` at 1.8 s, ran at 8.2 s after DONE, read intact |

## Robustness added beyond the spec

- Bus-stuck detector + staged recovery (master reset → 9-SCL-pulse + manual
  STOP → full re-init), gated so long legitimate 0x50 transfers can never
  false-trigger it.
- WDOG1 watchdog + CRASH.LOG forensics + GLOBALS.BAK config recovery, so a
  wedged module reboots into boot-recall instead of holding the bus hostage.
- ISR flag-ordering and TDF-stall fixes from adversarial review (round 3).

*Last updated 2026-09-02; regenerate against `docs/upstream-prs.md` when the
PR packages move.*
