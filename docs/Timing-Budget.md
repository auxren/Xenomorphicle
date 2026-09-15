# Timing budget

What "tight" means for the Xenomorpher, as numbers the firmware measures
itself. The evidence is the console `T` report (`src/RtStats.h`,
`src/RtStats.cpp`); the headline counters also survive a warm reset in the
CrashReport breadcrumbs and land in `CRASH.LOG` at the next boot.

The instrument has three equal uses: live 200e preset performance, audio
effects processing, and MIDI/clock hub. Each row below is a ceiling on one
path. A row that fails is a bug or a deliberate, declared exception; there
is no third category.

## Ceilings

| Path | Ceiling | Counter |
|---|---|---|
| Audio output blocks dropped, outside a declared window | 0 per hour | `audio out xrun` |
| Audio input blocks lost, outside a declared window | 0 per hour | `audio in xrun` |
| Declared persistence window | <= 250 ms, at most 1 per user gesture | `windows max` |
| Background-write windows (not user-initiated) | 0: background writes defer to idle | `windows count` by reason |
| CORE ISR duration, worst ever | <= 40 us of the 60 us period | `core isr max` |
| CORE ISR missed ticks, outside a declared window | 0 | `core isr missed` |
| loop() pass, worst outside a declared window | <= 5 ms | `loop pass max` |
| loop() pass, p95 | <= 200 us | `loop` histogram |
| MIDI poll-to-poll gap, p95 | <= 1 ms | `midi` histogram |
| MIDI poll-to-poll gap, any | <= 5 ms | `midi poll violations` |
| F32 and i16 audio pool allocation failures | 0 per hour | `alloc_fail` |
| ISR-to-loop deferred calls dropped (a missed realtime MIDI byte) | 0 | `defer dropped` |

## Arithmetic behind the numbers

- CORE ISR period: 60 us (`OC_CORE_TIMER_RATE`), 36,000 cycles at 600 MHz.
- Audio block: 128 samples at 44117.647 Hz = 2.902 ms. The DMA half-buffer
  interrupt fires every 1.451 ms; `update_all()` runs every 2.902 ms.
- A stall of S ms misses S / 0.060 CORE ticks.

**A masked stall does not show up as dropped audio blocks, and expecting
that was wrong.** With interrupts masked the audio ISR does not run at all,
so it is never in a position to notice a missing block; the DMA simply
replays its buffer, and the pending interrupt coalesces into a single call
when interrupts return. Measured 2026-09-14: a 193 ms save produced
**0 output xruns and 2971 missed CORE ticks**. Missed ticks are therefore
the measure of a masked stall. Output xruns measure the other failure --
the audio graph starving while interrupts are live.

## Detector self-check

New counters are not trusted until they reproduce a known stall:

1. `T` at idle: every row PASS. Measured 2026-09-14 after 25 s idle:
   `xrun=0`, `missed=0`, core ISR max 8 us, `gap_max` 60 us (exactly one
   period), loop pass max 162 us, p95 under 100 us.
2. `(` (save slot 0 locally), then `T`. The save prints its own wall time
   in DWT cycles. Expect `core missed` within 10% of wall / 0.060, and
   every one of those ticks attributed `in_window`. Measured: a 193 ms save
   gave 2971 missed against 3216 expected, 8% low, with 2971 of 2971
   in_window.
3. If missed ticks disagree by more than 10%, or any are not attributed to
   the window, the detector is wrong. Fix it before believing any later
   number.

`test/test_rtstats.cpp` holds the same arithmetic as host checks.

## Declared windows

A persistence window is the one sanctioned way to stall. `OC::RT::
PersistenceWindow`, constructed around an unavoidable write in loop
context, fades the audio to silence over 15 ms while the audio interrupt
is still running, zeroes both halves of the DMA buffer so nothing is
replayed, writes, and fades back over 25 ms when it goes out of scope. The
CV outputs hold by themselves: their only writer is the core timer
interrupt, which is simply not running. `SaveSlot` holds one.

Drops attributed to an open window (`in_window`) do not count against the
audio rows; the window's own length counts against `windows max`, and a
window that runs past its declared maximum is reported on the console and
counted in `windows violations`.

**Only a write the player asked for may fade.** A background write, the
debounced current-slot record or the card image flush, has to wait for an
idle gap instead. Taking the audio away for something nobody asked for is
worse than the stall it hides.

The fade is a raised cosine with exact endpoints (`src/Fade.h`). A fade
that only nearly reaches zero leaves a DC step for the buffer-zeroing to
turn into the very click the fade exists to avoid, and the zero slope at
both ends is what separates it from a straight line.

## Where each counter is taken

- audio out: the output ISR branch with no block for either channel
  (`extern/f32/output_i2s2_F32.cpp`, the memset-and-flush path); one-sided
  blocks count as `half`; `update()` scratch allocation failure as
  `alloc_fail`.
- audio in: the input ISR's two missing `else` branches (block full, or no
  block); `update()` allocation failure as `alloc_fail`. The Serial print
  that used to sit in that ISR path is gone.
- F32 pool: `allocate_f32()` null return. Its critical section now uses
  PRIMASK save/restore.
- CORE ISR: DWT cycle count at entry (gap to the previous entry: a gap of
  1.5 periods or more counts the missing ticks) and at exit (duration
  high-water, never reset except by `T`).
- loop pass: DWT delta between consecutive passes, max and histogram; the
  pass that held a declared window is skipped.
- windows: DWT cycles, not `millis()`. Systick is masked along with
  everything else while the flash programs, so `millis()` under-reports
  exactly the part the number exists to measure: the 193 ms save above
  reported 25 ms by `millis()` before this was fixed.
- MIDI: Captain's existing poll cadence meter feeds the histogram and the
  violation counter; `t` no longer resets the violation count. The first
  poll after a declared window measures the window rather than a cadence
  failure -- polling is loop-bound and a window stops the loop on purpose --
  so it goes in the histogram but not against the violation ceiling.
- deferred calls: the ISR-to-loop ring (`src/DeferRing.h`) reports what it
  had to drop, which is a realtime MIDI byte that never went out.
- recall staging: how many of a recall's files are still waiting for the
  idle sync to put them on disk, plus the images it refused or superseded
  (`src/PresetStage.h`).

## Reading `T`

Each row prints PASS or FAIL against its ceiling and the report ends with
the failure count. Pressing `T` again within 3 s resets every counter.
`t` prints a one-line summary. The six breadcrumbs (audio out xruns, core
missed ticks, loop pass max, allocation failures, window max, MIDI
violations) are refreshed once a second and appear with the crash report.

## What the budget has retired

Two pieces of planned work were dropped because the counters above said
there was nothing to fix. Both had looked obviously worth doing.

**A load-shedding governor**, to drop applet `Controller()` calls when the
CORE ISR ran long. Measured with Quadrants and its applets running, the ISR
peaks at 31 us of its 60 us period. There is no overrun to shed, and
skipping a `Controller()` is a wrong note.

**Removing the per-tick `AudioNoInterrupts`.** `AudioAppletSubapp::
Controller()` and `AudioAppletHost::Tick()` mask the audio interrupt around
each applet's `Controller()`, every CORE tick, so that a parameter written
as two stores cannot be read half-updated by `update()`. Replacing that with
a generation counter or seqlock was scoped at three days and would have
touched every audio applet -- the largest regression risk anywhere in the
plan. Measured on hardware 2026-09-14, T41_console:

| app | audio xrun | missed ticks | CORE gap max | CORE ISR max |
|---|---|---|---|---|
| Delay | 0 | 0 | 60 us | 24 us |
| Reverb | 0 | 0 | 60 us | 25 us |
| Bungverb | 0 | 0 | 60 us | 25 us |
| Quadrants | 0 | 0 | 60 us | 29 us |
| Delay, 120 encoder steps sweeping a parameter | 0 | 0 | 60 us | 27 us |

A `gap_max` of exactly 60 us is one period: the ISR never ran late once. The
sweep is the case the brackets exist for, and it costs nothing measurable.

Revisit either only if a row here starts failing. The point of writing the
ceilings down first was to be able to decide this with a number instead of
an intuition, and in both cases the intuition was wrong.
