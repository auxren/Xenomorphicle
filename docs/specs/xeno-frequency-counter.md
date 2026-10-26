# Frequency counter and edge timer

Status: estimator, service, Scope measurement line and console commands are built and host-tested (stages 0 to 3 of the plan below, minus the TR edge readout). Nothing has run on hardware yet.

| File | What |
|---|---|
| `software/src/ScopeFreq.h` | the estimator (header-only, no Arduino dependency) |
| `software/src/ScopeFreqService.h/.cpp` | the one static counter, the audio and CV feeds, gates, CV ranging, clock, ISR-to-loop hand-off |
| `software/src/ScopeFreqLine.h/.cpp` | the two-gate rule, the screen line, the console `SFREQ` line |
| `software/src/ScopeFreqConsole.h/.cpp` | the `%` console commands (byte parser) |
| `software/src/ScopeSettings.h/.cpp` | Scope settings, layout version 2 (clock bytes) |
| `software/test/test_scope_freq*.cpp`, `test_scope_sources.cpp` | host suites; `mutate-scope-freq.sh` (estimator) and `mutate-freq-wire.sh` (the rest) prove they can fail |
| `tools/xeno-sim` `--test-app-switch freq-*` | the real Scope, a tone through the real CV frame hook, the screen read back |

Decisions taken for this build (the questions at the end of the first version): the crystal is calibrated once against the Mac's clock over USB (about 1 ppm) and `clock_ratio` is stored; the settings layout changed with a version bump; the DC-coupled CV inputs are counted too, for low frequencies; the top of the range stays 5 kHz; the screen uses a 0.1 s gate and calibration will use 1 s.

## Goal

Make the Xenomorpher a measuring instrument so the bench meter is needed less:

1. a **frequency counter** on an audio input, 0.01 cent on a clean tone with a gate of 1 s or more;
2. **TR edge timestamps** for period and interval;
3. a **Scope measurement line**: frequency, nearest note, cents;
4. later, **Cal Routine** calibrating a 259e by measured pitch (CV out A drives it, its oscillator goes into IN L, fit volts per octave), with the Siglent only an occasional reference.

0.01 cent is 5.8 ppm of frequency. Everything below is about whether that is reachable and what stops it.

## Method

The audio tap delivers int16 at 48 kHz in 128-sample blocks (`AudioScopeCapture`, audio ISR).

1. **Learn** (0.1 s): peak-to-peak gives the level (the middle of the range) and the hysteresis half-width (15 % of peak-to-peak, never under 48 LSB). Peak-to-peak under 384 LSB is TOO QUIET.
2. **Schmitt trigger** on rising crossings of the fixed level. A crossing counts only after the signal has gone below level minus hysteresis and then above level plus hysteresis; its time is the last rising crossing before that commit.
3. **Linear interpolation** between the two samples around the level gives the crossing time to a fraction of a sample. The per-sample path is integer-only; one division per crossing.
4. **Gate**: the first committed crossing starts it, the first commit at least `gate_samples` (default 48000) later ends it. So the gate is a whole number of periods and never under 1 s.
5. **Fit**: least squares of crossing time against crossing number (online Welford sums; plain sums of squares lose all digits at 10^5 samples). The slope is the period in samples; `Hz = fs * clock_ratio / slope`. The residual gives a 1-sigma in cents and a jitter figure (residual rms over period).
6. The level and hysteresis are re-learned from each gate's own range for the next gate; within a gate they are fixed.

I tried a four-point cubic (Catmull-Rom with Newton) for step 3 and removed it: against linear it was no better on sine, triangle, saw or square at 0.05 s gates from 440 Hz to 5 kHz, and costs about 40 flops per crossing. What interpolation buys is against whole-sample timestamps: those scatter by 0.29 sample rms and would give about 0.06 cent on a 0.05 s gate (the mutation test confirms the suite sees this).

## Error analysis

| Source | Size | Effect |
|---|---|---|
| Sample rate / block size | 48 kHz, 128 | 20.8 us resolution before interpolation; result is independent of how samples are chunked (tested bit-exact for 1, 128 and ragged chunks) |
| Sampling clock jitter | ps | negligible (5 kHz at 100 ps: 3e-6 of a cycle, averaged over N crossings) |
| Interpolation | linear, error third order in the phase step | measured 0.0002 cent worst over the clean table below; 0.002 cent at 3 kHz on a 0.05 s gate |
| 16-bit tap | 0.29 LSB rms | a floor only for small signals: amplitude 300 LSB (-40 dBFS, about 85 mV at the jack) at 20 Hz gives 0.01 cent |
| Noise | see formula | the dominant term below 100 Hz |
| Harmonics | none on frequency | each cycle has the same shape, so every crossing shifts alike; only extra crossings matter, and hysteresis rejects them |
| DC offset | none | the level follows the middle of the range; a drifting offset leaks (below) |
| Clock reference | 1 ppm = 0.00173 cent | the floor of the absolute reading; see next section |

**Noise.** For a sine of amplitude A with white noise of rms s, crossing-time jitter is s / (2 pi f A) and the fit over N = f * T crossings gives

    sigma_f / f = sqrt(12) * (s / A) / (2 pi * N^1.5)       cents = 1731 * sigma_f / f

Error falls as T^1.5 and as f^1.5. Measured against it (30 trials each, rms error in cents; the model uses the measured gate):

| Hz | 60 dB SNR measured / model | 40 dB SNR measured / model |
|---|---|---|
| 20 | 0.0054 / 0.0072 | 0.042 / 0.073 |
| 100 | 0.00045 / 0.00067 | 0.0053 / 0.0067 |
| 1000 | 0.00002 / 0.00002 | 0.0002 / 0.0002 |
| 5000 | under 0.00001 | 0.00002 / 0.00002 |

The reported `cents_sigma` tracks the measured error within a factor of 3 in all rows. So the 0.01 cent goal holds at 1 s from 20 Hz upward when SNR is 55 dB or better, and with large margin above 100 Hz. At 20 Hz with 40 dB SNR it is 0.04 cent. Hum matters: 20 Hz with 50.37 Hz hum at -30 dBFS read 0.016 cent off (sigma said 0.21, deterministic hum is not white).

**Noise ceiling.** Hysteresis is 0.3 A wide. Noise up to 0.15 A rms (14 dB SNR) still counts correctly (error 0.08 cent at 100 Hz); 0.20 A at 100 Hz double-triggers and the status goes UNSTABLE; at 0.30 A both 100 and 440 Hz report UNSTABLE. It never returned OK with a wrong number.

**What is not covered by sigma.** Anything that moves the crossing times together, once, mid-gate: a DC step of 9 % FS on a 110 Hz sine read 0.93 cent off (sigma 0.05); on 1760 Hz 0.06 cent. An amplitude change on an asymmetric wave does the same (0.48 cent at 110 Hz). The next gate is clean (under 0.0001 cent), so the acceptance rule is: **two consecutive OK gates agree to within 3 sigma combined plus 0.01 cent**. The test refuses every step case under that rule and accepts the clean pairs. A frequency step (a semitone mid-gate) reads UNSTABLE, then OK at the new pitch. A slow DC ramp (1500 LSB/s) keeps reading OK with 0.16 cent worst bias. The audio inputs are AC-coupled (1.6 Hz), so real offsets decay in about 0.1 s.

**Measured, clean tones** (error in cents, 1 s gate, sine / triangle / saw / square band-limited to 22 kHz, no noise beyond int16 rounding):

| Hz | sine | tri | saw | square |
|---|---|---|---|---|
| 20 | 0.00000 | 0.00000 | 0.00000 | 0.00000 |
| 27.5 | -0.00003 | 0.00000 | -0.00011 | -0.00018 |
| 55 | 0.00000 | -0.00001 | +0.00013 | -0.00014 |
| 440 | 0.00000 | 0.00000 | +0.00005 | -0.00004 |
| 1000 | 0.00000 | 0.00000 | 0.00000 | 0.00000 |
| 5000 | 0.00000 | 0.00000 | 0.00000 | 0.00000 |

Worst over the full 11-frequency table: 0.00018 cent against the design 0.01. A 1.2 V/oct sweep of 8 tones (27.5 Hz to 3.5 kHz) recovers the volts-per-octave slope to within 0.0001 mV per octave for sources 0, +1 % and -0.35 % off nominal. A 0.1 s gate at 1234.5 Hz reads 0.00003 cent; a 0.05 s gate on a sine reads under 0.002 cent.

**With the 0.1 s gate the screen uses** (9000-count sine, 30 trials per noisy cell, rms error in cents; the sigma column is the counter's own estimate at 60 dB SNR):

| Hz | clean, worst | 60 dB SNR | 40 dB SNR | sigma at 60 dB |
|---|---|---|---|---|
| 20 | 0.0000 | 0.123 | 0.961 | 0.102 |
| 27.5 | 0.0045 | 0.112 | 0.688 | 0.083 |
| 55 | 0.0018 | 0.046 | 0.309 | 0.041 |
| 110 | 0.0006 | 0.019 | 0.145 | 0.017 |
| 220 | 0.0005 | 0.009 | 0.072 | 0.007 |
| 440 | 0.0001 | 0.002 | 0.025 | 0.002 |
| 1000 | 0.0000 | 0.0007 | 0.010 | 0.0008 |
| 2000 | 0.0000 | 0.0003 | 0.003 | 0.0003 |
| 5000 | 0.0000 | 0.0001 | 0.001 | 0.0003 |

That is the T^1.5 law at work: a tenth of the gate, about 30 times the noise error, and the line prints fewer digits when it is larger (decimals follow sigma). Clean tones stay under 0.005 cent at every frequency.

Range: from about 10 Hz (longest accepted period 4800 samples; below that NO SIGNAL) to a ceiling of fs/6 = 8 kHz (TOO HIGH under 6 samples per cycle; a 9 kHz sine reports it). The design band is 20 Hz to 5 kHz.

## Clock reference

Every reading is `f_true * (1 + e)` where e is the error of the Teensy's 24 MHz crystal, because the I2S audio clock, the cycle counter and `micros()` all derive from it. A typical untrimmed crystal is 10 to 30 ppm and moves a few ppm with temperature; the part is not specified in this repo, so measure it. 20 ppm is 0.035 cent (tested: a 20 ppm error reads -0.0346 cent uncorrected, 0.0000 with `clock_ratio`). The cycle counter shares the crystal, so it cannot check it. The PLL's fractional divider also leaves the achieved fs slightly off 48000; stage 1 reads the register values or compares sample count against an external time.

**The Siglent cannot do this.** My recollection is that a DMM's frequency function is specified around 0.01 % (100 ppm); read the SDM3065X datasheet, but it is very likely worse than the crystal.

One-time calibration, in order of effort:

1. **Host clock**: the Teensy prints its cycle counter and sample count over USB at known intervals; the Mac's NTP-disciplined clock stamps them. Over 1 to 2 hours, USB latency jitter of a few ms is under 1 ppm. No extra hardware. Result is `clock_ratio`, stored once.
2. **GPS 1 PPS into a TR jack**: edge timestamps over 100 s are good to far under 1 ppm (the `EdgeStats` class computes it).
3. A TCXO or lab frequency reference into IN L.

**For Cal Routine the clock does not matter.** Volts per octave and nonlinearity come from ratios between tones, and a common clock error cancels exactly. The clock only limits absolute pitch (a 259e's tuning) to about 0.03 cent uncalibrated.

## As built: where it plugs in

* **Lifetime.** One static `ScopeFreq::FreqCounter` in `ScopeFreqService.cpp`, outside the Scope's lazily taken arena. It runs only while the Scope is open: `AppScope::Rearm` calls `ScopeCapture::SelectFreq(source)` (the trigger trace, when it is an audio or CV input), `APP_EVENT_SUSPEND` calls `SelectFreq(-1)`. `Select` is idempotent (the Scope re-arms whenever the auto timebase moves; a reset there would keep a gate from ever finishing). It does not depend on the arena: with the allocation failing, the Scope shows "not enough memory" and the counter and its line still work (sim scenario `freq-no-arena`). A frozen view keeps counting. Leaving the Scope, by the menu or the console, stops it and takes the ADC frame hook off; the clock correction lives in the service and survives.
* **Audio feed.** `ScopeCapture::OnAudio` (audio ISR, one 128-sample block per tap) calls `ScopeFreqSvc::OnAudio` first: one load and compare when that tap is not the counted one. Audio in L/R and out L/R can be counted.
* **CV feed.** `ScopeCapture::OnCvFrame` (core ISR, once per FlexIO frame, `kCvFsHz` = 37,350.6 Hz, see "CV frame rate") calls `ScopeFreqSvc::OnCvFrame` first: the counted input's word, clamped at 0 and centred (word minus 16384). The one frame hook serves the rings and the counter; `UpdateCvHook` keeps it installed while either needs it. CV inputs are DC-coupled, so this is the route under the audio inputs' 1.6 Hz AC coupling. **Ranging:** a CV input starts in the CV profile (10 Hz to 5 kHz, 0.1 s gate). Two consecutive results that found nothing to count while the input is *moving* (more than 96 counts peak to peak, about 35 mV: NO SIGNAL or TOO QUIET), or TOO LOW, or SIGNAL LOST, move it to CV_LOW (down to 0.5 Hz, 2 s learning, 2 s gate); CV_LOW goes back on silence or on an OK reading above 30 Hz. A flat or noise-floor input never leaves the CV profile, so a tone patched in is read within 0.25 s. An LFO at 1 Hz reads in about 5 s and is shown in Hz without a note name.
* **Hand-off.** The ISR copies the 88-byte `Result` into one slot guarded by a sequence number (odd while writing); loop code retries when it moved. One slot rather than the two the first version proposed: there is only ever one writer, an ISR every 100 ms, and the reader is a few loop passes. Tested against a thread publishing as fast as it can.
* **Reading.** `ScopeFreqSvc::Poll()` (called from `AppScope::Loop`) feeds a `ScopeFreqLine::Meter` with each new result; the Meter applies the acceptance rule and the screen and the console both read `CurrentReading()`.
* **Gate.** `SetGate(FAST)` (0.1 s, the screen) or `SLOW` (1 s, for calibration; console `%gs`). Switching restarts the gate.
* **Clock.** `SetClock(calibrated, ppm10)`, tenths of a ppm, +-1000 ppm; `clock_ratio = 1 + ppm10 * 1e-7` is `true_rate / nominal_rate`, so a crystal 20 ppm fast is `+20.0`. Out of range is refused and nothing changes.

## CV frame rate

The nominal 300 kS/s over eight inputs is 37.5 kHz, but the code does not produce that. `ADC33131D_Start()` (OC_ADC.cpp) sets the trigger timer, TMR4 channel 3, as follows: clock source `PCS(8)` (IPG clock, prescaler 1), `LENGTH` mode, `OUTMODE(3)` (OFLAG toggles on every compare), `comp1 = int(F_BUS_ACTUAL / 300000 / 2 + 0.5f)`. `F_BUS_ACTUAL` is 150 MHz at 600 MHz (clockspeed.c: IPG divider = cpu/150 MHz rounded up, at most 4). So `comp1 = 250`. One reading is taken per OFLAG period (two compares), one frame is eight readings, so

    kCvFsHz = F_BUS / (2 * ticks_per_compare * 8)
            = 150e6 / (2 * 251 * 8) = 37,350.598 Hz      if a compare takes COMP1 + 1 ticks
            = 150e6 / (2 * 250 * 8) = 37,500.000 Hz      if it takes COMP1 ticks

The first is what the code uses (in `LENGTH` mode the counter re-initialises on the tick after the compare, which makes the period COMP1 + 1; the NXP SDK programs `COMP1 = period - 1` for this mode). **VERIFIED on the hardware, 2026-10-25/26** (build de16e011): one steady 513.39 Hz tone patched into IN L and CV IN 1, five interleaved readings each (0.1 s gates, all status OK): IN L mean 513.390384 Hz (spread 0.0007), CV1 mean 513.389406 Hz (spread 0.0011), **ratio CV1/IN L = 0.999998, -2 ppm**. The other reading, 37,500 Hz, would have shown the two 4,000 ppm apart. The agreement is good to +-5 ppm, including the audio PLL's <= 3.5 ppm truncation, and it also shows the two counters share the crystal path. The difference between the two candidates was 3,984 ppm, 6.9 cents, outside the +-1000 ppm clock range, so the one `clock_ratio` serves audio and CV. The constants live in one place (`ScopeFreqService.h`: `kCvBusHz`, `kCvTimerComp1`, `kCvTimerTicksPerCompare`, `kCvReadingsPerFrame`, `kCvRateVerified = true`), `test_cv_rate_formula` pins the value to this arithmetic, and the `#` mark (rate unconfirmed; `rate` 0 in `SFREQ`) is still in the code behind `kCvRateVerified = false` (`RateUnverified`, tested) for any future change of the constant. The gate and learning lengths are derived from it (`MakeConfig`), so the 0.1 s gate is 3,736 frames.

Time base: TMR4 counts the IPG clock, which comes from the ARM PLL (12 MHz times an integer from the 24 MHz crystal), so the crystal error is common with the audio sample rate and one correction applies to both; the audio PLL's own fractional-N error (small, not known) is not common.

**What was measured, and what was not:** ONE tone, 513.39 Hz, five readings each way. Other frequencies (50 Hz, 1 kHz, 4 kHz) were not tried, nor other CV inputs than CV1. A rate error would show as the same ratio at every frequency, so one tone settles the rate, but nothing here has been checked against a second tone.

**How to recheck** (module only, two cables, the console; this is the procedure the measurement above followed):
1. Scope on IN L, a steady tone from the module's own audio output (any tone generator patched out to IN L), no CV patched. `%gs`, `%r` three times: the `hz` with status `OK`. Call it f_audio.
2. Patch the same signal into CV1 (it must be within the CV input range: attenuate if needed), Scope on CV1. `%r` three times, status `OK`, rate field 0. Call it f_cv.
3. The ratio f_cv / f_audio must be 1.00000 within 20 ppm. That confirms `kCvTimerTicksPerCompare = COMP1 + 1`. A ratio of 0.99602 (CV reads 3,984 ppm low) means the compare takes COMP1 ticks and the true CV rate is 37,500 Hz: set the constant to `kCvTimerComp1`. Any other ratio means something else is wrong (the audio PLL, the signal, the input): stop and look.
4. Repeat at 50 Hz, 1 kHz and 4 kHz (the ratio must not move with frequency). If it ever disagrees, set `kCvRateVerified = false` (the `#` mark returns) and correct the constant and the test that pins it.


The plot area is now rows 11 to 54 (44 rows, was 53) and the bottom page, rows 56 to 63, is the measurement line; row 55 is the gap. Nothing in the plot code reaches below `kPlotBottom` (sim scenario `freq-layout`: single trace, overlaid, stacked, CV plus TR lanes, spectrum). The old `AC` label moved to the bottom left of the plot. Stacked lanes lose their number chips from five lanes up (a lane is 8 rows, the chip needs 9).

    440.012Hz A4 +0.05c     two consecutive gates agreed, clock calibrated
    *440.012Hz A4 +0.05c    agreed, but the crystal is not calibrated (absolute error up to ~30 ppm)
    ?440.012Hz A4 +0.05c    not confirmed by a second gate that agrees
    !440.01Hz A4 +3c        UNSTABLE: the average of something that moved
    #440.012Hz A4 +0.05c    steady, but the source's sample rate is unconfirmed (every CV input, until the bench procedure above)
    NO SIGNAL  TOO QUIET  SIGNAL LOST  TOO LOW  TOO HIGH  WAIT   freq: --  (source not countable, or Scope closed)

The tilde of the first version is `*`: the 6x8 font ends at `z`. The decimals follow the result's own sigma (the last digit is no coarser than sigma), the cents get 0, 1 or 2 decimals at sigma above 0.5, 0.05 and below, and the text is cut back to 21 columns by dropping decimals. Under 16 Hz there is no note name. The acceptance rule (two consecutive OK gates agree within 3 sigma combined plus 0.01 cent) marks a reading; it does not delay it, since the display must be fast.

## Spectrum view: zoom, windows, averaging (not part of the counter)

Added with the counter because it shares the capture path. The spectrum's capture ring is 1024 buckets (the FFT length), so a zoom cannot read it: a /64 span needs 65,536 raw samples. Instead the capture path feeds a **decimating cascade** (`ScopeDecim.h`): halfband /2 FIR stages in integers (DC gain exactly 1; 11 taps in every stage but the last, 47 taps in the last), 2, 4 or 6 stages, filling a 1024-sample ring that the loop-context FFT reads. Shown span = 0.8 of the decimated Nyquist (4.8 kHz, 1.2 kHz, 300 Hz) so that nothing above it folds in. Host-measured (test_scope_decim): passband flat to 0.07 dB over the whole shown span, worst alias into it -42.5 dB (/4) and -44.8 dB (/16, /64), peaks found to 0.2 Hz at /64.

* RAM, inside the existing arena allocation: feed 2,358 B (ring 1,025 slots: one spare, so a push in progress never touches the window being read; cascade 302) plus the averaging accumulator 2,052 B: **4,410 B** more on the arena (the review measured 4,412 B and `sizeof(Arena)` = 67,028 for the version before the spare slot); nothing at boot.
* CPU, **measured by the reviewer in an ARM emulator** (my first estimate, 45 to 60 instructions per sample, was 2.5 to 3 times low): the decimator averages 137 / 147 / 149 instructions per input sample at /4 /16 /64 (worst single sample 349 / 599 / 849); a 128-sample audio block costs 17.5 k to 19.1 k instructions (about 20 to 29 k cycles, 1.2 to 1.8 % of the block time); `OnCvFrame` is 142 instructions per frame without a zoom and 282 to 294 on average with one (worst 494 / 744 / 994), and a 7-frame burst adds 1,520 instructions (about 2.5 to 3.8 us). The review also found `Short()` was called, not inlined, 7 times per evaluation; both filters are now forced inline (the compiled `Cascade::Push` has no calls left); the new instruction counts are for the next review to measure. All of this only while the spectrum is shown with a zoomed span. The FFT, windows, averaging and dB math run in loop context.
* Zoom details fixed after review: the span is per source (0.8 of the decimated Nyquist at that source's real rate: CV 3,735 / 934 / 233 Hz), a frozen view keeps the ring, CV out and TR say `no zoom` (they are not fed to the decimator), the reader takes a snapshot of the ring and retries if a push moved the count (bounded; a spare slot covers a push in progress), averaging steps per half frame of fresh samples, the peak search starts at 20 Hz or three bins, and the CV rate everywhere (`RateHz`, the time axis, the latency) is `kCvFsHz`, so a 100 Hz CV tone reads 100.0 and not 100.4.
* Not built: a /256 span (0-75 Hz would take 5.5 s to fill); a labelled frequency axis beyond the span text and quarter marks.
* The low-band fix: columns narrower than a bin are interpolated linearly in dB between the two bins around them (they used to repeat the nearest bin: with a 1 kHz sine, an offset and 50 Hz hum the first 24 columns were one block at bin 1's level and the next 9 another, then a cliff). Bin 0 is never a column of its own; the offset is removed before the transform.
* Tests: `test_scope_decim`, `test_scope_fft` (windows, power and averaging, DC, columns, linear columns), `test_scope_fmt` (peak text); sim scenarios `freq-fft-header`, `freq-fft-zoom` (each span with real tones: peak reading, the tone's bar column, the label), `freq-fft-window` (presets, averaging cuts the column jitter), `freq-fft-lowband`.

## Number text on the module

The module's `snprintf` is newlib-nano without float support: `%f`, `%.*f` and `%+.*f` print nothing (a field formatted that way is blank on the OLED and in the console), and the host's libc prints them fine, so host tests cannot see it. The first hardware build showed `!Hz F2 c` for exactly that reason. Every number the Scope and the console print from a double now goes through `ScopeFmt::Fixed` (`ScopeFmt.h`, integer arithmetic only, round half away from zero, never `-0.00`, NaN and overflow guarded), checked against printf over 336,000 random values in `test_scope_fmt`. Recurrence is blocked twice: `run-host-tests.sh` fails if a format string in the Scope or counter sources (and the console handler in `Main.cpp`) contains a float conversion, and the line formatter's suite is compiled with `host_stubs/nofloat_printf.h`, whose `snprintf` aborts on `%f/%e/%g` the way the module blanks.

## Console

Prefix `%` (no other command uses it; every letter is taken and several are traps on an unlocked console). No Enter anywhere; a command ends with its last character; a byte that does not belong cancels the command and is swallowed (so a stray `%` cannot turn the next byte into `p`, `w`, `x`); a pending prefix expires after 3 s; and after a cancel the bytes that follow within 50 ms (the rest of what was typed or pasted: `.` is a broadcast recall, `(` a save) are swallowed too. A value without its sign (`%c0012.3`) is refused. The handler sits after every other console consumer in `Main.cpp` (the CV meter's `d`/`Q` prompts, the 259e arguments), so a dialog that owns the console still sees every byte; selfcheck has a source-order check for it. Replies start with `SFREQ,` (the older `n` counter on TR1 already uses `FREQ,`).

| keys | action |
|---|---|
| `%r` | `SFREQ,<src>,<hz>,<sigma_cents>,<status>,<gate_ms>,<flags>,<seq>,<cal>,<rate>` (`rate` 0 = the source's sample rate is not confirmed on hardware, true of the CV inputs for now); `<src>` is `INL`, `CV3`, ... or `-` with `CLOSED` when the Scope is not open on a countable source; status `OK`, `SETTLING` (OK but unconfirmed), `UNSTABLE`, `NO_SIGNAL`, `TOO_QUIET`, `LOST`, `TOO_LOW`, `TOO_HIGH`, `WAIT`; flags bit 0 = clipped |
| `%t` | read-only time stamp for the Mac-side crystal measurement (clock reference method 1): `STIME,<cycles64>,<audio_samples64>,<cv_frames64>,<seq>`. Implemented in `ClockStats.h/.cpp`; the audio count is one add per 128-sample block in `AudioScopeCapture::update`, the CV count one add per ADC pass in `OC_ADC.cpp`, both always on. The 64-bit cycle value is made from `ARM_DWT_CYCCNT` and `millis()` (the wrap count is the one that agrees with the elapsed milliseconds, so the gap between two `%t` may be anything up to the 49.7 days of the 32-bit `millis()` count (the 32-bit CV frame count wraps after 32 h and needs a `%t` at least that often)). Sign of the result: if the Mac measures `N` counts in `T` seconds of its own clock, `ppm = (N / (T * nominal) - 1) * 1e6`, and a crystal that is FAST (positive) is the value `%c` wants, unchanged. Latency: stamps are taken in the loop (jitter up to `loop_max`, about 6 ms), so average over many exchanges |
| `%c+0012.3` | set the crystal correction in ppm: sign, four digits, `.`, one digit. Answers `SFREQ,CLOCK,+12.3,SET,unsaved`; beyond +-1000.0 `SFREQ,ERR,RANGE`, malformed `SFREQ,ERR,cancelled`. Saved by `g` (or any app-data save), not by the command |
| `%cx` | forget the correction (uncalibrated) |
| `%gf` / `%gs` | fast (0.1 s) or slow (1 s) gate |

## Settings

The Scope's saved bytes went from 16 to 20: bytes 0 to 15 unchanged, 16 = layout version (2), 17 = clock calibrated, 18 to 19 = correction (int16, tenths of ppm). The framework used to drop any chunk whose length differed from the app's current one, so a longer layout would have thrown every user's Scope settings away; `ApplyAppData` now asks `OlderAppDataLayoutReadable` and the Scope answers for its 16-byte layout and for the 3-byte layout of the first Scope (a padded chunk arrives one byte longer; `ScopeSettingsPayloadLength` sees through that). That also makes the 3-byte restore that `ScopeSettings.cpp` always had actually reachable. A save that does not carry a calibration never overwrites a live one. **The correction belongs to the module, not to a patch:** a preset recall (`ApplyAppDataFromPreset`, which raises `AppDataIsPresetRecall()` for the duration) ignores bytes 17 to 19, so it can neither overwrite a calibration nor bring back one that `%cx` cleared; only the settings load at boot applies it. It is still written into every save (the boot save needs it). A factory reset (`InitDefaults`, so `AppScope::Init`) clears it, so it is not written back. **Backup restore** re-initialises the apps (which clears the correction) and then loads the backup's settings, so the module ends with the backup's correction, or uncalibrated if the backup predates calibration: intended, a restore replaces the module's stored state (read from the code path; not run on hardware). **Downgrade:** firmware from before layout 2 drops a Scope chunk whose length is not its own, so the Scope comes up with defaults and the correction is lost; bytes 0-15 keeping their meaning does not help there. Chosen over moving the value into the global settings store because that is a second storage path for one value; the cost is that the correction is in the preset files too, where it is inert. Tests: `test_scope_sources` (layouts, out-of-range clock, bad version) and sim scenario `freq-settings` (a real `BuildAppData`, the Scope chunk cut down to 16 and to 3 bytes, `ApplyAppData`).

## Sample gaps are silent

The counter sees only the samples that reach it. Nothing tells it about the ones that do not: an audio block the Audio library dropped (`AudioScopeCapture::update` gets nothing for that block), ADC frames lost because the core ISR missed ticks (the 16-frame ring is 427 us deep; a persistence window or a long loop stall misses many), or a stalled USB host. The crossing times after a gap are shifted by the missing samples, so the period fit sees a jump: a short gap inside a gate usually shows up as UNSTABLE or a reading that the next gate does not confirm (`?`/`SETTLING`), but a gap that happens to be a whole number of periods is invisible and the gate reads long. A bench script that logs a number is therefore not enough: **it must require `status OK` (not `SETTLING`, not `UNSTABLE`) on two consecutive `%r` reads with different `seq`, and compare them**, and it should discard readings taken while anything is saving. Cal Routine will do the same (two agreeing gates).

## Cost

RAM (static, RAM1/DTCM): counter 288 B, Meter 128 B, hand-off slot 88 B, selectors and clock about 20 B: about 520 B, nothing in RAM2. Measured with `teensy_size` against the previous main (db613385):

| | T41_console | T41_audio | T41_audio_dbg |
|---|---|---|---|
| RAM2 free for malloc | 337,216 (unchanged) | 347,968 (unchanged) | 336,352 (unchanged) |
| RAM1 variables | 108,768 to 113,312 (+4,544) | 115,296 to 115,840 (+544) | 114,016 to 118,528 (+4,512) |
| RAM1 free for local variables | 87,840 to 83,296 | 81,312 to 80,768 | 82,592 to 78,080 |
| flash code | 660,408 to 666,792 (+6,384) | 646,640 to 652,712 (+6,072) | 662,000 to 668,336 (+6,336) |
| flash data | 136,300 to 137,836 (+1,536) | 124,408 to 124,776 (+368) | 136,880 to 138,416 (+1,536) |

(Against main 8ce8461e. The linker asserts at 16 KB of free local variables.) The console and debug builds' +4.5 KB is the linker's 4 KB alignment, not the counter: `.bss` starts on a 4 KB boundary because of an aligned object in it, and that build's `.data` ended 75 bytes short of the boundary, so any 75 bytes of new data would have cost the same (the audio build, with room, shows the real +544). Kept `.data` small on purpose: no libm `log`/`log2` (their tables are 3.3 KB of RAM1 data on this toolchain, so the counter has its own `Log2`), no new virtual on `AppBase`, zero-initialised selectors, no jump tables in the new files.

CPU, from the Cortex-M7 `-O2` disassembly of `FreqCounter::Push` (not measured on hardware). The first version's `Push` saved `d8-d15` and built a 96-byte frame on every call, because some inlined code in it used doubles, and the first FPU instruction in an interrupt makes the core stack the FP context; the floating-point work (interpolation, the fit, the result) is now out of line (`SCOPEFREQ_NOINLINE`), and `Push` is integer only (no FPU use except at a crossing, where the candidate time goes to `Commit` as a double) with an 8-byte frame. Results are identical (all estimator tests unchanged). Now: the per-sample path in the running state is about 44 instructions, **30 to 45 cycles** with dual issue and branch costs, plus about 20 cycles per call (nine registers pushed and popped); the first version was 50 instructions plus the FP save, estimated 35 to 65 plus. A crossing costs about 250 cycles (one `vdiv.f64` for the interpolation, one in the Welford update). A gate end (`Finish`, an 88-byte result built and copied, a square root, a few divisions, and the hand-off copy) is about 1500.

* Audio source: 128 samples per 2.667 ms block, 3.9 to 5.8 k cycles plus one call, 0.2 to 0.4 % of a core, in the audio ISR (below the core ISR's priority; the core ISR is not touched). Up to 5000 crossings/s at 250 cycles: another 1.25 M cycles/s.
* CV source, per core tick (60 us, 36000 cycles): 2.24 frames (37,350 / 16,667) at 30 to 65 cycles each (a call per frame) = 70 to 150 cycles, plus at 5 kHz 0.3 crossings = 75 cycles, so **about 100 to 230 cycles per tick (0.2 to 0.4 us)**; the worst single tick is a gate end plus a crossing plus three frames, about 2000 cycles (3.3 us), once per 100 ms. Against the `core isr max` ceiling of 40 us (idle maximum 8 us) that is +3.3 us worst and +0.4 us typical, only while the Scope is open on a CV input, and the core ISR no longer pays an FP context stack for the counter's sake. With no source selected: one load and one compare per frame and per audio block.
* The Scope's own drawing is unchanged except 9 fewer plot rows.

The six `rt budget` rows that already fail on hardware are not made worse by construction (the ISR additions are bounded and small), but this is the same claim as the first version: **the proof is a before/after of the `T` report with the counter running for ten minutes**, every row's counters not moving beyond their noise. That has not been done.

## How Cal Routine would use it

New row **259e pitch (frequency)**; same patching as today (output A to the 259e pitch input, the 259e oscillator output to IN L), no DMM in the loop.

1. Preconditions: outputs calibrated by Cal Routine (as now), counter `clock_ratio` set or the result marked relative.
2. For 8 points at 1.2 V steps (27.5 Hz to about 3.5 kHz): set output A, discard the gate that straddles the step, then take the first two consecutive gates that agree (rule above); give up after 10 gates.
3. Fit log2(f) against V by weighted least squares (weights from sigma, floor 0.01 cent for the DAC). Report V/oct, the worst residual in cents (nonlinearity), and the gain error against 1.2 V/oct. About 4 s a point, under a minute for the row, against 26 minutes with the meter.
4. This is the missing octave-ratio check (gate X3 of the 259e plan), as a measurement.

What it measures is the 259e together with output A, so output A must have been calibrated against the Siglent first; its 0.2 to 0.7 mV is 0.2 to 0.7 cent at 1.2 V/oct, which is the real accuracy limit of the method and well above the counter's own resolution. The 259e write stays disabled until its own gates pass; this row is measuring only.

## Failure modes and messages

Screen text is at most 11 characters (`StatusText`), longer text for Cal Routine.

| Status | Cause (tested) | Cal Routine text |
|---|---|---|
| NO SIGNAL | silence, hiss under 48 LSB, below 10 Hz | "No tone on IN L: patch the 259e" |
| TOO QUIET | peak-to-peak under 384 LSB | "Tone too weak: raise 259e level" |
| SIGNAL LOST | nothing for 4800 samples mid-gate; re-learns and recovers | "Tone dropped out" |
| UNSTABLE | noise above about 0.15 A rms, hum, double triggering, FM, drift above about 2300/N cents per gate, a dropped cycle; hz is still filled in | "Pitch not steady (hum? FM?)" |
| TOO LOW | fewer than 3 crossings in the gate | "Below 20 Hz" |
| TOO HIGH | under 6 samples per cycle | "Above 5 kHz: lower the pitch" |
| flag CLIPPED | samples at the rails; the count is still right | "Clipping: attenuate" |
| two gates disagree | step, settling, drift | "Settling... (n)" then "Would not settle" after 10 gates |
| clock not calibrated | no correction stored | absolute Hz shown with a `*`; relative results unaffected |

Scope line: see "Screen" above. It updates once per 0.1 s gate.

## Staged plan

| Stage | Work | Hardware | State |
|---|---|---|---|
| 0 | Estimator and host tests | none | done |
| 1 | Wire the tap behind a console command that prints Hz and cents; real input noise and hum against the model (loopback of the module's own output, then a function generator); `T` before/after | module, two cables, a generator | wired and host-tested (`%r`); not run on hardware |
| 2 | One-time clock calibration, `clock_ratio` stored in settings; achieved fs from the PLL | Mac log, or a GPS 1 PPS | storage and `%c` done; the Mac-side logging tool and the run are not |
| 3 | Scope measurement line; TR period/interval readout; cycle-counter edge ring | module only (OLED check) | line done; TR readout not started |
| 4 | Cal Routine pitch row against the 259e simulator on the host (`sim259e.h`), then on a real 259e, compared with the Siglent once | 259e, Siglent for the reference run | not started (SLOW gate and acceptance rule are ready for it) |

## Open questions

1. **A4 reference.** The note and cents are against a fixed 440 Hz. Is there, or should there be, a global A4?
2. **CV thresholds.** The 48-count hysteresis floor, the 384-count quiet threshold and the 96-count "moving" threshold are on the 0..32767 CV word (about 17 mV, 140 mV, 35 mV) and are guesses until the real noise of the inputs is measured.
3. **Audio PLL error.** `clock_ratio` assumes the audio sample rate is exactly nominal apart from the crystal. The audio PLL's fractional divider leaves a small error of its own (<= 3.5 ppm by the 2026-10-25/26 measurement's bound) that has not been read from the registers; stage 2 should do that, or compare against an external time.
4. **Persistence of the correction in preset files.** It is written into preset chunks (inert on recall). If that is unwanted it needs a second storage path for the value.
