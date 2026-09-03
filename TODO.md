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
* **Resolved.** The two items below, both flagged the same morning this
  section was written, were fixed that same day by `af116a93` (which remounted
  `PhzConfig::myfs` in 4 KB LittleFS blocks) and are recorded here so they are
  not re-opened:
  - The 64-block budget was asserted in prose, not in code. It now is:
    `SaveSlot()` (`PresetEngine.cpp`) reads the mounted block size at runtime
    (`PhzConfig::myfs.blockBytes()`, 4 KB on T4.1 -- 1024 blocks in the 4 MB
    partition, not the old 64 KB/64-block geometry) and refuses a save with
    `HS::PokePopup(HS::MESSAGE_POPUP, "Disk full !!")` -- the same popup
    pattern used elsewhere in this file -- unless `kSaveBytesNeeded` worth of
    blocks (container + rename-through scratch file) is actually free. There is
    no `kSaveBlocksNeeded` any more; that fixed reservation was replaced by
    this dynamic per-save check.
  - `PresetEngine.h`'s header comment documented the retired multi-file slot
    layout and said slots live on SD when present. It now describes the
    current one-file-per-slot container format and states slots live on
    internal flash unconditionally, card or no card.

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

* **No audio output on real hardware, first bench test (2026-09-03) --
  root-caused, fix implemented, NOT yet re-verified on hardware.**
  Signal patched into IN L, monitored on OUT L, transport in WRITE, engine
  confirmed live (audio f32 pool grew from the idle baseline, CPU usage
  rose to ~21%/38.6% max, screens render and respond to panel input) --
  but total silence, not just missing wet/delayed repeats. Root cause:
  `OC::AudioIO::OutputStream()` (`AudioIO.cpp`) returned a reference to
  `output_route`, an `AudioPassthrough<2>` -- a pure per-channel relay, not a
  summing mixer. Quadrants' own audio-applet chain-tail
  (`AudioAppletSubapp.h`'s `ConnectMonoToNext()`/`ConnectStereoToNext()`) is
  unconditionally wired to it at boot for every app in `app_container`
  regardless of which is current, and Tweighty's `out_adapter_` is wired to
  the same destination the first time Tweighty is ever opened -- both by
  design, both meant to stay live. With a plain relay, whichever source's
  `update()` the audio ISR happened to run last for a given block silently
  won that channel; the other source's audio never reached the codec. Fixed
  by replacing `output_route`'s type with a new `AudioSummingRoute<NumChannels,
  NumSources>` (`Audio/AudioMixer.h`, alongside the existing `AudioMixer<N>`
  this reuses the q15<->float scale/add approach from) that actually sums
  per-channel, source-major-indexed inputs; Tweighty's connections
  (`TweightyApp.h::WireAudio()`) now claim a distinct reserved source slot
  (`OC::AudioIO::kOutputRouteTweightySlot`) instead of colliding with
  Quadrants' slot 0. Gain staging is NOT solved: both sources sum at unity,
  matching the existing `usbmix` precedent in the same file, so two
  simultaneously full-scale sources can clip (cleanly, via
  `arm_float_to_q15`'s saturation, not wraparound) -- revisit if that proves
  audible. No hardware access to confirm sound now actually reaches the
  jacks; needs a real bench retest.
* **Fixed.** `AudioConnection_F32::disconnect()` never reset the stream's
  `active` flag, unlike the stock int16 `AudioConnection::disconnect()` this
  codebase deliberately widened for the same case. `extern/f32/AudioStream_F32.cpp`
  now mirrors that widening exactly: `AudioConnection_F32::connect()`/
  `disconnect()` track a `numConnections` count per endpoint (inherited,
  protected, already used by the stock int16 side) and only clear `active`
  once an endpoint's last connection is actually gone. `AppTweighty::SetActive()`'s
  own workaround (the `AudioNoInterrupts()` bracket plus the engine's `ready_`
  gate) is left in place -- still needed to close the narrower window where
  `update()` is already mid-flight against a stream mid-`connect()`/`disconnect()`.
* **Fixed, for `AudioTweightyF32` only.** The recirculating buffer had no
  denormal guard: in RECIRC with `0 < feedback < 1` and a quiet captured
  window, the buffer's content decays geometrically toward (not through)
  zero every pass and crosses into float32 denormal range before reaching it
  exactly -- a CPU-time/glitch risk during quiet decaying tails.
  `AudioTweightyF32::update()` now brackets its per-block DSP in FPU
  flush-to-zero (FPSCR FZ+DN), same pattern and bit values as
  `AudioEffectModalResonator::update()`. `AudioDelayExtF32`, which this
  engine was modeled on, has the identical gap and was deliberately left
  alone here -- it backs the separate, already-shipped Delay audio applet,
  and fixing it wasn't exercised by this bug or this bench test.

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
