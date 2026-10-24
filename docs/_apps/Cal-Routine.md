---
layout: default
---
# Cal Routine

_Cal Routine_ calibrates the module's outputs and inputs against a USB bench meter, with no tweaking by hand. You patch cables where the screen says; the app measures, fits and checks, then stores the result. Every app uses the new calibration afterwards.

It is meant for a unit that tracks **1.2 V per octave**, with 0 V at the first entry of the output table. On any other unit the app says so on its first screen and does nothing.

### What you need

* A **USBTMC bench meter**. It has been tested on a **Siglent SDM3065X**.
* The meter plugged **alone into the module's host USB jack**: no hub and no other USB device (a MIDI keyboard counts). Readings shift by about 0.2 to 0.7 mV with a hub or after a replug, because the USB cable becomes part of the meter's ground path. Plug it in before you start, and **do not touch its USB cable while a calibration runs**.
* A patch cable from each output to the meter, one at a time, and then one cable from output A to each input in turn.

If the meter is unplugged during a run, every output is released and nothing is saved.

### Steps

1. Open **Cal Routine** from the App Selection Menu and choose **Self**. Read the two hint screens and press the right encoder.
2. Plug in the meter. The screen shows when it has been found and set up.
3. **Outputs.** For output A, then B, up to H: patch the output to the meter (nothing else on it) and press the right encoder to start. The app steps the output through its table, then checks points in between. Leave the cables alone until the next prompt.
4. **Inputs.** For input 1 to 8: patch **output A** into the input (nothing else on it; the meter may stay on) and press the right encoder. Press A to skip an input.
5. **Results.** Check the figures, then save or discard (below).

Jacks are named as on the panel.

### What the results mean

* **1 mV is 1 cent** at 1.2 V per octave, so a figure of 0.4 mV is 0.4 cents.
* **Outputs.** An output passes when its worst checked point is within **1 mV** of the meter. The screen shows the worst error and where it was. A **rail** flag marks a table entry the output cannot reach (it runs out of range before the target); such an entry is flagged but does not stop you saving.
* **Inputs.** Each input is calibrated against output A, so it is only as good as output A, which is why the outputs go first. The screen shows each input's **covered span**: the voltage range the curve is valid for. A rail on output A narrows every input's span.
* **A failed input stays as before.** Its previous calibration is kept; nothing is replaced for it.
* **A failed output** can be **retried**: turn the left encoder to put the cursor (>) on it, then press **A**. Save is offered only when every output has passed.

### Saving

Save is a **two-step** action. On Results, press the right encoder for **Save...**; a confirm screen says what will be replaced; press the **B** button there to write. The app is busy for about a second while it writes, and the module's loop pauses with it. The screen then confirms that the outputs and the passed inputs now use the calibration in every app.

To **discard** a finished run, press the left encoder on Results; it asks, and a second press within 3 seconds discards. Nothing is stored.

### How it relates to Setup

* Setup / Calibrate's **DAC steps replace** a Cal Routine calibration. Setup says so before you start them.
* Inputs with a Cal Routine calibration **ignore Setup's ADC steps**. Inputs without one still use them.
* The output tables and input curves go into a [Backup / Restore](Backup-and-Restore) dump. The "calibrated by Cal Routine" marker does not: after a restore, Setup no longer warns that its DAC steps would replace a Cal Routine calibration. Details are in [Saving State](../Saving-State).

### What every app gets

Outputs use the measured table instead of the nominal one, and each calibrated input uses its own measured curve instead of the shared scale. Quantizers, sequencers and the Hemisphere applets need no setting for this.

### How long it takes

Roughly **6 to 7 minutes** for the whole run (modelled; to be updated after bench timing). Retrying an output adds about a minute.
