---
title: Xenomorpher Presets
nav_order: 13
---

# Xenomorpher Presets

Thirty snapshots of **this module's** entire state, stored on internal flash and
recalled from the front panel or by the case's preset manager. They are the
Xenomorpher behaving like a native 200e module: when a 225e sends a bus-wide
RECALL, this module changes with everything else.

Not to be confused with the thirty preset slots *inside* a 251e, which the
[200e Modules app](200e-Modules-App) reads and writes. Different thirty.

## What a preset holds

One file per slot, `PB_NN.PBS`, containing up to five sections:

| section | contents |
|---|---|
| `G` | global settings and the slot manifest |
| `A` | every app's serialized state |
| `B` | the active Quadrants preset, extracted from its bank |
| `S` | Scenery |
| `C` | Captain MIDI |

**Calibration is never in a preset.** It belongs to the physical module, not to
a scene, so recalling somebody else's preset cannot untune your outputs.

## The overlay

Hold **both encoder buttons** to open it. A large seven-segment slot number
sits in the middle; the name row is below.

| gesture | does |
|---|---|
| `encR` turn | choose a slot |
| `encL` turn | move between slot / name / NEXT trigger / LAST trigger |
| hold `encL` **0.5 s** | STORE to the selected slot |
| hold `encR` **0.25 s** | RECALL — **bus-wide**, every module in the case |
| `A` or `B` | close |

Both holds show a progress bar in place of the word `hold`, and both start
filling immediately.

The two durations differ, and the shorter one is the more consequential: RECALL
changes the live state of every 200e module in the case. A firm ordinary press
is 150–250 ms, so treat that gesture with respect. The overlay ignores the
chord that opened it until you release, so entering the overlay can never roll
straight into a recall.

## Trigger inputs

Assign `NEXT` and `LAST` to a trigger input from the overlay's cursor, and a
clock pulse steps through presets. The assignment persists.

## Where presets live

**Always on internal flash**, whether or not an SD card is fitted.

This used to follow the card: inserting one made all thirty slots read empty
and pulling it brought them back. Nothing was lost, but you cannot tell that
from the front panel, and a preset store that answers differently depending on
an accessory is not a preset store.

The card carries presets *between* modules instead. From the bench console,
`E` exports every slot to the card and `J` imports every slot the card holds,
into the same numbered slot. There is deliberately **no panel gesture yet** — a
half-built one would be worse than none.

### Space

`LittleFS_Program` uses 64 KB erase sectors on a Teensy 4.1, so the 4 MB
partition is **64 blocks** and every file costs a whole one however few bytes
it holds. A container is about 4.7 KB and occupies one block.

Measured on hardware with all thirty slots filled:

```
Space Used  2,490,368 / 4,194,304  =  38 of 64 blocks
```

Thirty slots, thirty blocks, twenty-six spare. The layout before this one used
three to five files per slot — ninety blocks for thirty slots against a
filesystem that has sixty-four — and would have exhausted around slot 20 while
cheerfully reporting over a megabyte free.

### How long a save takes

Measured on hardware, forty consecutive saves to one slot:

```
 253 286 277 280 287 259 287 [527] 280 284 276 278 292 275 278 278 265 [685]
 282 265 270 286 281 283 277 285 276 [528] 284 275 276 285 271 285 259 263
 293 286 [554] 259 ms
```

The baseline is **one 64 KB block erase, about 250–295 ms**, and that is the
floor: LittleFS_Program erases with interrupts off, so audio, USB and the
preset bus all stand still for it. The container is staged in RAM and written
once, so a save never erases more than the block its file lands in.

Every tenth save or so costs a second erase (the bracketed ones, 530–690 ms).
That is littlefs compacting the root directory: every file create, close,
delete or rename appends a commit to the directory's log, and when the log
reaches its cap the pair's other block is erased and the log rewritten with
one entry per file. The cap is set to 8 KB in `PhzConfig::Init` — the library
default is the whole 64 KB block, and that default is a trap, see below.
Block usage does not grow across the cycle; nothing leaks.

So budget **roughly 300 ms for a save, and every tenth one about 600 ms**.
That matters because a save blocks `loop()` for its whole duration, which is
the condition that used to expose a fault in the USB audio input path. That
fault is fixed and guarded (see the note at the top of `src/Audio/USB_F32.cpp`),
but it is why saving is not something to do from a hot path.

### How long a recall takes

**About 3 ms.** A recall validates the container, moves bytes, and switches
the app; it writes nothing, so it never erases and never compacts. The only
write it causes is deferred: three seconds after the last recall the current
slot number is persisted so the next boot lands on it. That record lives in
the emulated EEPROM, not in a file — four bytes at the top of the EEPROM map
(`EEPROM_PRESETBUS_START`), written as one 2-byte flash program and skipped
when unchanged. It used to be a key in `GLOBALS.CFG`, which made every
preset change cost a 64 KB erase with interrupts off three seconds later,
while the performer was playing the sound they had just recalled. Modules
upgraded from that firmware migrate the key into the record on first boot.

The 3 ms depends on the directory log being short, and here is the trap. A
littlefs `open()` walks the directory log from the start, CRC-checking every
commit, so its cost is linear in log fill: measured at **4.5 ms per open
with the log at 60 KB**, which put a five-file recall at 25–30 ms, against
0.2 ms per open after compaction. The log had been fattened not by presets but
by dozens of `CRASH.LOG` appends during a boot loop — small commits from
anywhere count. With the cap at 8 KB an open costs under a millisecond at
worst. The console's `h` key prints the current per-open cost and the log
fill, which is the first thing to check if a recall ever feels slow.

### Slots saved by older firmware

Presets written before the one-file-per-slot layout still recall: the engine
looks for a container first and falls back to the old files. **Saving a slot
converts it**, one way, and reclaims two blocks. Legacy slots cannot be
exported to a card until they are converted; the console says so rather than
reporting a bare failure.

## At power-up

The module restores the slot the case was last on, which is what a 200e module
does. Captain MIDI's own configuration is deliberately **not** restored at boot
— it is your interface setup, not scene state, and restoring a snapshot of it
at power-up silently rewound mapping edits. Explicit recalls still restore it.

Note that this is the last *preset*, not the last live state: anything changed
since the last Save or Recall is not carried across a power cycle.
