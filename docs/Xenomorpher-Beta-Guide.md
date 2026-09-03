---
title: Xenomorpher Beta Guide
nav_order: 14
---

# Xenomorpher Beta Guide

This page is for people trying the Northern Light Modular **Xenomorpher**
firmware on the `preset-bus` branch before it merges to `main`. It covers
what's new, how to get it onto your module, what's solid, what's still
rough, and how to report back. If you just want to *use* the features once
they're installed, see [Xenomorpher Presets](Xenomorpher-Presets) and
[200e Modules App](200e-Modules-App) instead — this page is the "am I set
up correctly, and what am I actually testing" guide.

## What's new in this beta

The Xenomorpher now talks natively to a Buchla 200e case's preset bus:

* **A 30-slot preset engine**, always on the module's own internal flash —
  save/recall captures the whole module (active app, all app state,
  globals), not just the current app. See [Xenomorpher Presets](Xenomorpher-Presets)
  for the panel gestures and trigger-input details.
* **Coexistence with a 2WIRELESS (WPM) preset manager on the same bus** —
  broadcast SAVE/RECALL from the WPM works normally with the Xenomorpher
  attached, and the Xenomorpher can also broadcast its own save/recall to
  every module on the bus.
* **The [200e Modules App](200e-Modules-App)**, to read another module's
  program bank over the bus (251e sequences, 259e programs) into the
  Xenomorpher, edit it, and write it back.
* **Tweighty**, a new full-screen 8-tap WRITE/RECIRC delay/looper (design
  lifted from the Buchla-format 288r Time Domain Processor, reimplemented
  natively rather than ported) that keeps running through the audio ISR
  even when a different app has the front panel. Very new — see
  "What's still rough" below before relying on it for anything.

The full protocol-level writeup — what's implemented, what's tested, and
the specific hazards this design defends against — is in
[200e Conformance](200e-conformance).

## Getting the firmware onto your module

**Branch:** `preset-bus` on [this repo](https://github.com/auxren/Xenomorphicle) —
none of this has merged to `main` yet.

**Environment:** build/flash `T41_audio`. This is the only environment
that boots standalone on a Xenomorpher — `T41` and `T41_MTP` link to a
different flash address for multi-image setups and will *look* dead
(module goes dark, or shows up on your computer as USB device `1fc9:0135`
"SE Blank RT Family") if flashed alone. If that happens, it isn't bricked —
reflash with a `T41_audio` build and it comes back.

Two ways to get the `.hex`:

1. **Ask for a pre-built `firmware.hex`** (built from `T41_audio`) rather
   than building it yourself — simplest, and sidesteps the environment
   mix-up above entirely.
2. **Build it yourself**: install [PlatformIO](https://platformio.org/),
   clone the repo, check out `preset-bus`, and from `software/` run
   `pio run -e T41_audio`. The `.hex` lands at
   `software/.pio/build/T41_audio/firmware.hex`.

**Flashing it on:** the Teensy's own PROGRAM button isn't reachable once
mounted in a Xenomorpher, so use the front-panel path — see
[Installation Method A](Installation#method-a) for the full walkthrough
(install Teensy Loader, open the `.hex`, then on the module: Setup/About →
hold the LEFT encoder until `REFLASH: BOOTLOADER` appears → press **B** →
Program in Teensy Loader → Reboot).

**Checking what you're running:** the Setup/About screen prints the build
tag (git revision, plus `dirty` if it was a local edit) — no serial
console needed. Include that exact string in any bug report.

## What's solid

* Module-local save/recall (the panel gesture) and bus-wide broadcast
  save/recall via a WPM.
* Reading (`BACKUP`) a 251e or 259e's bank over the bus into the 200e
  Modules App — both module types confirmed on real hardware, including
  during simultaneous WPM traffic.
* The bus never activates on non-Xenomorpher hardware (it's gated by a
  runtime hardware check, not just the firmware build), and the module
  never serves its own card slot while a WPM is present on the bus — that
  refusal is hard-gated, not a setting you can turn off around it.

## What's still rough or unverified

* **Writing a bank back to a 251e/259e (`RESTORE`) has not yet been
  exercised on real hardware** — only reading has. The write path has a
  safety net (a pre-write snapshot to the Xenomorpher's own flash, a
  read-back verify, and an `UNDO` offer if that verify comes back bad),
  but until it's been run against real modules, treat *writing* to another
  module as the experimental half of this feature. Reading is not affected.
* **SD card export/import of presets isn't reachable from the panel yet** —
  presets always live on the module's internal flash, which is the safe
  default, but moving a preset to another module via SD card currently
  requires developer tooling. If you need that specific workflow before
  it's wired into the UI, ask.
* On the 200e Modules App's write-confirm screen, arming and confirming a
  write share one button on one of its two entry points (there's a
  built-in pause between them so a fumbled double-press can't complete a
  write by accident) — it can feel like the first press did nothing. That's
  expected; the screen tells you what it's waiting for.
* **Tweighty is brand new and its audio path has not yet been confirmed
  working on real hardware.** It builds, runs, and its screens render
  correctly, but a live bench test (real signal into IN L, monitoring OUT L)
  produced no audible output at all — under active investigation. Don't
  rely on it for anything yet; this note will be removed once it's
  confirmed working end to end on a real module.

## Safety notes

* A write to a 251e/259e rewrites **all 30 of its slots at once** — the
  card is the unit of transfer on that bus, not the one slot you touched.
  The confirm screen says this before you commit.
* Don't power down the Xenomorpher or unplug the bus mid-transfer. A
  half-finished read is just discarded and safe to retry; a half-finished
  write is the scenario the pre-write snapshot exists for, but letting a
  transfer finish is still the simple way to avoid needing it.
* The factory-reset gesture on this build is **A + B** at the boot
  splash screen, not A + encR — deliberately different from the app-switcher
  chord (hold A, press encR) so the two can't be confused.

## Reporting bugs

Open an issue at
[github.com/auxren/Xenomorphicle/issues](https://github.com/auxren/Xenomorphicle/issues).
Please include:

* The build tag from Setup/About (e.g. `abc1234` or `abc1234dirty`).
* Whether a WPM / 200e case was on the bus at the time, and what else was
  connected.
* What you did, what you expected, and what happened instead — screen
  text if there was any on screen when it went wrong.
