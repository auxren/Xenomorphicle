# Upstream PR packages for djphazer/O_C-Phazerville

Three stacked branches on `auxren/Xenomorphicle`, each building green against
upstream `main` (9a5b15e6, v2.0.1). Open them in order — each depends on the
previous. Draft PR text below; edit freely.

Stack: `main` → `pr/teensy4-itcm-console` → `pr/buchla-200e-preset-bus` →
`pr/captain-midi-sysex`

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
  BSP-free parser with 71 host-side checks (`test/test_bus200e.cpp`).
- **Transport**: neither stock Wire nor teensy4_i2c can ACK the general call
  (`SCFGR1[GCEN]` unset in both), so a ~120-line direct-register LPI2C1
  slave coexists with the polled stock Wire master. ISR → SPSC ring →
  loop-context parse/dispatch. QUERY replies quiet-gated. 5 V bus ⇒ level
  shifter required (documented).
- **Engine**: per-slot file sets on LittleFS/SD; save is ISR-bracketed +
  `APP_EVENT_FLUSH`; recall validates first, restores through the same code
  paths boot uses, hot-switches apps. Failure paths can't leave the shared
  PhzConfig map owned by slot content; saves verify files landed on disk.
- Everything behind `-DPRESET_BUS` (T41 env) + runtime I2C-header check;
  other targets compile inert stubs. Includes a T3 boot-order fix
  (settings loaded before the validity check — stops ConfirmReset showing
  every boot) and LittleFS 512 KB → 4 MB on T4.1.

Verified against a live 200e system: panel SAVE/RECALL decoded through a
level shifter (0 drops), save <1 s, recall <1 s including audio rebuild.

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
- Two-level UI, live activity indicators, 4 setups, auto-save.
- SysEx protocol v1 (`docs/hoc-midi-sysex.md`) + reference CLI
  (`tools/hoc_sysex.py`): INFO/GET/SET/dump+restore/SAVE/LOAD/FACTORY/
  PANIC/SELECT + Universal Device Inquiry. Parsed from Loop(), not the ISR.
- Bench-validated on NLM hOC (T3.2): 192/192 SysEx conformance checks,
  7-octave CV→MIDI tracking, sequencer round-trips.

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
