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
- A stall of S ms therefore drops S / 2.902 output blocks and misses
  S / 0.060 CORE ticks. The measured 2748 ms preset save is ~947 blocks and
  ~45,800 ticks.

## Detector self-check

New counters are not trusted until they reproduce a known stall:

1. `T` at idle: `xrun=0`, `missed=0`, `loop pass max` under 2 ms, every
   row PASS.
2. `(` (save slot 0 locally), then `T`: expect `xrun` within 10% of
   wall / 2.902, `missed` within 10% of wall / 0.060, `loop pass max` about
   the save's wall time (the DWT figure the save prints, not `millis()`).
3. If the counts disagree by more than 10%, the detector is wrong. Fix it
   before believing any later number.

`test/test_rtstats.cpp` holds the same arithmetic as host checks.

## Declared windows

A persistence window is the one sanctioned way to stall: a user-initiated
save fades audio out, holds CV, sends note-offs, writes, and fades back in.
Drops attributed to an open window (`in_window`) do not count against the
audio rows; the window's own length counts against `windows max`. A
background write (the debounced current-slot record, the card image flush)
is never allowed a window: it waits for an idle gap instead.

Until the window helper lands (Track C of the plan), every stall counts.
That is intended: the counters exist to show what the fixes remove.

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
- MIDI: Captain's existing poll cadence meter feeds the histogram and the
  violation counter; `t` no longer resets the violation count.

## Reading `T`

Each row prints PASS or FAIL against its ceiling and the report ends with
the failure count. Pressing `T` again within 3 s resets every counter.
`t` prints a one-line summary. The six breadcrumbs (audio out xruns, core
missed ticks, loop pass max, allocation failures, window max, MIDI
violations) are refreshed once a second and appear with the crash report.
