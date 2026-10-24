# Cal Routine bench checklist

Gates 1 to 6 are copied word for word from `design/cal-routine-design.md` section 7. Under each, the steps and the pass condition used on the bench. Items marked PASSED were done on 2026-10-24; the rest are open. Console keys are in [bench-console](bench-console); meter lines (`METER,...`) come from the console `e` key and run lines (`CALR,...`) from the app (`tools/hwqa/cvmeter.py` parses both).

Rig: Xenomorpher on the console build, Siglent SDM3065X alone on the host jack (unless the item says otherwise), outputs patched to the meter or to the inputs as the screen asks.

## Design gates

### 1. ISR budget

> **ISR budget** with every input calibrated and a heavy app running: core ISR max still <= 40 us, 0 missed ticks (`docs/Timing-Budget.md` method). Today's measured peak is 31 us (Quadrants with applets), so the per-channel multiply has room; measure it on hardware anyway.

Steps: calibrate all 8 inputs with the Cal Routine, load Quadrants with applets, run the `t` / `T` selftest for a few minutes. Pass: core isr max <= 40 us, missed 0.

- [x] PARTIAL, 2026-10-24 (input 2 only, via `Q21`): Quadrants (Pigeons + Carpeggio): core isr max 37 us, 0 missed, 0/11 FAIL; the same applets uncalibrated: 37 / 36 us, so the calibrated path's cost is not measurable. The 3 us of headroom is the applets' load (Timing-Budget recorded 31 us for Quadrants earlier). After the input-map fix (adc3c3cc): core isr 16 us, missed 0 on an idle app.
- [ ] Repeat with all 8 inputs calibrated by the Cal Routine and the heavy app.

### 2. Loop budget

> **Loop budget** during a run: loop pass max <= 5 ms; USB-MIDI on the host port still works with the meter on the hub.

Steps: during a whole run read the loop max from `T`. Pass: <= 5 ms. Note: Save blocks the loop for about 1 s while it writes; the 5 ms figure applies to the run, not to that write.

- [x] PASSED, 2026-10-24 (driver demo, no flow yet): loop max 174 us with the meter reading, 241 us with hub and keyboard.
- [x] PASSED, 2026-10-24: MIDI coexistence. KeyStep 37 mk2 (1C75:0229) on host1 and the meter via a hub: 10/10 `READ?` DONE, keyboard works.
- [ ] Loop max during a full Cal Routine run.
- [ ] Measure the Save stall: expected about 1 s, nothing stuck afterwards (outputs resume, display redraws).

### 3. Tracking check

> **Tracking check** on hardware after save: outputs at the half-way points against the meter, and a calibrated output looped into each input read back in the meter path, both at or better than the results screen; one app's pitch input (e.g. a quantizer) tracks within the reported figure.

Steps: after Save, hold each output at the half-way points (`d<n><mv>`) and read the meter (`eREAD?`); loop each calibrated output into an input and read the input on `v`; feed a quantizer's pitch input. Pass: each is at or better than the figure on the results screen.

- [x] PARTIAL, 2026-10-24: input 2 after `Q21` read the target within about 0.6 mV (it was 3.3 V off); against the meter it was off by output 1's own error (5 to 7.4 mV), which is why outputs are calibrated first.
- [ ] Full check after a Cal Routine save.

### 4. No-cal unit unchanged

> **No-cal unit unchanged:** a unit with no `ICA1` page and a stock calibration page behaves exactly as before (host test against an old unit's EEPROM image, as `test_cvmeter_math` does).

Steps: `cd software/test && ./run-host-tests.sh`. Pass: all suites pass.

- [ ] Final run on the release candidate.

### 5. Meter unplug mid-run

> **Meter unplug mid-run** releases every hold and saves nothing.

Steps: start a run, unplug the meter during an output. Pass: the screen says the meter was lost, every output is released, nothing is saved (`CALR,LOST`, then `CALR,FAIL`, no `CALR,SAVE`).

- [x] PASSED, 2026-10-24, driver level: unplug during a `READ?` loop gives `NOT_CONNECTED` at once for every query; the first query after a replug is DONE; no hang; module and console stayed up.
- [ ] Repeat inside a real run on the app (and unplug during the input part).

### 6. Hot-plug

> **Hot-plug:** plugging the meter into the host jack while the module runs does not drop the module off USB. It was seen once on 2026-10-23 during a replug and has not been reproduced. Pass = not seen in 10 replugs (*Claude's call*). If it does recur, it is an electrical or hub issue and is documented as a known issue ("plug the meter in before powering up"). It does not block the firmware.

- [x] PASSED, 2026-10-24: 9 replugs in all (1, then 8 more in 150 s). Every disconnect gave `NOT_CONNECTED` / `DISCONNECT` (mid-query), every reconnect gave DONE on the next query; the module never left USB and the console was continuous.

## Items found on the bench

### A. Meter driver and recovery

- [x] PASSED, 2026-10-24: driver demo. `u` lists the meter F4EC:1203; `*IDN?` gives Siglent SDM3065X fw 3.02.01.13; `CONF` and `READ?` work.
- [x] PASSED, 2026-10-24: timeout recovery. A forced timeout (`NPLC 100`, `e@200 READ?`) gave `TIMEOUT`, then two `XFER_ERR_IN`, then every query OK with no replug; `SYST:ERR?` said "No error". The flow retries a failed query at least 3 times.
- [x] PASSED, 2026-10-24: the meter through a hub works (`u`, `*IDN?`, `READ?`).

### B. Autozero

- [ ] Does the meter accept `VOLT:DC:AZ ONCE`? Steps: run the app through the first output, then `eSYST:ERR?`. Pass: `METER,DONE,+0,"No error"`. If it reports an error, note it and the flow's fallback.

### C. Real run time

- [ ] Time a whole run from start to Results (the `ms` field of `CALR,STATE` lines gives it). Model: about 6 to 7 minutes (5.5 / 6.3 / 8.0 min at 40 / 60 / 100 ms per `READ?`). Pass: within 8 minutes; record the figure and update `docs/_apps/Cal-Routine.md`.

### D. Screens on hardware

- [ ] The run screens (meter, output prompt, measuring, input prompt, measuring) read correctly, name the jacks as the panel does, and show the USB advice (meter alone on the host jack, do not touch its cable).
- [ ] The Results screen: figures per output and input, rail flags, the cursor, retry on a failed output, discard needing two presses.
- [ ] The Save confirm screen and the Saved screen (and the "not written" variant).
- [ ] Setup's DAC steps show the warning that they replace a Cal Routine calibration, and Setup's ADC steps do not change a Cal Routine input.

### E. Input map

- [x] PASSED, 2026-10-24 (adc3c3cc): panel n gives firmware n for all 8 inputs at boot and after 3 x 16 forced re-aligns at random trigger phase (`X`; count 48). After a counter reset and 16 more: core isr 16 us, missed 0, 0/11 FAIL (an earlier 5/11 was the known boot-time spike). Earlier builds with a fixed table rotated the inputs by one and were reopened; the deterministic start is confirmed on the normal build and on the 1700 ns and 3300 ns skew builds.
- [ ] Re-check on the release image: 8 outputs patched to 8 inputs (output n into input n), panel n gives firmware n for every n, and `X` (`ADC,REALIGN,count=n`; `realigns=` in `t`/`T`) leaves the mapping unchanged. Output and input 7 once reached only about 3.3 V: check that cable.

### F. Hub effect

- [x] Observed, 2026-10-24: a reading moved by +0.17 mV after a USB replug of the meter, and by 4.94 to about 4.27 mV (-0.67 mV) with a hub and a keyboard on the port. That is why the app says: meter alone, do not touch its cable.
- [ ] Calibrate once with the meter alone on the host jack and once with a hub, then compare the results (output worst figures and input offsets). Pass: the difference is recorded and is consistent with 0.2 to 0.7 mV.

### G. Release notes

- [ ] The folder layout reseeds when the app is appended to the app list. Check on a unit with a custom arrangement: after the update, the arrangement and the new app's place are as described, and say so in the release notes.
