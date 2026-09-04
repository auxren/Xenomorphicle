# Hosting one audio applet as a standalone full-screen app

The contract, verified against the tree with citations, and the decisions taken
on top of it. Written down because this was derived once already tonight and
would otherwise live only in an agent transcript.

## Decisions

**Ship three, not four: Freeverb, Samverb, Delay.**

`AbyssApplet` stays out. It is commented out of both registries
(`audio_applets/_config.h:79,108`) and `git log -L` on those lines gives one
commit, upstream's `871180d7` (2026-07-10), "disable new applets - if ya want
'em, you gotta work for it", which also disabled Animorf. The four days of
commits before it are feature work, not bug-fixing, so **it was disabled for
build budget, not because it is broken** -- `AudioEffectAbyssReverb::begin()`
takes ~150 KB from the RAM2 heap with a PSRAM fallback
(`Audio/effect_abyss.h:5-6,25-39`). It is also the only one of the four
deriving from `HemisphereAudioApplet` rather than `HemisphereAudioAppletF32`,
so it takes an int16 round-trip at its boundaries. Turning it on is a
deliberate memory-budget decision that is Oren's, not a side effect of a UI
change. **Flagged for Oren.**

**One shared output-route slot, claimed rather than released.**

`kOutputRouteSources` is 3 and all three are taken: 0 Quadrants' chain tail, 1
Tweighty, 2 Sampler (`AudioIO.h:26-29`). Bump to 4 and add one
`kOutputRouteEffectSlot = 3` shared by all standalone effect apps. Entering an
effect app claims the slot from whichever effect app held it last.

- Not one slot each: three reverbs permanently wired to the same input sum into
  each other and hold F32 blocks and CPU forever. The F32 pool is 80 blocks and
  Delay alone can hold ~10 per channel (`AudioIO.h:11-13`).
- Not disconnect-on-suspend: the instrument's idiom is that audio keeps
  sounding while another app is on screen. Claiming rather than releasing gives
  exactly one live effect *and* keeps it running after you leave.
- Sharing slot 0 with Quadrants is the one thing that must not happen: whichever
  `update()` the ISR runs last per block silently wins, which is the bug
  documented at `AudioIO.cpp:41-48`.

## The contract

### Lifecycle

Derive the app from `HSApplication`. That is not optional:
`HSApplication::BaseController()` is the **only** place `HS::frame` is loaded
(`HSApplication.h:73-85`), and every one of these applets reads its CV through
`CVInputMap`/`DigitalInputMap`, which read `HS::frame` internally
(`CVInputMap.h:71-90, 344-364`). An app that does not pump it gets modulation
frozen at whatever the last `HSApplication` left behind.

Per tick, from `Process(ioframe)`:

    BaseController(ioframe);      // -> HS::frame.Load, Controller(), frame.Send
      applet.Controller();
      HemisphereApplet::ProcessCursors();   // or cursors never blink (Quadrants.h:437)
      ClockSetup_instance.Controller();     // ONLY if Delay's CLOCK mode is wanted

Precedent for all of this is `apps/TunerApp.h:41,168` and
`apps/TweightyApp.h:92,437`.

**`applet_started` is uninitialized.** `HemisphereApplet.h:462-463` declares it
with no initializer and the class has no constructor; the registry gets away
with it only because its factory uses `calloc` (`AppletRegistry.h:47-50`). A
standalone host must hold its applet in zero-initialized storage (file-static
or `DMAMEM` global), or `Start()` may never run.

### Audio wiring

Lazily on first RESUME, bracketed in `AudioNoInterrupts()`/`AudioInterrupts()`
-- Tweighty's reasoning at `TweightyApp.h:394-402` applies, the graph is live.
`HemisphereAudioAppletF32` already presents int16 edge adapters as
`InputStream()`/`OutputStream()` (`HemisphereAudioAppletF32.h:66-71`), so the
host wires int16 both sides and never sees F32.

### Draw

`applet.SetDisplaySide(AUDIO_SLOT_L)` then `applet.BaseView(full, parked)`
(`AudioAppletSubapp.h:139-142`). `AUDIO_SLOT_L` is 6, and `gfx_offset` is
`(hemisphere & 1) * 64` (`HSUtils.h:34`), so slot L draws at x 0..63.

**`BaseView(true, true)` is NOT full screen.** It calls `DrawFullScreen()`,
whose default is `View()` plus one icon at x=96 (`HemisphereApplet.h:114-117`).
`View()` is still clipped to 64px, and the 64px assumption is baked past
`gfx_offset` into the gfx layer itself: `gfxHeader` hard-codes `62 -
strlen*6` and `gfxDottedLine(0, y+8, 62, y+8)` (`HemisphereApplet.cpp:363-379`),
`gfxCursor`/`gfxSpicyCursor` clamp with `min(x, 63 - box_w)`
(`HemisphereApplet.cpp:162,186`), `gfxEndCursor` clamps to `0, 63 - w`
(`:288`), and `gfxDisplayInputMapEditor` hard-codes `gfxClear(0,0,63,11)`
(`HemisphereApplet.h:347,368`).

So a real 128px layout is new draw code, not a flag. `DynamicsApplet.h:189-199`
shows the existing idiom for painting the far half: drop to raw `graphics::`
and hand-compensate by `-gfx_offset`.

`DrawMenu()` is `const` per `OC_APP_INTERFACE_DECLARE` but `View()` is not, so
the applet member must be `mutable` -- `ScopeApp.h:93-99` hit exactly this.

### Input

Three virtuals, all on `HemisphereApplet`: `OnEncoderMove(int)` (pure, :112),
`OnButtonPress()` (defaults to `CursorToggle()`, :111), `AuxButton()` (defaults
to `CancelEdit()`, :118). Delay overrides `AuxButton` (`DelayApplet.h:249`);
Freeverb and Samverb do not.

The host does **not** call `SetDisplaySide()` before input, only before draw
(`AudioAppletSubapp.h:139`), and everything the input handlers touch is keyed on
`hemisphere` (`enc_edit[hemisphere]`, `cursor_countdown[hemisphere]`). A
standalone host must call `SetDisplaySide()` once and never change it.

### Registration

Five places, worked from Scope:

1. `apps/ReverbApp.h`: `OC_APP_CLASS(AppReverb, TWOCCS("RV"), "Reverb", "Reverb")`
   + `OC_APP_INTERFACE_DECLARE(AppReverb, N)`, and definitions for all 13
   members the macro declares (`OC_apps.h:191-206`).
2. `apps/_config.h` (~:54): guarded `#include`.
3. `apps/_config.h` (~:164): `, AppReverb` inside `AppContainer<...>`.
4. `OC_app_folders.h` (~:100): `{ TWOCCS("RV"), FOLDER_AUDIO },`.
5. `platformio.ini`: `-DENABLE_APP_REVERB` in the audio envs.

**Insert after container index 4.** `apps/_config.h:186-196` hard-codes
`DEFAULT_APP_INDEX = 4` with a `static_assert(DEFAULT_APP_ID ==
AppCaptainMIDI::kAppId)` behind it.

**Budgets.** 15 apps on `T41_audio`, `kMaxApps` is 32, so 17 free
(`OC_app_folders.h:121-122`). The tighter constraint is EEPROM: the
`static_assert` at `apps/_config.h:180-181`. Scope uses 3 bytes, Tuner 5,
Tweighty 23 -- keep `kAppDataStorageSize` small, or persist applet params via
PhzConfig the way `AudioAppletSubapp::SavePreset` does (`:459-497`) at zero
EEPROM cost.

## Known hazard, do not copy

`AudioAppletSubapp::Init()` passes bare `LEFT_HEMISPHERE`/`side` to
`BaseStart()` (`:57,61`) while every other call site passes
`HEM_SIDE(side + AUDIO_SLOT_L)` (`:207,218,275,289`). It only escapes
consequence because `View()` re-asserts the right side every frame.

---

# Revision, after the architecture blueprint: SHIP DELAY

`Audio-Apps-Architecture.md` lands a hard stop that overrides the "ship three"
decision above.

**All four do not fit.** Computed from source and checked against tonight's
real link: Reverb 50,348 + Samverb 77,792 + Abyss stereo 230,028 + Delay
crossfade LUTs 32,768 = **390,936 B**, against `RAM2: free for malloc/new:
388992` measured on the T41_console build. That is 1,944 bytes short before any
headroom, and worse than it looks: `Factory::get()` and the applet Registry
silently fall back to `extmem_calloc` once `FreeRam() <= 10240`
(`OC_core.h:24,64-65`; `AppletRegistry.h:49-51`).

**The real obstacle is not summing-route arity.** That is one integer at
`AudioIO.h:27`. It is RAM2 contention with a silent, order-dependent PSRAM
fallback: whichever app you open third or fourth can be the one that lands in
PSRAM, and Abyss's own header records that fallback costing >50% CPU
(`effect_abyss.h:21-25`). A performance cliff that depends on visit order is a
horrible thing to ship and a worse thing to debug.

**Order, endorsed:** (0) AudioIO infrastructure; (1) the wrapper; (2) **Delay
first** -- most used, already proven in Quadrants, and PSRAM-backed so it does
not compete for RAM2 at all; (3) Reverb; (4) Samverb; (5) Abyss last or not at
all. If exactly one thing ships tonight, it is Delay.

## Verified bugs, each re-checked against source before being scheduled

| # | Bug | Verified | Scheduled |
| --- | --- | --- | --- |
| 1 | `AbyssApplet.h:31-59` runs the whole `Controller()` with no arena, writing faded dry gain -> **silence**, where Freeverb (`:38-41`) and Samverb (`:39-42`) both force dry to unity and early-return | yes -- Abyss has no `else { gain(1,1.0f); return; }` | deferred, Abyss is dead code |
| 2 | `SamverbApplet.h:38` passes `1.0f - damp*0.01f`, `FreeverbApplet.h:37` passes `damp*0.01f`. Same label, opposite meaning | yes, literally inverted | with Samverb |
| 3 | `DelayApplet.h:94-96`: `float t = d * (taps-tap)/taps;` is computed **before** `CONSTRAIN(d, ...)`, and the clamp lands on `d` while `t` is what reaches `cf_delay()`/`delay()` | yes | **now** |
| 4 | `AuxButton()` a silent no-op unless the cursor sits on MIX/WET (`AbyssApplet.h:64-67`, `DelayApplet.h:249-252`) | yes | with each app |
| 5 | `DelayApplet.h:399-401` documents a RAM2 fallback that does not exist -- `ExtAudioBuffer::Acquire()` only ever calls `extmem_calloc` (`AudioBuffer.h:146-150`), so a board with no PSRAM is **silent**, not shorter | yes | **now**, fix the comment |
| 6 | `effect_reverb_schroeder_F32.h:127` hard-codes `sr = 44100`; the Teensy 4 core defaults `AUDIO_SAMPLE_RATE_EXACT` to **48000.0f** and nothing in this project overrides it, so every comb/allpass line is 8.9% short | yes | **now**, standalone |

## A hazard nobody flagged: renaming Bungverb breaks saved presets

`SamverbApplet.h` declares `class BungverbApplet` (:13) whose `applet_name()`
returns `"Bungverb"` (:16). Three names for one effect, and it does need fixing
before anything prints a header -- but **`applet_id()` defaults to
`strhash(applet_name())`** (`HemisphereAudioApplet.h:45-47`), and that id is
what save/load matches on.

Changing the displayed string therefore silently invalidates every stored
preset that references this applet. If the name changes, `applet_id()` must be
overridden to return `strhash("Bungverb")` explicitly, and the commit must say
so.

## Scope split

The three shared-code inversion findings -- `gfxPrint(CVInputMap&)`'s 24px
spike (`HemisphereApplet.cpp:239-246`), `gfxPrint(DigitalInputMap&)` using
`gfxInvert` to mean "gate high" (`:235-238`), and
`gfxDisplayInputMapEditor()`'s inverted banner (`HemisphereApplet.h:344-370`)
-- are **not** part of this work. They are shared code touching every
Hemisphere and Quadrants applet, and the second and third are step one of the
L-06 migration. They go to the correctness reviewer first.

## Verification reality

`tools/xeno-sim/Makefile:41-43` builds with `-DNO_HEMISPHERE` and does not
compile `HemisphereApplet.cpp` at all. **The simulator cannot build any of
this.** Every claim about these apps is a hardware claim made through
`tools/hwqa/hwctl.py`, or it is not a claim. `make check` still has to stay at
158 as a no-regression gate, but it will not cover one line of the new work.
