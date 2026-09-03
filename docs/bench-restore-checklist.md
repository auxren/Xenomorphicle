# Bench checklist — first live RESTORE to a 251e/259e (preset-bus branch)

Branch: `preset-bus`. Build/flash `T41_audio` on the Xenomorpher (see
[Xenomorpher Beta Guide](Xenomorpher-Beta-Guide) for the flashing path; check
the build tag on Setup/About before starting). Target: a spare/test 251e or
259e — **not** a module whose current bank you cannot afford to lose, no
matter how much the safety net below is trusted. This is the first time
`RESTORE` has ever gone out over a real bus from this firmware; treat every
step as if the net might not catch it, because that is exactly what this
session is here to find out.

Read [200e Conformance](200e-conformance) for the protocol-level background
(the WPM-swallows-the-bus hazard, the XFER_DONE frame, the tx-gate holdoffs)
before starting — several steps below ask you to watch for things that doc
explains.

## 0. The one rule that matters more than any checkbox

**Only exercise RESTORE through the panel app (200e Modules App → Read →
Edit/Gen/Rec → Save → confirm).** The serial console's `x` command
(`OC::PresetBus::MasterRestore()`) is a raw bench/dev shortcut that pushes
whatever is in the resident card image straight at the module with **no**
`Buchla200eCheckWrite` guard, no pre-write snapshot, no diff, no read-back
verify, and no UNDO — its only safety net is a same-address double-entry
arm (press `x` + 2 hex digits, then the *same* 2 digits again within 3 s).
Everything this checklist exercises — the snapshot, the verify pass, the
UNDO offer — lives in `AppBus200e`/`Buchla200eWriteGuard`, not in `Main.cpp`'s
console handler. `m` (MasterBackup) and `c` (DumpCard) are fine to use for
diagnostics; `w` (byte-patch) and `x` are not part of this test.

## 1. Pre-flight

- [ ] The target module's current bank is independently known before the
      Xenomorpher touches it — a WPM/2WIRELESS card BACKUP you already trust,
      a prior export, or documentation good enough to rebuild it by hand.
      SD export/import isn't reachable from the panel on this branch yet, so
      "independently known" has to mean something outside this firmware.
- [ ] Console `b` (DebugDump) shows `owner_0x50` and `wpm present/absent`
      cleanly — if a WPM is present, confirm nothing on it is mid-operation
      (no card-transfer LED, no manager-side save/recall in progress) before
      you start. A WPM mid-transfer swallows the whole bus; see conformance
      doc hazard 2.
- [ ] `b` shows `dirty=0` and no stuck/recovery counters already elevated
      from a previous session.
- [ ] The target's model and address are what you expect — cross-check
      against `Buchla200eModuleForAddress` on the module-select screen
      (`251e @5C`, `259e @28`, etc.) before arming anything.
- [ ] Nothing else is queued to recall a Xenomorpher preset during this
      session (no trigger-input recall armed, no WPM broadcast recall
      expected) — see §4 below for why a recall matters here.

## 2. Baseline read

- [ ] Panel: 200e Modules App → select the target address → **Read**.
      Confirm `READ OK` and a sane slot 1 display (sequence stages, peak
      volts) before doing anything else.
- [ ] Console `b`: note the reported CRC/byte count for this capture if
      shown, and `c` to hex-dump it if you want an independent copy on the
      serial log.
- [ ] Note the exact bank size transferred (251e: 63,120 bytes / 30 slots;
      259e: 990 bytes / 30 slots — see `kSlotCount` × the per-module record
      size) and confirm it against what `b` / the read completed with. A
      short read must show as `READ_FAIL`, not a quiet partial success — if
      it doesn't, stop and report that first, before going anywhere near a
      write.

## 3. The actual test sequence

- [ ] **Modify a copy.** Pick ONE slot you can afford to lose entirely (all
      30 slots move together — see §5). Edit → toggle the loop point or
      nudge a stage, or Gen → build a new sequence, or Rec → record a short
      one. Confirm the working slot changed (`EDITED` indicator) and that
      slot 1's original content is otherwise untouched on screen.
- [ ] **Arm.** Press A (or cursor to Save + encR) on the module home screen.
      Confirm the write-confirm screen shows the right target address, "29
      others re-sent", and a plausible changed-byte count. If it refuses
      (`write_block_` text on screen), read the reason — this is
      `Buchla200eCheckWrite` doing its job, not a bug to route around.
- [ ] **Confirm.** There is a deliberate ~350 ms dead window
      (`kConfirmDeadMs`) between arm and the confirm screen accepting encR —
      a press inside that window does nothing, silently, by design (see
      Beta Guide "What's still rough"). Wait a beat, then press encR.
- [ ] **Watch it land.** Screen returns to module home; write is `ACTIVE`.
      Expect the send phase to grab the bus within ~2 s
      (`BUS200E_MASTER_SEND_TIMEOUT_MS`), the module to start responding
      within ~3 s of that (`BUS200E_MASTER_ACTIVITY_TIMEOUT_MS`), and the
      whole transfer to go quiet and reach `DONE` well under the 15 s hard
      cap (`BUS200E_MASTER_HARD_CAP_MS`) / 20 s app-level timeout
      (`BUCHLA200E_JOB_TIMEOUT_MS`). A real 251e BACKUP has measured ~7-11 s
      for the full bank on this bus; expect RESTORE to be in the same
      neighborhood, possibly a little slower — see the XFER_DONE note below.
- [ ] **Read-back verify happens automatically.** As soon as the write's
      `DONE`, the app immediately starts a fresh whole-bank BACKUP from the
      same module and compares it byte-for-byte against what was sent. Do
      not touch the module or the panel while this runs — it is not a second
      thing you need to trigger.
- [ ] **Verdict.** `WRITE OK` (green path: the edit is now proven to be on
      the module, `edited_` clears, the app's working copy re-syncs from the
      verified read-back) or `WRITE BAD` (the recovery row —
      `keep`/`UNDO` — replaces the normal action row). Either is a valid,
      informative outcome for a first hardware test; a `WRITE OK` with the
      right changed-byte count and the other 29 slots visibly unchanged
      (spot-check 2-3 other slots) is the actual pass condition.
- [ ] **Independent check.** Read the module back with an independent means
      if you have one (another controller/manager, or at minimum a second
      Xenomorpher Read + `c` hex-dump compared by eye against what you
      intended) — the point of this whole session is not to trust this
      firmware's own verdict about itself on the very first live write.

## 4. Deliberately exercise UNDO

Do this even if step 3 came back `WRITE OK` — the recovery path needs its own
proof, and provoking a real `WRITE BAD` on demand is hard, so the practical
way to test UNDO for the first time is to use it on a *good* write's
snapshot rather than wait for a real failure.

- [ ] Confirm a snapshot exists: after any completed write (`OK` or `BAD`),
      `PBSNAP.BIN` should hold the pre-write bank for this address. There is
      no direct panel readout of "snapshot present" outside the `WRITE_BAD`
      recovery row — this is a known UI gap (see §6) — so treat the presence
      of `WRITE OK`/`WRITE BAD` moments ago, on this same target, without an
      intervening navigation away, as your only reliable evidence.
      **Do not navigate to a different module or leave this screen between
      the write landing and testing UNDO** — the recovery row (and this
      firmware's fix for the module-switch snapshot hazard, see §6) only
      protects the module currently on screen.
  - If your build predates the module-switch fix below: leaving this
    screen for a different target before resolving a `BAD` verdict destroys
    the only recovery copy with no warning. Don't test that path on hardware
    you care about — verify it on a throwaway module or not at all.
- [ ] To reach the recovery prompt when the last verdict was BAD: encR with
      `recover_cursor_` on `UNDO`, then encR again through the same
      dead-window/confirm pattern as an ordinary write.
- [ ] If the last verdict was OK, there is currently no panel path to
      re-arm UNDO against a snapshot from a *previous* session or a
      resolved write — this is the gap in §6, not a step you can complete
      today. Note it in your bench log rather than trying to force it.
- [ ] Confirm the UNDO write goes through the *exact same* send → verify
      pipeline as an ordinary write (it does, by design — `CommitWrite`
      reuses `PumpWrite`/verify). Watch for the same `DONE` → automatic
      read-back → verdict sequence.
- [ ] After UNDO reports `WRITE OK`, read the module fresh and confirm it
      now matches the ORIGINAL baseline from §2, not the edit from §3 — this
      is the actual proof that UNDO restores correctly, not just that it
      "landed".

## 5. What to watch for as failure signs

- A `WRITE OK` verdict where a spot-check of an untouched slot (not the one
  you edited) shows different content than the baseline — the guard's
  "outside hash" check exists specifically to prevent this; if it happens
  anyway, that's the single most important thing to capture in a bug report
  (screen text + `b` output + serial log).
- `verify_covered_` effectively false — i.e. the write reports `BAD` and the
  reason implies a short read-back rather than a content mismatch. Note the
  byte count from `b`/`Bus200eMasterBytesTransferred()` against the expected
  bank size.
- `write_lost_` (the FSM went idle out from under the app — see
  `Buchla200eUiGate.h`'s "IDLE means lost, not pending") — this points at
  something else on the bus (a console command, the USB bridge) touching the
  same master FSM mid-job. Note what else was active.
- The bus-stuck detector firing (`b` will show it) during or right after the
  write — note whether it was a false trigger against a legitimately long
  transfer or a real recovery.
- Any `WDOG1` reset / non-empty `CRASH.LOG` (console `r`) after a write —
  capture the log before it's overwritten by a later boot.
- A `gap=` value from `b` far outside the previously-measured range (415 µs
  for a 259e, up to ~5.2 ms for a 251e, per the conformance doc) — a much
  larger gap during YOUR write's chunk-to-chunk timing is worth logging even
  if the write itself reports OK, since it's new data about the RESTORE
  direction specifically (only BACKUP gaps have been measured live so far).
- `b`'s `owner_0x50` changing to something other than what you expect mid-test
  — that means a WPM appeared or disappeared during the write, not before it.

## 6. Known gaps this session should keep in mind (not blockers, but don't be surprised)

- **XFER_DONE has only been observed for BACKUP, live.** A module announcing
  the end of a transfer (`BUS200E_OP_XFER_DONE`) shortens the post-write
  quiet wait from 1500 ms to 200 ms once seen. Whether a 251e/259e sends the
  same frame after a RESTORE (module *reading* from us) rather than a BACKUP
  (module *writing* to us) has not been confirmed on hardware. If your write
  takes closer to the full 1500 ms quiet window rather than 200 ms, that is
  expected, not a fault — capture whether XFER_DONE showed up in `b`'s stats
  either way; it's useful data regardless of the outcome.
- **The single-slot snapshot only protects the module currently on screen,
  and only until you leave.** `PBSNAP.BIN` holds ONE bank, not a history.
  Firmware built after the fix in this review refuses to leave the module
  home screen (encL) while a write is `BAD` and its snapshot survives —
  you'll see "resolve write first" instead of navigating away. If you see
  silent navigation away from a `BAD` verdict instead, you're on firmware
  predating that fix; don't switch targets before resolving.
- **A Xenomorpher preset RECALL (this module's own 30-slot save/recall, not
  the 251e/259e bank) is NOT deferred by an unresolved `BAD` verdict.**
  It IS deferred while a transfer is actually on the wire
  (`PresetBus::MasterTransferring()`), but a `BAD` verdict is a terminal,
  no-longer-transferring state — a recall landing after the verdict (a
  trigger-input recall, a WPM broadcast recall) can silently retarget the
  app and orphan the snapshot exactly like the navigation case above, and
  the panel fix above does not close this path. Keep trigger-input recall
  and broadcast recall out of the picture for the duration of §3-4 if you
  can (unplug the recall trigger source, or make sure nothing on the bus is
  set up to broadcast a recall mid-test).
- A 259e write path does not exist yet — this checklist's write/verify/undo
  steps apply to a 251e today. Read-only testing is the only option for a
  259e on this branch.

## Fail-fast notes

- Confirm screen visibly up but encR does nothing → check you're past the
  350 ms dead window; a press inside it is deliberately silent, not a
  freeze.
- Nothing happens at all when arming (no confirm screen, no refusal text) →
  this would be the exact bug class `Buchla200eUiGate.h` was built to kill;
  it should not be reachable, so treat it as a priority report if seen.
- Write reports `WRITE_FAIL` (not `WRITE_BAD`) with no bytes visibly moved →
  the transfer never got past `SENDING`/`WAIT_ACTIVITY` — likely a bus
  arbitration/gate issue, not a content issue; `img` was already patched in
  RAM by this point, so the very next commit attempt should refuse with
  "Image changed-reread" until you re-Read. If it doesn't refuse, that's a
  bug — the guard is supposed to catch exactly this.
- `x` (console) does something you didn't expect → you used the raw
  bypass path; see §0. Nothing in this checklist applies to it.
