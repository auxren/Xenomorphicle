[![CI](https://github.com/auxren/Xenomorphicle/actions/workflows/ci.yml/badge.svg)](https://github.com/auxren/Xenomorphicle/actions/workflows/ci.yml)

Xenomorphicle - an active o_C>Phazerville Suite firmware fork
===


## About this fork

This repository started as a fork of Phazerville Suite and has grown into its
own firmware for one specific instrument: a **Northern Light Modular Buchla
4U** module — the _Xenomorpher_ — living inside a real Buchla 200e case. It's
no longer aiming to stay a general-purpose, upstream-compatible fork; the
goal is making this particular Teensy 4.1 module a first-class citizen of
that specific system, and using the same hardware to explore what a modern
DSP-capable module can add to a vintage-format instrument that neither the
format's original design nor the stock O_C firmware anticipated. Where the
Xenomorpher's needs and upstream Phazerville's diverge, the Xenomorpher
wins.

Two pieces of work, at different levels of maturity:

* **The 200e preset bus** is the mature, hardware-verified half. The module
  talks I²C to the other modules in the case, follows and drives bus clock and
  bus MIDI, coexists with a Buchla preset manager on the same wire without ever
  auto-claiming a resource the manager owns, and can read and rewrite another
  module's preset bank — with a pre-write snapshot and verified read-back
  before any of those 30 slots on someone else's module get touched. This is
  the part that's been run against real 251e/259e modules on the bench, not
  just built.
* **Tweighty** (`software/src/apps/TweightyApp.h`) is the newer, more
  experimental half — an original 8-tap WRITE/RECIRC delay/looper, design-led
  by the Buchla-format 288r Time Domain Processor but reimplemented rather than
  ported, built to keep processing audio in the background even while a
  different app has the front panel. It's real, self-contained new
  instrument functionality this hardware didn't have before, not a bus
  feature — and, as of this writing, its output audio path has a known,
  unresolved routing conflict (see [TODO.md](TODO.md)'s Tweighty section)
  that hasn't reached a fix yet. Screens and controls are done and verified on
  real hardware; the sound isn't reliably reaching the jacks yet.

Both halves share the same standard: host-tested pure logic where the logic
can be isolated from hardware, every claim about ISR safety or memory budget
checked against the actual linked ELF rather than the source alone, and
nothing declared working until it's been run on the physical module — a
mockup or a simulator render has caught real bugs in this project, but it has
also missed real bugs that only showed up once someone looked at the actual
device.

Where to start:

* **Build environments** — `software/platformio.ini`. `T41_audio` is the slot-0
  image the module actually runs, and it carries `-DPRESET_BUS` and the 200e
  app; `T41` links to slot 1 and **will not boot on its own**.
  `software/flash.sh` builds locally and then flashes over ssh to a bench rig,
  refusing any image not linked for slot 0 and verifying by USB enumeration
  afterwards. The `nlm*` environments target the older, T3.2/T40-based NLM 4U
  hardware line (a separate product from the Xenomorpher) and add
  `-DNORTHERNLIGHT`, which among other things changes the factory-erase
  gesture at the splash screen.
* **The bus** — `-DPRESET_BUS` turns it on; `-DENABLE_APP_BUS200E` adds the
  _200e Modules_ app on top of it. See
  [200e conformance](docs/200e-conformance.md) for how coexistence with a
  Buchla preset manager is guaranteed, and
  [252e clock sync](docs/252e-clock-sync.md).
* **Tweighty** — `-DENABLE_APP_TWEIGHTY` (on by default in `T41_audio`).
  Read the class-header comment in `TweightyApp.h` for the control model; see
  [TODO.md](TODO.md) for its current known issues before relying on it.
* **Audio path** — the codec (`PCM1754` DAC / `PCM1808` ADC on the O.R.N.8
  shield) runs float32 end to end through `AudioIO.cpp`, on 32-bit I2S
  frames that carry the converters' full 24-bit resolution; the
  Hemisphere/Quadrants applet bus is still int16 and bridges at that
  boundary until applets are ported to F32. `T41_audio` also builds with
  `-DAUDIO_SUBSLOT_SIZE=3`, so the module enumerates to the host as a
  class-compliant USB Audio Class 2 device at 24-bit/48kHz, 4 channels each
  way (ch1/2 = engine bus, ch3/4 = codec monitor) — no vendor driver needed
  on macOS, Windows, or Linux — alongside USB MIDI in the same composite
  device. That 24-bit USB bridge is compile-verified but hasn't completed
  its full on-hardware A/B pass yet; see
  [the bench checklist](docs/bench-float32-checklist.md).
* **State and presets** — [Saving State](docs/Saving-State.md), including the
  30 preset-bus slots and why they live on internal flash.
* **Trying this before it merges** — [Xenomorpher Beta Guide](docs/Xenomorpher-Beta-Guide.md):
  what's solid, what's rough, how to get the firmware on, how to report back.
* **Bench work** — [console cheat sheet](docs/bench-console.md) and
  [flashing notes](docs/bench-flashing.md). Both are written for a headless rig
  where the Teensy's PROGRAM button is unreachable.
* **Interface review** — `design/ui-review/`, a design canvas of findings and
  proposals for the whole-instrument navigation and the 200e app. Every item is
  marked shipped or open.
* **A host simulator** — `tools/xeno-sim/` runs the real firmware's screens
  (including Tweighty's, and — a first for this simulator — its audio-DSP
  engine's compilation, though not its actual sound) on a desktop, no module
  required. See its own README.
* **Known open work** — [TODO.md](TODO.md), Xenomorpher and Tweighty sections.

What follows is inherited from upstream Phazerville — background and
attribution, not a design constraint this fork still tracks.

***

Grab Paul's [**Screen Capture**](https://github.com/PaulStoffregen/Phazerville-Screen-Capture) program to view the module's screen on a PC via USB — handy on the bench without a monitor cable in the way.

## Hardware Info

The Xenomorpher is a Teensy 4.1 on the **O.R.N.8** shield (aka "O_C T4.1",
https://github.com/PaulStoffregen/O_C_T41 — designed by Paul, derived from
mxmxmx's original o_C): 8ch ADC / 8ch DAC (`DAC8568` on a dedicated SPI0;
OLED on a dedicated SPI1, no shared bus), 2ch audio in + 2ch audio out
through the `PCM1808`/`PCM1754` I2S codec pair, serial MIDI in/out, and USB
host MIDI. That's the only shield/MCU combination this fork's `T41_audio`
build targets or is tested against.

Upstream Phazerville also supports the original 4ch **o_C** shield (Teensy
3.2/4.0, DAC and OLED sharing one SPI bus) and older Teensy generations —
see `platformio.ini`'s `T32`/`T40`/`nlm*` environments if you're building for
that hardware instead; none of it is exercised by anything in this fork's
own work.

## Stolen Ornaments

Using [**Benisphere**](https://github.com/benirose/O_C-BenisphereSuite) as a starting point, this project takes the **Hemisphere** ecosystem in new directions, with many new applets and enhancements to existing ones. An effort has been made to collect all the bleeding-edge features from other developers, with the goal of cramming as much functionality and flexibility into the nifty dual-applet design as possible!

I've also included **all of the stock O&C firmware apps** plus a few others, _but they don't all fit in one .hex_. As a courtesy, I provide **pre-built .hex files** with a selection of Apps in my [**Releases**](https://github.com/djphazer/O_C-Phazerville/releases). You can also tell a robot to make a [**Custom Build**](https://github.com/djphazer/O_C-Phazerville/discussions/38) for you... (T3.2 only)

...or clone the repo, customize the `platformio.ini` file, and build it yourself! ;-)
I think the beauty of this module is the fact that it's relatively easy to modify and build the source code to reprogram it. You are free to customize the firmware to work in your system, similar to how you've no doubt already selected a custom set of physical modules.

## How To Hack It

This fork is built with [PlatformIO](https://docs.platformio.org/en/latest/core/installation/methods/installer-script.html)
(standalone CLI, [full IDE](https://platformio.org/install/ide), or a
VSCode/other-IDE plugin) — the project lives in `software/`. For the
Xenomorpher, build and upload with:
```
pio run -e T41_audio -t upload
```
or use `software/flash.sh` to build and flash a bench rig over ssh (see the
[bench flashing notes](docs/bench-flashing.md)). Customize apps and other
flags inside `software/src/OC_options.h`, or per-environment in
`platformio.ini`.

`platformio.ini` also carries `T32`/`T40`/`nlm*` environments and an
Arduino-IDE path (`software/src/src.ino`) for hardware outside this fork's
scope — not exercised by anything here.

_**Pro-tip**_: If you decide to fork the project, and enable GitHub Actions on your own repo, GitHub will build the files for you... ;)

## Credits

Many minds before me have made this project possible. Attribution is present in the git commit log and within individual files.

Thanks & Shoutouts:
* **[Paul Stoffregen](https://github.com/PaulStoffregen)** (PJRC) for Teensy 4.x driver code, new hardware designs, and lots of support!
* **[beau-seidon](https://github.com/beau-seidon)** for polyphonic MIDI handling, **ProbMeloD** mask rotation, **WTVCO**, and free-flowing enthusiasm.
* **[qiemem](https://github.com/qiemem)** (Bryan Head) for **Ebb&LFO** and its _tideslite_ backend, the Audio Applet framework, and many other things.
* **[Logarhythm1](https://github.com/Logarhythm1)** for the incredible **TB-3PO** sequencer, as well as **Stairs**.
* **[herrkami](https://github.com/herrkami)** and **Ben Rosenbach** for their work on **BugCrack**.
* **[benirose](https://github.com/benirose)** also gets massive props for **DrumMap**, **Shredder** and the **ProbDiv / ProbMeloD** applets.

And, of course, thank you to **[Chysn](https://github.com/Chysn)** (RIP) for the clever applet framework from which we've all drawn inspiration - what a legend!

This is a fork of [Benisphere Suite](https://github.com/benirose/O_C-BenisphereSuite) which is a fork of [Hemisphere Suite](https://github.com/Chysn/O_C-HemisphereSuite) by Jason Justian (aka Chysn / [Beige Maze](https://soundcloud.com/beige-maze)).

ornament**s** & crime**s** was a collaborative firmware project by Patrick Dowling (aka **pld**), mxmxmx, and Tim Churches (aka **bennelong.bicyclist**), considerably extending the original firmware for the o_C / ASR eurorack module, designed by **mxmxmx**.

http://ornament-and-cri.me/

## License

Except where otherwise noted in file headers, all code herein is generally considered MIT licensed. However, there are some GPLv3 bits included, so the whole thing is also subject to compliance with the GPL. [More info here](https://ornament-and-cri.me/licensing/).
