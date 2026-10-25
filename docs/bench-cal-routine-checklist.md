# Cal Routine bench checklist

Gates 1 to 6 are copied word for word from `design/cal-routine-design.md` section 7. Under each, the steps and the pass condition used on the bench. Legend: `[x]` passed, `[~]` partly done (see the note), `[ ]` open. Items marked PASSED were done on 2026-10-24; the rest are open. Console keys are in [bench-console](bench-console); meter lines (`METER,...`) come from the console `e` key and run lines (`CALR,...`) from the app (`tools/hwqa/cvmeter.py` parses both).

Rig: Xenomorpher on the console build, Siglent SDM3065X alone on the host jack (unless the item says otherwise), outputs patched to the meter or to the inputs as the screen asks.

## Design gates

### 1. ISR budget

> **ISR budget** with every input calibrated and a heavy app running: core ISR max still <= 40 us, 0 missed ticks (`docs/Timing-Budget.md` method). Today's measured peak is 31 us (Quadrants with applets), so the per-channel multiply has room; measure it on hardware anyway.

Steps: calibrate all 8 inputs with the Cal Routine, load Quadrants with applets, run the `t` / `T` selftest for a few minutes. Pass: core isr max <= 40 us, missed 0.

- [~] PARTIAL, 2026-10-24 (input 2 only, via `Q21`): Quadrants (Pigeons + Carpeggio): core isr max 37 us, 0 missed, 0/11 FAIL; the same applets uncalibrated: 37 / 36 us, so the calibrated path's cost is not measurable. The 3 us of headroom is the applets' load (Timing-Budget recorded 31 us for Quadrants earlier). After the input-map fix (adc3c3cc): core isr 16 us, missed 0 on an idle app.
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

- [~] PARTIAL, 2026-10-24: input 2 after `Q21` read the target within about 0.6 mV (it was 3.3 V off); against the meter it was off by output 1's own error (5 to 7.4 mV), which is why outputs are calibrated first.
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

> **Hot-plug:** plugging the meter into the host jack while the module runs does not drop the module off USB. It was seen once on 2026-10-23 during a replug and has not been reproduced. Pass = not seen in 10 replugs. If it does recur, it is an electrical or hub issue and is documented as a known issue ("plug the meter in before powering up"). It does not block the firmware.

- [x] PASSED, 2026-10-24: 9 replugs in all (1, then 8 more in 150 s). Every disconnect gave `NOT_CONNECTED` / `DISCONNECT` (mid-query), every reconnect gave DONE on the next query; the module never left USB and the console was continuous.

## Items found on the bench

### A. Meter driver and recovery

- [x] PASSED, 2026-10-24: driver demo. `u` lists the meter F4EC:1203; `*IDN?` gives Siglent SDM3065X fw 3.02.01.13; `CONF` and `READ?` work.
- [x] PASSED, 2026-10-24: timeout recovery. A forced timeout (`NPLC 100`, `e@200 READ?`) gave `TIMEOUT`, then two `XFER_ERR_IN`, then every query OK with no replug; `SYST:ERR?` said "No error". The flow retries a failed query at least 3 times.
- [x] PASSED, 2026-10-24: the meter through a hub works (`u`, `*IDN?`, `READ?`).

### B. Autozero

- [ ] Does the meter accept the setup (`VOLT:DC:NPLC 10`, `VOLT:DC:AZ ON`)? Steps: start the app, then `eSYST:ERR?`. Pass: `METER,DONE,+0,"No error"` (bench 2026-10-24: `0,"No error"`). (The flow asks `SYST:ERR?` itself after the setup only, and stops with `CALR,MERR,...` then `CALR,FAIL,METER_CONFIG` on anything but an empty queue. There is no per-output re-zero: autozero is on for every reading.)

### C. Real run time

- [ ] Time each routine from start to Results (the `ms` field of `CALR,STATE` lines gives it). Model (`test_run_time_model`): **Outputs about 22 minutes** (19 / 22 / 27 min at 450 / 530 / 650 ms per `READ?`; each measurement is a settle, one throwaway and 3 readings, a node's final value 10; 20 verify points per output), **Inputs about 25 seconds** of measuring (no meter) plus the patching, **Self about 22 minutes**. Pass: Outputs and Self within 25 minutes, Inputs within 1 minute; record the figures and update `docs/_apps/Cal-Routine.md`.

### D. Screens on hardware

- [ ] The menu shows Outputs, Inputs, Self (both); either encoder moves it and the hint line follows.
- [ ] The run screens (meter, output prompt, measuring, patch prompt, patch check, input sweep) read correctly, name the jacks as the panel does, and show the USB advice (meter alone on the host jack, do not touch its cable) for Outputs and Self only.
- [ ] Inputs on a unit without the Cal Routine marker shows the warning; R goes on, L cancels with nothing held.
- [ ] Patch check: swap two cables (e.g. into inputs 3 and 5): the screen names both ("C reaches 5, not 3", "E reaches 3, not 5") and A does nothing; fix and R checks again. Pull one cable: "D: input 4 silent", A goes on without it and input 4 is listed as skipped. Every output is released on the fail screen. In Self, B on the fail screen goes to Results with the outputs only, and Save writes only the output tables.
- [ ] The Results screen: figures per output and input, rail flags, the cursor, retry on a failed output, discard needing two presses.
- [ ] The Save confirm screen and the Saved screen (and the "not written" variant).
- [ ] Setup's DAC steps show the warning that they replace a Cal Routine calibration, and Setup's ADC steps do not change a Cal Routine input.

### E. Input map

- [x] PASSED, 2026-10-24 (adc3c3cc): panel n gives firmware n for all 8 inputs at boot and after 3 x 16 forced re-aligns at random trigger phase (`X`; count 48). After a counter reset and 16 more: core isr 16 us, missed 0, 0/11 FAIL (an earlier 5/11 was the known boot-time spike). Earlier builds with a fixed table rotated the inputs by one and were reopened; the deterministic start is confirmed on the normal build and on the 1700 ns and 3300 ns skew builds.
- [ ] Re-check on the release image: 8 outputs patched to 8 inputs (output n into input n), panel n gives firmware n for every n (Cal Routine's Inputs patch check shows on every run that input n is reached by output n, `CALR,PRES,PASS,00,00`: the input map relative to the output map, so check one input against a known source as well), and `X` (`ADC,REALIGN,count=n`; `realigns=` in `t`/`T`) leaves the mapping unchanged. Output and input 7 once reached only about 3.3 V: check that cable.

### F. Hub effect

- [x] Observed, 2026-10-24: a reading moved by +0.17 mV after a USB replug of the meter, and by 4.94 to about 4.27 mV (-0.67 mV) with a hub and a keyboard on the port. That is why the app says: meter alone, do not touch its cable.
- [ ] Calibrate once with the meter alone on the host jack and once with a hub, then compare the results (output worst figures and input offsets). Pass: the difference is recorded and is consistent with 0.2 to 0.7 mV.

### G. Release notes

- [ ] The folder layout reseeds when the app is appended to the app list. Check on a unit with a custom arrangement: after the update, the arrangement and the new app's place are as described, and say so in the release notes.

## 259e target (experimental; the write is DISABLED in the firmware)

**State of the evidence.** The measurement half has been bench-run on a real 259e (600, 1200 and 3000 mV points within 0.4 mV of the meter; second input fit worst 1.83 cents, nonlinearity 1.08 cents; gain 0x8597 against stock 0x9041, about 7.5 % below stock on both inputs). One record was committed to that bench 259e (one of its 8 lifetime slots is used) and the module reported it valid (status `05`, fmt 1, seq 1), but the read-back differed. That mismatch was traced to a bug in the 259e firmware 31.2C4's reply buffer (below), not to a bad write; it is fixed in the 259e release candidate 31.2C5, which is **host-tested only**. The Xeno-side read workaround for 31.2C4 is **not yet proven on hardware**, and the fit's scale (4096 counts per octave) is **unproven**. The **write path is EXPERIMENTAL**. It is **DISABLED** in the shipped firmware (`Cal259eFlow::kAllowWrite`): a 259e run is measuring-only and **ends with a discard at Results (L L)**. It is re-enabled (a build with `-DCAL259E_ALLOW_WRITE=1`) only after (1) the read-back is proven correct on hardware and (2) the target scale is proven by gate X3 below. A build with the write compiled in prints `WARNING: 259e write ENABLED in this build` in the `t` self-test.

**Slots.** Each 259e accepts only **8 commits for its whole life** (compaction is disabled). Do not spend slots on checklist tests. The "8 slots full" case below is run in the **8051 emulator, never on the bench 259e**.

**The read-back quirk (found on the first run).** 31.2C4 sends every 8-byte record reply with data byte 5 as 00 and data byte 7 as 0C: its reply buffer (XDATA 0x067A..) overlaps the stock send routine's scratch (0x8D22 clears 0x0684 before it sends; 0x8815 keeps its byte index in 0x0686, so it reads 12 while that byte goes out). Pre-fix, a stored record read back wrong at record offsets 7, 13, 15, 21, 23, 27, 29 exactly as seen. The Xenomorpher now reads the record with seven overlapping READs (args 0, 5, 10, 15, 20, 21, 22) using only data bytes 0 to 4 and 6; the last byte (CRC low) is inferred from the CRC of the first 28. **Not yet confirmed on hardware**: that the fixed read returns the record the module holds (check `=28r` against a logged `CALR,259REC`).

The "259e pitch inputs" row of the app (`Cal259eFlow`, host-tested) and the console `=` command (`=28s`, `=28r`, `=28a0`, `=28a1`, read-only). Design: Buchla_FW `docs/superpowers/specs/2026-10-24-cv-calibration-xeno-coordinator.md`; frames and record: Buchla_FW `docs/cv-calibration/CONTRACT.md`. The gates X2, X3 and X4 are the CV-calibration plan's. Apart from the single run described under State of the evidence, nothing in this section has been run on the bench.

**Only output A is ever driven.** There is no output B in this flow. The operator moves the one cable between the 259e's principal pitch input and its mod pitch input when the screen prompts (principal first). The meter's + stays on the jack the cable is in; its COM goes on the 259e's ground near that jack.

### X2. Read-only (no PUT, no COMMIT)

Rig: the T41_console build (check its RAM2 "free for malloc" against an armed FWCARD first: the Xeno mallocs the whole card at boot, and too little heap boot-loops), the 259e on 31.2C4, output A patched to the principal pitch input; the meter is not needed. Steps: `=28s` (flags, fmt=1, seq come back); `=28a0` while stepping `d` on output A (1200, 4800, 9600 mV): the raw word rises and `n` advances; the same with `a1` on the mod pitch input; `=28r` once.

- [ ] `=28s` answers.
- [ ] `=28a0` rises with output A, `n` advances (principal pitch).
- [ ] `=28a1` rises with output A, `n` advances (mod pitch).
- [ ] `=28r` shows the record state (record or `REC,NONE`); with a record, its bytes 0 to 28 equal the last `CALR,259REC` line and the console line's `inferred_crc_lo=` equals that line's byte 29 (byte 29, the CRC low byte, is inferred from the CRC of the first 28, not read from the module, so "CRC-valid" for the 30 bytes holds by construction for byte 29; the check that the overlapping read works on a real module is bytes 0 to 28).
- [ ] A measuring-only run (Cal Routine, 259e): Results shows "Write disabled: scale unproven", encR and B do nothing, `L L` discards, and `=28s` seq is unchanged.

### Before the first write: the octave-ratio proof (gate X3)

The record's scale (4096 counts per octave at the corrected code) is inferred, not yet proven on a unit. Gate X3 proves it with the frequency counter: the 259e's oscillator output into a Xeno input (`n`, the console frequency counter) at held 1200, 2400 and 4800 mV on the principal pitch input; both ratios must be 2.000 within 0.003. **Run it BEFORE the first write if the setup allows it**: the unit's stock correction (no record yet) is close enough to show a scale that is wrong by a factor, though not to prove 0.003; if it cannot be run first, it is a pre-write requirement that the operator knowingly accepts the first write as the proof, and the run after it is the check.

- [ ] Octave ratios 1200 to 2400 and 2400 to 4800 mV, before the first write (or: waived by the operator for the first write, noted here).
- [ ] The same after the first write: both ratios 2.000 +- 0.003.

### Write session (an explicit write approval, with the operator present)

All of these, before B is held on the write screen (only in a build with the write enabled; the shipped firmware stops at Results):

- [ ] The `T41_console` build **built with the write enabled** (`PLATFORMIO_BUILD_FLAGS=-DCAL259E_ALLOW_WRITE=1 pio run -e T41_console`) is on the Xenomorpher (it has `=` and the `CALR` lines), flashed with `Qp` and the image hash recorded before and after.
- [ ] The Xenomorpher was **rebooted with the firmware card DISARMED and not serving** (`M` toggles the arming for the *next* boot; the Xeno keeps serving until a reboot, a console `k`, or a complete card read plus 30 s of bus quiet; the app refuses the backup while it serves: "Firmware card served"). The 259e's own firmware (31.2C4) is already loaded.
- [ ] No console `W`, `m`, `x`, `=`, `w`, `k`, `e` or `p` used during the session, and **never `pew!` sent to a console that may be unlocked** (`p` toggles the overlay, `e` starts a bench-DMM SCPI line that swallows following input and sends it to the meter, `w` patches a card byte); probe with `t` first (the app and the console gate each other while a run is open, but a typed `x` before the run is not undone).
- [ ] Bus Buccaneer is closed, and the 200e app is not in use on the Xenomorpher (it shares the master state machine and the card image). Bus Buccaneer currently shows only "error 10" when the Xenomorpher refuses a request because the firmware card is being served; it does not show a distinct reason, so "error 10" there means: reboot with the card disarmed.
- [ ] The octave-ratio proof (X3, above) was done first.
- [ ] The 259e was **power-cycled just before the run** (its status bits 2 and 3 describe the last commit since power-up; a fresh start makes them mean something) and has run for 30 minutes.
- [ ] A fresh dated backup of the Xenomorpher's calibration exists in a calibration backups folder, and the 259e's presets are what the run backs up to `CAL259BK.BIN` itself.
- [ ] The Siglent alone on the host jack, set up by the app (NPLC 10, autozero on).

### X3/X4 and the first calibration

- [ ] Cal Routine, 259e: warm-up confirmed, principal pitch then mod pitch patched when prompted; Results show each input: worst check error <= 3 cents, nonlinearity <= 5 cents (**the 5 cent limit `kMaxNonlinearityCents` is provisional: confirm it from the real figures**), the ADC span / offset / channel check passes (gain within ±1.5 % of T × 65536 / 7864.32, offset −50 to 150, the two gains within 1 %; it cannot detect a wrong counts-per-octave constant, so a pass says nothing about T); the age on the Results is under 10 minutes.
- [ ] On the write screen: a B tap does nothing and the screen says "Hold B to write"; **hold B 2 s** (not a chord; the bar fills, release early empties it). Expect `CALR,259BK,<addr>,990,<crc>,OK`, then `CALR,259REC,...`, then `CALR,259END,OK`; the Done screen. Keep the `CALR,259REC` line.
- [ ] If the run ends "COMMIT unknown", "Written: UNKNOWN" or "module refused": do nothing else; read `=28s` and `=28r` by hand. Never retry the write blindly (COMMIT is not idempotent).
- [ ] Afterwards: the 259e power-cycled, `=28s` bit0 (record valid) set, seq one more than before; a re-run gives seq + 1; a reload of the 31.2C4 card keeps the record.
- [ ] 8 slots full: the run ends "all 8 calibration slots are used; compaction is disabled" and writes nothing (compaction is switched off). **Run this in the 8051 emulator, never on the bench 259e** (each 259e takes only 8 commits for its whole life; this test would need them all).
- [ ] X4: a full-case soak with the calibration READs on the bus, banks byte-identical before and after.
- [ ] Cancel after the COMMIT went out (left encoder, or switch app): the Xenomorpher still logs `CALR,259END,CANCELLED_<result>` and the Cal Routine menu shows "259e cancel: ...".

## 251e target (experimental; the write is DISABLED in the firmware)

**State of the evidence.** None: nothing here has run on a 251e. The target is host-tested against a simulated 251e and the simulator. Design, limits and open questions: `docs/specs/cal-251e.md`. The write (`Cal251eFlow::kAllowWrite`) is compiled in only by `-DCAL251E_ALLOW_WRITE=1`; CI refuses that define in any shipped build file, and a build with it prints `WARNING: 251e write ENABLED in this build` in the `t` self-test. A measuring-only run is discarded at Results (L L) and is safe to repeat.

The 251e has no slot count: its one record page is erased and rewritten by every COMMIT (a power cut during that leaves the module on its stock path). The flow logs the previous record (`CALR,251OLD`) before any write.

### H1. Loopback alone (no 251e)

Rig: the T41_console build, the Outputs and Inputs calibrations done (`Qp` shows `verify_mv` about 0.5 mV), four cables, no 251e. Run the 251e row as far as the loopback and stop.

- [ ] Outputs E, F, G, H into inputs 1 to 4: all 20 `CALR,251LOOP` readings within 2 mV of the commanded voltage. Record the worst error and the spread (this is the real chain error and noise; the design budget assumes about 1.3 mV combined).
- [ ] An unpatched input stops the run with a PATCH message; a 3 mV gain error on one input (a wrong cable to an attenuated source) stops it with LOOPBACK.
- [ ] Run it twice, five minutes apart: the worst error moves by less than 1 mV (thermal).

### H2. 251e status and measuring-only run

Rig: a 251e on 30.7C4 on the bus (its bench gates in Buchla_FW `docs/cv-calibration/BENCH.md` come first), sequencers stopped, `=5Cs` answers (`fmt` 1).

- [ ] `=5Cs` answers; flags show whether a record is held.
- [ ] A full measuring-only run reaches Results. Keep the `CALR,251LINE`/`PT`/`FIT` lines: the measured mV per DAC code `a` and the zero `b` per channel are the **first measurement of the 251e's analog scale** (design: about 2.610 mV per code, 10.00 V at code 3831). Note which check, if any, failed.
- [ ] Status bit1 (override active) shows during the run (`SCALE`, `NO_OVERRIDE` and `IDENTITY` tell the story if not).
- [ ] After Results, left encoder twice: the outputs return to the sequencer; `=5Cs` shows the same flags and sequence number as before (nothing written).
- [ ] Cancel at every stage (L): outputs return within 5 s at the latest (normally at once); the Xenomorpher's outputs E to H are released.
- [ ] Unplug the Xenomorpher's bus cable mid-measure: the 251e's outputs return to the sequencer 5 s after the last override.
- [ ] Repeat the run three times: gains agree within 2 Q2.14 steps, offsets within 1 code.

### H3. Limits

- [ ] From H1 and H2 set (and write into `docs/specs/cal-251e.md`) the write limits: loopback, non-linearity (placeholder 3 mV), verify (4 mV), anchors (8 mV), scale window (4 %). A measuring-only run only WARNs on the placeholders; read them from the `CALR,251LINE` (bow, INL), `251STAT` (noise, DAC step, verify, anchor) and `251WARN` lines.
- [ ] The firmware under test is 30.7C4 or 30.7C5: note which, and whether `=5Cs` shows the commit bits clearing during a COMMIT (C5) at H4.

### H4. First write (only in a build with the write enabled, with the owner present)

- [ ] H1 to H3 done; open questions 1, 2 and 3 of the design answered.
- [ ] The T41_console build with `-DCAL251E_ALLOW_WRITE=1`, flashed with `Qp` and the image hash recorded; the 251e power-cycled and warm.
- [ ] Hold B for 2 s: expect `CALR,251OLD,...` (the previous record), `CALR,251REC,5C,...`, `CALR,251COMMIT,<count>,<seq>`, `CALR,251END,OK` (at most 20 COMMITs per power-up); `=5Cs` shows flags 05 and the new sequence number after a power cycle.
- [ ] Never retry a COMMIT blindly: after COMMIT unknown or REFUSED read `=5Cs` by hand.

### H5. End-to-end proof after a write

- [ ] Run a 251e sequence of stages at known volts (1.2 V steps) into the Xenomorpher's inputs 1 to 4; every stage within 4 mV of its displayed volts. The bus override path cannot measure the corrected output, so this is the only proof of the stored correction.
