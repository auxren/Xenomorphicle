# Cal Routine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A system app, Cal Routine → Self, that calibrates the Xenomorpher's 8 outputs (all 11 DAC table points, closed loop against a USB bench DMM on the host jack, verified by a sweep) and then its 8 inputs (multi-point curve against a calibrated output), with every app using the result.

**Architecture:** Hardware-free logic lives in small host-tested `.cpp/.h` units, in the `CvMeterMath` style: USBTMC framing and SCPI allowlist, the calibration maths, the curve-file codec and the flow state machine behind injectable interfaces. Thin hardware glue sits around them: a `USBDriver` subclass, the ADC path, `PhzConfig` files and the app UI. The per-input curve becomes a 17-point integer table in DTCM that `IO::Read` and one shared accessor apply in the core ISR.

**Tech Stack:** C++17, Teensy 4.1 / PlatformIO (`software/platformio.ini` envs `T41`, `T41_audio`, `T41_audio_dbg`, `T41_console`), USBHost_t36, LittleFS via `PhzConfig`, g++ host tests (`software/test/run-host-tests.sh`), Python 3 hwqa (`tools/hwqa/`).

**Spec:** `design/cal-routine-design.md` (approved 2026-10-23, amended the same day for the sweep, multi-point inputs and curve storage).

## Global Constraints

- No EEPROM layout moves: the calibration page `[0,224)` (FOURCC `CAL1`), app data, `ICA1` `[4164,4276)` and the preset-bus record keep their size and place. The marker in Task 7 uses the gap `[4140,4164)` or `ICA1`'s 4 spare bytes.
- No stored Cal Routine data, or invalid data → bit-for-bit today's behaviour on every input and output.
- Core ISR max ≤ 40 µs of 60 µs, 0 missed ticks; loop pass max ≤ 5 ms (`docs/Timing-Budget.md`). ISR code uses integers and DTCM data only.
- USBHost objects and EHCI-visible buffers live in DTCM (`Main.cpp:60-72` comment). USB pipe callbacks only move bytes; everything else runs from `loop()` and never blocks it.
- Meter allowlist: `*IDN?`, `*CLS`, `CONF:VOLT:DC <range>`, `VOLT:DC:NPLC <n>`, `VOLT:DC:AZ <ON|OFF|ONCE>`, `TRIG:SOUR IMM`, `READ?`, `SYST:ERR?`. Everything else is refused, including every `CAL…` and every other `SYST:…`.
- Units: 1.2 V/oct, so 1 mV = 1 cent. DAC entry k = k × 1200 mV, k = 0…10, 16-bit codes, monotone, in 0–65535. Calibrated input value units match the nominal model: **409.6 value counts per 1200 mV** (`value = mV × 4096 / 12000`). Pitch = `value × 15360 >> 12` (1536 per octave).
- Output pass: every sweep residual ≤ 1 mV. Iteration aims for ≈ 0.2 mV (one code ≈ 0.18 mV). Save is offered when all 8 outputs pass. A failed or skipped input is saved as "not calibrated".
- Save order: calibration page, then the curve file, then `ICA1`. Nothing is stored before Save. Every exit path releases every `DAC::Hold`.
- Cal Routine is appended **last** in `app_container` (slot 32 of `kMaxApps = 32`), TWOCC `"CR"`, `FOLDER_SYSTEM`, in every env, no feature flag. Also added to the `tools/xeno-sim/Makefile` app list.
- Every new host-test suite is added to `software/test/run-host-tests.sh` and must pass under both `./run-host-tests.sh` and `SAN=1 ./run-host-tests.sh`. The CI firmware matrix (all 4 envs) builds.
- Commits: lowercase prefix, imperative, ending with the session's `Co-Authored-By` / `Claude-Session` lines. Nothing is pushed.

## Review Focus

1. **Meter unplugged, or stops answering, mid-point.** Expect: the step fails within its timeout, every hold is released, nothing is saved, and the screen says what happened. → Task 6 test `meter_timeout_releases_all`, Task 2 bench step.
2. **A unit already in the field: old EEPROM, no curve file, a Setup-only calibration.** Expect: identical CV values, pitch and DAC codes to today. → Task 5 test `uncalibrated_bit_exact`, Task 5 test `old_unit_eeprom_image`.
3. **The DAC cannot reach 12 V on a channel** (entry 10 at the rail). Expect: that entry is flagged at the rail, reported, and does not block Save. Entries stay monotone. → Task 3 test `rail_entry_flagged_not_clipped`.
4. **One curve file corrupt, both missing, or the partition reformatted.** Expect: the other copy is used, else the `ICA1` line fit, else the shared scale. Never garbage. → Task 4 tests `ab_newest_valid_wins`, `torn_copy_falls_back`, `no_files_uses_line_fit`.
5. **Owner moves the lead early, or a loose cable** (noisy or flat readings). Expect: a NOISY or FLAT verdict on that point and a retry prompt, never a stored bad fit. → Task 6 test `noisy_point_retries_not_stores`.

---

### Task 1: USBTMC framing and the SCPI allowlist (pure)

**Files:**
- Create: `software/src/UsbTmc.h`, `software/src/UsbTmc.cpp`
- Test: `software/test/test_usbtmc.cpp`
- Modify: `software/test/run-host-tests.sh` (add `suite test_usbtmc ../src/UsbTmc.cpp`)

**Interfaces:**
- Produces (namespace `UsbTmc`):
  - `constexpr uint8_t kDevDepMsgOut = 1, kRequestDevDepMsgIn = 2;`
  - `size_t BuildMsgOut(uint8_t tag, const char *scpi, uint8_t *out, size_t cap);` writes the 12-byte header (MsgID, bTag, ~bTag, 0, u32 LE TransferSize, bmTransferAttributes = 0x01 EOM, 3 × 0) plus the payload, padded to a multiple of 4. Returns the total length, or 0 if it doesn't fit.
  - `size_t BuildRequestIn(uint8_t tag, uint32_t max_len, uint8_t *out);` builds the 12-byte header with MsgID 2 and TermCharEnabled 0.
  - `bool ParseMsgIn(const uint8_t *buf, size_t len, uint8_t expect_tag, const uint8_t **payload, uint32_t *payload_len, bool *eom);`
  - `uint8_t NextTag(uint8_t tag);` returns 1…255, skipping 0.
  - `bool ScpiAllowed(const char *line);` implements the Global Constraints list. It is case-insensitive and accepts the short and long SCPI forms of the listed commands (e.g. `CONFigure:VOLTage:DC`).
  - `bool ParseReading(const char *reply, double *volts);` parses SCPI NR3 (`+1.23456789E+00`).

- [ ] **Step 1: Write failing tests** in `test_usbtmc.cpp`, in the existing plain-assert style of `test_cvmeter_math.cpp`:
  - `BuildMsgOut(1,"*IDN?\n")` → 20 bytes: `01 01 FE 00 06 00 00 00 01 00 00 00 '*' 'I' 'D' 'N' '?' '\n' 00 00`.
  - `BuildRequestIn(7, 256)` → `02 07 F8 00 00 01 00 00 00 00 00 00`.
  - `ParseMsgIn` accepts a well-formed reply. It rejects a wrong tag, a wrong ~tag, a TransferSize longer than the buffer and MsgID ≠ 2.
  - `NextTag(255) == 1`, `NextTag(1) == 2`.
  - `ScpiAllowed`: true for `READ?`, `conf:volt:dc 20`, `VOLTage:DC:NPLC 10`, `SYST:ERR?`. False for `CAL:VAL 1`, `CALibration:SECure:STATe OFF`, `SYST:PRES`, `*RST`, `MEAS:VOLT:DC?`, `""`, and `READ?;CAL:STOR` (compound lines are refused).
  - `ParseReading("+1.19998765E+00\n")` → 1.19998765. `ParseReading("9.9E37")` (overload) → false.
- [ ] **Step 2: Run** `cd software/test && ./run-host-tests.sh`. Expected: FAIL to compile (`UsbTmc.h` missing).
- [ ] **Step 3: Implement** `UsbTmc.cpp`. It has no Arduino includes. `ParseReading` treats |v| ≥ 9.9E37 as overload → false.
- [ ] **Step 4: Run** `./run-host-tests.sh && SAN=1 ./run-host-tests.sh`. Expected: all suites pass.
- [ ] **Step 5: Commit** `usbtmc: message framing, SCPI allowlist and reading parser (host-tested)`.

### Task 2: USBTMC host driver and console bring-up

**Files:**
- Create: `software/src/drivers/USBTMCDevice.h`, `software/src/drivers/USBTMCDevice.cpp`
- Modify: `software/src/Main.cpp:60-72` (instance in DTCM next to `usbHostMIDI`), `Main.cpp:1700-1708` (`u` also lists the meter), the console dispatcher (new `U` command), `docs/bench-console.md` (rows for `U`, and the `u` change)

**Interfaces:**
- Consumes: Task 1 `UsbTmc::*`.
- Produces (class `USBTMCDevice : public USBDriver`, global `extern USBTMCDevice usbMeter;`):
  - `bool connected() const; uint16_t idVendor() const; uint16_t idProduct() const;`
  - `enum class QueryState { Idle, Busy, Done, Refused, Timeout, Error };`
  - `bool BeginQuery(const char *scpi, uint32_t timeout_ms);` returns false and sets `Refused` when `!UsbTmc::ScpiAllowed(scpi)`, or false if busy or not connected. A line ending in `?` expects a reply; otherwise the query completes after the OUT transfer.
  - `QueryState Poll();` is called from `loop()`. It advances OUT → REQUEST_IN → IN, applies the timeout, and on timeout or disconnect lands in `Timeout` or `Error`.
  - `const char *Reply() const;` gives the NUL-terminated reply once `Done`, up to 255 chars.
- Claims at interface level (`type == 1`) with `bInterfaceClass == 0xFE && bInterfaceSubClass == 0x03`. It opens the bulk OUT and bulk IN pipes only, from the endpoint descriptors (max packet 512 at high speed, 64 at full speed). It contributes its own `Pipe_t`/`Transfer_t` pools (32-byte aligned) and calls `driver_ready_for_device(this)`, as `midi.cpp` does. `disconnect()` clears all state; a pending query ends `Error`.

- [ ] **Step 1: Implement the driver** following `USBHost_t36/midi.cpp` (claim, pipes, callbacks, requeue) and `hub.cpp:37` (pools). Callbacks copy the received bytes and set a flag; all parsing happens in `Poll()`.
- [ ] **Step 2: Console `U`:** `U<scpi line>⏎` calls `BeginQuery` and prints `METER,DONE,<reply>`, `METER,REFUSED`, `METER,TIMEOUT` or `METER,ERR,<state>`. The line is collected with the console's existing argument rules (a stray control byte cancels). `u` prints `meter: vid=XXXX pid=XXXX connected=<0|1>` after the MIDI lines.
- [ ] **Step 3: Build** `pio run -d software -e T41_console && pio run -d software -e T41_audio`. Expected: both succeed. Record flash/RAM use in the commit body.
- [ ] **Step 4: Bench check (Oren, with the bench owner's agreement).** Flash `T41_console` and check:
  - `u` shows `meter: vid=F4EC pid=1203 connected=1`.
  - `U*IDN?⏎` prints `METER,DONE,Siglent Technologies,SDM3065X,…`.
  - `UCONF:VOLT:DC 20⏎` then `UREAD?⏎` returns a reading.
  - `UCAL:VAL 1⏎` prints `METER,REFUSED`.
  - Unplugging the meter mid-`READ?` gives `TIMEOUT` or `ERR`, and the console keeps working.
  - USB-MIDI on the host hub still works.
  - 10 meter replugs with no module USB drop (design gate 6).

  Record the results in `docs/bench-console.md`.
- [ ] **Step 5: Commit** `usb host: USBTMC meter driver, console U query and u listing`.

### Task 3: Calibration maths (pure)

**Files:**
- Create: `software/src/CalRoutineMath.h`, `software/src/CalRoutineMath.cpp`
- Test: `software/test/test_calroutine_math.cpp`
- Modify: `software/test/run-host-tests.sh` (`suite test_calroutine_math ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp`)

**Interfaces:**
- Consumes: `CvMeterMath::{Fit, PointStats, Verdict, InputCalibration, kRail*, kMax*}`.
- Produces (namespace `CalRoutineMath`):
  - `constexpr int kNodes = 11; constexpr int32_t kNodeMv = 1200; constexpr int kCurvePoints = 17; constexpr int kCurveShift = 8;`
  - `constexpr float kNominalCodesPerMv = 65535.f / 12000.f; constexpr float kTargetMv = 0.2f; constexpr float kPassMv = 1.0f; constexpr int kMaxIterations = 6;`
  - `struct Reading { double median_mv; double spread_mv; int n; };`
  - `Reading Summarize(const double *mv, int n);` returns median and max−min.
  - `bool ReadingTrusted(const Reading &r);` false when `n < 3` or `spread_mv > 0.5`.
  - `uint16_t NextCode(uint16_t code, double measured_mv, int32_t target_mv, float codes_per_mv, bool *at_rail);` steps toward the target and clamps to 0…65535, setting `at_rail` when clamped.
  - `float LocalSlope(const uint16_t table[kNodes], const double measured_mv[kNodes], int k);` gives codes per mV from neighbouring measured nodes, or the nominal value when not enough nodes are known.
  - `enum TableVerdict { TABLE_OK, TABLE_NOT_MONOTONE, TABLE_SLOPE };`
  - `TableVerdict CheckTable(const uint16_t table[kNodes]);` requires codes strictly increasing and each step's codes per 1200 mV within ±15 % of nominal.
  - `int SweepPoints(int32_t out_mv[], int cap);` returns the verify-sweep targets: 4 points between each pair of nodes, 0–12000 mV, nodes excluded, in ascending order.
  - `struct InputSweepPoint { int32_t mv; CvMeterMath::PointStats stats; };`
  - `CvMeterMath::Verdict BuildInputCurve(const InputSweepPoint *pts, int n, int16_t mv_x2_out[kCurvePoints], CvMeterMath::Fit *line, float *worst_dev_mv, float *holdout_mv);` resamples the points onto raw 0, 256, …, 4096 (true mV × 2, linear between measured points, linear extrapolation at the ends). Even-indexed points build the curve and odd-indexed points give `holdout_mv`. It also returns the least-squares `line`, and checks RAIL, NOISY, FLAT, SIGN, GAIN (on the line) and MONOTONIC (raw strictly monotone, mV increasing).
  - `void CurveToValueTable(const int16_t mv_x2[kCurvePoints], int32_t value_q8_out[kCurvePoints]);` gives value counts × 256 at each breakpoint (`mV × 4096 / 12000 × 256`).
  - `void LineToValueTable(const CvMeterMath::Fit &line, int32_t value_q8_out[kCurvePoints]);` does the same from the `ICA1` line, as the fallback.
  - `inline int32_t ApplyValueTable(const int32_t q8[kCurvePoints], uint32_t raw12)` gives `seg = raw12 >> 8; frac = raw12 & 255; (q8[seg] + (((q8[seg+1] - q8[seg]) * frac) >> 8)) >> 8`. It is header-inline because the ISR uses it.

- [ ] **Step 1: Write failing tests:**
  - `node_converges_linear`: a simulated DAC with mV = code × 12000/65535 + 3.0 reaches |err| ≤ 0.2 mV within `kMaxIterations` at every node.
  - `node_converges_bowed`: a DAC with a smooth bow of 4 mV peak at mid-scale converges at every node. The sweep residual between nodes is computed and reported, not hidden.
  - `rail_entry_flagged_not_clipped`: a DAC that tops out at 11 900 mV leaves entry 10 at 65535 with `at_rail` set, and `CheckTable` still returns `TABLE_OK` with entries monotone.
  - `table_checks`: a non-monotone table gives `TABLE_NOT_MONOTONE`, and a step 30 % off nominal gives `TABLE_SLOPE`.
  - `sweep_points`: 40 points, ascending, none equal to a node, all within 0…12000.
  - `summarize`: the median of {1,9,2,3,2} is 2 with spread 8; `ReadingTrusted` is false for spread 0.6 and true for 0.3.
  - `input_curve_linear`: a linear input gives a curve within 0.5 mV of the line everywhere and `holdout_mv ≤ 0.5`.
  - `input_curve_bent`: a 6 mV S-bend input gives curve error ≤ 1 mV at the held-out points, while the line's worst deviation is ≥ 5 mV.
  - `input_curve_verdicts`: a rail point gives RAIL, a 30-count spread gives NOISY, all-equal raw gives FLAT, and raw rising with mV gives SIGN.
  - `apply_value_table`: with a nominal table, `ApplyValueTable` equals today's `offset − raw` mapping within 1 count across raw 0…4095. It never reads past index 16.
- [ ] **Step 2: Run** `./run-host-tests.sh`. Expected: FAIL (missing header).
- [ ] **Step 3: Implement** `CalRoutineMath.cpp`, hardware-free, reusing `CvMeterMath` verdict thresholds rather than copying them.
- [ ] **Step 4: Run** `./run-host-tests.sh && SAN=1 ./run-host-tests.sh`. Expected: all pass.
- [ ] **Step 5: Commit** `cal routine: node iteration, table checks, sweep and multi-point input curve maths`.

### Task 4: Input-curve files: codec, A/B store, migration priority, backup

**Files:**
- Create: `software/src/InputCurveStore.h`, `software/src/InputCurveStore.cpp` (pure codec plus a storage interface), `software/src/InputCurveFiles.cpp` (the `PhzConfig`/LittleFS glue)
- Modify: `software/src/PhzConfig.cpp` (`migrate_legacy` keeps the curve files in its first pass with `GLOBALS.*`), `software/src/apps/Backup.h` (the dump includes the curve file)
- Test: `software/test/test_input_curve_store.cpp`; `run-host-tests.sh` gets `suite test_input_curve_store ../src/InputCurveStore.cpp`

**Interfaces:**
- Produces:
  - File names `"ICURVE_A.BIN"` and `"ICURVE_B.BIN"` (under the 16-byte `Held::name` limit).
  - `struct InputCurveRecord { uint32_t magic /* 'ICV1' */; uint16_t version /* 1 */; uint16_t reserved; uint32_t seq; uint8_t valid_mask; uint8_t source_output[8]; uint8_t reserved2[3]; int16_t mv_x2[8][17]; uint32_t crc32; };` Little-endian, `static_assert(sizeof == 304)`. `crc32` covers every byte before it, using poly 0xEDB88320 with init and xorout 0xFFFFFFFF.
  - `bool InputCurveEncode(InputCurveRecord &rec);` fills in the CRC. `bool InputCurveValid(const InputCurveRecord &rec);` checks magic, version and CRC.
  - `struct CurveStorage { virtual bool Read(const char *name, void *buf, size_t len) = 0; virtual bool Write(const char *name, const void *buf, size_t len) = 0; };`
  - `bool InputCurveLoad(CurveStorage &st, InputCurveRecord *out, int *slot);` returns the valid record with the highest `seq` (wrap-safe), or false.
  - `bool InputCurveSave(CurveStorage &st, InputCurveRecord rec);` sets `seq = newest + 1`, writes the slot not holding the newest valid copy (A when neither is valid), then reads it back and compares.
  - `InputCurveFiles.cpp`: `class PhzCurveStorage : public CurveStorage` writes inside `OC::RT::PersistenceWindow`.

- [ ] **Step 1: Write failing tests** (with an in-memory `CurveStorage` fake):
  - `roundtrip`
  - `crc_rejects_one_bit`
  - `ab_newest_valid_wins` (A seq 5, B seq 6 → B; after a save, A holds seq 7)
  - `torn_copy_falls_back` (newest copy corrupted → older valid one returned)
  - `seq_wrap` (0xFFFFFFFF then 0 → 0 is newer)
  - `no_files_uses_line_fit` (load returns false, so the caller uses `ICA1`)
  - `readback_mismatch_fails_save`
- [ ] **Step 2: Run** the tests. Expected: FAIL.
- [ ] **Step 3: Implement** the codec and the store. In `PhzConfig::migrate_legacy`, files whose names start with `ICURVE_` are held in the same priority pass as `GLOBALS.*`. In Backup, add the curve record to the dump and restore. A restore of a dump without one leaves the stored curves untouched, and old dumps still restore.
- [ ] **Step 4: Run** `./run-host-tests.sh && SAN=1 ./run-host-tests.sh` and build all 4 envs. Expected: pass.
- [ ] **Step 5: Commit** `cal routine: input-curve file codec, A/B store, migration priority and backup`.

### Task 5: Calibrated inputs for every app

**Files:**
- Create: `software/src/InputCal.h`, `software/src/InputCal.cpp` (owns the DTCM tables and the load/apply entry points)
- Modify:
  - `software/src/OC_io.cpp:51-72` (`IO::Read`)
  - `software/src/OC_ADC.h` (a shared calibrated accessor)
  - `software/src/OC_calibration.cpp` (`calibration_load` calls `InputCal::Reload()`)
  - the direct readers: `apps/H1200.h:1152-1153`, `apps/QQ.h:1575`, `apps/DQ.h:1461`, `apps/ASR.h:191`, `apps/Sequins.h:1437,1463`, `apps/ScopeApp.h:262` (voltage display), `apps/Calibr8or.h` (channel meter)
  - `software/src/CvMeter.cpp` (`InputMv` and the `v` stream use the shared calibrated path)
  - `docs/CV-Meter.md` (rewrite "What apps see")
- Test: `software/test/test_input_cal.cpp` (`suite test_input_cal ../src/InputCal.cpp ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/InputCurveStore.cpp`, with `-I host_stubs` if needed)

**Interfaces:**
- Consumes: Task 3 `CurveToValueTable`, `LineToValueTable`, `ApplyValueTable`; Task 4 `InputCurveLoad`; `OC::input_calibration` (`ICA1`).
- Produces (namespace `InputCal`):
  - `extern int32_t value_q8[8][17];` and `extern volatile uint8_t calibrated_mask;`, both in DTCM.
  - `void Reload();` builds the tables from the curve file if valid, else from the `ICA1` line, per input. It publishes by clearing a channel's mask bit, writing its table, then setting the bit, so the ISR never reads a half-written table.
  - `inline int32_t Value(int phys, uint32_t raw12, int32_t uncal_value)` returns the calibrated value when the bit is set, else `uncal_value`.
  - `inline int32_t Pitch(int32_t value)` returns `(value * 15360) >> 12` for calibrated channels. Uncalibrated channels keep `ADC::value_to_pitch`.
  - `OC_ADC.h`: `int32_t ADC::calibrated_value(ADC_CHANNEL ch)` and `int32_t ADC::calibrated_pitch(ADC_CHANNEL ch)`, the single accessor for direct readers. They respect the IO setting's filter choice (smoothed or unsmoothed raw), as `IO::Read` does.
- In `IO::Read`: for a calibrated channel, `value = InputCal::Value(phys, raw12, …)` and `pitch = InputCal::Pitch(value)`, both before `Attenuate`. Otherwise the existing lines run unchanged.

- [ ] **Step 1: Write failing tests:**
  - `uncalibrated_bit_exact`: with no curve and no `ICA1`, the new `Value`/`Pitch` path equals the stock `ADC::value`/`value_to_pitch` arithmetic for raw 0…4095 with the default and two non-default offsets and scales.
  - `curve_applied`: a known bent curve gives values within 1 count of the reference.
  - `line_fallback`: with no curve and a valid `ICA1` channel, the line is applied.
  - `mixed_channels`: channels 0 and 3 calibrated, the rest stock.
  - `old_unit_eeprom_image`: an EEPROM image from an old unit (as `test_cvmeter_math` builds one) loads with `calibrated_mask == 0`.
  - `publish_order`: a channel's mask bit is clear while its table is being written (observed through a write hook).
- [ ] **Step 2: Run** the tests. Expected: FAIL.
- [ ] **Step 3: Implement** `InputCal` and the `IO::Read` change. Move each direct reader that treats the reading as a voltage or pitch onto `ADC::calibrated_value`/`calibrated_pitch`. Leave the random seed (`Main.cpp:617`) and Sampler's sample stream raw, and add a comment in each saying why. Gates from `pitch_values` (`HSIOFrame.cpp:775`): confirm in the commit body that the shift is within the gate hysteresis.
- [ ] **Step 4: Run** `./run-host-tests.sh && SAN=1 ./run-host-tests.sh` and build all 4 envs. **Bench (Oren):** on `T41_console`, with a heavy app (Quadrants with applets) and all 8 inputs calibrated, the core ISR max is ≤ 40 µs with 0 missed ticks (`docs/Timing-Budget.md` method). Record the before and after figures.
- [ ] **Step 5: Commit** `inputs: per-input calibration curves applied for every app (IO::Read + shared accessor)`.

### Task 6: The calibration flow engine (pure, injectable)

**Files:**
- Create: `software/src/CalRoutineFlow.h`, `software/src/CalRoutineFlow.cpp`
- Modify: `software/src/CvMeter.cpp` (the `Q` command runs on `CalRoutineFlow`'s input part; the `INCAL,*` line formats are unchanged; golden fixture `software/test/fixtures/cvmeter_lines.txt` still passes)
- Test: `software/test/test_calroutine_flow.cpp` (`suite test_calroutine_flow ../src/CalRoutineFlow.cpp ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp`)

**Interfaces:**
- Consumes: Task 3 maths. The hardware sits behind interfaces:
  - `struct MeterPort { virtual bool Begin(const char *scpi, uint32_t timeout_ms) = 0; virtual int Poll(const char **reply) = 0; /* 0 busy, 1 done, <0 error/timeout/refused */ virtual bool Connected() const = 0; virtual const char *Identity() const = 0; };`
  - `struct OutputPort { virtual void Hold(int out, uint16_t code) = 0; virtual void ReleaseAll() = 0; virtual uint16_t TableCode(int out, int k) const = 0; virtual uint16_t CodeForMv(int out, const uint16_t table[11], int32_t mv) const = 0; };` Here `out` is the panel output 0–7; the glue maps it to the physical channel as `CvMeter.cpp:49-60` does.
  - `struct InputPort { virtual void StartWindow(int in) = 0; virtual bool WindowDone(CvMeterMath::PointStats *s) = 0; };`
  - `struct Clock { virtual uint32_t Ms() const = 0; };`
- Produces:
  - `class CalRoutineFlow` with `void Begin(MeterPort&, OutputPort&, InputPort&, Clock&);` and `void Tick();`, called from the app's `Loop()` and doing bounded work per call.
  - `void Continue();` (the owner confirmed the patch) and `void Cancel();` (releases everything and goes to `Idle`).
  - `Stage stage() const;` where `enum class Stage { Idle, NoMeter, ConfigMeter, PromptOutput, CalOutput, SweepOutput, PromptInput, CalInput, Results, Failed };`
  - `int current_output() const; int current_input() const;` and `const Results &results() const;`
  - `struct Results { uint16_t table[8][11]; bool out_pass[8]; bool at_rail[8][11]; float out_worst_mv[8]; int32_t out_worst_at_mv[8]; int16_t curve_mv_x2[8][17]; CvMeterMath::Fit line[8]; bool in_pass[8]; CvMeterMath::Verdict in_verdict[8]; float in_holdout_mv[8]; };`
  - `bool CanSave() const;` is true when all 8 `out_pass` are set.
  - A line sink for console logging: `void SetLog(void (*fn)(const char *line));`, emitting `CALR,…` lines.
- Meter setup sequence (allowlisted): `*CLS`, `CONF:VOLT:DC 20`, `VOLT:DC:NPLC 10`, `VOLT:DC:AZ ON`, `TRIG:SOUR IMM`. Per measurement: settle 150 ms after a code change, then 5 × `READ?` → `Summarize`; an untrusted reading gets one re-read, then the point fails NOISY. The input part uses output 0 (A) as the source at the sweep points, with the `Q` settle and measure timings (300 ms / 600 ms).

- [ ] **Step 1: Write failing tests** with fakes. The DAC fake is the bowed simulated DAC from Task 3; the meter fake reads that DAC plus noise; the input fake is a bent ADC that reads the DAC output.
  - `happy_path_all_pass`
  - `results_match_math` (the table equals what the Task 3 iteration produces)
  - `meter_timeout_releases_all` (the meter stops answering in `CalOutput` → `Failed`, `ReleaseAll` called, `CanSave()` false)
  - `cancel_any_stage_releases_all` (cancel from every stage)
  - `noisy_point_retries_not_stores`
  - `no_meter_stays_nometer` (`Stage::NoMeter`; no `Hold` ever called)
  - `holds_only_current_output` (only the output under test is ever held)
  - `failed_input_is_uncalibrated` (`in_pass[3] == false`, everything else saves)
  - `rail_does_not_block_save`
  - `q_lines_unchanged` (the `Q` flow through the new engine still prints the fixture's `INCAL` lines)
- [ ] **Step 2: Run** the tests. Expected: FAIL.
- [ ] **Step 3: Implement** `CalRoutineFlow`, then re-point `CvMeter`'s `Q` onto its input stage. `CvMeter`'s other commands are unchanged. `CalRoutineFlow` compiles in every env; `CvMeter` stays `PRINT_DEBUG`-only.
- [ ] **Step 4: Run** `./run-host-tests.sh && SAN=1 ./run-host-tests.sh` and build all 4 envs. Expected: pass.
- [ ] **Step 5: Commit** `cal routine: flow engine (outputs, sweep, inputs) behind injectable ports; Q runs on it`.

### Task 7: The Cal Routine app

**Files:**
- Create: `software/src/apps/CalRoutine.h`, `software/src/apps/CalRoutinePorts.cpp` (real `MeterPort` over `usbMeter`, `OutputPort` over `DAC::Hold` and `calibration_data.dac`, `InputPort` over `ADC::smoothed_raw_q8`, `Clock` over `millis`)
- Modify: `software/src/apps/_config.h` (append `AppCalRoutine` last, plus its `#include`), `software/src/OC_app_folders.h:99-108` (`{TWOCCS("CR"), FOLDER_SYSTEM}`), `tools/xeno-sim/Makefile` (app list), `software/src/apps/SETTINGS.h` (warning lines), the console (`CALR` lines via `SetLog` on `T41_console`), `software/test/fixtures/` (golden `CALR` lines)

**Interfaces:**
- Consumes: Task 6 `CalRoutineFlow`; Task 4 `InputCurveSave`/`PhzCurveStorage`; Task 5 `InputCal::Reload`; `OC::calibration_save()`, `OC::input_calibration_save()`, `autotune_calibration_data.Reset()`.
- Produces:
  - `OC_APP_CLASS(AppCalRoutine, TWOCCS("CR"), "Cal Routine", "Calibration")`, with app-data size 0.
  - A "Cal Routine calibration stored" marker: one byte in the gap `[4140,4164)`, holding magic and version, written on Save. Setup's DAC and ADC steps read it.

Behaviour:
- **Menu:** a list holding only "Self".
- **Self screens:** meter status (shows `*IDN?`), then "Plug output X into the meter (+ to X, COM to ground)", progress per node and sweep, "Patch output A → input N", and finally Results: worst mV per output and input, failed checks named, rail flags.
- **Save** uses a two-step confirm (`Backup.h` style, `kConfirmDeadMs = 350`). It writes:
  1. `calibration_data.dac` → `calibration_save()`
  2. the curve record → `InputCurveSave`
  3. `ICA1` lines → `input_calibration_save()`
  4. the marker
  5. `autotune_calibration_data.Reset()`
  6. `InputCal::Reload()`
- **Discard** writes nothing. encL cancels.
- On `SUSPEND`, `SCREENSAVER_ON` or app switch, the app calls `Cancel()`.
- **Setup warning:** when the marker is set, Setup's DAC steps and ADC steps each show one line: "Replaces Cal Routine calibration".

- [ ] **Step 1: Write failing tests:**
  - `test_calroutine_flow` gains `save_sequence_order` (a fake persistence layer records the call order 1–6 above).
  - The golden `CALR` fixture is checked by the flow test's log sink.
- [ ] **Step 2: Run** the tests. Expected: FAIL.
- [ ] **Step 3: Implement** the app and ports, register it last, and add the folder entry, the Makefile entry and the Setup warnings. Respect `kMaxApps`: the existing `_config.h:196` static_assert must still pass on every env.
- [ ] **Step 4: Run** the host tests, all 4 env builds, `make -C tools/xeno-sim check`, and `xeno-sim --app CR` (or the simulator's equivalent) to show the menu. Expected: all pass.
- [ ] **Step 5: Bench (Oren):** a full Self run on the Xenomorpher with the SDM3065X. The results screen shows every output ≤ 1 mV. After Save:
  - `d1 6000⏎` on the console measures 6.000 V ± 1 mV on the meter.
  - The `v` stream on a calibrated input agrees with the meter within the input's reported figure.
  - A quantizer app tracks a calibrated output looped into an input within that figure.
  - A meter unplug mid-run releases everything and saves nothing.
  - During a run, the loop pass max stays ≤ 5 ms and the core ISR max stays ≤ 40 µs (design gate 2).
- [ ] **Step 6: Commit** `apps: Cal Routine (Self): closed-loop output and input calibration with a USB DMM`.

### Task 8: Host tooling, docs and the bench checklist

**Files:**
- Modify: `tools/hwqa/cvmeter.py` (parse `CALR,…` and `METER,…`; `SimModule` gains a simulated meter), `tools/hwqa/test_cvmeter.py`
- Create: `docs/_apps/Cal-Routine.md` (operator page), `docs/bench-cal-routine-checklist.md` (design gates 1–6 with steps and pass conditions)
- Modify: `docs/App-and-Applet-Index.md` (a row), `docs/Saving-State.md` (the `ICURVE_A/B.BIN` files), `docs/bench-console.md` (the `U` command and `CALR` lines, if Task 2 left gaps)

- [ ] **Step 1: Write failing tests** in `test_cvmeter.py`: parse the golden `CALR` lines; an end-to-end run against `SimModule` with the simulated meter completes with all-pass results.
- [ ] **Step 2: Run** `python3 tools/hwqa/test_cvmeter.py`. Expected: FAIL.
- [ ] **Step 3: Implement** the parser and the simulated meter, then write the docs. The operator page covers what you need (a USBTMC meter on the host jack), the steps, what the results mean (1 mV = 1 cent), and what Setup / Calibrate does to a Cal Routine calibration. The checklist carries the design's gates 1–6, word for word.
- [ ] **Step 4: Run** `python3 tools/hwqa/test_cvmeter.py` and `./run-host-tests.sh`. Expected: pass.
- [ ] **Step 5: Commit** `docs+hwqa: Cal Routine operator page, bench checklist, CALR parser and simulated meter`.
