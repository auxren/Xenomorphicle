---
layout: default
---
# Cal Routine

_Cal Routine_ calibrates the module's outputs against a USB bench meter, and its inputs against those outputs, with no tweaking by hand. You patch cables where the screen says; the app measures, fits and checks, then stores the result. Every app uses the new calibration afterwards.

It is meant for a unit that tracks **1.2 V per octave**, with 0 V at the first entry of the output table. On any other unit the app says so on its first screen and does nothing.

### Three routines

The app opens on a list. Turn either encoder to choose, press the right encoder to open.

| Routine | What it does | Needs | When to use it |
|---|---|---|---|
| **Outputs** | Sets all 11 table points of outputs A to H against the meter, then checks 20 points in between | the USB meter | After touching an output trimmer, or when the outputs drift |
| **Inputs** | Patch A→1 … H→8; the app checks the patch, then measures all 8 inputs at once against the outputs | 8 patch cables, no meter | After touching an input gain trimmer, or after an Outputs run when the inputs were not done |
| **Self (both)** | Outputs, then Inputs on the new output tables | the meter and 8 cables | A first calibration, or after touching any trimmer when you have the time |

Each routine saves only its own part: **Outputs** replaces the output tables, **Inputs** replaces the input curves, **Self** replaces both.

**Inputs is only as good as the outputs.** Each input is measured against the output patched into it, so calibrate the outputs first (Outputs or Self). If the outputs were never calibrated by Cal Routine, Inputs says so before it starts; you can go on or cancel.

### The hardware trimmers

The module has trimmers that the calibration sits on top of:

* **One gain trimmer per output.**
* **One shared 0 V offset trimmer for all eight outputs** (BIAS1). Turning it moves every output.
* **One gain trimmer per input.** It also sets where that input tops out (below).

**Touching any trimmer means running the matching routine again**: an output gain trimmer or BIAS1 → Outputs, and then Inputs too, since the inputs are measured against the outputs (or simply Self); an input gain trimmer → Inputs.

### Before you start

* **Warm up first.** Let the module and the meter run for a while before calibrating (15 minutes is a good rule); the figures move a little while the circuits come up to temperature.
* For **Outputs** and **Self**: a **USBTMC bench meter**, tested on a **Siglent SDM3065X**, plugged **alone into the module's host USB jack**: no hub and no other USB device (a MIDI keyboard counts). Readings shift by about 0.2 to 0.7 mV with a hub or after a replug, because the USB cable becomes part of the meter's ground path. Plug it in before you start, and **do not touch its USB cable while a calibration runs**.
* **Put the meter's COM on the module's ground near the jack** you are measuring (the sleeve of a nearby jack, or a ground point on the module), not on a distant case or bus ground. Shifts of about 1 mV seen on the bench came from ground and return paths, not from the outputs.
* For **Inputs** and **Self**: eight patch cables.

If the meter is unplugged during a run, every output is released and nothing is saved.

### Outputs (and the first part of Self)

1. Choose **Outputs** (or **Self (both)**). Read the hint screen and press the right encoder.
2. Plug in the meter. The screen shows when it has been found and set up.
3. For output A, then B, up to H: patch the output to the meter (+ to the output, COM to ground, nothing else on that output) and press the right encoder. The app steps the output through its table, then checks the points in between. Leave the cables alone until the next prompt.

The outputs are **actively driven**: what they put out does not depend on what they drive. They are calibrated **unloaded** (the meter's input is the reference); patching one into a module's input lowers it by about 1 mV, the same at every voltage.

### Inputs (and the second part of Self)

1. Patch **output A → input 1, B → 2, C → 3 … H → 8**: eight cables, nothing else on those inputs. The meter is not used (in Self it may stay plugged in). Press the right encoder.
2. **Patch check.** The app raises each output alone and watches which input moves. A wrong patch stops here and the screen names the jacks:
   * "C reaches 5, not 3": output C's cable goes to input 5 (a swap, or a stacked cable). Fix it and press the right encoder to check again.
   * "D: input 4 silent": nothing reached input 4 (no cable, or a dead one). Fix it and check again, or press **A** to go on without the silent inputs; they are then left uncalibrated.

   The check also proves that each input jack is the one the firmware thinks it is.
3. The app steps all eight outputs together and reads all eight inputs at each step. It takes about half a minute.

**Below about 0.6 V the input curves are extrapolated.** The outputs cannot go below 0 V, and the sweep starts at 0.6 V, so the stretch from there down to an input's bottom (about −1.35 V) is an extension of the measured curve, not a measurement. At the top, each input's curve ends where that input tops out (about 10.6 V, set by its gain trimmer).

### What the results mean

* **1 mV is 1 cent** at 1.2 V per octave, so a figure of 0.4 mV is 0.4 cents.
* **Outputs.** An output passes when its worst checked point is within **1 mV** of the meter. The screen shows the worst error and where it was. A **rail** flag marks a table entry the output cannot reach (it runs out of range before the target); such an entry is flagged but does not stop you saving.
* **Inputs.** Each input is calibrated against its own output. The screen shows each input's **covered span**: the voltage range the curve was measured over. A rail on an output narrows its input's span.
* **A failed or skipped input is saved as not calibrated.** Saving inputs replaces the stored calibration of every input: an input that did not pass (listed as **Uncal** on the confirm screen) then reads on the shared scale, as on a unit that was never calibrated, until a later run calibrates it.
* **A failed output** can be **retried**: turn the left encoder to put the cursor (>) on it, then press **A**. In Self, the inputs are then patched, checked and measured again.
* **When you can save.** Outputs and Self: every output has passed. Inputs: at least one input has passed.

### Saving

Save is a **two-step** action. On Results, press the right encoder for **Save...**; a confirm screen says what will be replaced; press the **B** button there to write. Saving output tables makes the app busy for about a second, and the module's loop pauses with it. The screen then confirms what now uses the new calibration in every app.

To **discard** a finished run, press the left encoder on Results; it asks, and a second press within 3 seconds discards. Nothing is stored.

### How it relates to Setup

* Setup / Calibrate's **DAC steps replace** a Cal Routine output calibration. Setup says so before you start them.
* Inputs with their own calibration **ignore Setup's ADC steps**, and Setup says so on those steps ("Per-input cal: kept"). Inputs without one still use them.
* The output tables and input curves go into a [Backup / Restore](Backup-and-Restore) dump of the **Calibration** region only; a **Data** dump carries neither. The "calibrated by Cal Routine" marker travels in no dump: after a calibration restore, Setup no longer warns that its DAC steps would replace a Cal Routine calibration, and Inputs warns that the outputs were not set by Cal Routine. Details are in [Saving State](../Saving-State).
* **Changing the controls-and-IO reversal** (the left/right arrow in [Setup / About](../Setup-About)) after calibrating means running Cal Routine again. The stored calibration follows the jack names, so after a reversal each name sits on a different physical jack than the one that was measured.

### What every app gets

Outputs use the measured table instead of the nominal one, and each calibrated input uses its own measured curve instead of the shared scale. Quantizers, sequencers and the Hemisphere applets need no setting for this.

### How long it takes

Modelled at 0.53 s per meter reading, to be updated after bench timing:

* **Outputs:** about **22 minutes** (19 to 27 minutes depending on the meter's speed). The meter reads at 10 power-line cycles with autozero on, which is slow but gives real, quiet readings. Retrying an output adds about 3 minutes.
* **Inputs:** about **half a minute** of measuring, plus the time to patch eight cables. No meter.
* **Self:** about **22 minutes**: the outputs, then the patching and half a minute of inputs.
