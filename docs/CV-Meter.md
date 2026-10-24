# Bench CV meter, input calibration and frequency counter

The console build (`T41_console`) turns the Xenomorpher into a bench
instrument for calibrating other modules: an 8-channel CV meter streamed over
USB serial, a precise voltage source on any output, a closed-loop calibration
of each CV input against those outputs, and a frequency counter that reads
pitch in cents. Everything is driven from the [bench console](bench-console)
(unlock with `pew!`); `tools/hwqa/cvmeter.py` does the same from a host and
logs CSV.

Every app reads an input through the same per-input calibration the meter
uses, once one is stored; an input without one reads exactly as it always did
(see [What apps see](#what-apps-see)).

## Commands

| key | action |
|---|---|
| `v` | toggle the CV stream: one line per 20 ms (50 Hz) with all 8 inputs |
| `d` `<o>` `<mV>` Enter | hold output `o` (`1`-`8` or `a`-`h`) at a calibrated voltage, in signed millivolts, e.g. `d3-1500⏎`, `d1 9600⏎` |
| `d` `<o>` `x` | release output `o` back to the running app |
| `d` `x` | release every held output |
| `Q` `<i>` `<o>` | calibrate input `i` (`1`-`8`) using output `o` as the reference |
| `Q` `a` `<o>` | calibrate inputs 1 to 8 in turn, all from output `o` |
| `Q` `p` | print the input calibration, per input |
| `Q` `c`, `Q` `c` | clear the input calibration (second `Qc` within 3 s) |
| `n` | toggle the frequency counter on **TR1** |

A held output ignores the running app until it is released; the app keeps
running and gets the output straight back. Holds live in RAM: a reboot
releases them. The hold accepts the whole calibrated table range of the
output (0 to 12 V on a Xenomorpher), but only entries 0 and 8 (0 V and 9.6 V)
were set against a meter by the DAC calibration; the rest of the table is
interpolated from those two, and above 9.6 V extrapolated.

Like every argument on this console, a stray byte cancels a half-typed `d` or
`Q` rather than being read as something else.

## Line formats

Every line starts with a tag, fields are comma-separated, and the formats are
fixed: `software/test/fixtures/cvmeter_lines.txt` holds golden examples that
the firmware's host test prints byte for byte and the host tool's test parses.

### CV stream (`v`)

On start, a header and the range each input can read before it clips, by the
calibration in effect for it:

```
CV#,ms,seq,dropped,raw1,...,raw8,mv1,...,mv8,cal,clip
CVRANGE,<input>,<lo_mv>,<hi_mv>,<C|S>          (8 lines)
```

then every 20 ms:

```
CV,<ms>,<seq>,<dropped>,<raw1>..<raw8>,<mv1>..<mv8>,<cal>,<clip>
CV,123456,7,2,4095.00,3685.50,...,0.0,1000.2,...,CCSSSSSS,H..L...H
```

| field | meaning |
|---|---|
| `ms` | module uptime, milliseconds |
| `seq` | line counter since the stream started, including dropped lines |
| `dropped` | lines dropped so far because the host was not reading |
| `raw1..8` | ADC counts (0-4095, two decimals): the mean of every reading in the 20 ms window, inputs in panel order |
| `mv1..8` | millivolts, one decimal |
| `cal` | 8 characters: `C` = this input has its own calibration (its curve file or its `Q` line, the same one the apps use), `S` = shared scale + offset |
| `clip` | 8 characters: `L` a reading reached raw 0-1, `H` reached 4094-4095, `.` neither. Raw goes **down** as volts go up on this hardware, so `L` is the top of the voltage range and `H` the bottom |

`v` again stops it and prints `CV#,stopped,lines=<n>,dropped=<n>`.

The stream never stalls the module. Teensy's `Serial.write()` waits up to
120 ms for a host that is not draining the port; the stream checks the room
in the USB buffer first and drops the line (counting it) instead.

### DAC hold (`d`)

```
DAC,<o>,HOLD,<mV>,<code>
DAC,<o>,RELEASED
DAC,ALL,RELEASED
DAC,ERR,<o>,<reason>
```

### Input calibration (`Q`)

```
INCAL,PATCH,in=<i>,out=<o>,points=<mV1>,<mV2>,<mV3>
INCAL,POINT,in=<i>,<k>,<mV set>,<mean raw>,<spread p-p counts>,<samples>
INCAL,RESULT,in=<i>,out=<o>,gain_uv=<uV/count>,offset_mv=<mV>,resid_mv=<mV>,noise_pp=<counts>,verdict=<OK|...>
INCAL,SAVED,in=<i>          INCAL,DISCARDED,in=<i>
INCAL,ABORT,in=<i>,<why>    INCAL,ERR,...     INCAL,DONE     (end of Qa)
INCAL#,version=1,mask=<hex>,stored=<yes|no>,scale=<pitch scale>,octave_mv=<1000|1200>
INCAL,CH,<i>,C,gain_uv=..,offset_mv=..,verify_mv=..,src=<o>,range=<lo>..<hi>,apps=<curve|line|shared>
INCAL,CH,<i>,S,adc_offset=..,range=<lo>..<hi>,apps=<curve|line|shared>
```

Lines that start with two spaces are prose for a person and can be ignored by
a parser.

### Frequency counter (`n`)

```
FREQ#,jack=TR1,gate_ms=250,a4=440 -- ...
FREQ,<ms>,<hz>,<periods>,<note>,<cents>
FREQ,99,440.0000,110,A4,+0.00
FREQ,<ms>,NOSIG                                (no edge for 2 s; repeats each second)
FREQ#,stopped,dropped=<n>
FREQ#,stopped,an app switch took the counter
FREQ#,ERR,busy: ...
```

`hz` has 4 decimals, `cents` is signed with 2 decimals, relative to the
nearest 12-TET note with A4 = 440 Hz (MIDI numbering: A4 = 69, C4 = 60).

## Calibrating the inputs

The DAC outputs were already calibrated against a meter by the existing
calibration (Setup / Calibrate), so they are the reference. Each input is
calibrated by patching one output into it: the module sets the output to
three voltages itself, averages the input at each, fits gain and offset from
two of them and checks the fit at the third. No meter reading is needed.

1. Calibrate the DAC outputs first if that has not been done. The input
   calibration is only as good as the output it is measured against.
2. Let the module warm up for a few minutes.
3. `Q11` (or `cvmeter.py PORT cal --input 1 --output 1`). The module answers
   with the patch to make and the three voltages it will set.
4. Patch the output to the input with nothing else on either jack. Press `y`.
5. It sets each voltage, waits 300 ms, averages for 600 ms, and prints a
   `POINT` line for each, then the `RESULT`.
6. If the verdict is `OK`, `y` stores it; anything else discards it.
   `Qa1` repeats this for inputs 1 to 8 with one cable from output 1, which
   you move from input to input.

The three voltages sit at about 10 %, 90 % and 50 % of the span both the
output and the input can cover, moved onto the DAC's own calibration points
and never at a rail. On a Xenomorpher that is **1.2 V, 8.4 V and 4.8 V**:
inside the 0-9.6 V the DAC calibration measured with a meter, so no
extrapolated DAC code is ever the reference.

A fit is stored only if every check passes:

| verdict | refused because |
|---|---|
| `RAIL` | a point's mean is within 8 counts of either end of the 12-bit window |
| `NOISY` | readings at a point spread over more than 24 counts peak to peak (loose cable, something else driving the input) |
| `FLAT` | the two fit points are less than 50 counts apart: the input did not follow the output (wrong jack, nothing patched) |
| `SIGN` | the input moved the wrong way |
| `GAIN` | the gain is more than 15 % from the shared scale, or outside 1-6 mV per count |
| `MONOTONIC` | the verification reading does not lie between the two fit readings |
| `RESIDUAL` | the line misses the verification point by more than **10 mV** |

`Qp` prints what is stored, and in `apps=` what the apps use for each input;
`Qc` twice clears the `Q` lines. An input that also has a Cal Routine curve
keeps using its curve: the curve takes precedence over the line, and `Qc`
does not touch the curve files.

### Where it is stored

In its own 112-byte EEPROM page just below the preset-bus record
(`EEPROM_INPUTCAL_*` in `OC_config.h`), not in the DAC/ADC calibration page:
that page fills its region exactly, and growing it would have moved the
app-data page and invalidated every unit's stored calibration and settings.
The page is versioned (`InputCalibration::kVersion`, FourCC `ICA1`). A unit
updated from older firmware has no page there, loads "no input calibration",
and behaves exactly as before; its DAC/ADC calibration and app data are not
touched (the host test checks both, against an image of an old unit's
EEPROM). A page from a newer layout, or a stored gain that fails the sanity
window, is ignored rather than trusted. The explicit EEPROM erase in the
factory reset leaves it alone, as it leaves the DAC/ADC page alone.

## What apps see

The per-input calibration, in every app, from the moment it is stored.

- **Which calibration.** Per input: its Cal Routine curve (17 points, in
  `ICURVE_A.BIN` / `ICURVE_B.BIN`) when the file is valid and holds one for
  that input; else its `Q` line (`ICA1`); else the shared scale and offset
  from Setup / Calibrate, exactly as before. Anything out of range in either
  store (a curve that is not strictly falling, a line outside the sanity
  window, a table that would overflow) is ignored for that input rather than
  trusted. `Qp` shows the choice per input (`apps=`).
- **Where it applies.** `IO::Read` in the core ISR, which fills every app's
  `cv.values` and `cv.pitch_values`, and `OC::ADC::calibrated_value()` /
  `calibrated_pitch()`, the one accessor for the apps that read the ADC
  directly (QQ and DQ's CV bars, ASR's slew CV, Sequins' 10 V input map,
  Passencore's chord CVs, the Sampler's gate and rate CV, Scope's CV trace
  and the counts it prints, H1200's debug view).
  The console meter (`v`, `Qp` ranges) uses the same tables. Both values the
  apps get move together, so a voltage an app shows is the calibrated one.
- **How.** At load and after every save the calibration is turned into one
  17-point integer table per input in DTCM (`InputCal.h`), and the ISR
  interpolates it: integers only, a shift, two multiplies and a few adds per
  calibrated input per tick. A calibrated input's pitch uses the nominal
  1536 pitch units per 1.2 V on true volts, in place of the shared
  `pitch_cv_scale`.
- **Unchanged.** An input with no calibration runs the stock code
  (`ADC::value()`, `value_to_pitch()`) with the same arithmetic; a unit with
  nothing stored behaves exactly as before. The random seed at boot,
  Setup / Calibrate's own screens and the debug screens stay on the raw
  reading.
- **Gates from CV inputs** (Quadrants and every applet, via
  `pitch_values > GATE_THRESHOLD`, about 1.5 V) switch at a true 1.5 V on a
  calibrated input instead of the shared estimate of it. The comparator has
  no hysteresis to stay within; the threshold moves by the calibration
  correction at 1.5 V, a few mV to a few tens of mV on a real unit (the
  15 % gain acceptance bounds it near 0.2 V), against gate swings of 5-10 V.

## The frequency counter

- **Jack: TR1**, the first trigger input -- the same input and the same
  driver instance (`FreqMeasure`) the autotuner and the References app use.
  While it runs, TR1 does not act as a trigger input for the running app.
- It refuses to start (`FREQ#,ERR,busy`) when something else already owns
  the counter, and stops by itself (`FREQ#,stopped,an app switch took the
  counter`) when an app switch takes it back. `n` again stops it and returns
  TR1 to the app.
- **Signal:** TR1 is a gate/trigger input; the firmware times edges that
  cross the input's switching threshold, which is set by the hardware and is
  not known to the firmware. Use a pulse or square output where the
  oscillator has one, at gate level. A sine or a small audio-level signal may
  not cross the threshold at all (the counter then reports `NOSIG`) or may
  cross it noisily. Check the setup against a known frequency first.
- Each line averages every period in a 250 ms gate (at least one period;
  below 4 Hz a line comes once per period). Periods are counted in 150 MHz
  bus clocks and summed, so the count error over a gate is one tick in tens
  of millions: far below a cent at any audio frequency. What limits a
  reading in practice is the oscillator's own jitter and the edge's noise at
  the threshold, plus the Teensy's crystal (tens of ppm absolute, about
  0.05 cent).

## Input range: what can clip

Two ADC paths exist. On every Xenomorpher it is the external 8-channel
ADC33131D, chosen at boot from the hardware ID (`OC_gpio.cpp:176`).

**ADC33131D path (Xenomorpher).** Each sample is a signed 16-bit reading
(`OC_ADC.cpp:36`); negative sums are clamped to 0 (`OC_ADC.cpp:448`), doubled
(`:457`) and cut to 12 bits before smoothing (`OC_ADC.h:128`). So every input
is a 0-4095 window and **no reading can go past either end**. On Northern
Light hardware 0 V is set at the top of that window, raw 4095
(`OC_calibration.h:19`, used at `OC_calibration.cpp:106`): volts rise as
raw falls. The default scale is 409.6 counts per pitch octave
(`OC_ADC.h:42` with `value_to_pitch`, `OC_ADC.h:110`), and on this hardware
an octave is 1.2 V (the calibration screens, `apps/SETTINGS.h:375` and
`:409`; `gfxPrintVoltage`, `HSUtils.cpp:806`). That makes the firmware's
model of an uncalibrated unit **0 V to 12.0 V at 2.93 mV per count**, and a
Buchla 0-10 V CV lands no lower than raw ~682: by that model **the top of
0-10 V does not clip**, with about 2 V to spare.

The bottom has no margin at all: 0 V is the rail. Anything below 0 V reads
0 V, and a unit's stored 0 V offset can itself be rail-limited (an offset of
exactly 4095 means "at or beyond the end of the window").

What the firmware cannot tell you is where the analog front end really puts
those ends: the divider in front of the ADC33131D is not in the code, and
the model above is only as good as each unit's stored offset and scale
(which the existing calibration takes from CV1 alone). The stream's
`CVRANGE` lines print each input's actual readable range from the
calibration in effect, and the `clip` flags show a rail being hit. **Bench
check before relying on the top of the range:** hold an output at 10 V
(`d1 10000⏎`) into each input and confirm the reading is well clear of
raw 0 and the `clip` flag stays `.`.

**Internal ADC path (other hardware IDs only).** The Teensy's ADC2 in 12-bit
mode with 4-sample hardware averaging (`OC_ADC.cpp:213`, `:223`), 0 V at
2/3 of the window (`OC_calibration.h:20`). With 1 V octaves that is about
-3.3 V to +6.7 V: a 10 V CV clips at about 6.7 V. Not used on a Xenomorpher.

## Accuracy: what to expect

- **Resolution.** The 16-bit converter is cut to 12 bits before smoothing,
  so one count is about 2.9 mV on a Xenomorpher. A stream line is the mean of
  every reading in its 20 ms window (about a hundred), which resolves below a
  count only as far as the input's own noise dithers it.
- **Accuracy after input calibration: a few mV**, bounded by the DAC
  reference (set against a meter at 0 V and 9.6 V, linear between) and the
  verification residual (at most 10 mV, typically well under). Without it,
  inputs other than CV1 can be off by tens of mV: the shared scale is
  measured on CV1 only.
- **Not a pitch reference.** One cent is 1 mV at 1.2 V/octave (0.83 mV at
  1 V/octave), below what this meter can promise. Do not set an
  oscillator's tracking to a cent from voltage readings here; patch the
  oscillator into TR1 and use the frequency counter, which resolves far
  below a cent.

## Host tool

`tools/hwqa/cvmeter.py` (Python 3, pyserial for a real port, nothing else):

```
cvmeter.py /dev/ttyACM0 stream --csv cv.csv --seconds 60
cvmeter.py /dev/ttyACM0 cal --input all --output 1
cvmeter.py /dev/ttyACM0 print
cvmeter.py /dev/ttyACM0 freq --csv f.csv
cvmeter.py /dev/ttyACM0 check --output 1 --input 3 --mv 6000
cvmeter.py /dev/ttyACM0 set --output 2 --mv 4800 ; cvmeter.py /dev/ttyACM0 release
cvmeter.py sim cal --input 2 --output 2         (simulated module, no hardware)
```

It unlocks the console only if a probe shows it is locked (the token is
itself four commands on an unlocked console; see `tools/hwqa/hwctl.py`).
`check` exits non-zero when the reading misses by more than `--tolerance`
(10 mV). Tests: `python3 tools/hwqa/test_cvmeter.py`.
