# Panel binding matrix — every app, every control

Source: QA audit pass, 2026-09-03, branch `preset-bus`. Read against
`software/src/apps/*.h` (`HandleButtonEvent` / `HandleEncoderEvent` in each),
`OC_ui.cpp`, `OC_app_base.cpp`, `PresetBusUI.cpp`, `OC_apps.cpp`.

**Why this file exists.** This is the only consolidated record of what every
control does in every app. The Panel Audit (2026-09-01) could not reach
Quadrants, Hemisphere, Captain MIDI, Calibr8or or Scale Editor because the
host simulator cannot build them, so for those apps this matrix is the *only*
written record. It was nearly lost once already — two UX proposals evaporated
when a session compacted — so it lives on disk rather than in a transcript.

Legend: **p** = short press · **lp** = long press · **dn** = acts on the DOWN
edge · **ld** = acts on long release · `-` = unbound.

Note on naming: `CONTROL_BUTTON_A` is aliased to `CONTROL_BUTTON_UP` and `B` to
`DOWN` (`OC_ui.h:53-57`), inherited from 2-button-shield hardware. There is no
button silkscreened UP or DOWN on this panel. Several apps bind
`CONTROL_BUTTON_A` to a function *named* `OnDownButtonPress()` and vice versa —
see "name/binding inversions" below before copying any of them as a template.

---

## Global — above the app layer

| Gesture | Effect | Where |
|---|---|---|
| Hold **Z**, press **encR** | App switcher (the only route between apps) | `OC_ui.cpp:236-240` |
| Hold **Z**, press **encL** | Per-app I/O settings | `OC_ui.cpp:242-246`, `OC_app_base.cpp:564-575` |
| Hold **Z**, press **A** | Screensaver | `OC_ui.cpp:248-252` |
| **Both encoder pushes** | 200e preset-bus overlay | `OC_ui.cpp:211-221`, `PresetBusUI.cpp:94-124` |
| Hold **Z** alone ≥700 ticks | Chord card overlay | `OC_app_base.cpp:384-455` |
| App switcher: encR turn / p / **lp** | Scroll / switch app / switch **and persist to EEPROM** | `OC_apps.cpp:956-966` |
| App switcher: encL p / **lp** | Cancel / `DebugStats()` service loop (exit only via encR) | `OC_apps.cpp:967-978` |
| App switcher: **B lp** | Toggle global encoder acceleration — unlabeled, ~4px of feedback | `OC_apps.cpp:988-1003` |
| Overlay: encL turn / **lp 500ms** | Move cursor / **STORE**, broadcast bus-wide | `PresetBusUI.cpp:231-234, 270-280` |
| Overlay: encR turn / **hold 250ms** | Change field / **RECALL**, broadcast bus-wide | `PresetBusUI.cpp:235-251, 573-589` |
| Overlay: **A**/**B** p | Exit overlay | `PresetBusUI.cpp:226-229` |
| TR1-4 jacks (assignable) | NEXT/LAST preset cycling, live even with overlay closed | `PresetBusUI.cpp:527-556` |

`A` also still works as an alias for `Z` in the two encoder chords. As of
2026-09-03 `A` no longer raises the chord card — that is Z-only, and `A` is a
free ordinary app button.

---

## Quantizer / generator family

Shared shape: A/B = coarse adjust, encL = channel select / commit, encR = edit
toggle. **The shape is not actually uniform — see the collisions below.**

| App | A | B | Z | encL p / lp / turn | encR p / turn |
|---|---|---|---|---|---|
| **ASR** `ASR.h:872-989` | p: octave (toggle-based ±1) | p: **freeze S&H** (not octave) | - | commit scale / commit+open editor / pick scale | edit toggle, or open scale editor on mask row / scroll |
| **QQ (Quantermain)** `QQ.h:1359-1500` | p: octave −1 | p: octave +1; **lp: clear channel scale mask** | - | next channel / **copy scale+root to other 3** / select channel | edit toggle, open scale editor / scroll |
| **DQ (Meta-Q)** `DQ.h:1246-1417` | p: octave −1 | p: octave +1 | - | next channel / **copy scale+root to other** / select channel | edit toggle / scroll |
| **Chords** `Chords.h:1162-1341` | p: octave ±1 *only on MENU_PARAMETERS*, else jumps there; lp: stub | p: **toggles MENU_PARAMETERS↔MENU_CV_MAPPING**; lp: clear CV mapping | - | commit scale / stub / pick scale | edit toggle, scale editor, chord editor / scroll |
| **H1200** `H1200.h:1020-1031` | p: octave +1 | p: octave −1 | - | toggle note-name display / **reset to defaults** / inversion | edit toggle / scroll |
| **Passencore** `Passencore.h:1060-1231` | stub | stub | - | commit scale / — / pick scale | scale editor on mask row, edit toggle / scroll |
| **Lorenz** `Lorenz.h:338-357` | p: freq ±32 | p: freq ∓32 | - | switch generator 1/2 | edit toggle / scroll |
| **Piqued** `Piqued.h:1156-1320` | p: segment value +32 | p: segment value −32 | - | toggle Segments↔Settings / — / select channel-segment | toggle segment/setting edit / scroll |
| **Viznutcracker** `Viznutcracker.h:528-601` | p: equation +1 | p: equation −1 | - | - | edit toggle / select channel |
| **BBGEN** `BBGEN.h:350-382` | p: gravity −32 | p: gravity +32 | - | - | edit toggle / select channel |
| **References** `References.h:825-851` | p: octave +1 | p: octave −1 | - | — / — / select DAC channel | edit toggle, open Autotuner on that row / scroll |

---

## "OnXxxButton" family — name/binding inversions

These apps bind `CONTROL_BUTTON_A` to a function *named* `OnDownButtonPress()`
and `B` to `OnUpButtonPress()`. Behaviour is internally consistent; the
**names are swapped relative to the binding**. Anyone standardising gestures by
reading these files risks copying the label rather than the binding and
inverting the actual behaviour.

| App | Confirmed inversion | Notes |
|---|---|---|
| **NeuralNetwork** `NeuralNetwork.h:657-678` | yes | tip: `Up/Dn: Setup 1-4` |
| **WaveformEditor** `WaveformEditor.h:414-436` | yes | tip: `Up/Dn: Switch Wave` |
| **ScaleEditor** `ScaleEditor.h:380-402` | yes | popup, used inside ASR/QQ/DQ/Chords/Passencore/Sequins |
| **Enigma** `Enigma.h:1244-1265` | no | encL cycles LIBRARY→ASSIGN→SONG→PLAY |
| **TheDarkestTimeline** `TheDarkestTimeline.h:585-606` | no | tip: `Up/Dn: Arm Rec Trk` |

---

## Distinct-shape apps

| App | A | B | X | Y | Z | encL | encR | Chords |
|---|---|---|---|---|---|---|---|---|
| **Automatonnetz** `:728-754` | p: Reset | p: Clock | - | - | - | toggle edit cell/grid; **lp: ClearGrid+Reset**; turn: select cell | edit toggle; turn: edit/scroll | - |
| **Calibr8or** `:927-1010` | p: SwitchChannel(up); dn in q_edit: octave −1 | p: SwitchChannel(down); lp: preset screen | **p: toggle incoming/outgoing CV display** | - | p: ToggleClockRun | press / long / turn handlers | press / turn handlers | `A+B: Clock Setup`, `Z: Clock Run` |
| **CaptainMIDI** `:2366-2724` | p: SwitchSetup(+1) | p: SwitchSetup(−1); lp: ToggleCopyMode | - | - | - | ToggleDisplay; **lp: Panic() (all-notes-off)**; turn | ToggleCursor; turn: port scroll / param edit | **A+B: Clock Router overlay** |
| **Hemisphere** `:1711-1774` | p/dn: applet select (double-click → full-screen help) | same, right side; **lp: ToggleConfigMenu** | - | - | dn: ToggleClockRun | push; **ld: ToggleClockRun**; turn | push; **ld: JumpToMenu()**; turn | **A+B: Clock Setup** |
| **Quadrants** `:1727-1899` | p/dn: applet select NW | NE | SW | SE | p or ld: ToggleClockRun; lp: reserved | push (or ClockSetup); turn | same | **A+B** Clock Setup · **X+Y** Audio Setup · **A+X** Preset Selector · **B+Y** Input Mapping · **A+Y / X+B** Overview · **encL+A / encL+B** swap view slot |
| **Scenery** `:710-756` | p: SwitchEditChannel(down) | p: SwitchEditChannel(up); lp: handler | dn: PreviousScene | dn: NextScene | dn: ZapButton (random scene) | press / long / turn | press / turn | `X/Y: Change Scene`, `Z: Random Scene` |
| **SETTINGS** `:508-…,1073-1078` | dn solo: toggle pixel-invert | - | edit 200e bus addr (held while turning encR) | - | - | p: StartCalibration; **lp: arm bootloader** | **p: arm factory-reset prompt** | **A+B (dn): flip screen 180°** |
| **Bus200eApp** `:2589-2860` | p: **arm whole-bank Write** (module home) / toggle loop point / octave nudge (edit) | p: cycle sequence A-D | p: octave down (edit) | p: octave up (edit) | - | back / cancel (context-dependent) | **confirm/commit after dead-window**; turn: scroll | arm(A) and confirm(encR) are deliberately different buttons |
| **TweightyApp** `:473-524` | p: transport toggle | p: envelope out on/off | **held**: redirects encR to FEEDBACK instead of MIX | - | - | back home; turn: cursor / select tap | cycle HOME→EDIT→MIXER; turn: adjust | - |
| **ScopeApp** `:334-357` | p: **freeze/pause** | p: reset gain to 1.0× | unbound | unbound | - | toggle live-trace screensaver; turn: select channel | turn: gain | - |
| **SamplerApp** `:384-402` | p: preview/trigger slot | p: cycle focused field | unbound | unbound | - | turn: select slot | turn: adjust field | - |
| **UsbDriveApp** `:235-259` | - | p (RECOVER item): run recovery; **lp (USBDRIVE item): arm USB-drive mode** | - | - | - | turn: flip cursor | - | - |
| **Backup** `:321-329` | context | context | - | - | - | arms Restore | sends backup | `L: Restore  R: Send` |
| **TunerApp** `:313-326` | - | - | - | - | - | toggle MIDI-out passthrough | lock/unlock strobe; turn: A4 Hz | - |
| **PongGame** `:464-497` | p: toggle P1, reset scores | p: toggle P2, reset scores | - | - | - | toggle P1 analog/digital; turn: paddle 1 | toggle P2 analog/digital; turn: paddle 2 | - |

---

## Inconsistencies — the standardisation case

**Same control, different meanings.**
`A`/`B` as "coarse adjust ±" is octave in QQ/DQ/H1200/References, but ±32 raw
in Piqued/Lorenz/Viznutcracker/BBGEN, scene/channel select in Scenery, MIDI
setup ± in CaptainMIDI, applet select in Hemisphere/Quadrants (meaning varies
*again* per applet). **ASR is asymmetric** — B is freeze-S&H, not octave.
**Chords overloads A/B with menu-page navigation** on top of octave.
A naive "A/B always means octave" pass would silently delete both.

`Z` is clock run/stop in Hemisphere/Quadrants/Calibr8or, random-scene in
Scenery, and unbound in most other apps.

**`encL` long-press means six different things**, all confirmed:
copy-scale-to-channels (`QQ.h:1484-1493`, `DQ.h:1393`), bootloader arm
(`SETTINGS.h:516-532`), reset-to-defaults (`H1200.h:1037-1039`), clear-grid
(`Automatonnetz.h:747-749`), DebugStats (`OC_apps.cpp:967-977`), and
bus-wide STORE (`PresetBusUI.cpp:270-280`).

**Same action, different controls.** "Freeze the live signal" is **B** in ASR
but **A** in ScopeApp. "Next screen/page" is B in Bus200eApp, encR in
Tweighty, A/B in CaptainMIDI, B in Chords.

**Free real estate.** `X` and `Y` are unbound in most apps outside
Quadrants/Hemisphere/Scenery/Bus200e/Tweighty — the largest reservoir for new
gestures. They do not exist on non-T4.1 hardware, which is acceptable for a
Xenomorpher-only fork.

**Must not be re-bound.** The 200e app's arm(A)/confirm(encR) split is a
hardened carve-out: a fumbled `A+encR` is also the global app-switcher chord,
and a mis-commit writes 63KB to the wrong module. `A+B` on its confirm screens
is deliberately inert (`Bus200eApp.h:242-246, 421`).

---

## Test surface

`tools/xeno-sim` runs the real firmware against shimmed hardware, driven by
`--keys` scripts, with framebuffer capture (`--dump-fb`), exact-glyph text
decode (`fbtext.py`) and right-edge clip detection (`edgecheck.py`). It already
regression-tests the chord-guard property directly.

It builds **six apps**: Setup/About, 200e Modules, Scenery, Pong, Tweighty,
Back It Up!. It **cannot build Hemisphere or Quadrants at all**. Captain MIDI
(the default boot app), Calibr8or and Scale Editor are blocked by a
const-correctness bug — a non-const member called from a const draw path, which
`arm-none-eabi-g++` accepts and Apple clang rejects
(`CaptainMIDI.h:393-394`, `Calibr8or.h:755,757`, `ScaleEditor.h:192-213`).
Fixing that is small, named, and is the cheapest way to widen automated
coverage before any module-wide gesture change.

`edgecheck.py` can false-positive on Setup/About: the decorative icon at x=120
is chosen at random from eight, and some have pixels in the last two columns.
An 8px icon at x=120 fits exactly.
