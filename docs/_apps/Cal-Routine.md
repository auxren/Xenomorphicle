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
| **259e pitch inputs** | Calibrates a Buchla 259e's two pitch CV inputs over the 200e bus, using output A and the meter | the meter, one cable, a 259e on firmware 31.2C4 | **Experimental; measuring only (the write is disabled).** After the outputs are calibrated, when the 259e's pitch tracking needs it |
| **251e CV outputs** | Calibrates the four CV outputs of a Buchla 251e over the 200e bus, reading them on the Xenomorpher's own inputs 1 to 4 after a loopback check of those inputs | four patch cables, a 251e on firmware 30.7C4, no meter | **Experimental; measuring only (the write is disabled).** After Outputs and Inputs have been done |

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

### 259e pitch inputs (experimental; the write is DISABLED)

> **Status.** The **measurement half** has been run on the bench with a real 259e: 600, 1200 and 3000 mV points agreed with the meter within 0.4 mV, the second input fit to a worst error of 1.83 cents with a nonlinearity of 1.08 cents, and the fitted gain was 0x8597 against stock 0x9041 (the first fit came out about 7.5 % below the stock gain on both inputs: that was the 4096-counts-per-octave premise, not a fault the old 10 % window could have caught). On that bench 259e **one record was committed** (one of its 8 lifetime slots is used). The read-back then did not match, which was traced to a bug in the 259e firmware 31.2C4's reply buffer; it is fixed in the 259e release candidate 31.2C5, which is **host-tested only**. The Xenomorpher's own read workaround for 31.2C4 is **not yet proven on hardware**, and the fit's scale (4096 counts per octave) is **unproven**. The write half is **EXPERIMENTAL**, and in this firmware it is **DISABLED** (`Cal259eFlow::kAllowWrite` is false, compiled out of every default firmware build): the row warms up, scans, identifies, measures, fits and shows **Results** read-only ("Write disabled: scale unproven"), there is no confirm screen, and no backup, PUT or COMMIT can be reached. A **measuring-only run must be discarded at Results**: press the left encoder twice ("L L: discard"). The write stays disabled until (1) the record read-back is proven right on hardware and (2) the target scale (4096 counts per octave) is proven by the bench octave-ratio check, gate X3 (see the checklist). The text below describes the write path as built, for those later sessions.
>
> **Each 259e accepts only 8 commits for its whole life.** Compaction (resetting the slots) is disabled in the firmware, so every write uses one of the 8 slots for good, and a failed or ambiguous COMMIT may still have used one. Do not spend slots on tests: the "all 8 slots full" case is run in the 8051 emulator, never on a bench 259e.
>
> **A 31.2C4 quirk the Xenomorpher works around.** The module's 8-byte record replies (READ `what 0`) arrive with data byte 5 forced to 00 and data byte 7 to 0C (its reply buffer overlaps the stock send routine's scratch bytes at XDATA 0x0684 and 0x0686). A byte-for-byte read of the record therefore never matched, although the module itself reported the record valid. The Xenomorpher now reads the record with seven overlapping READs and keeps only data bytes 0 to 4 and 6 of each; the last record byte (the CRC low byte) can never be read at a good position and is taken from the CRC of the other 28 (the console's `=28r` prints it apart, as `inferred_crc_lo=`).


Output A is the voltage source and the meter reads it; the 259e is the unit being calibrated, so it has to be on firmware 31.2C4 and the **outputs must have been calibrated by Cal Routine first** (otherwise the row says so and stops). Let the Xenomorpher and the 259e run for 30 minutes before you start.

**Only output A is ever driven.** There is no output B in this routine: you move the one cable from the principal pitch input to the mod pitch input when the screen asks.

1. Choose **259e pitch inputs**, read the meter notice and press the right encoder. The app finds the 259e (bus address 28 to 2B). If a cancelled run's write is still being confirmed ("259e: please wait"), it waits: try again in a few seconds.
2. Confirm the warm-up with the right encoder.
3. When asked, patch **output A into the 259e's principal pitch input**, with the meter's + on that same jack and its COM on the 259e's ground. Press the right encoder. The app raises output A and checks that exactly one 259e responds, then measures 8 fit points and 5 check points (600, 3000, 5400, 7800 and 9000 mV). The 600 mV point lies below the lowest fit point (1200 mV), so it checks the line's extrapolation toward 0 V. The 9000 mV point checks the top of the range: the 259e clamps its corrected code at 0x7FFF, one count under the 9600 mV fit point's target, so that fit point cannot show a positive error.
4. The same for the **mod pitch input**: move the same cable when asked.
5. **Results** shows, per input, the worst error in cents over the check points, the worst error over the fit points (nonlinearity, "nl"), and the fitted gain against the gain the ADC span predicts for the target scale (not against stock 0x9041), and how many minutes ago it was measured. Each input must have a worst check error within **3 cents**, a nonlinearity within **5 cents** (a provisional limit, to be confirmed from real bench data) and pass the **ADC span / offset / channel check** (`CalRec::Sanity259e`: gain within **±1.5 %** of T × 65536 / 7864.32, offset within **−50 to 150** counts, the two inputs' gains within **1 %** of each other), otherwise the run stops (`SCALE_<check>` in the log) and nothing is written. This check tests the ADC span, the offset and that both inputs agree; it **cannot detect a wrong counts-per-octave constant T**, because the fit's gain is proportional to T whatever the meter reads. Only a frequency measurement (bench gates X3/G1 to G5) can settle T. **The 10 minutes run from the first measured point of the principal input**, so a patch prompt left open for the mod input counts, as does a slow second input: anything older ends the run ("Readings over 10 min old") and it starts again from the measurement.
6. (Only in a build with the write enabled.) Press the right encoder for the write screen. It is a different screen from Results: an inverted banner **WRITE TO 259e @xx**, "This uses 1 of 8 slots, forever", and a bar. **Hold B for 2 seconds** (alone, not a chord, and not in the first 0.35 s after the screen appears): the bar fills and the write starts when it is full. Letting go early, touching any other key or encoder, or switching app empties the bar, writes nothing, and the screen says "Hold B to write"; a tap does nothing. The left encoder is a two-press discard here too. Holding B then backs up the 259e's presets, writes the record, and reads it back. The backup must be exactly 990 bytes and is stored in `CAL259BK.BIN`, then read back; any other size, or a file that does not read back, stops the run before anything is written. The backup is **refused while the firmware card is being served** (a Xenomorpher that booted with the card armed keeps serving it after `M` disarmed it, until a reboot, a console `k`, or a complete read of the card followed by 30 s of bus quiet): reboot the Xenomorpher with the card disarmed first. It is also refused while a browser (Bus Buccaneer) dump session is open. The 200e app and Bus Buccaneer show a distinct reason for the served card ("READ FAIL: fw card"; the bridge answers NAK 10), not a generic busy.
7. **If all 8 record slots are used the run stops** (each 259e takes 8 commits for its whole life) ("all 8 calibration slots are used; compaction is disabled"): nothing is written. Resetting the slots (compaction) is switched off in the firmware until a tested way to restore `CAL259BK.BIN` exists.

The left encoder backs out at every step and never writes anything by itself; the record is written only by the B press. The run's `CALR,259...` lines on the USB console include `CALR,259BK` (the backup: bytes transferred, CRC, result) and `CALR,259REC`, a copy of the record written. COMMIT is sent once and never retried: if the module's answer is lost the run ends **COMMIT unknown** or "Written: UNKNOWN", and you read `=28s` and `=28r` by hand. A `module refused` result means the 259e refused the COMMIT, possibly **after** programming (firmware 31.2C4 also refuses before programming, and checks the read-back last), so **a slot may be used: read `=28s` and `=28r`**. A bus error where the COMMIT never went out says nothing was written; one where it may have is "Written: UNKNOWN".

While a run is open, and until a cancelled run's write has been confirmed, nothing else may write to a 259e or restore presets: the console `=` (read-only anyway), the raw-frame sender `W` (only CAL frames, never PUT 12 or COMMIT 0B), `m`, `x`, `w` (it patches a byte of the resident card image, which may be the run's backup) and `k` (card serving), the broadcast save and recall keys, the USB SysEx bridge's GET_DUMP and PUT_DUMP, and the 200e app's reads and writes all refuse. If you cancel (or switch app) after the write started, the Xenomorpher still reads the module's answer in the background and logs `CALR,259END,CANCELLED_<result>`; the Cal Routine menu then shows "259e cancel: WRITTEN / REFUSED / UNKNOWN" until the next 259e run begins.

### 251e CV outputs (experimental; the write is DISABLED)

> **Status.** Nothing in this section has run on a real 251e. The row, the bus client and the fit are host-tested against a simulated 251e, and the write is compiled out of the firmware (`Cal251eFlow::kAllowWrite`, built on only by `-DCAL251E_ALLOW_WRITE=1`): the row warms up, checks the Xenomorpher's own inputs, patch-checks the 251e, measures, fits and shows **Results** ("Write off"); there is no confirm screen and no PUT or COMMIT can be reached. A measuring-only run is **discarded at Results** (press the left encoder twice) and is safe to repeat. Design, numbers and open questions: [docs/specs/cal-251e.md](../specs/cal-251e).

This row needs no meter. The Xenomorpher drives the 251e's four CV outputs to known raw DAC codes over the bus and measures them on its **own inputs 1 to 4**, so the **outputs and inputs must have been calibrated by Cal Routine first** (the row says so and stops otherwise). The 251e must run firmware 30.7C4 or 30.7C5, C5 preferred (it answers a status request at bus address 5C); both units should have run for 30 minutes.

1. Choose **251e CV outputs**. The app looks for the 251e, then asks you to confirm the warm-up. **Stop the 251e's sequencers and unpatch whatever else its CV outputs feed** (oscillators stacked on them will jump): the outputs move while it is held.
2. **Loopback.** Patch Xenomorpher **output E to input 1, F to 2, G to 3, H to 4** (no 251e cable yet) and press the right encoder. The app holds five voltages and compares what the inputs read with what the outputs were told to put out; every reading must be within **2.5 mV** or the run stops ("Xeno out vs in differ"). On the bench a healthy chain reads about 1 mV high on average with a worst case of 2.1 mV (Xenomorpher calibrated against a meter on a short ground reference near the outputs), while a stale input curve or a bad cable shows as many mV. A missing or wrong cable stops it with a different message. This checks the measuring chain before the 251e is touched; it is a consistency check of the Xenomorpher against itself, not an independent standard.
3. **Patch.** Move the **output end of each cable** from E-H to the 251e's **CV 1, CV 2, CV 3, CV 4** (input ends stay in inputs 1 to 4) and press the right encoder. The app raises each 251e channel alone and checks that only its own input follows; a crossed or missing cable stops the run and names the channel.
4. The app measures eight points from 1.2 to 9.6 V, fits a gain and an offset per channel, then measures five check points at the **corrected** codes, and repeats the first point to see drift. The 251e's overrides are refreshed all the time and released at the end (or at once if you cancel).
5. **Results** shows, per channel, the record's gain, offset and worst check error in mV, and how old the readings are. A run stops only on gross errors: a measured output scale more than 4 % off the stock 383.1 DAC codes per volt (10 V at code 3831), a zero floor over 100 mV, a wrong channel, an input near its top, a failed loopback, or a gross non-linearity (over 12 mV), check error (over 16 mV) or record error (over 50 mV). Smaller departures from the **write** limits (non-linearity 3 mV, check error 4 mV, 1 / 7.5 / 10 V within 8 mV) are shown as **WARN** (the channel's row starts with `!` and is inverted, and the bottom row names the channels: `WARN 2 nl`) and logged (`CALR,251WARN`; the run's `CALR,251END` line ends `warn=<hex>`, `warn=0` when clean), because those limits are not yet set from real figures; a build with the write on fails the run on them. Press the left encoder twice to discard.

The run's `CALR,251...` lines on the USB console (`CALR,251LINE` per channel: the measured mV per code and the zero) are the data from a first run; keep them. `=5Cs` reads only the 251e's status; `=5Cr` and `=5Ca0` are 259e readers and are not valid for a 251e. The 251e's existing record is logged at the start of every run, at Scan, as `CALR,251OLD`; to read it after a failed write, start a new 251e run, take that line, then press L at Warm-up. **No 251e status reply from address 5C** is what the row's first check reports (it does not read the firmware version). A write build refuses to write over a record it could not read (`OLD_UNREADABLE`).

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
