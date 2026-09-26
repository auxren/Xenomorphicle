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
| Declared persistence window | <= 300 ms, at most 1 per user gesture (see "The save window, measured") | `windows max` |
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
- Audio block: 128 samples at 48000 Hz = 2.667 ms. The DMA half-buffer
  interrupt fires every 1.333 ms; `update_all()` runs every 2.667 ms.
  (44117.647 Hz, and the 2.902/1.451 ms that follow from it, is the Teensy
  3.x rate. This core defines `AUDIO_SAMPLE_RATE_EXACT` as 48000.0f and
  nothing in `platformio.ini` overrides it. The graph-order section below
  always used 48 kHz, so the two halves of this document disagreed.)
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

## The save window, measured

The declared persistence window ceiling is 300 ms. It started at 250, set
before anything had been measured, against a documented save cost of 2748 ms.
Neither number survived contact with the bench.

32 consecutive saves of slot 0, T41_console, 2026-09-14:

| | wall |
|---|---|
| four saves in five | 129-139 ms |
| every fifth save | 192-259 ms |

The fifth-save step is deterministic, not noise: 6 spikes in 6, at saves 5,
10, 15, 20, 25 and 30.

The save is one phase. Of a 130 ms save, `cw_commit` is 126; capture, flush,
bank, files, globals, appdata, verify and resume are 0 or 1 ms each. That
phase writes a 4940-byte container across two 4 KB flash blocks, and the
erase is the floor -- roughly 43 ms per block on this part. No amount of code
makes writing those bytes faster.

The periodic step on top is LittleFS compacting the directory metadata pair.
Each save spends several metadata commits: the temp file is removed, created,
written, verified and renamed, and the Teensy wrapper additionally writes a
creation and a modified timestamp attribute on every open for write. Those
fill a 4 KB metadata block after about five saves, and compacting it costs
another erase.

**Why it is not reduced.** The obvious cheap win -- dropping the `remove()`
before the truncating open in `cw_commit` -- is not available: the wrapper
opens with `LFS_O_RDWR | LFS_O_CREAT` and no `LFS_O_TRUNC`, so that remove is
load-bearing and without it a shorter container would keep the previous one's
tail. Moving the compaction to an idle gap would need `lfs_fs_gc`, which
arrived after the LittleFS 2.4 this framework ships. Writing less would mean
the container referencing its sections instead of copying them, which trades
the one property that makes a preset safe -- that it is a self-contained
snapshot -- for about 40 ms.

So the window is bounded by flash physics and a filesystem that cannot be
asked to tidy up early. A 250 ms line failed a budget row every fifth save on
behaviour that is correct and unimprovable. 300 reflects what the hardware
does and still catches a regression.

## App switching, measured

Switching apps is a user gesture that tears down and rebuilds the audio
graph, so it is not free. Measured per switch on T41_console, 2026-09-25,
counters reset immediately before each one:

| switch into | loop pass max | audio xrun |
|---|---|---|
| Setup, Tuner, Scope, Bungverb, ScaleEdit | 1.1-2.0 ms | 0 |
| Calibr8or, Captain, Quadrants, Sampler, Reverb, Backup, Tweighty | 1.5-3.8 ms | 0 |
| 200e Modules | 4.4 ms | 0 |
| **Delay** | **18.6 ms** | 0 |

Every one of them drops zero audio blocks and misses zero CORE ticks. The
loop pass is long because loop() is doing real work, not because anything is
masked.

**The Delay is the outlier and it is inherent.** Its buffer is 512K samples
of PSRAM, 2 MB, and `extmem_calloc` hands it out zeroed -- the pool is built
with `do_zero` (startup.c), so the allocator clears it. Zeroing 2 MB at PSRAM
bandwidth is about 20 ms. It has to stay zeroed: the read head trails the
write head, so an unzeroed buffer would play whatever the previous tenant
left there for a whole delay period. See `src/Audio/AudioBuffer.h`.

A full sweep of all fourteen apps costs about 23 ms of worst-case loop pass
in steady state, with zero dropped audio blocks and zero missed ticks across
three consecutive passes. The first sweep after a flash is dearer -- one pass
measured 200 ms and a 147-block audio gap -- which is filesystem and SD
warm-up, not a recurring cost.

So the `loop pass p100 <= 5 ms` row fails during app switches, and that is
the row describing steady-state operation rather than a deliberate rebuild.
It is left as-is rather than widened: it costs one known, explained failure
during a gesture the user just made, and keeping it tight means an
*unexpected* 20 ms stall still shows up.

## Audio graph update order, measured

Console `O` (T41_console) walks the live audio graph and reports every patch
cable whose source updates *after* its destination. Such a cable costs one
whole audio block — 2.666 ms at 48 kHz / 128 samples — because the
destination already ran and consumed what the source left on the previous
pass. Nothing faults and the signal sounds correct, so this is invisible to
every other counter here.

It is a property of object construction, not of wiring. The Teensy Audio
library links each `AudioStream` into one global list in its constructor
(`AudioStream.h:149-156`), the ISR walks that list in order, and nothing
reorders it. There is no destructor, so nothing leaves it either. The
library's own comment calls this a TODO awaiting "a proper data flow
analysis".

Measured at idle, 2026-09-25, T41_console, 76 nodes and 70 cables:

| class | count | what it is |
|---|---|---|
| static → static | 6 | declaration and link order between translation units |
| runtime → static | 12 | an applet feeding fixed infrastructure |
| runtime → runtime | 2 | ordering inside one F32 applet |
| feedback (self) | 0 | inherent, not a defect |

The three classes need different fixes. **runtime → static is structural:**
the infrastructure is static and applets are built on demand by
`Registry::get()`, which memoises per (slot, id) and never destroys, so an
applet used for the first time mid-session cannot have been constructed
before the thing it feeds. No declaration order fixes that.
**runtime → runtime** is `HemisphereAudioAppletF32`'s edge adapters being
base-class members, so `output_adapter` is constructed before the derived
DSP that feeds it.

### Dead end: `init_priority` on AudioIO's streams

The six static → static cables sat on both ends of the main audio path —
`input_route` reached `audio_app`'s slot inputs a block late, and the chain
tail reached `app_container`'s monitor tap a block late — so 5.3 ms before
any applet did any work. They are late because `audio_app` and
`app_container` are constructed before `AudioIO.cpp`'s statics, and
cross-translation-unit static init order is unspecified.

`__attribute__((init_priority(101)))` on `input_i2s`, `conv_in`,
`input_route` and `output_route` fixes the ordering exactly as intended:
**static → static went 6 → 0**, with no new late cables created.

**It also kills the audio.** On the bench: `audio in peak` 0 where it had
been ~6.9M, and `audio out` in one unbroken xrun run that kept growing
(28,000+ and climbing). Reverting restored both immediately — input peak
back to 4.2–6.9M, xrun 0, 0 of 11 rows FAIL — so the causation is not in
doubt in either direction.

The cause is that `AudioInputI2S2_F32`'s constructor calls `begin()`, which
configures I2S2 and starts its DMA. Moving that ahead of every other
translation unit's static init breaks it. Both halves of the codec path
have to be brought up together, and `output_stream` is deliberately built
lazily in `OutputStream()` long afterwards, so raising the input's priority
pulls the two further apart rather than closer.

**Do not retry this without first making the codec bring-up explicit** —
ordered in `AudioIO::Init()` rather than implied by constructor side
effects. 2.666 ms of latency is not worth a dead input.

### The fix that would work, and why it is not here

Reordering the update list itself — a topological sort after each topology
change — fixes all three classes at once and stays correct as applets are
swapped. It is what the library's own TODO asks for.

It is blocked on access, not on the algorithm. `first_update` and
`next_update` are private, and the core's `AudioDebug` friend
(`AudioStream.h:205-230`) exposes only getters. Writing them needs either a
patched vendored header — out of tree, so not reproducible in CI — or the
explicit-template-instantiation access loophole, which is standard-conforming
but exotic and would silently rot against a library update. Both change the
data structure the audio ISR walks, so neither should land unattended.

## Round-trip latency, measured

Console `Y` (needs AUDIO OUT jumped back to AUDIO IN) writes a marker into
the outgoing I2S half-buffer from the output DMA ISR and timestamps the
first sample over threshold in the input DMA ISR. Both ends are taken in the
hardware DMA ISRs, not in `update()`, so the audio software ISR's own
scheduling is not folded into the number.

Measured 2026-09-26, T41_console, quiet graph (Captain MIDI active, no
applet chain in the path):

| run | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| ms | 3.186 | 3.186 | 3.186 | 3.186 | 3.183 |

**3.186 ms, or 1.19 audio blocks**, repeatable to 3 µs. That spread is
small enough to say the probe is tracking a real DMA boundary rather than
sampling noise.

This is the **floor**: TX DMA buffering, the codec's DAC, the patch cable,
the codec's ADC and RX DMA buffering, with nothing of ours in between.
Whatever the applet graph adds — including every late cable the `O` report
counts, at 2.666 ms each — stacks on top of it.

Running the probe costs nothing: 0 of 11 rows FAIL with probes in the
window, xrun 0, core ISR max 14 µs, missed 0. When idle it is one
predictable branch per half-block in each DMA ISR.

**Bench note.** With both outs jumped to both ins and any passthrough app
active, the module self-oscillates to full scale — `audio in peak` reads 99%
where the floor with a cable attached is under 0.3%. That is the patch, not
a fault, but it makes the probe meaningless (detection triggers instantly on
the feedback), so measure with a non-audio app in front.

## Update order, fixed

The graph is now sorted into dependency order rather than left in
construction order. `src/AudioGraphOrder`'s `MaintainOrder()` runs from the
loop, notices when the wiring has changed, and re-sorts. Console `U` forces
it; `O` reports the result.

Measured 2026-09-26, T41_console:

| | before | after |
|---|---|---|
| cables running backwards | 20 of 70 | **0** |
| nodes in the list | 76 | 76 |
| budget rows failing | 0 of 11 | 0 of 11 |

Steady-state cost is below the noise: loop pass max 372 us against the 5 ms
ceiling, p95 under 100 us, xrun 0, missed ticks 0, CRASH.LOG unchanged.

### The trigger is the wiring, not the node count

The first attempt at an automatic trigger would have watched for new nodes.
That is wrong, and the bench said so: opening five apps left the node count
at **76** -- `Registry::get()` memoises per (slot, id), so those apps reused
objects that already existed -- while the cable count went **70 -> 110** and
**nine late cables reappeared**. A new connection between two objects that
both already exist is as damaging as a new object.

So `MaintainOrder()` hashes the live wiring (one pass over the update list
and both destination lists, endpoint pointers into an FNV-1a mix), 4 Hz, and
re-sorts when it changes. It is skipped while a persistence window is open: a
save has already faded the output and is about to mask interrupts, which is
not the moment to walk the graph.

### What it does not fix

Feedback loops. Around 14 nodes sit in loops with the full app set loaded,
and the handful of cables inside them stay one block late. A one-block delay
in a feedback path is what feedback *is*; `O` names them rather than hiding
them.

### Why this is safe to do to the ISR's own list

`first_update` and `next_update` are private and the core's `AudioDebug`
friend has getters only, so the links are reached through the explicit
instantiation access rule -- `[temp.spec]` does not access-check names in an
explicit instantiation, which makes it standard-conforming rather than a
layout guess, and keeps the vendored header untouched so CI builds what the
bench runs.

`Reorder()` refuses, leaving the list alone, on any of four conditions: the
walk truncated, the sort did not return every node, the result is not a
permutation, or the private links disagree with `AudioDebug`'s getters on any
node. After relinking it walks the list the ISR will actually walk and
confirms the count. Pausing is safe rather than hopeful: the audio software
ISR preempts thread mode, so if the reorder is running then no `update_all()`
is part-way through the list, and the pause stops another starting.
