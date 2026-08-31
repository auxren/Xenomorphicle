# Project review team

Six specialist subagents, formalizing the ad hoc expert-review pattern that
proved out on the preset-bus epic (a UXR pass, a firmware-correctness pass,
and a protocol-vs-source pass each caught real bugs, including one critical
data-loss bug). Invoke any of them via the `Agent` tool with
`subagent_type: "<name>"` — each starts fresh with no memory of prior runs,
so brief it with the specific files/change in question.

| Agent | Domain | Reach for it when… |
|---|---|---|
| `ux-reviewer` | Front-panel UI (128x64 OLED, encoders, buttons) vs the Buchla/Fairlight design-system grammar | adding/changing a screen, menu, or overlay |
| `fw-correctness-reviewer` | ISR safety, ring-buffer races, ITCM/FLASHMEM budget, PhzConfig map-residency, boot ordering | touching Main.cpp, PresetBus*.cpp, PresetEngine.cpp, an ISR, or Suspend/Resume paths |
| `protocol-reviewer` | Buchla 200e bus / WPM wire-protocol conformance, byte-level, against the reference firmware source | changing frame formats, timing/arbitration, or bus-MIDI/WPM-coexistence logic |
| `audio-qa-reviewer` | F32 audio engine, I2S/USB audio I/O, DSP graph correctness, bench-measured signal QA | touching AudioIO.cpp / audio_applets / extern/f32, or when a hardware audio QA pass is needed |
| `hw-bringup-reviewer` | Electrical bring-up, flashing/recovery procedure, live-hardware verification method | before/after any bench session on the physical module |
| `release-manager` | Stacked-PR packaging for upstream, build/test matrix, PR description accuracy | prepping PRs, or before declaring a batch of work "done" |

## Composition notes

Capped at 6 of a possible 8: each role maps to a real, previously-proven need
rather than being invented to round out a number, and the boundaries are
chosen to minimize overlap (e.g. `fw-correctness-reviewer` owns runtime
correctness, `release-manager` owns whether the *matrix* stays green —
distinct concerns even though both touch builds). Add a 7th/8th only when a
genuinely new recurring need shows up (candidates that came close: a
dedicated test-suite/coverage role, folded into `fw-correctness-reviewer`
and `release-manager` for now; a docs/protocol-spec writer, folded into
`protocol-reviewer`).

All six are read/review-oriented by default (they have `Bash` for
verification — builds, greps, bench commands — but their prompts frame them
as reviewers who hand back findings, not silent code-changers). Ask
explicitly if you want one to patch code directly.

## 251e-sequencer build team

A second, build-oriented team for a separate initiative, originally scoped
as a web applet that programs all 4 sequences of a Buchla 251e over the 200e
bus, from a Xenomorpher plugged into a Mac via USB. Modeled on a flatter "VP
+ solo desks" structure (one lead, four independent specialists, no further
delegation) rather than the review team's flat roster.

| Agent | Domain |
|---|---|
| `sequencer-lead` | Sequences the initiative, reconciles the four specialists' work, escalates real forks to Oren |
| `avr32-reverse-engineer` | Decodes the 251e's sequence-data format from `~/Documents/GitHub/Buchla_FW`'s `251Ev307.hex` (AVR32, not the 8051 tooling already in that repo) |
| `bus200e-master-engineer` | Makes the Xenomorpher transiently master the 200e bus to BACKUP/RESTORE a 251e's dump, and bridges it to USB |
| `sequencer-webapp-engineer` | Builds the actual browser UI (WebMIDI SysEx transport, sequence editor) |
| `hw-bringup-gatekeeper` | Standing safety gate — blocks live-bus work until the Phase 0 electrical check (3.3V vs 5V) is confirmed; also owns local flashing |

**Status as of 2026-08-31: the original charter is DONE, not blocked.** The
Phase 0 electrical check passed and the live bus has been in active use for
an extended real-hardware session — reading and writing a real 251e (bus
address 0x5C) repeatedly with no electrical issues (a separate, unrelated
USB-audio driver bug caused reboots during that session and has since been
root-caused and fixed; it was never an electrical/bring-up problem). The
251e's full preset-bank byte format is fully decoded and verified
(`tools/251e-sequencer/sequence-codec.js`; written up at
`~/Documents/GitHub/claude_trix/tricks/reverse-engineering/re-buchla-251e-sequence-format.md`).
Bus-mastering BACKUP/RESTORE of another module's card is working and
committed (`software/src/Main.cpp` console commands `m`/`c`/`w`/`x`/`S`/`R`,
built on `OC::PresetBus`). The web applet (`tools/251e-sequencer/`) programs
a full bank over WebMIDI SysEx and works end-to-end. A full 4-sequence
musical composition was written to a real 251e and independently read back
byte-exact.

The team's next phase is a **different, on-device app** — generating and
pushing 251e sequences from the Xenomorpher's own OLED/encoder UI, no
laptop required — see `sequencer-lead.md` for the updated brief.
