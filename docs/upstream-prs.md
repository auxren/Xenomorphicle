# Upstream PR packages for djphazer/O_C-Phazerville

Three stacked branches on `auxren/Xenomorphicle`, each building green against
upstream `main` (9a5b15e6, v2.0.1). Open them in order — each depends on the
previous. Draft PR text below; edit freely.

Stack: `main` → `pr/teensy4-itcm-console` → `pr/buchla-200e-preset-bus` →
`pr/captain-midi-sysex`

Branch heads (rebuilt 2026-08-19): PR1 `3e048a16`, PR2 `22d3c614`,
PR3 `23a84002`.

> **Folded in (2026-08-19):** the hardening round-2 commits — `ad78499b`
> (0x50 card serving), `568ec7ab` (watchdog / crash log / config backup /
> selftest / bus-stuck recovery), `d0150f94` (DMAMEM zeroing hook),
> `08123210` (console `a`, selftest printf fixes), `404f6746` (-Werror
> header fix), `f71bffa3` (console `z` help, bench cheat sheet),
> `6f2d5522` (C/F double-press confirm, hoc_sysex echo), `9fc2322e`
> (FW-review round 3: ISR ordering, TDF 0xFF, TxRewind, boot SRSR,
> CRASH.OLD, echo 7-byte cap), `b5202a44` (HemisphereApplet draw-helper
> FLASHMEM) — are now in the PR branches; each PR section below lists its
> pieces as "Included". Stack mechanics: PR1 was extended in place
> (fast-forward), PR2 merged PR1 then added its fold commit
> (fast-forward), PR3 was restacked onto PR2 (force-pushed).
>
> **Deliberately kept fork-side, not in any PR:** the F32 audio engine and
> the three F32 applet ports (`dfb7fcf9`/`dfd8de71`/`985382ba`), the
> `.github/workflows/ci.yml` (references fork-local builds), the
> `_config.h` DMAMEM `app_container` move (RAM2 stack-headroom trade that
> upstream doesn't need), and the selftest's F32-pool line.

---

## PR 1 — `pr/teensy4-itcm-console`
**Title:** Fix T41_audio RAM1 overflow (LTO-proof FLASHMEM placement) + serial console hardening

T41_audio on current main fails to link — RAM1 comes up 13.7 KB short. Two
root causes, both verified by inspecting the linked ELF with `nm`:

1. `loop()` was `FASTRUN`; under LTO it gets inlined into `main()` wholesale,
   dragging several KB of cold menu/console code into ITCM. Now
   `FLASHMEM __attribute__((noinline))` — noinline is required so an
   out-of-line body exists for the section attribute to act on.
2. LTO silently **drops** `FLASHMEM` on in-class (implicitly inline) method
   definitions but honors it on out-of-class definitions — including virtual
   methods. `AppQuadrants::View` and `ClockSetup::View` (T4) moved
   out-of-class; their single-call-site inlined draw trees (~6 KB) follow
   them into flash. `AppSettings`' out-of-class definitions annotated too.

Also hardens the debug console: all input ignored until the literal sequence
`pew!` arrives once per boot. Linux ModemManager AT/MBIM-probes every new CDC
port, and on the bench that byte soup hit real commands (`D` = display off,
`C`/`F` = config reset / filesystem erase).

Results (`teensy_size`, RAM1 free for locals):
| env | before | after |
|---|---|---|
| T41_audio | **−13664 (link failure)** | +20000 |
| T41 | — | +24896 |
| T40 | — | +100960 |

> **Included (folded 2026-08-19, `3e048a16`)** — fork-general T4
> infrastructure:
> - `startup_middle_hook()` zeroing `.bss.dma`: the core never zeroes
>   DMAMEM, so C++ objects there boot with garbage (USBHost_t36 state
>   machines wedge). Root-cause fix, benefits every T4 user.
> - WDOG1 hardware watchdog (128 s, fed from `loop()`, armed post-setup,
>   CCM clock-gate + WMCR PDE traps handled) + CrashReport persisted to
>   `CRASH.LOG` (8 KB rotation, one generation kept as `CRASH.OLD`);
>   `SRC_SRSR` captured+cleared at boot.
> - GLOBALS.CFG → GLOBALS.BAK auto-backup; corrupt primary restores from
>   backup instead of prompting a factory wipe.
> - Console `t` selftest (health report) and `a` (app switch via the
>   proper suspend/resume path); C/F destructive keys need a double press
>   within 3 s ('C' fallthrough fixed); `F` format runs under a temporary
>   watchdog feeder; rewritten `z` help.
> - `usbHostMIDI`: comment documenting why the objects MUST stay in DTCM
>   (EHCI DMA buffers, no cache maintenance in USBHost_t36).
> - ITCM: BootMenu FLASHMEM+noinline; HemisphereApplet draw helpers
>   moved to FLASHMEM via out-of-class definitions (`b5202a44`).
>
> The `.github/workflows/ci.yml` from `d0150f94` references fork-local
> host tests — kept fork-side, not in the PR.

---

## PR 2 — `pr/buchla-200e-preset-bus`
**Title:** Buchla 200e preset bus: whole-state save/recall as a native 200e module
**Depends on PR 1** (needs the ITCM headroom).

Makes a T4.1 o_C (e.g. NLM Xenomorpher in a Buchla 4U system) a slave on the
200e preset bus: broadcast SAVE stores complete module state (active app, all
app state, globals — not calibration) into one of 30 slots; RECALL applies it
live, including app switching and audio-graph rebuild.

- **Protocol** recovered from the 2WIRELESS source + a prior implementation:
  multi-master I2C @100 kHz, everything broadcast on general call 0x00; both
  frame formats (long/PRIMO, short/V2); RECALL/SAVE/remote-enable/QUERY
  (identity reply as temporary master). `src/PresetBus200e.{h,cpp}` is a
  BSP-free parser with 88 host-side checks (`test/test_bus200e.cpp`).
- **Transport**: neither stock Wire nor teensy4_i2c can ACK the general call
  (`SCFGR1[GCEN]` unset in both), so a ~120-line direct-register LPI2C1
  slave coexists with the polled stock Wire master. ISR → SPSC ring →
  loop-context parse/dispatch. QUERY replies quiet-gated. 5 V bus ⇒ level
  shifter required (documented).
- **Engine**: per-slot file sets on LittleFS/SD; save is ISR-bracketed +
  `APP_EVENT_FLUSH`; recall validates first, restores through the same code
  paths boot uses, hot-switches apps. Failure paths can't leave the shared
  PhzConfig map owned by slot content; saves verify files landed on disk.
- **MIDI over the bus**: the bus doubles as a MIDI interface (the WPM
  dialect) — a new `mMaskBus` interface bit makes it selectable as MIDI
  source/destination everywhere the other interfaces are (thru, clock
  RX/TX, msg RX/TX); 200e bus lines A–D map to MIDI channels 1–4. Echo
  suppression and transfer holdoff keep preset traffic and MIDI from
  stepping on each other; WPM coexistence (presence detection, commander
  mode, dialect reporting).
- **Front-panel preset manager**: 225e-style overlay (`PresetBusUI`),
  opened by holding both encoder buttons — save/recall any slot locally
  or bus-wide, 16-char slot rename, hold-progress store feedback,
  last/next trigger inputs, boot recall of the last bus preset.
- Captain MIDI (upstream version): MIDI polling moved from the 16.67 kHz
  app ISR to `Loop()` — USBHost_t36 is not interrupt-safe, and a keyboard
  on the T4.1 host jack locked the module.
- Everything behind `-DPRESET_BUS` (T41 env) + runtime I2C-header check;
  other targets compile inert stubs. Includes a T3 boot-order fix
  (settings loaded before the validity check — stops ConfirmReset showing
  every boot) and LittleFS 512 KB → 4 MB on T4.1.

Verified against a live 200e system: panel SAVE/RECALL decoded through a
level shifter (0 drops), save <1 s, recall <1 s including audio rebuild.

> **Included (folded 2026-08-19, `22d3c614`)** — bus robustness:
> - Bus-stuck watchdog: BBF held ≥3 s with no slave ISR activity, no ring
>   traffic, and no recent card-transfer window triggers staged recovery
>   (master engine reset → 9 SCL pulses + manual STOP → full pad/slave
>   re-init); recovered/detected counters surface in selftest.
> - Ring/MIDI-queue high-water tracking (`bus rings hw` selftest line).
> - 0x50 card serving (`ad78499b`): slave-TX preset-card emulator for
>   WPM-less buses, hard-gated off when a live WPM is detected; console
>   `k` toggle. Plus its host-test suite (buscard, 41 checks).
> - ISR flag-ordering fixes (`9fc2322e`): SOF with SDF/RSF pending closes
>   the previous transaction first; unroutable TDF fed 0xFF (no TXDSTALL
>   SCL hold); `BusCardTxRewind()` keeps chunked current-address reads
>   24xx-compatible. `test_bus200e.cpp` -Werror=comment fix.

---

## PR 3 — `pr/captain-midi-sysex` (discussion / draft)
**Title:** Captain MIDI rework: full I/O engine, SysEx remote config, host tests
**Depends on PR 2** (uses `APP_EVENT_FLUSH`). Offered as a discussion PR —
it effectively rewrites the app, so direction buy-in comes first.

- `HSMIDITypes.h`: host-testable types; static NoteBuffer (no heap churn in
  the 16.67 kHz ISR); bit-packed port settings = stable EEPROM/SysEx format.
  773 host checks.
- `HSIOFrame::ProcessOutputs()`: 4 CV + 4 trigger ports, VCMC-grade function
  sets, paired pitch+gate, ADC lag, rate-limited continuous sends, deferred
  panic.
- Two-level UI, live activity indicators, 4 setups, auto-save (Suspend
  only touches flash when the live config is dirty — the unconditional
  rewrite made the app-menu gesture take seconds).
- USB-host device profiles: remembered per VID/PID across reboots, humane
  binding UI, lean host-port menu; console `u` dumps port identities +
  the profile table. MIDI thru (including the 200e bus) so a host-port
  keyboard reaches the bus without switching apps.
- SysEx protocol v1 (`docs/hoc-midi-sysex.md`) + reference CLI
  (`tools/hoc_sysex.py`): INFO/GET/SET/dump+restore/SAVE/LOAD/FACTORY/
  PANIC/SELECT + Universal Device Inquiry. Parsed from Loop(), not the ISR.
- Bench-validated on NLM hOC (T3.2): 192/192 SysEx conformance checks,
  7-octave CV→MIDI tracking, sequencer round-trips.

> **Included (folded 2026-08-19, `23a84002`)**:
> - `SYX_ECHO` (0x0B) latency probe → `SYX_ECHO_R` (0x4B), answered from
>   the normal loop-context SysEx path, token capped at 7 bytes so a
>   max-size token round-trips whole; bench RTT over USB MIDI: median
>   0.17 ms, p95 0.24 ms, max 0.51 ms (200 probes, 0 drops).
> - Worst-case poll-gap instrumentation (>1 s suspension gaps ignored) +
>   `CaptainMidiHealth()` selftest hook (`captain:` line — poll_gap_max,
>   echo count).
> - `tools/hoc_sysex.py` `echo` subcommand wrapping the probe.
> - `-Werror=comment` fix in `test_midi_types.cpp` (Linux gcc).
> - Doc updates: `docs/hoc-midi-sysex.md` (echo command) and the operator
>   cheat sheet `docs/bench-console.md` (adapted: no F32 pool line,
>   double-press confirm documented).

Note: T3 targets don't build on upstream main today (pre-existing
PhzConfig/USBHost rot). Our fork has the fixes — available as a separate PR
if wanted.

---

### Opening the PRs

```sh
gh pr create --repo djphazer/O_C-Phazerville \
  --head auxren:pr/teensy4-itcm-console \
  --title "Fix T41_audio RAM1 overflow (LTO-proof FLASHMEM placement) + console hardening"
# then pr/buchla-200e-preset-bus (base: main, mention stacking on #1)
# then pr/captain-midi-sysex as --draft
```
