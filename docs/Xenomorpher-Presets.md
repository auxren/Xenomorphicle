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

Measured on hardware, saving repeatedly to one slot:

```
 336  347  358  418  380  390  403  471  423  433  446  898 ms
  69   73   80   88   89   98  105  115  115  122  128  140 ms
```

A sawtooth. Each save costs a little more than the last as LittleFS fills its
current block, then one save pays for compaction — the 898 ms above — and the
baseline resets. Block usage does not grow across the cycle; nothing leaks.

So budget **roughly 70–500 ms for a save, and occasionally about a second**.
That matters because a save blocks `loop()` for its whole duration, which is
the condition that used to expose a fault in the USB audio input path. That
fault is fixed and guarded (see the note at the top of `src/Audio/USB_F32.cpp`),
but it is why saving is not something to do from a hot path.

A recall is much cheaper — it validates, then moves bytes — and does not
compact.

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
