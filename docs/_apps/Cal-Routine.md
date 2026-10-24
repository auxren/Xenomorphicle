---
layout: default
---
# Cal Routine

_Cal Routine_ calibrates the module's outputs against a USB bench meter, and its inputs against those outputs, with no tweaking by hand. You patch cables where the screen says; the app measures, fits and checks, then stores the result. Every app uses the new calibration afterwards.

It is meant for a unit that tracks **1.2 V per octave**, with 0 V at the first entry of the output table. On any other unit the app says so on its first screen and does nothing.

### The routines

The app opens on a list. Turn either encoder to choose, press the right encoder to open.

| Routine | What it does | Needs | When to use it |
|---|---|---|---|
| **Outputs** | Sets all 11 table points of outputs A to H against the meter, then checks 20 points in between | the USB meter | After touching an output trimmer, or when the outputs drift |
| **Inputs** | Patch A→1 … H→8; the app checks the patch, then measures all 8 inputs at once against the outputs | 8 patch cables, no meter | After touching an input gain trimmer, or after an Outputs run when the inputs were not done |
| **Self (both)** | Outputs, then Inputs on the new output tables | the meter and 8 cables | A first calibration, or after touching any trimmer when you have the time |
| **259e pitch inputs** | Calibrates a Buchla 259e's two pitch CV inputs over the 200e bus, using output A and the meter | the meter, one cable, a 259e on firmware 31.2C4 | **Experimental.** After the outputs are calibrated, when the 259e's pitch tracking needs it |

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

The outputs are **actively driven** and load-independent: what they put out does not depend on what they drive.

### Inputs (and the second part of Self)

1. Patch **output A → input 1, B → 2, C → 3 … H → 8**: eight cables, nothing else on those inputs. The meter is not used (in Self it may stay plugged in). Press the right encoder.
2. **Patch check.** The app raises each output alone and watches which input moves. A wrong patch stops here and the screen names the jacks:
   * "C reaches 5, not 3": output C's cable goes to input 5 (a swap, or a stacked cable). Fix it and press the right encoder to check again.
   * "C reaches 3 inputs": output C's signal reaches three or more inputs (stacked cables).
   * "D: input 4 silent": nothing reached input 4 (no cable, or a dead one). Fix it and check again, or press **A** to go on without the silent inputs; they are then left uncalibrated.
   * "No input reading": the inputs delivered no samples, so nothing could be judged. Press the right encoder to check again.
   * In **Self**, press **B** to give up the inputs for now and keep the outputs just measured: you get the outputs' results and can save them as an Outputs run.

   The check also proves that input n is reached by output n: the inputs are numbered the same way as the outputs.
3. The app steps all eight outputs together and reads all eight inputs at each step. It takes about half a minute.

**Below about 0.6 V the input curves are extrapolated.** The outputs cannot go below 0 V, and the sweep starts at 0.6 V, so the stretch from there down to an input's bottom (about −1.35 V) is an extension of the measured curve, not a measurement. At the top, each input's curve ends where that input tops out (about 10.6 V, set by its gain trimmer).

### 259e pitch inputs (experimental)

Output A is the voltage source and the meter reads it; the 259e is the unit being calibrated, so it has to be on firmware 31.2C4 and the **outputs must have been calibrated by Cal Routine first** (otherwise the row says so and stops). Let the Xenomorpher and the 259e run for 30 minutes before you start.

**Only output A is ever driven.** There is no output B in this routine: you move the one cable from the principal pitch input to the mod pitch input when the screen asks.

1. Choose **259e pitch inputs**, read the meter notice and press the right encoder. The app finds the 259e (bus address 28 to 2B). If a cancelled run's write is still being confirmed ("259e: please wait"), it waits: try again in a few seconds.
2. Confirm the warm-up with the right encoder.
3. When asked, patch **output A into the 259e's principal pitch input**, with the meter's + on that same jack and its COM on the 259e's ground. Press the right encoder. The app raises output A and checks that exactly one 259e responds, then measures 8 fit points and 5 check points (600, 3000, 5400, 7800 and 9000 mV). The 600 mV point lies below the lowest fit point (1200 mV), so it checks the line's extrapolation toward 0 V. The 9000 mV point checks the top of the range: the 259e clamps its corrected code at 0x7FFF, one count under the 9600 mV fit point's target, so that fit point cannot show a positive error.
4. The same for the **mod pitch input**: move the same cable when asked.
5. **Results** shows, per input, the worst error in cents over the check points, the worst error over the fit points (nonlinearity, "nl"), and the fitted gain against stock, and how many minutes ago it was measured. Each input must have a worst check error within **3 cents**, a nonlinearity within **5 cents** (a provisional limit, to be confirmed from real bench data) and a gain within **10 %** of stock, otherwise the run stops and nothing is written. **Results older than 10 minutes expire** and must be measured again.
6. Press the right encoder for the write screen. **B** on that screen (a press of its own, not a chord) backs up the 259e's presets, writes the record, and reads it back. The backup must be exactly 990 bytes and is stored in `CAL259BK.BIN`, then read back; any other size, or a file that does not read back, stops the run before anything is written. The backup is **refused while the firmware card is being served** (a Xenomorpher that booted with the card armed keeps serving it until a reboot, even after `M` disarmed it): reboot the Xenomorpher with the card disarmed first.
7. **If all 8 record slots are used the run stops** ("all 8 calibration slots are used; compaction is disabled"): nothing is written. Resetting the slots (compaction) is switched off in the firmware until a tested way to restore `CAL259BK.BIN` exists.

The left encoder backs out at every step and never writes anything by itself; the record is written only by the B press. The run's `CALR,259...` lines on the USB console include `CALR,259BK` (the backup: bytes transferred, CRC, result) and `CALR,259REC`, a copy of the record written. COMMIT is sent once and never retried: if the module's answer is lost the run ends **COMMIT unknown** or "Written: UNKNOWN", and you read `=28s` and `=28r` by hand. A `module refused` result means the 259e refused the record **after** programming it (firmware 31.2C4 checks the read-back last), so a slot may have been used: read `=28s` and `=28r` too.

While a run is open, and until a cancelled run's write has been confirmed, nothing else may write to a 259e or restore presets: the console `=` (read-only anyway), the raw-frame sender `W` (only CAL frames, never PUT 12 or COMMIT 0B), `m` and `x`, the broadcast save and recall keys, the USB SysEx bridge's GET_DUMP and PUT_DUMP, and the 200e app's reads and writes all refuse. If you cancel (or switch app) after the write started, the Xenomorpher still reads the module's answer in the background and logs `CALR,259END,CANCELLED_<result>`; the Cal Routine menu then shows "259e cancel: WRITTEN / REFUSED / UNKNOWN" until the next 259e run begins.

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
* **259e pitch inputs:** a few minutes: 13 meter points per input, each a few readings.
