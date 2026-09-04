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
