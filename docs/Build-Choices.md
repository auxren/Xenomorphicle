---
title: Build Choices
nav_order: 3
---

# Build Choices

This fork builds for one piece of hardware: the Northern Light Modular
**Xenomorpher** (Teensy 4.1 on the O.R.N.8 shield, 8 CV in, 8 CV out, I2S
audio, USB host and device MIDI, 200e bus header). The upstream Phazerville
variants for Teensy 3.2, Teensy 4.0, VOR hardware and the older NLM 4U
modules were removed in September 2026; use
[djphazer's tree](https://github.com/djphazer/O_C-Phazerville) for those.

There is no hardware selection to make. The choice is which image to flash:

| Environment | What it is |
|---|---|
| `T41_audio` | The image the module runs. Boots standalone (slot 0), USB audio and MIDI, every audio app. |
| `T41_console` | `T41_audio` with a USB serial console in place of USB audio, for bench work. Same apps. |
| `T41_audio_dbg` | `T41_audio` plus USB serial and boot trace. Tight on DTCM; see the comment on the env in `platformio.ini`. |
| `T41` | The slot-1 payload the boot menu jumps to (hold Z then B). Does not boot on its own. |
| `T41_MTP` | The slot-2 recovery image (hold Z then X at boot). Mounts the module's filesystem over USB as an MTP disk so a bad file can be pulled or deleted without a reflash. |

Build and flash from `software/`:

```
pio run -e T41_audio -t upload
```

See [Installation](Installation) for the toolchain and
[bench flashing](bench-flashing) for flashing a rig over ssh.

### Flipped Operation

Flipping the screen and controls is a calibration option, not a build. In
[Setup/About](Setup-About) the two arrows in the title bar show screen
flip (up/down) and controls-and-IO reversal (left/right); dual-press
UP+DOWN to cycle. Return to the main menu to save; power cycle to take
effect.
