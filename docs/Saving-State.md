---
title: Saving State
nav_order: 5
---

# Saving State

Your App / applet state will not be remembered between power cycles unless you:

* (A) Manually save to EEPROM _(Long-press RIGHT encoder to escape to main menu, long-press RIGHT again to save)_
* (B) Store the current state of Hemisphere to a [preset](Hemisphere-Presets)
* (C) Turn on [Auto Save](Hemisphere-Presets#auto-save)

To Save/Load presets or toggle Auto Saving in Hemisphere, long-press the DOWN button to open the config menu, and (if necessary) rotate the LEFT encoder to paginate to the floating preset menu.

When storing a Preset, it immediately triggers an EEPROM Save (with a potential 2ms interruption, fyi) so there is no need to also long-press-save on the main menu.

## EEPROM Save

All of the Apps included in your firmware build have Config Settings that can change at runtime. You can make them persist after a power cycle with the built-in Ornament and Crime storage system:
* Return to the main menu by long-pressing the right encoder.
* Then, long-press the right encoder again.

This will save the state of all applications in the module, as well as the active App and various Global Settings (user scales, waveforms, sequence patterns, turing machines, etc).

## Teensy 4.0 & 4.1

Several Apps have been updated to store settings in binary files on T4 hardware instead of using emulated EEPROM. This includes: **Hemispheres**/**Quadrants**, **Scenery**, **Calibr8or**, **Captain MIDI**.

Settings are typically saved when you store a Preset, which happens automatically in some cases. Global settings like custom Scales and Vector Waveforms are stored in a separate config file, only when you invoke it with a R-Enc-Long-Press on the Main Menu.

Newer Teensy's have enough space in flash storage to accomodate a filesystem (aka LittleFS or LFS). Teensy 4.1 also has a microSD card slot; you could theoretically add one to a Teensy 4.0 as well. This allows significantly more storage, and will also makes it easier to backup/restore/transfer settings by simply copying files. (Files on the internal LFS storage will become more accessible when USB MTP Disk support is stable.)

The binary file format used by Phazerville Apps (PhzConfig) has a small header with a signature & checksum, followed by a simple KEY-VALUE hashmap using 16-bit KEYs and 64-bit VALUEs. External editors are a possibility.

## Preset-bus slots (Xenomorpher / `PRESET_BUS` builds)

Xenomorpher builds add a second, separate store: **30 whole-instrument preset
slots**, saved and recalled from the preset-bus overlay (both encoder buttons)
and shared with the rest of a Buchla 200e case over the bus. Slots are numbered
1–30 on screen and 0–29 in the protocol.

**They always live on internal flash (LittleFS), card or no card.** This is
deliberate, and it changed: the engine used to prefer the SD card whenever one
was seated, which meant inserting a card made all 30 slots read *Empty preset*
and pulling it brought them back. Nothing was lost either way, but nothing on
the front panel could tell you that. The card is now for *carrying* presets,
not for holding them.

**One file per slot: `PB_NN.PBS`.** This is not tidiness. `LittleFS_Program`
allocates whole erase sectors, and on Teensy 4.1 a sector is 64 KB — so the
4 MB partition is **64 blocks**, and every file costs a whole block however
little it holds (a 588-byte file measured on the bench occupied 64 KB). The
old layout used three to five files per slot; 30 × 3 = 90 blocks against a
64-block filesystem, which ran out around slot 20 while reporting more than a
megabyte free. Each container holds up to six sections in one file:

| section | contents |
|---|---|
| `G` | global settings + the slot manifest, in PhzConfig format |
| `A` | the per-app chunk stream (`OC::AppData` serialization) |
| `B` | the active Quadrants bank, extracted from its bank file |
| `S` | a copy of `SCENERY.DAT` |
| `C` | a copy of `CAPTAIN.DAT` |

`SCENERY.DAT` and `CAPTAIN.DAT` are read from and written back to internal
flash explicitly, because Scenery and Captain MIDI themselves call PhzConfig
with no filesystem argument — which defaults to internal flash. Quadrants bank
files are the one thing that still follows the card, because that is where
Quadrants looks for them.

The older multi-file slot layout (`PB_NN_G.CFG`, `PB_NN_A.BIN`, …) is still
*read* so existing slots survive a firmware update, and is retired once the
same slot has been written as a container. It is never written.

**Export / import to SD is not available yet.** The engine has the functions;
nothing in the UI or the serial console calls them, so at present there is no
way to move a preset slot to or from a card. Slots written to an SD card by an
older build are not read by the current one.
