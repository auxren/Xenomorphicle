TODO (Roadmap)
===

This file has two halves. The **Xenomorpher** section is this fork's own open
work — verified against the tree, with the file that carries the evidence named
so a claim here can be checked rather than believed. Everything from `v2.0`
down is the inherited upstream Phazerville roadmap.

Xenomorpher — known open
===

## Storage and presets

* **`ExportSlot` / `ImportSlot` have no caller.** Preset containers now live on
  internal flash unconditionally (`PresetEngine.cpp`, `preset_fs()`), and
  SD is meant to become an explicit Export/Import. The engine functions and
  their `ExportResult` codes exist; nothing in `PresetBusUI`, the apps or the
  serial console calls them. **Until a UI lands, there is no way for a user to
  move a preset to or from a card at all.** Do not describe Export/Import as a
  feature until it has an entry point.
* **Presets written to SD by an older build are orphaned.** `preset_fs()` is
  now always `myfs`, which fixes the disappearing-slots bug (a preset saved
  with no card became invisible the moment a card was seated, and vice versa) —
  but nothing reads the old SD copies. Container-format ones could be recovered
  through `ImportSlot`; legacy multi-file ones (`PB_NN_G.CFG` + friends) on SD
  are unreachable by any code path. Needs a decision: recover, or document as
  lost.
* **The 64-block budget is asserted in prose, not in code.**
  `LittleFS_Program` uses 64 KB erase sectors on T4.1, so the 4 MB partition is
  **64 blocks total** and every file costs one whole block whatever it holds.
  30 containers + `PBNAMES.BIN` + `PBSNAP.BIN` + `GLOBALS.CFG` + `SCENERY.DAT`
  + `CAPTAIN.DAT` (+ any `BANK_NN.DAT` that lands internally) is claimed to
  settle "near 43". Nothing enumerates or checks that, and `kSaveBlocksNeeded`
  reserves only 3.
* `PresetEngine.h`'s header comment still documents the retired multi-file slot
  layout and says slots live on SD when present. Both are false; the header is
  the first thing a new contributor reads.

## Boot and storage budgets

* **App-data EEPROM budget: measured 2026-09-02, 608 of 3900 bytes (~16%).**
  A compile-time `static_assert` in `apps/_config.h` sums every app's
  declared chunk against `EEPROM_APPDATA_BINARY_SIZE` (3900 on T4.1,
  `OC_config.h`), so for the *declared* budget the runtime overflow ought to
  be unreachable -- but that only bounds the ceiling, not the actual
  `out.used`. Measured on the bench module (T41_console, which shares
  T41_audio's exact `ENABLE_APP_*` set) via a local preset save: `App data
  restored: 608, expected 608` -- comfortable headroom today (~3.3 KB), and
  not a live risk at this app count. If it ever IS reached, `BuildAppData`
  (`OC_apps.cpp`) `continue`s past the app that did not fit, and the loop
  starts at `random(num_apps)`, so **the app that gets silently dropped is
  different on every save** -- and so is the byte order of the whole
  stream, including the copy captured into every preset. The only report is
  an `APPS_SERIAL_PRINTLN`, which is compiled out of `T41` and `T41_audio`;
  nothing is drawn on the OLED and no flag is stored. The margin makes this
  low-urgency now, but the failure mode (silent, different app every time,
  invisible on a beta tester's non-debug build) is still worth a visible
  guard before the app count grows.
* **The boot factory-erase gesture differs between builds, and one of the two
  collides with the app-switcher chord.** `Ui::Splashscreen` selects on
  `#if defined(NORTHERNLIGHT) && !defined(IO_10V)`: A + encR on the `nlm*`
  environments, A + B everywhere else — including `T41_audio`, the image
  `flash.sh` builds by default. A + encR is the same pair, in the same order,
  as hold-A-press-encR for the app switcher, and the `ConfirmReset()` prompt it
  leads to answers OK on encR as well. Two builds of one firmware with different
  destructive gestures is a hazard on its own. (Setup/About's own factory reset
  is a separate, safer path: encR arms, B erases, no second prompt.)
* `AppSwitcher::Init` no longer wipes live state before asking — it decides
  (reset vs. restore) and only then acts. Recorded here so it is not re-opened;
  the residual is the shared-control note above.

## 200e app

* **Arm and confirm are the same button on one of the two routes.** From the
  module home, A arms and encR commits. Reached through the action row's *Save*
  entry, encR arms *and* encR commits, with only the 350 ms dead window between
  them.
* `apps/Bus200eApp.h`'s encL handler says "see the note in the module-select
  handler"; there is no such note. A comment survived a code move.

## Tweighty (the 288r-derived delay/looper app)

* **`AudioConnection_F32::disconnect()` never resets the stream's `active`
  flag**, unlike the stock int16 `AudioConnection::disconnect()` this codebase
  deliberately widened for the same case. Any F32 `AudioStream_F32` that does
  an `Acquire()`/`Release()`-style resource cycle around its active lifetime
  (Tweighty's `AudioTweightyF32` is the first) keeps getting `update()` called
  by the audio ISR after a logical "stop," `active` having latched `true`
  permanently on first connect. `AppTweighty::SetActive()` works around this
  for itself (an `AudioNoInterrupts()` bracket plus the engine's own `ready_`
  gate, checked before touching anything `Release()` frees), but the root
  cause is in `extern/f32/AudioStream_F32.cpp` and would affect the next F32
  stream built the same way. Worth fixing at the source once a second
  consumer needs the same pattern.
* **Recirculating buffer content has no denormal guard.** In RECIRC with
  `0 < feedback < 1` and a quiet captured window, the buffer's content decays
  geometrically toward (not through) zero every pass and will cross into
  float32 denormal range before reaching it exactly — a CPU-time/glitch risk
  during quiet decaying tails, not a correctness bug. `AudioEffectModalResonator.h`
  sets FPU flush-to-zero (FPSCR FZ+DN) around its own recirculating filter for
  exactly this reason; neither `AudioTweightyF32` nor the `AudioDelayExtF32`
  it's modeled on does. Inherited from `AudioDelayExtF32`, not introduced by
  Tweighty — worth fixing at the shared root if it's ever audible.

## UI

* **`read_deliberate()` has no callers.** The global release-first rule
  (`Ui::IgnoreUntilRelease`) filters the *event* stream, and is armed at all
  four global chord entries. A screen that times a hold by sampling the pin
  bypasses it — and the preset overlay's STORE/RECALL bars still do exactly
  that, through `read_immediate()` and their own `*_needs_release` booleans.
  The accessor added for this is unused; the per-screen guard is what actually
  prevents the phantom recall.
* **The per-app A+B screens arm no guard.** Hemisphere and Quadrants call
  `SetButtonIgnoreMask()` (which now routes into the rule); Captain MIDI,
  Calibr8or and Setup call nothing.
* **The chord card does not reach the two screens that are not apps.** Holding
  A (or Z) alone for 700 ms now draws a card listing what that modifier does —
  but `DrawChordHint` is called from `AppBase::Draw`, and `Main.cpp` skips that
  for `UI_MODE_APP_SETTINGS` (the app switcher) and while `PresetBusUI` is
  active. Both are screens a player can arrive at by accident. The splash
  factory-erase gesture is also listed nowhere; that may be deliberate, but it
  should be a decision rather than an omission.
* **Z itself is unconfirmed on hardware.** The firmware maps it (`but_mid`,
  pin 20, polled as `CONTROL_BUTTON_M`) and the simulator drives it, but two
  bench attempts to observe the grey *clock* button firing were inconclusive.
  If Z is not reachable, the screensaver chord is not either. Z's clock toggle
  also fires on three different edges across Hemisphere, Calibr8or and
  Quadrants.

# v2.0
* Eliminate vtable overhead
* T4.1 - expand to 8 channels: Quantermain, Quadraturia, Sequins
* Auto-tuner with floor/ceiling detection (fail gracefully)
* generalized AppletParams for flexible assignment, extra virtual I/O
* Integrate Calibr8or with DAC for global tracking adjustments

# v2.1
* Migrate to HexeFX/OpenAudio libraries for 32-bit float processing
* USB Gamepad support

# ???
* Re-implement Piqued envelopes in an applet
* Audio Applets for T4.1
  - add VCF+VCA to Osc
  - WAVPlay: rework looping/caching; support more metadata tags (tempo, cue points)
* Update Boilerplates - I just assume this needs attention
* MORE MIDI STUFF:
    - MIDI looper applet!
    - MIDI output for all apps?
    - Implement some MIDI SysEx commands, sheesh
    - WebMIDI interface
* ~~Config option for LFS vs. SD for preset storage~~ — **deliberately
  rejected** on this fork. Preset containers are always on internal flash; a
  preset store that answers differently depending on whether an accessory is
  seated is not a preset store. SD becomes explicit Export/Import instead (see
  the Xenomorpher section — the UI for that does not exist yet). Quadrants
  banks still prefer SD, because that is where Quadrants looks.

# APP IDEAS
* Modul8or
  - 8 independent channels, maybe reusing applets
  - various engines (VectorOsc, tideslite, etc.)
  - freely assignable inputs; static channel outputs
* Two Spheres (two applets in series on each side)
* Snake Game
* Tetris

# [DONE]
* T4.1 - expanded to 8 channels: Piqued, Captain MIDI
* MTP Disk mode for file management over USB
* Pong 2.0 with sound effects
* **Fully merge "abandoned/refactoring" branch from pld**
* Pop-up MIDI Map editor
- 3-band EQ / multi-band dynamics
* MIDI mapping for param modulation sources
- multi-mode (HP, BP, LP) for Filt/Fold
* Quadrants Preset Bank switching
* Config files on LittleFS / SD for T4.x
* Unipolar randomize in SequenceX
* better Polyphonic MIDI input tracking
* Multipliers in DivSeq (maybe a separate applet)
* Runtime filtering/hiding of Applets
* QUADRANTS
* Automatic stop for internal Clock
* global quantizer settings in Hemisphere Config
* Flexible input remapping for Hemisphere
* Move calibration routines to a proper App
* add swing/shuffle to internal clock
* applet with modal interchange - MultiScale or ScaleDuet
* ~~Add auto-tuner to Calibr8or~~ — **regressed, not done in this tree.** The
  `OC::Autotuner` member and every call site in `apps/Calibr8or.h` are commented
  out behind `// TODO: refactor Autotuner, again...`. Only per-preset scale and
  offset survive. Autotune still works in References.
* ProbMeloD - alternate melody on 2nd output
* Fix FLIP_180 calibration
* Add Clock Setup to Calibr8or
* Calibr8or screensaver
* Pull in Automatonnetz
* Sync-Start for internal Clock
* General Config screen (long-press right button)
* better MIDI input message delegation (event listeners?)
* import alternative grids_resources patterns for DrumMap2
* Add Root Note to DualTM
