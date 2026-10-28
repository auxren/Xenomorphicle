# Cal Routine target: Buchla 251e CV outputs (design)

Status: design plus host code. **Nothing here has run on hardware.** The write is compiled out by
default (`CAL251E_ALLOW_WRITE`, same rule as the 259e). The first bench runs are measuring-only and
end with a discard.

## 1. What this is

A Cal Routine row that calibrates the four CV outputs of a Buchla 251e with **no bench meter**. The
Xenomorpher drives the 251e's calibration protocol over the 200e bus and reads the 251e's outputs on
its own CV inputs 1 to 4, which Cal Routine has already calibrated. Before it trusts those inputs it
checks them against its own outputs (the loopback, section 5).

Not in scope: the 251e's pulse outputs, its CV inputs (it has none that matter here), and any change to
the Xenomorpher's own calibration (the target never saves anything of the Xeno's; section 9).

## 2. What the 251e's calibration record is

Protocol facts, in our words (frames and record layout are the shared contract in the Buchla_FW repo,
`docs/cv-calibration/CONTRACT.md`, section "251e behaviour"; firmware 30.7C4).

* One record of 38 bytes: "CL", format 1, module code 0x51, 4 channels, 8-byte unit ID, a 16-bit
  sequence number, a 32-bit date, then per channel a gain (u16) and an offset (s16), then a CRC-16
  (CCITT-FALSE) over the first 36 bytes. All fields big-endian.
* Per channel (CV outputs 1 to 4 = channels 0 to 3) the module computes
  `out = clamp(((code * gain) >> 14) + offset, 0, 4095)`. `code` is the 12-bit DAC code the stock
  sequencer path already computed from the stage's volts and cents; `out` is what goes to the DAC.
  Gain is Q2.14 (0x4000 is unity), offset is in DAC codes. The shift floors.
* The DAC is unipolar: codes 0 to 4095. A negative offset cannot lower an output below its natural
  zero, so a channel whose analog zero sits above 0 V keeps that floor (section 7, `b` limit).
* The module acts only on frames with destination 0x00, source 0x22, its own bus address (0x5C) and
  magic 0xCA. Commands used: OVERRIDE 0x10, RELEASE 0x11, PUT 0x12, COMMIT 0x0B, READ 0x18.
  * OVERRIDE `ch, code_hi, code_lo` holds channel 0 to 3 at a **raw** DAC code. The code is sent to the
    DAC as given: **it bypasses the correction.** Any accepted OVERRIDE restarts a 5 s timer for all
    overrides; when it runs out every channel returns to the sequencer. RELEASE ends them at once.
    Neither gets a reply. Consequence: the run measures the analog stage's raw response, whatever
    record the module holds, but it cannot measure the *corrected* path end to end through the bus.
  * PUT fills a 40-byte RAM staging buffer, at most 8 bytes per frame. COMMIT names a length (38)
    and a CRC; the module checks the staged bytes against both, then erases and rewrites the record
    page, reads it back and reloads. PUT and COMMIT are never answered: only READ gets a reply.
  * READ of the record returns 8 bytes starting at a given offset (nothing if there is no valid
    record or the offset is past 30). READ of the status returns a flags byte, a format number (1)
    and the active record's sequence number. Flags: bit0 a valid record, bit1 an override is
    active, bit2 the last commit succeeded, bit3 it was refused. The 259e's ADC read is not used.
    A READ can take a few ms to be answered.
* **Firmware.** Supported: 30.7C4 and 30.7C5, C5 preferred. They differ in one thing the client
  cares about. C4 leaves bits 2 and 3 at their previous value until a COMMIT ends, so a stale ok or
  refused bit can be read (also after a COMMIT frame that was lost). C5 clears both the moment a
  COMMIT is accepted, so flags without bits 2 and 3 mean "still running", and sets them when it ends
  (and C5 re-sends the outputs after a reload, C4 only at the next change). The client is written for
  both: COMMIT is never resent; ok is believed only with bit0 and the sequence number of the record
  put; a refused bit is believed only if the status read just before the COMMIT did not already show
  it; anything else keeps polling until the window ends and the verdict is "unknown".
* **Slots.** There are none to exhaust: one record page that every COMMIT erases and rewrites. A
  retried COMMIT of the same record just rewrites it. The limits are flash endurance of that page
  (open question 3) and a power cut during the rewrite, which leaves a torn page; the module then
  runs stock until the next commit. Hence the previous record is logged before every write (section 8)
  and the previous calibration is lost in that case.
* Whether a firmware reload erases the record page is not known (a Buchla_FW bench gate decides it).

## 3. What physical quantity is measured, and the scale

**Measured:** output voltage at the jack against the raw DAC code held, per channel, at 13 codes.
**Wanted:** a (gain, offset) such that a stage that asks for code `c` gets `c` times a fixed
millivolts-per-code constant, so that the displayed stage volts equal the volts at the jack.

Scale, from first principles and independent evidence (this is the number the 259e work got wrong, so
it is stated here and checked physically, not inferred from the fit):

* The stock firmware turns a stage's "volts x 10" field into codes with a table of 38.31 codes per
  0.1 V (entry 100 = 3831 codes = 10.00 V). Its "cents" field adds 0.3831 codes per cent, so 1 cent
  = 1 mV, which is the 1.2 V per octave the whole Buchla 200e standard and the Xenomorpher use. Two
  firmware tables agree on **383.1 codes per volt**, so K = 10000 / 3831 = **2.61028 mV per code**,
  and code 4095 would be 10.69 V.
* The analog stage that follows the DAC has never been measured in this repo (the Buchla_FW premise
  measurement "B4" is not done). So 383.1 is a firmware fact and the hardware scale is **unproven**.
  The run measures it (`a`, below) and refuses to write when it is not within 4 % of K.

Constants live in one place, `CalRec251e` (header comment "ASSUMPTIONS"). The fit target is derived
from K; the **absolute anchors** used by the sanity check are separate literals (section 7).

## 4. Fit

For each channel: points `(r_i, V_i)`, `r_i` the raw code held, `V_i` the mean of two Xeno input
windows in millivolts. Ordinary least squares of V on r gives `V = a*r + b` (a in mV per code, b in
mV; noise is in V, so V is the dependent variable). Then, in real numbers,
`gain = 16384 * K / a`, `offset = -b / a` (codes), because the corrected path must map `c` to
`r = (c*K - b) / a`. Both are then **refined over integers**: gain in a window of +-4 around the
rounded value and offset in +-6 around the rounded `offset + 0.5` (the 0.5 pays for the floor), choosing
the pair that minimises the worst predicted error `|a*Apply(c) + b - c*K|` over **every** stage code
c = 0 to 3831 that the analog stage can reach (`r` in 0..4095). Ties go to the pair nearest the
rounded one. The second implementation that derives the test vectors, and the statistics below, is
`software/test/ref_cal251e_fit.py`. The gain must land in 1..65535 and the offset in s16, else `F_FIT`.
Arithmetic is IEEE double, summed in input order, built without floating-point contraction (like
`Fit259e`).

Points (millivolts as stage volts; codes `round(mv * 3831 / 10000)`):

| set | stage mV | used for |
|---|---|---|
| fit, 8 | 1200, 2400, ... 9600 | the straight line, held at raw code `c` |
| verify, 5 | 300, 3000, 5400, 7800, 9600 | held at the **corrected** code `Apply(c)` of the fitted record; the measured volts are compared with `c*K` |
| drift, 1 | 1200 again | repeats fit point 1 at the end; must agree with the first reading |
| loopback, 5 | 300, 1200, 3600, 6000, 9600 | section 5 |

The verify points are real measurements of the corrected codes (an OVERRIDE at `Apply(c)` puts that
raw code on the jack), so DAC non-linearity the straight line cannot absorb shows up there. 10.0 V is
not measured (the Xeno input tops out near 10.6 V and varies per unit); it is covered by the anchors.

## 5. Measurement chain, loopback first

Chain: 251e DAC and analog stage -> cable -> Xeno input n (12-bit ADC, per-input calibration curve:
17 breakpoints, `InputCal::Millivolts`) -> millivolts. The Xeno's outputs were calibrated against the
bench meter, and the input curves were built from those outputs, so a loopback is a **consistency
check of Xeno out against Xeno in, not an independent standard**. It is still the right gate: it uses
the very cables and inputs of the 251e run, it crosses the pairing the curves were built on (output
E to input 1, F to 2, G to 3, H to 4; the curve for input n was measured against output n), and it
catches a stale or damaged curve, a bad cable and drift since the last Cal Routine run.

Loopback: for each of the 5 loopback voltages, hold outputs E to H at that voltage (each through its
own stored table), read inputs 1 to 4 with two windows, compare with the commanded voltage. Pass
only if every reading is within **2.5 mV** (`F_LOOPBACK`; a reading exactly 2.5 mV off still
passes); a reading more than 500 mV off is a cable problem (`F_PATCH`). Nothing is sent to the 251e
during the loopback.

Measured chain error (gate H1, real hardware, Xeno outputs and inputs calibrated against the bench
meter on a short ground reference near the outputs). Reading minus commanded, mV, at 300 / 1200 /
3600 / 6000 / 9600 mV:

| input | 300 | 1200 | 3600 | 6000 | 9600 |
|---|---|---|---|---|---|
| 1 | +2.01 | +1.42 | +1.76 | +2.12 | +0.77 |
| 2 | +1.07 | +1.03 | +0.68 | +0.33 | -0.49 |
| 3 | +1.84 | +1.34 | +1.49 | +1.44 | +0.63 |
| 4 | +1.46 | +1.05 | +0.70 | +0.75 | -0.27 |

Over the 20 readings the mean is +1.06 mV (standard deviation 0.69 mV, 18 of 20 positive, range
-0.49 to +2.12 mV); the offset is larger at the low voltages. This is one run on one unit, made after
the Xeno was recalibrated on a short ground reference near the outputs (build 5097a27b).

How this compares with the budget below. The loopback only sees the terms that are in the chain it
exercises: output 0.5, input curve 0.5, noise 0.3 and thermal 0.3, about 0.8 mV (1 sigma), about
1.65 mV expanded. The 1.3 mV combined figure (2.6 expanded) also carries the 1.0 mV ground and cable
term, which the loopback does not cover. The measured data exceed the loopback-only figure because
of a systematic offset of about +1 mV that was not in the budget; it is probably the ground
reference used by the calibration run. A first limit of 2.0 mV rejected this healthy chain. The
limit is now **2.5 mV**, a pragmatic choice that still catches gross faults and is far below the
500 mV cable-problem threshold. The worst reading (2.12 mV) is only 0.38 mV inside it, from a single
run, and thermal drift between runs can move the worst error by about 1 mV (checklist H1), so a
healthy unit can sit close to the limit. The check remains the Xeno against itself, not an
independent reference.

The loopback does not cover the ground difference between the 251e and the Xeno. Patch the cable
sleeves (they join the grounds) and keep the budget term below.

### Uncertainty budget (mV, 1 sigma unless noted; bench figures are marked)

| term | value | basis |
|---|---|---|
| Xeno output vs bench meter | 0.5 typical, 1.0 limit | Outputs pass criterion (`kPassMv`); meter accuracy about 0.5 mV at 10 V on its 20 V range (datasheet, not re-read here) |
| Xeno input curve | 0.5 | `verify_mv` about 0.5 mV seen with the console `Qp` |
| ADC resolution | 2.9 mV per count; 0.85 rms single sample | span about -1.35 to 10.6 V over 4096 counts; averaged, see next |
| noise and averaging | 0.3 | assumed 1.5 counts rms, about 100 independent samples per 600 ms window, two windows. The loopback spread (SD 0.69 mV over 20 readings) includes this term but is dominated by the offset pattern, so noise is not separately resolved; still unverified |
| INL | inside the curve term | the 17-point curve absorbs the large-scale INL; its hold-out error is the 0.5 above |
| thermal | 0.3 | assumed after the 30 min warm-up over a run of about 1 min; bounded by the drift check (1.5 mV) |
| ground and cable | 1.0 worst | the Cal Routine docs report about 1 mV shifts from return paths on the bench; not covered by the loopback |
| **combined** | **1.3 (RSS), expanded k=2: 2.6; worst-case sum 3.9** | |

What the 251e fit can tolerate, and what the limits mean. Corrected: the integer gain and offset
cannot reach the line exactly. Over 300 random sound stages (gain error +-3 %, zero +-30 mV, readings
with 0.3 mV noise; `ref_cal251e_fit.py --stats`) the best pair's worst predicted error over all stage
codes has median 1.9 mV, 95th percentile 2.5 mV and maximum 2.65 mV, i.e. about one DAC step (2.61 mV),
not half a step: the floor in the module's shift and the whole-code offset cost the rest. The
measurement error adds to that. Orders of magnitude: a common offset error in the readings moves the
stored offset one for one (a step is 2.6 mV); a scale error of 0.01 % in the readings is 1 mV at 10 V,
about 1.6 steps of gain (0.1 % is 10 mV, about 16 steps).

What `F_VERIFY` does and does not show. The fit points and the verify points go through the same
input chain in the same minute, so a bias common to all readings (a ground offset, an input curve
gain error) cancels in the comparison: passing the verify limit shows the straight-line correction
is **self-consistent on this chain**, not that the output is right in absolute volts. The absolute
bound comes from the measurement budget above (about 2.6 mV expanded, plus the loopback that checks
the Xeno's own inputs and outputs against each other) and from the physical-scale premise, which only
bench data can test.

Why the limits are warnings in a measuring-only run. The limits below (non-linearity 3.0 mV, verify
4.0 mV, anchors 8 mV) are derived, not measured. The same simulation says how often they reject a
sound stage: with bow up to 2 mV and INL up to 1 mV, never (0 %); with bow up to 5 mV and INL up to
2.6 mV, non-linearity rejects about 15 % and verify about 3 to 4 %. A real 12-bit DAC and output
stage could be on either side. So the shipped (write-off) firmware only **warns and reports**
(`CALR,251WARN`, `CALR,251STAT`, a WARN line on Results) and stops on **gross** errors: the hardware
scale or zero (`F_SCALE`), a wrong channel (`F_IDENTITY`), an input near its top, a loopback or
patch failure, non-linearity over 12 mV, verify over 16 mV, anchors over 50 mV. The same limits are
**hard failures in a build with the write on**, so a write still needs them until the first runs'
figures justify better ones.

## 6. Patch plan

All four channels are measured at once (the Xeno reads its eight inputs in one window).

1. Loopback (Xeno only): outputs **E, F, G, H** to inputs **1, 2, 3, 4**, four cables.
2. 251e: **move the output end of each cable** from Xeno E to H to the 251e's **CV 1, CV 2, CV 3, CV 4**
   (inputs stay as they are, so the loopback exercised the same cables). 251e CV n goes to Xeno input n.
   Nothing else on those inputs. Stop the 251e's sequencers and unpatch whatever else the 251e's CV
   outputs feed (oscillators stacked on them will jump): the overrides move the outputs.
3. Identify: with every channel at 1.2 V, each channel in turn is raised to 6.0 V; its own input must
   rise by more than 2.4 V and no other input by more than 0.5 V (`F_IDENTITY`; the screen names the
   channel and the input that moved). A first OVERRIDE must also show status bit1 (`F_NO_OVERRIDE`).

The Xeno inputs 1 to 4 must be calibrated by Cal Routine (the row refuses otherwise,
`F_INPUT_UNCAL`), and the outputs must have been (`F_NO_SELF_CAL`).

## 7. Sanity checks (physical, with numbers)

The 259e lesson: the fit target (4096 counts per octave) was assumed, the verify used the same
assumption, and a +-10 % window around the stock gain could not see a 7.5 % error. Here every check is
in **volts at the jack**, and the key ones use literals that do not come from the fit.

Order, per channel, after the 8 fit points:

1. **Hardware scale (`F_SCALE`)**: the measured line must give `a*3831 + b` within **9600 to 10400 mV**
   (literals; +-4 % of 10 V) and `|b| <= 100 mV`. (Reachability of 10 V needs `a >= 0.9355 K`; 4 %
   leaves 10.26 V at code 4095.) A 4096-codes-per-10-V stage, 6.5 % low, fails here; so would a Xeno
   input curve that is 4 % or more off.
2. **Fit (`F_FIT`)**: gain in range, offset in s16.
3. **Non-linearity (`F_NONLIN`)**: worst fit-point residual about the line. Write limit **3.0 mV**,
   gross limit 12 mV (section 5: warn first). The report also gives the bow and the INL left after a
   quadratic fit.
4. **Record against anchors (`F_RECORD`)**: predicted jack voltage for stage codes 383, 2873 and 3831
   through `Apply` and the measured line, against the **literals** 1000, 7500 and 10000 mV. The codes
   are chosen so that 383.1 codes per volt rounds to the code within 0.7 mV; the write limit of
   **8 mV** includes that, the gross limit is 50 mV. These are predictions from the measured line
   and the record, **not independent measurements**, and they share the 383.1 premise with the fit.
   What they catch is a wrong constant or a wrong fit in the code (a record fitted for 4096 codes per
   10 V puts code 3831 at 9350 mV against the literal 10000); a wrong premise shows only in the
   measured line (check 1 and the logged `a` and `b`). The check repeats at Confirm and at the write.
5. **Verify (`F_VERIFY`)**: the 5 measured corrected-code points against `c*K`. Write limit **4.0 mV**,
   gross limit 16 mV. Self-consistency only (section 5).
6. **Drift (`F_DRIFT`)**: the two windows of each point agree within 1.5 mV, and the repeat of fit
   point 1 at the end within 1.5 mV of the first.
7. **Input range (`F_INPUT_RANGE`)**: no reading within 100 mV of the top of its input's calibrated span.
8. **Noise (`F_NOISY`)**: a window's peak-to-peak above `CvMeterMath::kMaxNoisePpCounts` (24 counts).

Regression tests for the wrong scale (host, all in the suite and in the mutation check):
* unit level: a record fitted with K replaced by 10000 / 4096 looks fine against its own constant
  (worst error under 3 mV) and is rejected by the anchors against the true line (over 500 mV);
* flow level: the flow is run with its fit target replaced by 10000 / 4096 on a good simulated 251e and
  must end `F_RECORD` with no PUT and no COMMIT (a 3 % mistake too; a 0.1 % mistake, 10 mV at 10 V, is
  a warning in a measuring-only run and a failure in a write build; 0.02 % passes);
* hardware level: a simulated 251e whose stage is really 6.5 % low (4096 codes for 10 V) must end
  `F_SCALE`, again with no PUT and no COMMIT, although its own fit and verify are self-consistent.

After a write the corrected path cannot be re-measured over the bus (OVERRIDE bypasses it). The
verdict is the read-back of the record plus the prediction from the measured line; true end-to-end
proof is a bench step (gate H4): sequencer stages at known volts into the Xeno inputs.

## 8. Writing (compiled out by default)

`Cal251eFlow::kAllowWrite` is false unless built with `-DCAL251E_ALLOW_WRITE=1`. In the shipped build
Results says "Write disabled" and ends with the two-press discard (L L); no PUT or COMMIT is reachable
and the flow's test proves it. Defining `CAL251E_ALLOW_WRITE` in a shipped build file fails CI (same
guard as the 259e); a build with it on prints `WARNING: 251e write ENABLED in this build`.

If enabled: Confirm (B held 2 s, the 259e's gate) -> re-check both the age (10 min from the first
loopback point) and the absolute anchors -> READ the old record again (logged as `CALR,251OLD`, a second time when a record exists) -> PUT x5 ->
a status read (the baseline for the refused bit) -> COMMIT once (never resent; a Wire error counts
as sent) -> status poll (bit2, bit0 and the seq of the record put) -> READ the record back, byte-exact, CRC valid -> Done. The unit ID is kept and the
sequence number incremented when a record exists, else the entropy port supplies the ID and seq is 1.
**Old record.** The Scan reads the module's record and logs it (`CALR,251OLD,<76 hex>`, `NONE` when there is none, `ERR_<n>` when it could not be read). The write refuses (`F_OLD_UNREADABLE`, nothing sent) when the status said a record exists and either the Scan could not read and decode it, or the write-time READ returns bytes that do not decode: the write would otherwise carry a new unit ID and seq 1 and the old record would never be logged. The first write on a module with no record is unaffected. After the B hold a successful write logs, in order, `CALR,251OLD` (second time), `CALR,251REC`, `CALR,251COMMIT,<n>,<seq>` and `CALR,251END,OK`; the Confirm screen says the old record was logged at Scan.
**Cap and log:** at most 20 COMMITs per power-up (`F_WRITE_CAP`, the counter survives runs, not a
reboot), and one `CALR,251COMMIT,<count>,<seq>` line per COMMIT. There is no preset backup: a COMMIT touches only the record page. A cancel after the COMMIT frame
went out waits for its verdict in the background, like the 259e.

## 9. Safety rules

* Write disabled by default; measuring-only runs end with a discard.
* The flow cannot touch the Xeno's calibration: it has no save port, only holds outputs and releases them.
* Safe to repeat: a measuring-only run changes nothing persistent. OVERRIDE and RELEASE are volatile and
  time out in 5 s; Fail, Cancel and Done release the Xeno outputs and send RELEASE.
* While a Cal Routine run is open the existing gates already refuse console PUT and COMMIT frames,
  `m`, `x`, `w`, `k` and the SysEx bridge's dump writes.
* Console: **no new `=` command.** `=5Cs` (the existing 259e status reader, address 5C allowed) reads only the
  251e status. To read the old record, start a new 251e run and take `CALR,251OLD` at Scan, then press L at Warm-up. `=5Cr` and `=5Ca0` are 259e readers (30-byte record, byte-5/7 workaround) and are
  **not valid for a 251e**; do not use them. (Any `=` command is a keystroke state machine with no Enter.)

## 10. UI states

Menu row "251e CV outputs" (hint "251e>Xeno in 1-4"), above the 259e row; no meter notice (no meter). Screens, per `Cal251eFlow::Stage`:

| stage | screen |
|---|---|
| NoSelfCal | "Run Outputs and Inputs first." (nothing happens; L back) |
| Scan | "Looking for 251e at 5C" |
| Warmup | "Xeno + 251e on 30 min?" R: yes |
| PromptLoop | "LOOPBACK  Out E>In 1, F>2, G>3, H>4 (no 251e cables) R: go" |
| Loopback | "Loopback n/5", worst so far |
| PromptPatch | "Move cable ends: CV1>In1 CV2>In2 CV3>In3 CV4>In4, stop sequencers R: go" |
| Identify | "Checking patch" |
| Measure | "251e fit n/8" / "verify n/5" / "drift" |
| Results | title "251e results N min"; per channel gain, offset, worst verify mV; a channel over a write limit starts with `!` and letters (n non-linearity, v verify, a anchors) and its row is inverted, and the summary row reads `WARN <channels> nl ver anc` (inverted); with no warning it shows the loopback worst and "Write off"; footer "L L:discard" or "R:write L L:discard" |
| Confirm (write build) | inverted "WRITE TO 251e @5C"; one record page, each write erases it, old record logged at Scan (`CALR,251OLD`); "Hold B 2 s" |
| Write, Verify | progress |
| Done / Failed | result; Failed shows the code name and what became of the record |

## 11. Failure codes (append-only; the log and the screen name them)

`NONE, NO_SELF_CAL, INPUT_UNCAL, NO_251E, PATCH, LOOPBACK, IDENTITY, NO_OVERRIDE, NOISY, DRIFT,
INPUT_RANGE, SCALE, FIT, NONLIN, RECORD, VERIFY, BUS, COMMIT_REFUSED, COMMIT_UNKNOWN, READBACK,
CANCELLED, EXPIRED, INTERNAL, NO_INPUT, WRITE_CAP, OLD_UNREADABLE` (`NO_INPUT`: an input window delivered no samples; `OLD_UNREADABLE`: a record exists that could not be read, so the write is refused). `NO_251E` means only that the status reply from address 5C did not come or was not format 1; its text names no firmware version.
New codes are appended after `OLD_UNREADABLE`; none renumbered.

Log lines (`CALR` v2 style): `CALR,251START`, `CALR,251LOOP,in,mv,reading`, `CALR,251PT,ch,kind,r,mv`,
`CALR,251FIT,ch,gain,offset,pred_worst,anchor`, `CALR,251STAT` (noise, DAC step, verify, anchor, warn bits), `CALR,251WARN`, `CALR,251COMMIT,n,seq`, `CALR,251OLD`, `CALR,251REC`, `CALR,251END,<result>,warn=<hex>`: the warning word (non-linearity channels in bits 0-3, verify 4-7, anchors 8-11; 0 when clean), so a warned run never ends like a clean one.
The first measuring-only run's value is these lines: the measured `a` and `b` per channel are the
hardware scale.

## 12. What needs hardware, by gate

| gate | what | needs |
|---|---|---|
| H0 | host tests, simulator (this change) | nothing |
| H1 | loopback alone on a Xeno (E to H into 1 to 4) | the Xeno and four cables; no 251e. Gives the real chain error and noise |
| H2 | status and override only: `=5Cs`, a measuring-only run on a 251e at 30.7C4 or 30.7C5 | a 251e on 30.7C4, bus, stopped sequencers. Gives `a` and `b`, INL, the override and READ behaviour |
| H3 | measuring-only repeated, with a record present or not | same; confirms repeat-safety and that OVERRIDE ignores the record |
| H4 | first write (build with the flag) with the owner present | after H2 sets the limits and open question 1 is answered; previous record logged first |
| H5 | end-to-end proof after a write: sequencer stages at known volts into the Xeno inputs | the 251e, the Xeno |

## 13. Open questions and answers

Answered by the owner: the scale is not known, and the first measuring-only run logs it (a measured
scale more than 4 % off 383.1 codes per volt still stops a run with `F_SCALE`, in any build, since
nothing sensible can be fitted; the softer limits gate writes only); the limits are to be set from data (section 5, warn first); bring-up
writes are allowed, capped at 20 per power-up with a log line per COMMIT; the first write follows the
measuring-only run; overrides moving a running sequencer is acceptable (the screen says to stop it
and to unpatch the CV outputs' other destinations); the 251e row stays above the 259e row.

Still open:
1. The true scale (383.1 codes per volt, 10.00 V at code 3831?): H2 measures it.
2. Which limits the H2 figures justify for the write path (3.0 / 4.0 / 8 mV are placeholders).
3. Flash endurance of the record page (the cap is per power-up, not a lifetime count).
4. Whether the READ reply path has a quirk like the 259e C4's: unknown until a record exists, hence
   the byte-exact read-back and the CRC, and H4 after H2.

## 14. Assumptions in one place

`CalRec251e.h` (constants block "ASSUMPTIONS"): K, the 3831 and 10000 anchors, the tolerances.
`Cal251eFlow.h`: the write limits and the gross limits ("warn first"), the write cap.
`Cal251eBus.h`: reply and COMMIT timings (the COMMIT status poll starts 300 ms after the frame, an
assumption about a few-ms page rewrite) and the READ chunking (args 0, 8, 16, 24, 30). Reply data of
a `what 1` is never used.
