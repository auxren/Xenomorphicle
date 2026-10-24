# Cal Routine: closed-loop self-calibration with a USB bench DMM — design

Date: 2026-10-23. Status: draft for Oren's approval (builder check done; its questions are settled in the text).
Paths are relative to `software/src/` unless they start with `docs/`, `tools/` or `software/`.
"*Claude's call*" marks a decision Oren did not make; everything else is his.

## 1. Intent

Calibrating a Xenomorpher by hand today means Setup / Calibrate: dial each output until a meter reads
0 V and 9.6 V. Only those two of the 11 table points per output are measured; the rest are
interpolated or extrapolated. The inputs get one shared scale and offset, and the per-input
calibration from the `Q` console command feeds the console meter only.

Oren wants a closed loop. A USB bench DMM on the Xenomorpher's USB host jack: the module tells the
owner what to plug where, measures, and adjusts its own calibration so that what the meter reads is
what was intended. Then it calibrates its inputs against those now-accurate outputs. **Once
calibrated, all apps use the calibration, so pitch tracking in and out is as close to perfect as the
hardware allows.**

First meter: Siglent SDM3065X (verified 2026-10-23: VID 0xF4EC, PID 0x1203, USB high speed, one
interface class 0xFE / subclass 0x03 / protocol 0x01 = USBTMC-USB488, 3 endpoints: bulk OUT, bulk IN,
interrupt IN). Oren has used it on this bench before; ground isolation is not a concern.

## 2. Goals

1. **A system app, "Cal Routine"**, in the system folder. It opens on a list of things to calibrate.
   Today the list has one entry, **Self**. Later entries (bus modules such as the 251e and 259e) are
   added by later work and are not shown until they exist.
2. **Self, part 1 — outputs:** for each of the 8 outputs in turn the screen says which output to plug
   into the meter; the module then sets **all 11 points** of that output's DAC table (0, 1.2, … 12 V)
   by measuring with the meter and adjusting the code until the meter reads the target.
3. **Self, part 2 — inputs:** the screen says which output to patch into which input; the module
   steps that output through known voltages and fits the input to them, one input at a time.
4. **Results and save:** a summary of the residual error per output and per input (mV and cents at
   1.2 V/oct). Save or discard. Nothing is stored until save.
5. **All apps use it:** after save, every app's pitch output uses the new DAC tables (already true of
   the table) and every app's pitch input uses the per-input calibration.

## 3. Non-goals

- Calibrating other modules (251e, 259e) from Cal Routine. Later work; the menu is built so they
  can be added.
- Bus Buccaneer / SysEx control of the meter. Later work.
- Replacing Setup / Calibrate. It stays as the manual fallback for owners with no USB meter.
- Correcting input nonlinearity. Inputs get gain + offset; the extra points measure the
  nonlinearity and report it.
- Meters other than USBTMC on the host jack (no serial/RS-232, no LAN from the module).

## 4. Anti-goals (failure even if it "works")

*Claude's calls — Oren did not answer this question; check these.*
- Losing or shifting existing calibration, app data, presets or any EEPROM page. No layout moves.
- An output left at a test voltage after cancel, an error, an app switch or a meter unplug.
- Apps tracking worse than today when no Cal Routine calibration is stored, or when what is stored
  is invalid: they must behave exactly as today.
- Stalling the core ISR past its budget, dropping audio or USB-MIDI while the routine is idle, or
  slowing the main loop past its ceiling (`docs/Timing-Budget.md`: core ISR ≤ 40 µs of 60 µs; loop
  pass ≤ 5 ms).
- Sending the meter anything that changes its calibration or system settings.

## 5. Constraints (from the code, 2026-10-23)

- **App roster:** apps are `AppBase` subclasses listed in `app_container` (`apps/_config.h:92-183`);
  `kMaxApps = 32` (`OC_app_folders.h:121`) and the maximal roster is at 31, so Cal Routine takes the
  last slot. Append it last (`DEFAULT_APP_INDEX` is positional). Folder via `kDefaults`
  (`OC_app_folders.h:99-108`) → `FOLDER_SYSTEM`. Template: `apps/Backup.h`. Built in every env
  (*Claude's call*: no feature flag; owners need it in `T41_audio`). Also added to the
  `tools/xeno-sim/Makefile` app list.
- **DAC table:** `calibrated_octaves[8][11]` (`OC_DAC.h:~104-110`), 16-bit codes, unipolar 0–12 V on
  the Xenomorpher (`kOctaveZero = 0`), entry k = k × 1.2 V. Apps convert pitch piecewise-linearly
  between adjacent entries (`DAC::PitchToScaledDAC`, `OC_DAC.h:209-232`), so every measured entry
  counts. `GateToDAC` uses entry 9 for gate high. Entries must stay monotone and in 0–65535.
  `DAC::Hold(ch, code)` / `Release` / `ReleaseAll` pin a physical channel over whatever the app
  writes (`OC_DAC.h:~148-165`); panel ↔ physical mapping as in `CvMeter.cpp:36-60`.
- **Autotune tables** shadow the base table on a channel set to "Auto" (`OC_DAC.h:~222`) and are RAM
  only.
- **ADC pitch path:** every app's CV comes through `IO::Read` (`OC_io.cpp:51-72`) in the 60 µs core
  ISR: `ADC::value(ch)` (shared per-channel offset, `OC_ADC.h:~83`) → `ADC::value_to_pitch` (one
  shared `pitch_cv_scale`, `OC_ADC.h:~108`) → `Attenuate`. Some apps read `ADC::value` directly
  (H1200, QQ, DQ, ASR, Sequins, Scope, Sampler, Calibr8or comment; list in §6.6).
- **Storage:** EEPROM is 4284 bytes and nearly full. The calibration page (`[0,224)`, FOURCC `CAL1`)
  fills its region exactly and must not change layout. The per-input calibration has its own
  versioned page `ICA1` (`[4164,4276)`, `CvMeterMath::InputCalibration`: float gain[8], float
  offset_mv[8], int16 verify_mv10[8], uint8 source_output[8]; 4 bytes spare). The DAC table needs no new
  storage: all 88 entries already live in the calibration page.
- **USB host:** `USBHost thisUSB; USBHub hub1; MIDIDevice_BigBuffer usbHostMIDI[2]` in DTCM
  (`Main.cpp:60-72`; EHCI buffers get no cache maintenance, so DTCM). `thisUSB.Task()` once per
  `loop()` (`Main.cpp:1026`). No USBTMC driver exists in the library or the repo.
- **Existing closed-loop code:** the `Q` flow (`CvMeter.cpp:~246-445`, settle 300 ms / measure
  600 ms, verdicts in `CvMeterMath.cpp`) is compiled only with `PRINT_DEBUG` (`CvMeter.h:18`).
  `CvMeterMath` is hardware-free and host-tested.
- **Saving the calibration page** blocks ~1 s with its own screen (`OC::calibration_save()`,
  `OC_calibration.cpp:~219`).

## 6. Decided design

### 6.1 Meter driver (USBTMC)
A `USBDriver` subclass on the existing host and hub, claiming any interface with class 0xFE,
subclass 0x03 (*Claude's call*: any USBTMC meter, not just the Siglent; the Siglent is the one tested
and named on screen from `*IDN?`). Bulk OUT + bulk IN only; the interrupt endpoint and USB488 extras
are unused. Object and buffers in DTCM next to `usbHostMIDI`. Pipe callbacks run in USB interrupt
context and only move bytes; everything else runs from `loop()` as a non-blocking state machine, so
no loop pass waits on the meter. Request/reply: `DEV_DEP_MSG_OUT` with the SCPI line, then
`REQUEST_DEV_DEP_MSG_IN`; bTag increments per transfer; a timeout aborts the transfer and the step.
The console's `u` also lists a claimed meter.

**Allowlist** (in the driver's API, not the app): only the SCPI the routine needs — `*IDN?`, `*CLS`,
`CONF:VOLT:DC <range>`, `VOLT:DC:NPLC <n>`, `VOLT:DC:AZ`, `TRIG:SOUR IMM`, `READ?`, `SYST:ERR?`
(read-only). Anything else, and anything starting `CAL` or any other `SYST:` form, is refused
before it reaches the bus.

### 6.2 Cal Routine app
Menu list → Self. Self is a step machine run from `Loop()`:
1. **Meter check.** No meter → "Plug a USB meter into the host jack" and nothing else. Meter found →
   `*IDN?` shown, meter configured: DC volts, fixed 20 V range, NPLC chosen by the builder for
   ≈ 0.1 mV noise, autozero on.
2. **Outputs A → H.** "Plug output A into the meter (+ to A, COM to ground)". Continue. The output is
   held by `DAC::Hold` only while its step runs; the others go back to the running app.
3. **Inputs 1 → 8.** "Patch output A into input 1" (*Claude's call*: one calibrated output, output A,
   is the source for every input, and the meter is not needed for this part). Continue.
4. **Results.** Worst residual per output and per input, in mV and cents; any failed check named.
   Save / Discard (two-step confirm, `Backup.h` style). Save writes the calibration page
   (`calibration_save()`), then the `ICA1` page.

**Pass criteria and save policy** (*Claude's calls*). At 1.2 V/oct, 1 mV = 1 cent. An output
passes when every verify residual is ≤ 1 mV. The iteration aims for ≈ one DAC code (≈ 0.2 mV) and
reports what it reached, so a unit limited by its own DAC noise or INL reports its figure rather
than failing below 1 mV. An input passes on the `Q` verdicts (residual ≤ 10 mV today; the builder
may tighten it from bench data, bounded below by ADC noise at ≈ 2.93 mV per count). Save is offered
when **all 8 outputs pass**. A failed or skipped input is saved as "not calibrated", so it stays on
the shared scale exactly as today, and is named on the results screen. A 12 V entry flagged at the
rail does not block save. Write order: the calibration page first, then `ICA1`. Each page checks
itself, so a power cut between the two leaves new outputs and old inputs. That state is consistent
(the inputs keep their previous calibration or the shared scale), so the pair does not need to be
atomic.

Cancel at any step (encoder L, as elsewhere) releases every hold and leaves stored data untouched.
Suspend, screensaver, app switch, meter unplug or error do the same. A completed part 1 is not kept
if part 2 is cancelled (*Claude's call*: one save at the end, as Oren described; the builder may add
"save outputs only" if part 2 fails).

### 6.3 Output calibration (per output, per entry k = 0…10, target k × 1.2 V)
Start from the stored code, measure, correct using the local slope (neighbouring entries, or the
nominal ≈ 5461 codes/V for the first), re-measure; stop when within the builder's tolerance (target
≤ 0.2 mV, about one code) or after a few iterations. Each measurement: settle, several readings,
median and spread; a spread above a threshold fails the point as a loose lead. Checks before the
table is accepted: monotone across the 11 entries; no entry pinned at 0 or 65535 (an entry the DAC
cannot reach — possibly 12 V — is flagged, kept at the rail and reported, not silently clipped:
*Claude's call*); slope within a sane window of nominal. **Verify pass:** measure the half-way
points (0.6, 1.8, … 11.4 V) through the normal pitch-to-code path and report the residual as the
output's figure. Expected run time is a few minutes for all 8 outputs (*Claude's estimate*).

On save, any valid autotune table is reset (`autotune_calibration_data.Reset()`) so the new base
table takes effect on every channel (*Claude's call*; the tables are RAM-only anyway).

### 6.4 Input calibration (per input, against the calibrated output)
The inputs are unipolar: each is a 0–4095 window with 0 V at the rail (raw 4095) and roughly 12 V at
the other end, ≈ 2.93 mV per count (`docs/CV-Meter.md`, "Input range"). Nothing below 0 V can be
read, so the 0–12 V source covers the whole usable span. The source output steps through about 5
voltages inside the overlap of the output's 0–12 V and the input's `InputRange`, kept clear of both
rails (lowest point ≥ ≈ 0.6 V, since 0 V is the rail; builder's choice), settle + average as the `Q`
flow does. The meter is not in the loop for part 2 (Oren: "closed loop calibrate them to the
calibrated output voltages"); a patch cable into a high-impedance input adds no measurable error, and
the NOISY / FLAT / SIGN verdicts catch a bad patch. **Least-squares
gain + offset** over the points (*Claude's call*: uses every point, unlike `Q`'s 2-fit + 1-verify),
with the per-point residual as the nonlinearity figure. The `Q` flow's verdicts (RAIL, NOISY, FLAT,
SIGN, GAIN, RESIDUAL) apply. Stored in the existing `ICA1` page format (gain, offset_mv, verify,
source output), so no storage change; `source_output` records the output used.

The flow logic and the maths move out of the `PRINT_DEBUG`-only `CvMeter` into code compiled in
every env (`CvMeterMath`-style, hardware-free and host-tested); the console `Q` command keeps
working on top of it.

### 6.5 All apps use the input calibration
At load and after save, each valid `ICA1` channel is turned into a small **integer** per-channel
pitch scale and offset held in DTCM (the floats stay out of the ISR). In `IO::Read`, a calibrated
channel's `pitch_values` comes from that scale + offset in place of `value_to_pitch` with the shared
scale; uncalibrated or invalid channels run the existing code unchanged. Cost: one multiply and add
per calibrated channel per tick.

*Claude's call:* `cv.values` (the raw-ish CV that apps use for modulation and thresholds) stays as
today. Only pitch changes. Changing `values` would move every app's thresholds by a few percent for
no pitch benefit. Gates derived from `pitch_values` (`HSIOFrame.cpp:775`) move by at most the
calibration correction; the builder checks that this stays within the gate hysteresis.

### 6.6 Direct readers
Apps that read `ADC::value` / `value_to_pitch` directly for **pitch** (H1200 `:1152`, QQ `:1575`,
DQ `:1461`, ASR `:191`, Calibr8or's channel meter) move onto one shared accessor that applies the
same per-channel calibration. Raw-signal readers (Scope, Sampler, Sequins thresholds, random seed)
stay as they are. The builder confirms each against its use.

### 6.7 Setup / Calibrate coexistence
Setup's DAC steps rewrite entries 0 and 8 and re-interpolate the rest, which would wipe a Cal Routine
table. *Claude's call:* when a Cal Routine calibration is stored, Setup's DAC and ADC steps show a
one-line warning that they will replace it. Setup's ADC steps continue to write the shared
offset/scale, which uncalibrated inputs use; a channel's `ICA1` calibration takes precedence. The
calibration page gets no new field, so "a Cal Routine calibration is stored" is the `ICA1` page's
`valid_mask`, plus a marker the builder chooses outside the full calibration page (the 24-byte gap
at `[4140,4164)` or the 4 spare bytes in `ICA1`).

### 6.8 Console and host tooling
`T41_console` prints tagged lines for each step and point (`CALR,…`, in the `docs/CV-Meter.md`
style, with golden fixtures), so a run can be logged. `tools/hwqa` gains a parser and a simulated
meter for the end-to-end test. *Claude's call.*

## 7. Gates

1. **ISR budget** with every input calibrated and a heavy app running: core ISR max still ≤ 40 µs,
   0 missed ticks (`docs/Timing-Budget.md` method). Today's measured peak is 31 µs (Quadrants with
   applets), so the per-channel multiply has room; measure it on hardware anyway.
2. **Loop budget** during a run: loop pass max ≤ 5 ms; USB-MIDI on the host port still works with the
   meter on the hub.
3. **Tracking check** on hardware after save: outputs at the half-way points against the meter, and
   a calibrated output looped into each input read back in the meter path, both at or better than
   the results screen; one app's pitch input (e.g. a quantizer) tracks within the reported figure.
4. **No-cal unit unchanged:** a unit with no `ICA1` page and a stock calibration page behaves exactly
   as before (host test against an old unit's EEPROM image, as `test_cvmeter_math` does).
5. **Meter unplug mid-run** releases every hold and saves nothing.
6. **Hot-plug:** plugging the meter into the host jack while the module runs does not drop the
   module off USB. It was seen once on 2026-10-23 during a replug and has not been reproduced. Pass =
   not seen in 10 replugs (*Claude's call*). If it does recur, it is an electrical or hub issue and
   is documented as a known issue ("plug the meter in before powering up"). It does not block the
   firmware.

## 8. Left to the builder

SCPI sequence details, NPLC, settle times and reading counts; iteration tolerance and limits; exact
input test voltages; the integer format of the per-channel pitch scale; screen layout and wording
(in the `docs/ui-redesign/Established-Rules.md` rules); the marker location in §6.7; test structure
(pure maths and USBTMC framing in host-tested files, a simulated meter); console line formats;
plan phasing (*suggested*: driver + console `m` test → output cal → input cal → apps switch-over →
app UI polish).
