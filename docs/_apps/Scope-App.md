---
layout: default
---
# Scope (full-screen app)

_Scope_ is an oscilloscope and spectrum analyser for the Xenomorpher's own jacks. It is meant for use while patching: plug a signal in, open the app, and it finds the trigger, the timebase and the volts per division on its own. (Not to be confused with the small Hemisphere [Scope](Scope) applet.)

It lives in the **AUDIO** folder of the app switcher (hold A, press encR, turn encL to AUDIO).

### What it can show

| Jacks | Captured at | Notes |
|---|---|---|
| **Audio in L/R** | 48 kHz | **AC-coupled** in hardware (about 1.6 Hz), so DC and slow LFOs don't show here; full scale is about ±9.3 V at the jack. The screen says **AC**. |
| **CV in 1–8** (bananas) | 37.5 kHz each | DC-coupled, about −1.3 V to +10.6 V, using the input calibration. Fast enough to draw audio-rate shapes too. |
| **TR 1–4** | gate state, and every rising edge | Shown as on/off lanes, or used as an external trigger. |
| **CV out 1–8**, **audio out L/R** | — | What the module itself is sending. |

The channel field steps through ready-made groups: IN L, IN R, IN L+R, each CV, CV1–4, CV5–8, CV1–4 with TR1–4, the outputs. Its last entry, **Custom**, opens a picker when you press encR: tick any jacks you like, up to eight, and press encL when you're done.

### The screen

One line of text at the top; the trace below it (rows 11 to 54); the frequency line at the bottom.

```
IN L 1msA 2VA AUTO
```

That line is **channel · time per division · volts per division · trigger**. The field under the cursor is inverted. An **A** after the time or volts means it's on automatic.

The trigger field reads:

* **AUTO** — locked onto the signal itself. **FREE** means there's no repeating shape to lock to (an unpatched jack, a flat CV), so the trace free-runs.
* **A2**, **F2**… — the same, when several traces are showing: the number is which trace it locks to.
* **+2.5V** — a level trigger, at that voltage.
* **TR1**…**TR4** — external sync: every sweep starts on a rising edge at that TR jack.

Auto mode locks to the **whole repeat** of a complex wave, not to whichever wiggle crosses first, and stays on the same feature while the wave changes shape — turning a wavefolder or a ring modulator's depth keeps the picture still. For the steadiest picture of all, patch the oscillator's sync or a clock into a TR jack and choose that TR.

With several traces, **Y** switches between **overlaid** (one shared volt scale, each trace in its own line style with its number at the right end) and **stacked** (one lane each, each scaled on its own, the jack number at the left and the live voltage at the right; TR jacks draw as on/off lanes).

**X** switches to the **spectrum**: frequency runs left to right on a log scale from 20 Hz to 20 kHz, with faint lines at 100 Hz, 1 kHz and 10 kHz, and the top line shows the strongest peak (`pk 550Hz`). A ring modulator's sum and difference tones show up as pairs either side of the carrier. The spectrum analyses the trigger trace; a TR jack has none. The top line there has fixed places: the channel at the left, the peak reading right-aligned just left of `FFT` (so a change in its digits moves nothing), and only the channel field can be selected. The average of the signal (any offset) is removed before the analysis, for every source.

### The frequency line

The bottom line of the screen counts the frequency of the **trigger trace** when it is an audio or CV input (the other jacks show `freq: --`). It updates ten times a second:

```
440.012Hz A4 +0.05c
```

The number of decimals follows what the counter can actually tell, so a low tone prints fewer digits than a high one. A mark in front says how far to trust it. No mark means the reading is confirmed (two 0.1 s counts in a row agreed) and the module's crystal has been calibrated.

| Mark | Meaning |
|---|---|
| **`?`** | not confirmed yet: the last two 0.1 s counts have not agreed. Wait a moment, or the signal is moving. |
| **`!`** | the input is not a clean repeating tone (noise, hum, several tones at once). The number is the best guess, not a measurement. |
| **`*`** | steady, but the Xeno's crystal has not been calibrated yet, so the absolute value is only as good as the crystal (typically a few cents at most). |
| **`#`** | a CV-input reading whose sample rate is still unverified on the hardware. |

The line follows the trigger trace's source: the jack whose trace the Scope is triggering on. Instead of a number you may see NO SIGNAL, TOO QUIET (the tone is too small to count), SIGNAL LOST, TOO LOW or TOO HIGH (the range is 10 Hz to 8 kHz, specified 20 Hz to 5 kHz). Below 16 Hz it shows plain Hz, no note: an LFO on a CV input is counted down to 0.5 Hz (the reading then takes a few seconds). It keeps counting when the trace is frozen. The console commands that read it and set the crystal correction are in [the bench console](bench-console).

### Controls

| Control | Does |
|---|---|
| **encL turn** | move along the top line |
| **encR turn** | change the field under the cursor. Turning time, volts or trigger takes it off automatic. |
| **encR push** | put that field back on automatic (on the channel field showing Custom: open the picker) |
| **encL push** | back (closes the picker) |
| **A** | freeze / run |
| **B** | everything back on automatic |
| **X** | trace ⇄ spectrum |
| **Y** | overlaid ⇄ stacked |

In level-trigger mode, encR turns the level; encR push goes back to AUTO.

### How the automatics behave

They are damped on purpose. The timebase moves one step at a time and only after the signal has wanted it for a second; the volts per division zoom in after a second but zoom **out at once** if the signal would run off the screen. Nothing flickers between two settings. A flat signal changes nothing.

### Settings

The channel group, the Custom picks, the view, every manual setting and the crystal correction of the frequency counter are saved with the rest of the module's state. Settings saved by older Scopes (the first one and the one before the frequency line) are restored; the correction starts uncalibrated.
