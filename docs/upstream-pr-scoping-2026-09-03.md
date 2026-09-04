# Upstream PR scoping — 200e preset bus → djphazer/O_C-Phazerville

Scoping pass run 2026-09-03. Supersedes the stale strategy in
`docs/upstream-prs.md` (last touched 2026-08-19) where the two disagree.

**Ownership:** reassigned by Oren on 2026-09-03 to the session working in
`~/Documents/GitHub/O_C-Phazerville`. This file exists so that decision costs
nothing — everything below was derived once and should not be re-derived.

**STATUS: both PRs are now OPEN** (2026-09-03), superseding this doc's
"nothing pushed" starting point:

- **djphazer/O_C-Phazerville#260** — T4 infrastructure (DMAMEM zeroing,
  WDOG1 + CRASH.LOG, GLOBALS.BAK, console lock).
  `auxren:pr1/t4-infra-hardening`, +423/−130 across 9 files.
- **djphazer/O_C-Phazerville#261** — Buchla 200e preset bus + the 225e-style
  L+R overlay. `auxren:pr2/buchla-200e-preset-bus`, +4448/−542 across 29
  files. It carries #260's commits because GitHub will not let a cross-fork PR
  base on a branch in the head fork; the body says so.

Verification that landed: `test_bus200e` 88 checks / 0 failures,
`test_buscard` 41 / 0. `T41`, `T41_audio`, `T40`, `nlm_T40`, `nlm_hoc_T40` all
build on the pinned toolchain. `T40` (no `PRESET_BUS`) comes out **+32 bytes
better than main** — the zero-cost evidence for users without the hardware.

One qualification added during PR authoring that is worth keeping: the bench
session that verified Layer A predates the toolchain pin, so it exercised an
unpinned build.

---

## Upstream state

- Latest release **PSv2.0.1**, commit `9a5b15e6`, 2026-07-11 — the exact commit
  the prepped PR branches are based on.
- `main` is **one trivial commit** past it: `195932ab`, a `platformio.ini`
  platform-teensy version pin, which PR2 does not touch.
- A `v1.14 (T32 only)` release exists (2026-08-30) — a legacy side release,
  irrelevant to `main`.

So this is a **refresh-and-verify pass, not a port**. Estimated at hours, not
days.

## The prepped branch stack

On `auxren/Xenomorphicle`, built to merge cleanly against upstream `main`:

```
pr/teensy4-itcm-console  →  pr/buchla-200e-preset-bus  →  pr/captain-midi-sysex
```

- **PR1 is a hard prerequisite — but NOT for the reason originally recorded
  here.** The earlier claim ("`T41_audio` link-fails on upstream main without
  it, RAM1 −13.7 KB") is **WRONG and has been retracted.** It was measured
  against `platform = https://github.com/platformio/platform-teensy.git`, an
  unpinned git HEAD. Upstream's `195932ab` pinned `teensy @ 5.2.0`, and on that
  toolchain `T41_audio` links fine on bare `main` with 19,040 bytes RAM1 free;
  PR1 moves that to 19,072 — a **+32 byte no-op**. There is no overflow to fix.
  Do not go looking for one.

  The real dependency is a **compile-time entanglement** found only by trying
  the "drop PR1, ship PR2 alone" split: PR2's `Main.cpp:464` calls
  `PhzConfig::load_config(PhzConfig::BACKUP_FILENAME)`, and `BACKUP_FILENAME`
  is introduced by `3e048a16`, one of PR1's fold commits. Without PR1 the build
  fails with `'BACKUP_FILENAME' is not a member of 'PhzConfig'`, plus a
  redefinition of `AppQuadrants::View()` because PR2's fold commits carry
  FLASHMEM out-of-class moves that assume PR1's refactor. The two are
  entangled through the fold commits, not merely adjacent — so the stack
  stands, with PR1 justified on its own merits instead.
- PR3 is best offered as draft/discussion — it is a large rewrite.
- These branches are **siblings** of `preset-bus`, not ancestors: both diverged
  at `9a5b15e6`. `pr/buchla-200e-preset-bus` carries 7 commits that exist
  nowhere else (stray `src/Scenery.h` removal, hardening round 2, the
  ITCM-console merge, the T41_audio RAM1 overflow fix). **Those 7 are wanted.**

### Known gap in the prepped branch

`pr/buchla-200e-preset-bus` is **missing the MIDI-over-bus work** — `mMaskBus`
/ `HSIOFrame.h` including `PresetBus.h`, channels A–D, commit `c64d2336`
(2026-08-18) — even though PR2's description in `docs/upstream-prs.md`
promises it. The branch was rebuilt 2026-08-19, one day later, and never
picked it up. Either fold it in or strike it from the description; today the
doc and the branch disagree.

## Snapshot from the PR branches, not from `preset-bus` HEAD

`preset-bus` is ~254 commits ahead and **actively moving** (three sessions were
committing to it on 2026-09-03). Of those 254, roughly **71 touch
`software/src/Preset*`, and ~30 of those touch Layer B files** — so it is not
true that the gap is unrelated to the preset bus; it is ~28% related, and much
of that is Layer B, which should not ship. Same conclusion, defensible if a
maintainer asks.

---

## Layer A vs Layer B — the boundary that matters

The obvious file list conflates two features of very different maturity.

**Layer A — mature, bench-verified, this is what goes upstream.**
The module as a 200e bus **slave**, saving/recalling its own whole state to 30
slots over a from-scratch LPI2C1 slave transport, opened by the both-encoders
overlay. `PresetEngine`, `PresetBus`, `PresetBus200e`, `PresetBusUI`.
Verified against a live 200e system; panel SAVE/RECALL decoded with 0 drops.

**Layer B — new, larger, hardware-UNVERIFIED. Keep fork-side.**
`Buchla200eModuleTable`, `Buchla200eUiGate`, `Buchla200eWriteGuard`,
`Buchla251eGenerator`, `Buchla251eRecorder`, `Buchla251eSlotCodec`,
`Buchla259eSlotCodec`, `Bus200eMaster`, `Bus200eBridge*`, `Bus200eSysEx`,
`apps/Bus200eApp.h`.

This makes the module a bus **master that BACKUP/RESTOREs other
manufacturers' modules**. A 251e RESTORE rewrites that module's entire
**63,120-byte bank in one shot** — there is no partial-write command on the
bus. The fork's own `docs/bench-restore-checklist.md` states this has never
gone out over a real bus. Shipping it to strangers' Buchla systems is a
different risk class from shipping a preset slave.

Layer B is not in the prepped branch today, so this is "don't add it," plus one
forward-reference line in the PR description.

---

## Blocking fix before upstreaming: the VOR chord collision

`OC_app_base.cpp` binds `(CONTROL_BUTTON_L | CONTROL_BUTTON_R)` under
`#ifdef VOR` on `EVENT_BUTTON_DOWN` to `VBiasManager::AdvanceBias()`. That file
is shared across every hardware target.

The fork's `OC_ui.cpp` adds a chord-claim block for the same `L|R` combo that
calls `PresetBusUI::Enter()` and `continue`s. It is **not** wrapped in
`#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)`, unlike
`PresetBusUI::Enter()`/`Active()` in the header, which *are* stubbed inert
off-target.

**It is worse than "compile-time unguarded":** the block's body runs
`SetButtonIgnoreMask()` and `continue` with **no runtime `PresetBusUI::Active()`
test either**. The inline no-op stubs make `Enter()` harmless off-target, but
the *swallow* happens on every build. `OC_ui.cpp::DispatchEvents` runs before
`app->DispatchEvent()`, so **any VOR build loses VBias cycling regardless of
`PRESET_BUS`**.

It has never fired only because the fork does not build VOR targets at all —
not because VOR never meets T4.1. Upstream, where VOR users are real, it
would silently break their gesture.

**Fix:** wrap the `OC_ui.cpp` block in the same
`ARDUINO_TEENSY41 && PRESET_BUS` guard used in `PresetBusUI.h`.

**The fix cannot currently be build-verified, and the PR body says so.**
Teensy 3.x does not build on upstream `main` at all: `T32`, `T32_vor` and
`stock_vor` all fail on a clean `195932ab` checkout with nothing applied —
`'PhzConfig' has not been declared` in `HemisphereApplet.h` and `Main.cpp`,
plus `'USBHost' does not name a type` in `OC_debug.cpp`. Since no VOR env
compiles anywhere, the guard rests on inspection rather than a green build.
That pre-existing T3 breakage was reported upstream as an unrelated FYI.

---

## Dependencies — already upstream, contrary to assumption

Verified against upstream `main`, not the fork:

- **`PhzConfig` / LittleFS** — present and used upstream. No gap.
- **`I2C_Expansion`** — already exists upstream as a runtime hardware-detect
  flag (`OC_gpio.cpp:185`). `PresetBus.cpp` already gates on it
  (`if (!I2C_Expansion) return;`). No new detection mechanism needed.
- **T4.1 8-channel I/O** — stock upstream, not a fork addition.

**Gating recommendation:** keep exactly what the fork already does —
compile-time `-DPRESET_BUS` on T41 envs **plus** the runtime `I2C_Expansion`
check. Costs nothing at compile or runtime for users without the hardware.
T3.2 was never a plausible target; this is T4.1-only.

---

## PR strategy

**Recommended: keep the existing three-branch stack as-is.** At ~4,400 changed
lines PR2 is large for one review, but it is already hardware-verified end to
end, and none of its pieces is independently useful to a reviewer — splitting
adds coordination cost without reducing risk.

**Fallback if the maintainer asks for smaller**, in this order:
1. `OC_apps` factoring + `APP_EVENT_FLUSH` + `AppSwitcher::Init` return value
   + the T3 EEPROM-ordering / `ConfirmReset` fix
2. `PresetEngine` + `PresetBus` + `PresetBus200e` + host tests
3. `PresetBusUI` overlay + the L+R chord

### Refresh checklist before opening PR2

1. Fold in `c64d2336` (MIDI-over-bus) or strike it from the description.
2. Fix the VOR chord-gating bug above.
3. Re-verify clean builds of `T41_audio` and `T41` against current `main` in a
   scratch clone — **not** in the shared `Xenomorphicle/software` checkout;
   concurrent `pio run` there has corrupted build dirs before.
4. Confirm host tests still build/pass standalone. The test Makefile compiles
   the whole directory including Layer B test files not meant for this PR, so
   this needs a **filtered** run, not a blind `make`.
5. Explicitly exclude Layer B, plus a one-line note that a module-programming
   follow-up exists but is not bench-verified.

## Attribution

The fork's work is Oren Levy's (oren@auxren.com). Preserve existing copyright
headers on ported files — the per-file credits to djphazer, Patrick Dowling,
Max Stadler, Tim Churches and the other o_C authors stay as they are.
