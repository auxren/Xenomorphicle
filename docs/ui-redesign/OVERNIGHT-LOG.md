# Overnight program log, 2026-09-03/04

Durable record of what was decided and what was actually verified. Written
because two design proposals were lost tonight when a session compacted:
anything that exists only in an agent transcript is not a deliverable.

**Rules in force for every agent on this program:**

1. Exactly one agent at a time may build or write to the working tree. Design,
   review and audit agents are read-only and may run in parallel.
2. Exactly one process at a time may open `/dev/cu.usbmodem192573201`. Hardware
   QA is serial, never parallel.
3. Nothing writes to the 200e bus. Not one byte has ever been written into
   Oren's 251e/259e and tonight does not change that.

## Baseline, established before any change

Measured at `c960f649`, not assumed:

| Check | Result |
| --- | --- |
| `pio run -e T41_console` | SUCCESS (flash 561272, RAM1 free 99616) |
| `pio run -e T41` | SUCCESS (flash 539424, RAM1 free 100320) |
| `cd tools/xeno-sim && make check` | **158 checks, 0 failing** |

158 is the floor. It may not go down, and new behaviour owes new checks.

## Finding H-01 -- the bench tool was measuring the wrong screen

**Severity: high. Found on hardware, not in review. Fixed in `418a9e0e`.**

`tools/hwqa/hwctl.py` sent the console unlock token `pew!` unconditionally on
every invocation. But `Main.cpp` only feeds the unlock shift register while the
console is LOCKED (Main.cpp:1162). Once unlocked, every byte dispatches through
the command switch, so a second `pew!` is not a token at all -- it is four
commands:

| byte | effect |
| --- | --- |
| `p` | toggle the preset-bus overlay (Main.cpp:1521) |
| `e` | unrecognised -> framebuffer capture |
| `w` | begin "patch a byte in the resident card image", then eat the next 6 bytes as hex digits (Main.cpp:1550) |
| `!` | not a hex digit, so it cancels the pending patch |

Observed directly: three consecutive `--dump-fb` runs with no keys sent between
them returned three *different* screens -- the running app, then the preset-bus
overlay, then a frame short enough to decode as blank (the `w` argument reader
had eaten the capture request). Every panel script run through an
already-unlocked console was measuring something other than what it asked for.

**No bus traffic resulted.** `p` is display-only; `w` patches a RAM image and
its own comment says "no bus traffic"; the one command that can change what a
physical Buchla module holds is `x` (MasterRestore), which was never sent and
carries a double-press-within-3s guard of its own. The 251e/259e were not
touched.

Fix: `unlock()` now clears any half-finished multi-byte argument with `!`,
probes with a capture request, and sends the token only if no frame comes back
-- a locked console answers nothing, so a returned frame proves it is unlocked.
Verified on the module: three consecutive invocations all report "already
unlocked -- token not sent", the first two frames are byte-identical, and the
app name decodes cleanly instead of as fragments.

**Left for Oren (deliberately not changed unattended):** the deeper fix is an
unlock token whose bytes are all no-ops. It was not attempted because a wrong
guess locks the console out entirely, and the console is the only route to the
hardware.

## Verified on hardware

Distinguish these from simulator claims; they are different claims.

- **App-switcher folders (`887b6ba2`) work on the module.** `a+r` opened the
  switcher on `< AUDIO 5/7 >` listing Tweighty / Scope / Sampler / Wave-Edit,
  with the cursor inverted on Wave-Edit, the running app. That confirms the
  "opens on the folder holding the app you are in" guard and the folder
  taxonomy on the full 32-app build, which the simulator cannot reach (it
  builds 6 apps and neither Hemisphere nor Quadrants).
- Footer legend reads `L:fldr R:pick X:move`.

## P0 design decisions (PM, pending blueprint reconciliation)

Recorded before implementation so they can be argued with.

**Scope.** `AbyssApplet` is commented out of *both* registries in
`audio_applets/_config.h` and derives from `HemisphereAudioApplet` rather than
`HemisphereAudioAppletF32<>` like the other three. It is not in any shipping
build. Resurrecting dead code is a different and riskier task than moving live
code, so the default plan is to ship **Freeverb, Samverb and Delay** and report
Abyss as a decision rather than half-ship it.

**Output routing.** `AudioIO::kOutputRouteSources` is 3 and all three slots are
claimed: 0 Quadrants, 1 Tweighty, 2 Sampler (AudioIO.h:27-29). The new apps
need a slot.

Decision: **add one shared effect slot, not one per app.** All standalone
effect apps share `kOutputRouteEffectSlot`, and entering an effect app claims
the slot from whichever effect app held it last.

- Not one slot each: three reverbs permanently wired to the same input would
  sum into each other, and would hold F32 blocks and CPU permanently. The F32
  pool is 80 blocks and the Delay applet alone can hold ~10 per channel
  (AudioIO.h:11-13).
- Not disconnect-on-Suspend either: that would break the instrument's
  established idiom, which is that audio keeps sounding while another app is on
  screen. Claiming rather than releasing means exactly one effect is live at a
  time *and* it keeps running after you leave it.

Rewiring happens inside `AudioNoInterrupts()` brackets, which
`UI-Redesign-Constraints.md` section 1 records as proven safe.

**Inversion.** These screens obey L-06: inversion means "the right encoder
changes this", full stop. Cursors get a leading `>`, state gets a suffix or
bracket, banners get a drawn box.

## Folder data-integrity fixes, verified on hardware (`5f4c7b69`)

The two HIGH findings against `887b6ba2` were re-verified against source, fixed,
and then checked on the module rather than asserted.

**Method.** Flash `T41_console`, then in one `hwctl.py` session: open the
switcher, press X to move Tweighty out of AUDIO, and leave with `encL` -- a
plain cancel, deliberately NOT the long press that saves everything. The screen
followed the app to SYSTEM and the footer read `moved to SYSTEM`. Then reflash
to force a real boot, and look at AUDIO again.

**Result.** AUDIO came back holding only Scope, Sampler and Wave-Edit. Tweighty
was still filed under SYSTEM.

That is both fixes at once:

- **MEDIUM 4** -- the move survived an exit that never saved anything before.
- **HIGH 1** -- the arrangement survived the boot recall. Before the fix this is
  exactly where it was overwritten with compiled defaults, so the pre-fix
  outcome would have been Tweighty back in AUDIO.

The module's Setup/About page reported build `64dcbe7b`, confirming the binary
under test was the one carrying the fix.

## Bench-harness behaviour worth knowing

- **Use `z+r`, not `a+r`,** to reach the app switcher. `A` belongs to the app
  (shipped design), so `A+encR` does nothing in apps that use A -- confirmed in
  Tuner, where `a+r` fell through to the app and `z+r` opened the switcher.
- **The chord needs ~800 ms of settle** before the following key. At 600 ms the
  chord was intermittently missed and the key went to the app instead. Several
  confusing captures came from this before it was pinned down.
- **RETRACTED, see the corrections section below: the switcher does NOT
  activate the app under the cursor as the cursor moves.** That is what it
  looked like from the bench and it is wrong. `set_current_app()` is called at
  exactly one place, `OC_apps.cpp:288`, guarded by `if (change_app)`, and
  `change_app` is set at exactly one place, `case CONTROL_BUTTON_R` at `:224`.
  The `CONTROL_ENCODER_L` and `CONTROL_ENCODER_R` cases (`:170`, `:175`) never
  touch it, and `APP_SELECTION_TIMEOUT_MS` (`:163`) expiring leaves it false --
  so an idle timeout closes the switcher WITHOUT switching apps.

  What actually changed the running app between bench sessions was the harness:
  sending `z+r` while the switcher was ALREADY open injects an encR PRESS into
  it, which is a pick. The confusing app changes were self-inflicted.

## Corrections to earlier claims in this log

Both of these are claims I made tonight and then disproved. Recorded because a
wrong claim that sounds confident is worse than an admitted gap, and one of
them is already in a commit message where it will outlive this session.

**The app switcher does NOT commit the app as the cursor moves.** Commit
`3c27a768`'s message says it does. That is wrong. Tested directly: open the
switcher, turn `encL` to another folder, press `encL` to cancel -- the module
returns to the app it started in, unchanged. It also does not time out within
5 s of sitting open. Something across the longer gaps between bench sessions
did commit the app under the cursor, but I did not pin the mechanism down and
am not going to guess at one. What is verified: `encR` press picks, `encL`
press cancels, and cursor movement alone changes nothing.

**Framebuffer captures can tear.** One capture of Setup/About decoded as
garbled wrapped text -- runs starting mid-word at x=0 on the following row --
and the identical screen decoded cleanly moments later with no input in
between. So a garbled decode is worth one retry before it is believed to be a
rendering bug. Several confusing captures earlier tonight were this, not the
firmware.

## Finding H-02 -- `-fpermissive` is on for every target build

**Verified, and it reaches well past tonight.**

Commit `3e2893be`'s message, and the comment at `SegmentDisplay.h:147-148`,
both say `arm-none-eabi-g++` accepted a const violation that Apple clang
rejected. That is false. GCC rejects it too. What actually lets it through is
**`-fpermissive`, injected by the PlatformIO Teensy platform builder** at
`~/.platformio/platforms/teensy/builder/frameworks/arduino.py:111` and `:157`
-- not from `platformio.ini`, which is why nobody looking at this repo would
find it.

So every target build downgrades that whole class of error to a warning. More
const violations of the same kind are very likely still in the tree, and the
**only** thing that will ever find them is the clang-based simulator -- which
builds 6 apps of 15 and neither Hemisphere nor Quadrants. That is a standing
hole in the safety net, not a one-off bug.

## Known gap in tonight's own work

The roster stamp added in `5f4c7b69` (HIGH 2) is new behaviour and, by this
program's own rule, owes a new self-check. It does not have one. The simulator
compiles `OC_apps.cpp` so the code is built, but `selfcheck.sh` drives only
through `--keys` and framebuffer capture, and there is no way to pre-seed a
stored arrangement carrying a mismatched stamp without adding a new simulator
entry point. That is real work and it was not done. **The stamp-refusal path is
therefore build-verified and reasoned about, but not exercised by any test.**

One consequence worth stating plainly: a module that already has a stored
arrangement written before this commit has no stamp, so the fix **resets that
arrangement to defaults exactly once** on upgrade. That is deliberate -- bits
that cannot be attributed to a roster must not be reinterpreted -- and in
practice it affects nobody, because folders shipped only hours earlier the same
night.

## What the bench harness can and cannot test (measured, not assumed)

The correctness review's MEDIUM 3 says `hwctl.py` "cannot test any
LONG_PRESS/LONG_RELEASE distinction". Tested on hardware, and the truth is
narrower and more useful than that:

- **Injection DOES deliver `LONG_PRESS`.** Sending the hold form of Z while
  Quadrants was on screen raised `Clock Armed` immediately -- Quadrants acts on
  the event when it arrives, so it works.
- **A consumer that LATCHES on `LONG_PRESS` and CLEARS on `LONG_RELEASE` never
  sees it.** `ConsolePress` (Main.cpp:386-391) queues DOWN, LONG_PRESS and
  LONG_RELEASE back to back and `AppSettings` drains all three in one
  while-loop pass, so `save` is set true and then false again before it is
  tested. On a real panel those land in different `loop()` passes. This is the
  app switcher's hold-to-save specifically, not holds in general.
- **Anything driven by SUSTAINED BUTTON-DOWN DURATION cannot be expressed at
  all.** The chord card's 700 ms hold and its progress bar need a button to
  stay down over time; injection has no sustained down-state to offer, only
  discrete events. Holding Z through the harness never raised the card.

So the chord card is **not automatable through this harness**. Its hardware
verification remains the manual one Oren did on 2026-09-03. Any future claim
that a self-check "covers the chord card on hardware" is false by construction
unless the console gains a real press-and-hold-for-N-ms injection.

## Delay standalone app: hardware QA of `d85d556d`

Flashed T41_console (FLASH 565,096 / RAM1 96,992 / RAM2 137,280, free 387,008
-- the implementer's reported figures reproduced exactly). Driven over serial.

| Check | Result |
| --- | --- |
| Appears in the AUDIO folder | PASS, 4th of 5 |
| Full-width 128px layout, not a 64px render | PASS |
| MAIN / MODE / CV pages render | PASS, all three |
| encR press cycles pages | PASS, MAIN -> MODE -> CV -> MAIN |
| encL press returns to MAIN | PASS |
| encR turn edits the cursor row | PASS, Time 0.50s -> 0.65s |
| A toggles bypass | PASS, footer reads `BYPASSED - A:active` and back |
| Right-edge clipping (`edgecheck.py`) | PASS on all three pages |
| L-06: exactly one inverted region | PASS, the cursor band only |
| **Does not strand the instrument** | PASS, z+r opens the switcher from inside Delay |

**The pixel arithmetic was right.** Every predicted x-coordinate reproduced on
the panel: labels x=4, Time value x=97, Fdbk value x=115, Wet value x=109,
footer x=1, header x=1. That is worth stating because this project's history is
that pixel maths reviewed on paper has been wrong more than once.

### A bug I nearly reported, and did not

Delay first appeared to open on the MODE page rather than MAIN, which would
have been an entry-gesture leak -- the switcher commits on encR RELEASE, and
that release reaching the new app would cycle the page. That is a real hazard
class here and it was fixed once already in `be1573c3`.

It was not happening. Re-entry lands on MAIN and stays there, and `page_` is
initialised properly at `apps/DelayApp.h:178`. The giveaway was the Time value
reading **0.65s instead of the 0.50s default**: an earlier bench session had
left the switcher open, it committed Delay between sessions, and the three
encoder turns I thought were moving a switcher cursor were raising Time by
0.05s each -- after which my "pick" press was just a page cycle in an app that
was already running.

Recorded because the false version was more interesting than the truth, and
because it is the second time tonight that the switcher committing between
bench sessions produced a confusing result.

### Verified by accident: the roster stamp works

Adding DelayApp took the roster from 15 apps to 16, so the stored
position-indexed folder words no longer matched `AppFolderRosterStamp()`. The
arrangement I had made by hand earlier in the night (Tweighty moved to SYSTEM)
was **refused and re-seeded to the ID-keyed defaults**, and Tweighty came back
in AUDIO where its default says it belongs.

That is HIGH 2 doing exactly its job, in exactly the scenario it was written
for, on real hardware -- and it happened without being staged. Without it those
nibbles would have been reinterpreted against a shifted roster and the taxonomy
would have quietly scrambled.

### Still NOT verified, and cannot be from this bench

- **Whether it makes sound.** Claiming `kOutputRouteEffectSlot` is a
  build-verified claim. Nothing here can hear the module.
- **The CLOCK time unit** on the MODE page (the path needing
  `ClockSetup_instance.Controller()` pumped per tick).
- **The ~10.9 s maximum delay**, which is arithmetic on AUDIO_SAMPLE_RATE_EXACT
  and has never been timed.
- **The A2 reverb decay change**, unverified by ear.

### One design question for Oren

The Time bar's range is 0..10,921 ms, so a 500 ms delay fills **2 pixels of
52** and reads as empty. Every musically common delay time (100 ms - 1 s) sits
in the bottom 10% of that bar. The arithmetic is right and the display is
useless, which is a scale choice, not a bug. A log or dual-rate scale would fix
it. Not changed unattended because it is a design decision.

## Reverb standalone app, and Delay re-verified: hardware QA of `e664198a`

Built and flashed from a CLEAN tree so the binary under test corresponds to a
commit. That matters here: `pio run -t upload` builds from the WORKING TREE,
not from HEAD, so an uncommitted edit is silently compiled into the binary and
the QA result then describes a build that exists in no commit and cannot be
bisected to. The first Delay QA pass was against `d85d556d` and did NOT contain
the `AudioNoInterrupts` bracket or the screen refactor, so it was re-run rather
than assumed to carry.

**Delay, re-verified after `502e5c79` moved its draw code.** All three pages,
geometry identical to the pre-refactor pass (labels x=4, Time x=97, Fdbk x=115,
Wet x=109, header and footer x=1), `edgecheck.py` clean on all three. The "pure
code move" claim now rests on measurement rather than on reading.

**Reverb.** AUDIO folder, TWOCCS `RV`. Two pages, MAIN -> CV -> MAIN, and
correctly no MODE page -- it has no time unit, clock source or modulation type
to put on one, and a page that would be empty does not exist.

| Check | Result |
| --- | --- |
| MAIN renders full-width | PASS |
| `Cut :` value at x=97 | PASS, exactly as predicted |
| Size / Damp / Mix values at x=109 | PASS |
| CV page values at x=103 | PASS |
| Footer `A:byp  R:cv` / `A:byp  Y:src  R:main` | PASS |
| encR press cycles MAIN <-> CV | PASS |
| encL press returns to MAIN | PASS |
| A toggles bypass | PASS, `BYPASSED - A:active` |
| `edgecheck.py`, both pages | PASS, no clipping |
| L-06: one inverted region | PASS, cursor band only |
| Does not strand the instrument | PASS, z+r opens the switcher from Reverb |

**Two predictions confirmed end to end, which is stronger than "it looks
right":**

- The CV page reads **100%**, confirming the attenuversion arithmetic:
  `Atten(60) = 10*60*60/36 = 1000 tenths = 100%`.
- `Cut` renders **`15.0k`**, not `15000Hz`. The applet's own `%5dHz` is 7
  characters and would have collided with the bar's right edge at x=87;
  kHz-with-one-decimal is 5 and starts at x=97. That is finding M-6, fixed and
  measured.

**The roster stamp fired a second time.** Roster went 16 -> 17, the stamp
changed, and the arrangement was refused and re-seeded again. Two flashes in a
row where HIGH 2 did its job unprompted.

### How far the abstraction actually generalised -- the honest version

`git show e664198a -- software/src/AudioAppletHost.h` is **zero lines**, and so
is `AudioEffectScreen.h`. A MONO applet with no clock, no `DigitalInputMap` and
no `HS::` dependency went onto a host built around a STEREO applet with all
three, unchanged.

That is only true because of `502e5c79`, which came first and was not free.
Writing Reverb is what revealed that nine pieces of screen grammar were sitting
private inside `DelayApp.h` about to be copied. So the accurate claim is:

> The **lifecycle** host generalised untouched. The **screen** did not
> generalise until it was made to, and making it took one commit plus a
> refactor of an already-hardware-verified file. A third cost neither shared
> file absorbs: each applet needs a public accessor block before it can be
> hosted -- 41 lines for Freeverb, similar for Delay. That is per-effect work
> that does not shrink.

Two of three layers are shared; the third is a fixed per-effect cost of known
size.

### THE GAP THAT MATTERS: nobody has heard any of this

Delay, Reverb and the A2 decay-time change are **all** unheard. Every audio
claim in this series is arithmetic that reproduces on a framebuffer. The bench
can drive the panel and read the screen; it has no ear on the jacks.

Specifically unverified: whether either app reaches the codec at all, whether
claiming `kOutputRouteEffectSlot` disconnects the previous holder as designed,
whether Reverb (the first MONO applet through the host's summed-to-both path)
is audible on both channels, Delay's CLOCK unit, and its ~10.9 s maximum.
