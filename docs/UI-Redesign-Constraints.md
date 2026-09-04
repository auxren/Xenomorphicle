# UI redesign: the constraints a proposal has to answer to

Captured from the FW/SW/QA feasibility audits run against this tree for the
module-wide UI/UX redesign. Written down because the audits themselves were
lost: the subagent transcripts holding two of the three design proposals became
unreachable ("No transcript found for agent ID") once the parent session
compacted and its job directory moved. These are the load-bearing findings that
survived. Treat this file as the durable copy.

Verified against the tree, not inferred. Where a claim is unverified it says so.

## 1. Concurrency splits in two, and the halves are nothing alike

**Audio concurrency already works, and it is free.** The audio graph is
DMA-driven on ~2.9 ms / 128-sample blocks and is fully decoupled from app
switching. Quadrants, Tweighty and Sampler are permanently wired into an
`AudioSummingRoute<2,3>` and keep sounding while another app is on screen.
That template *is* the "mix several sources onto one output" primitive the
redesign asks for — already in production. Live audio-graph rewiring is proven
safe when rebuilt inside `AudioNoInterrupts()` brackets.

**CV-rate concurrency is very hard.** All app logic runs in a 16.666 kHz core
ISR: a **60 microsecond** budget shared with display flush, DAC write, ADC scan
and digital-input scan. Exactly one app gets it — `AppSwitcher::current_app_`
is a single `RuntimeSlot`. There is no scheduler, no preemption and no per-app
budget. Quadrants is the only proof concurrency works at all there, and only
because Hemisphere applets are deliberately tiny. Full apps own exclusive
resources (SD, USB MIDI, audio engine) with no arbitration layer, and there is
a documented real bug from two unsynchronized writers racing app state (the
Tweighty background pump).

**Both the FW and SW audits independently recommended** generalizing the
existing lightweight applet system rather than building a concurrent-app
runtime. The codebase's own precedent supports it: the most sophisticated
runtime-configurable signal system here — the audio applet graph — was built as
a screen inside an applet host, not as a new runtime.

## 2. Routing is roughly half built

This is the biggest opportunity, and the reason the redesign is cheaper than it
looks.

- `CVInputMap` / `DigitalInputMap` (`software/src/CVInputMap.h`): global arrays,
  one per input channel, each holding a 3-bit typed source
  (`TYPE_NONE`/`ADC`/`DAC`/`MIDI`/`INTERNAL`) plus an index and an attenuverter
  (+/-448%). `TYPE_DAC` already patches an input to **another slot's output**,
  i.e. self-patching works today. `ChangeSourceType`/`ChangeSource` step only
  through valid `(type, index)` pairs, so the map is **type-safe by
  construction** — the redesign's type-safety requirement is already met.
  Scoped today to CV/trigger *inputs* in Hemisphere-family apps.
- **Outputs are not routable.** An applet's output is fixed by `io_offset`, a
  macro `(hemisphere * 2)`, so slot N owns physical channels 2N/2N+1 purely by
  position. This is the single biggest genuinely missing piece.
- `MIDIMapSettings` (`HSIOFrame.h`): 32 typed slots — PITCH (mono/poly/min/max/
  pedal/invert), GATE, TRIGGER (including clock divisions), MODULATOR
  (velocity/aftertouch/bend), CCONTROL (any of 128 CCs, auto-learn) — plus
  `MIDIFrame::outports[]` for the CV/trigger -> MIDI-out direction. Bidirectional.
  The MIDI half of the redesign is largely built.
- **Persistence is solved.** Routing config rides existing PhzConfig bank-global
  keys already swept into all 30 preset-bus slots: ~250 bytes, no new file
  format, no container-section change.

### Two routing traps

- **Quadrants already has a routing-page prototype**: B+Y opens an input-mapping
  page over trigmap/cvmap (`Quadrants.h:637-642`). A global routing UI should
  promote or extract that renderer. Inventing a parallel screen ships the
  instrument with two different routing screens.
- **Persistence is split, and it is user-visible.** IOSettings (gain, filter,
  scaling) is *per-app*, serialized inside each app's own EEPROM chunk;
  cvmap/trigmap are *HS globals*. So half the rows of any combined routing
  screen would follow you between apps and half would not. That is a real
  mental-model decision and it is Oren's to make, not an implementation detail.

## 3. Menu curation is already shipped, for applets

`hidden_applets[2]` is a 128-bit bitmask with a toggle screen (encR toggles one,
encL inverts all), persisted in bank globals. Generalizing it to *apps* is one
`uint32_t` plus a skip-check in the app switcher's cursor loop. Note the
T41_audio build actually compiles **15** apps, not the ~31 declared, so one
`uint32_t` covers the full roster with room to spare.

## 4. Gesture real estate — the hard constraint

Claimed globally: `Z+encR` (app switcher), `Z+encL` (I/O settings), `Z+A`
(screensaver), both-encoder-push (200e preset overlay), hold-Z-700ms (chord card).

**Shipped since the audits and verified on hardware:** the chord card is
**Z-only**. Holding A draws nothing; A is a free ordinary app button. Card rows
are allocated adaptively (Z's own action, then A+B, then the screensaver hint
only if the app left room) because killing A's card orphaned A+B's only
documentation. 148/148 sim checks pass, including a new assertion that A raises
no card.

Quadrants alone claims A+B, X+Y, A+X, B+Y, A+Y, X+B, encL+A, encL+B. **X and Y
are the largest free reservoir** outside Quadrants/Hemisphere/Scenery — with the
caveat that they do not exist on non-T4.1 hardware, which is acceptable for a
Xenomorpher-only fork.

Quadrants' `CheckButtonCombo()` is an **exact mask match**
(`mask == combo && mask != last_mask`), not a subset test, so `Z+X` / `Z+Y`
cannot collide with its eight bindings. That removes the main risk from any
new-gesture proposal.

**Do not invent a new ignore mechanism for a new chord.** Extend
`IgnoreUntilRelease()`'s call sites instead. That machinery exists because a
fumbled chord once fired a **bus-wide preset recall across every 200e module in
the case**, and two simpler fixes were each proven wrong by a live incident.

## 5. Testing reality — this shapes what is safe to ship

`xeno-sim` runs the *real* firmware (`OC_ui.cpp`, `OC_app_base.cpp`,
`PresetBusUI.cpp`, the display pipeline) against shimmed hardware, driven by
`--keys` scripts, with framebuffer capture (`--dump-fb`), exact-glyph text
decode (`fbtext.py`) and right-edge clip detection (`edgecheck.py`). It already
regression-tests the chord-guard property directly.

**But it builds only 6 apps:** Setup/About, 200e Modules, Scenery, Pong,
Tweighty, Back It Up!. It **cannot build Hemisphere or Quadrants at all** — the
largest part of the UI. Captain MIDI (the default boot app), Calibr8or and Scale
Editor are blocked from simulation by a small const-correctness bug (a const
draw path calling a non-const member; `arm-none-eabi-g++` accepts it, Apple
clang rejects it). Fixing that is cheap and is arguably a **prerequisite to any
large gesture refactor**, since otherwise the refactor cannot be regression
tested across most of the UI.

`edgecheck.py` can false-positive on Setup/About: the decorative icon at x=120
is picked at random from eight, some have pixels in the last two columns, and an
8px icon at x=120 fits exactly. Not a real clip.

## 6. weegfx / panel grammar

128x64, 1-bit. ~21 characters per row at 6px/char, 9px row pitch, so ~6 usable
text rows. The chord card's `rows[5]` is a **hard cap** — a 6th row pushes card
height past 64px.

Grammar already established in the tree, and a proposal should not contradict it:

- Inversion means "the right encoder changes this". Never decoration.
- CAPS = active, lowercase = inactive.
- Presence dots per the existing system idiom.
- Errors outrank informational rows.

## 7. Unverified, and load-bearing

- **Z's reachability on the real panel is unconfirmed.** The firmware maps it,
  but two attempts to observe the grey clock button firing it on hardware were
  inconclusive. The chord card is now Z-only and the screensaver chord is
  Z+A — so if Z is not reachable on this panel, both are unreachable. Cheap to
  settle on the bench; two shipped changes depend on it.
- **Calibr8or's CV voltmeter** (`HS::frame.In` -> `OC::IO::pitch_to_millivolts`,
  committed in 34c29316) has never been checked against a real signal. Sign and
  scale unverified. Test is a jumper from an output to its own input.
