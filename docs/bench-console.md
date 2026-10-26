# Bench console cheat sheet

Operator reference for the USB serial debug console (T4.1 builds with
`PRINT_DEBUG`). Open the CDC port at any baud rate; output is plain text.

## Unlock: type `pew!`

The console ignores **all** input until the literal 4-character sequence
`pew!` arrives (once per boot). This is deliberate: Linux hosts
(ModemManager on the Jetson) AT/MBIM-probe every new CDC port, and that
byte soup has hit real commands (`D` froze the display, `C`/`F` are far
worse). You get `-=[ console unlocked ]=-` back when it lands.

> **Never send `pew!` to a console that may already be unlocked.** Once unlocked the
> four bytes are commands: `p` toggles the preset-bus overlay, `e` starts a bench-DMM
> SCPI line that swallows everything after it and sends it to the meter, `w` arms the
> card-patch prompt (a byte of the resident card image, which may be a backup, is
> patched by the digits that follow), and `!` is an unmapped key. Probe with a
> harmless command (`t`) first and unlock only if it draws no answer. While a Cal
> Routine run is open, `w` and `k` are refused (`CALR,BUSY_CAL`) as `m` and `x` are.

After unlocking, `z` prints the live key map (with current toggle states).
**Any key not listed below triggers a framebuffer screen capture** — so
stray typing is harmless but will spew capture bytes.

## Key map

### System
| key | action |
|---|---|
| `z` | print this key map with live toggle states |
| `t` | selftest — one-shot health report (see below) |
| `T` | real-time budget report: every counter with PASS/FAIL per ceiling (docs/Timing-Budget.md); `T` again within 3 s resets the counters. Both `t` and `T` also print `adc capture: realigns=<n>`: how many times the 8-input ADC capture was restarted after a lost reading (or by `X`); it should stay 0 |
| `X` | force 16 restarts of the 8-input ADC capture, each at a different point in the trigger cycle, then print `ADC,REALIGN,count=<n>` (the running total). A bench check that the input order survives a restart: with outputs patched to inputs, panel input n must still read as `v` column n afterwards |
| `a` | activate Captain MIDI (suspend/switch/resume, same path as the app menu; for headless benches where boot recall restored a different app) |
| `A` | list the app table (index, id, name), then type 2 decimal digits to switch to one by index — same path as `a`, any app. This is how a cross-app preset slot gets made from a headless bench |
| `I` | toggle app ISR |
| `D` | toggle display redraw |
| `L` | toggle app loop |
| `i` | scan all I2C addresses |
| `u` | dump USB host port device identities (VID/PID/product), then the bench DMM line `meter: vid=XXXX pid=XXXX connected=<0\|1>` (see `e` below; `0000` when none is attached), then the Captain MIDI per-device profile table |
| `j` | **press the panel** — then one key: `l` `r` `a` `b` `z` `x` `y` tap that button (capitals hold it), `[` `]` turn encL one detent, `-` `+` turn encR. Goes through the real UI event queue, so it reaches whatever screen is up; any other key cancels. This is how a headless bench walks the 200e app (`jl` on the module list starts a scan; `jr` picks the target) |

### Preset engine / bus
| key | action |
|---|---|
| `(` / `)` | **local** save / recall, slot 0 |
| `{` / `}` | **local** save / recall, slot 1 |
| `>` / `<` | **broadcast** save / recall, slot 0 — hits every module on the 200e bus |
| `.` / `,` | **broadcast** save / recall, slot 1 |
| `S` / `R` | **broadcast** save / recall to an arbitrary slot — then type 2 decimal digits (`00`–`29`) |
| `p` | toggle the preset-bus overlay UI (remote inspection of the front-panel preset manager) |
| `b` | preset-bus debug dump |
| `B` | toggle preset-bus verbose logging |
| `k` | toggle 0x50 card serving (slave-TX card emulator; hard-gated off when a live WPM is present) |
| `H` | **load a firmware-card image** into `FWCARD.BIN` (the host streams hex bytes, then `#` + 8 hex crc32 + 8 hex length; any host script that speaks this protocol works). Verifies length, `#!200EFU` header and CRC-32, writes the file, reads it back, then arms firmware-card mode. Any non-hex byte aborts. Refused while card serving is on |
| `M` | arm / disarm firmware-card mode (see below) |

### 200e master operations
| key | action |
|---|---|
| `q` | QUERY a module's identity — then type 2 hex digits for its address |
| `m` | master **BACKUP** (read a whole bank) — then 2 hex digits for the address |
| `c` | hex-dump the last capture held in the resident card image |
| `w` | patch one byte of the resident card image — 4 hex digits offset, then 2 hex digits value. No bus traffic |
| `x` | master **RESTORE** the resident card image to a module — 2 hex digits for the address, then press `x` and the **same two digits again within 3 s** to actually push it |
| `W` | send a **raw frame** on the general call — hex bytes, then Enter or a space (`045C221AFF` is a read-only QUERY of the 251e). **Anything goes: it can send a 259e PUT (`12`) and COMMIT (`0B`).** While a Cal Routine run is open (or a cancelled 259e run's COMMIT answer is still being read) it passes CAL frames only and refuses every PUT and COMMIT whatever the address: `rawframe: refused, a Cal Routine run is open (CALR,BUSY_CAL)` |
| `y` | USB bridge status; toggles the fallback `usbMIDI` poll (leave OFF under any app that reads USB MIDI itself) |

`w` and `x` together are a whole-bank write with none of the 200e app's guards
in front of them. `x` rewrites all 30 slots of the target module. A second `x`
with a **different** address inside the window is a fresh arm, not a
confirmation — it used to fire at the new address unconfirmed.

`m`, `x`, the broadcast save and recall keys (`S`, `R`, `<`, `>`, `,`, `.`), and
`W` frames that are not CAL frames refuse while a Cal Routine run is open or a
cancelled 259e run's COMMIT answer is still pending (`... refused, a Cal Routine
run is open (CALR,BUSY_CAL)`). Outside a run they are unguarded, so a 259e
calibration write session must not use them (see the 259e section of
[bench-cal-routine-checklist](bench-cal-routine-checklist)). `m` also refuses
while the firmware card is being served.

Every multi-character prompt (`x`, `q`, `w`, `A`, `N`, `V`, `j`, `d`, `Q`) eats
the next keystrokes as digits, and a scripted console helper that probes with a key
before each command will feed that probe into an open prompt. Send the digits
in the same write as the key, and never leave a prompt armed between calls.

The same goes for `pew!` itself: a helper that re-sends the unlock whenever a
command draws no reply will, on an already-unlocked console, type `p` (the
overlay opens), `e`, `w` (the patch prompt arms) and `!`. Two things make a
reply go missing on an unlocked console: a second process holding the port,
and a command that legitimately prints nothing. One `p` closes the overlay.

### CV meter, DAC hold, input calibration, frequency counter
Bench-instrument use of the module: the procedure, every line format and the
accuracy limits are in [CV-Meter](CV-Meter); `tools/hwqa/cvmeter.py` drives
all of it from a host and logs CSV.

| key | action |
|---|---|
| `v` | toggle the 50 Hz CV stream: `CV,<ms>,<seq>,<dropped>,<raw1..8>,<mv1..8>,<cal>,<clip>` (inputs in panel order; `cal` per input `C` = own calibration, `S` = shared scale + offset; `clip` `L`/`H` = a rail was hit). Starts with a `CV#` header and one `CVRANGE,<in>,<lo_mv>,<hi_mv>,<C/S>` per input. Never blocks: when the host is not reading, lines are dropped and counted |
| `d` | **hold an output** at a calibrated voltage until released, overriding the app on that output only: `d<o><mV>` then Enter (`o` = `1`-`8` or `a`-`h`, mV signed, e.g. `d3-1500⏎`) answers `DAC,<o>,HOLD,<mV>,<code>`; `d<o>x` releases one, `dx` all (`DAC,...,RELEASED`). RAM only |
| `Q` | **per-input calibration** against a DAC output: `Q<i><o>` one input, `Qa<o>` inputs 1-8 in turn; the module prompts for the patch, sets three voltages itself, fits, verifies, and stores only on `y` with the residual under 10 mV. `Qp` prints it, `Qc` twice within 3 s clears it. While the dialog is open **every byte belongs to it**; any key but `y` aborts |
| `n` | toggle the **frequency counter** on TR1: `FREQ,<ms>,<hz>,<periods>,<note>,<cents>` every 250 ms (or `FREQ,<ms>,NOSIG`). Refuses while an app owns the counter; TR1 is not a trigger input while it runs |

### Scope frequency counter (`%`)
The Scope counts the frequency of its trigger trace when that is an audio or CV input, and shows it on its bottom line (see the [Scope](Scope-App) page). These commands read and set it. The prefix is `%`, a character no other command uses; there is **no Enter**: a command ends with its last character. A byte that does not belong cancels the command and is swallowed (so a stray `%` can never turn the next byte into `p`, `w` or `x`), a pending `%` expires after 3 s, and the bytes that follow a cancel within 50 ms (the rest of what was typed) are swallowed too. A clock value needs its sign. The replies begin with `SFREQ,` (the `n` counter above prints `FREQ,`).

| keys | action |
|---|---|
| `%r` | one line: `SFREQ,<src>,<hz>,<sigma_cents>,<status>,<gate_ms>,<flags>,<seq>,<cal>`. `<src>` is the Scope's source (`INL`, `INR`, `OUTL`, `OUTR`, `CV1`-`CV8`); with the Scope closed or on a source that cannot be counted: `SFREQ,-,0.00000,0.00000,CLOSED,0,0,0,<cal>`. `<status>` is `OK`, `SETTLING` (an OK gate that the next one has not confirmed), `UNSTABLE`, `NO_SIGNAL`, `TOO_QUIET`, `LOST`, `TOO_LOW`, `TOO_HIGH` or `WAIT`; `<hz>` and `<sigma_cents>` are 0 unless OK, SETTLING or UNSTABLE; `<flags>` bit 0 = clipped; `<seq>` counts readings; `<cal>` is 1 once a crystal correction is set; `<rate>` is 0 while the source's sample rate is not confirmed on hardware, 1 otherwise (always 1 now: the CV rate was verified against the audio counter on 2026-10-25/26, ratio 0.999998). **A script must require `status` `OK` on two consecutive reads with different `seq` and compare them**: a dropped audio block, lost ADC frames or a save in progress leave a gap the counter cannot see, and it may still print a number |
| `%t` | one line, read-only: `STIME,<cycles64>,<audio_samples64>,<cv_frames64>,<seq>`. Three free-running counters read in one interrupts-off window (so they describe one instant): the ARM cycle counter extended to 64 bits (600 MHz, from the 24 MHz crystal), the audio samples the graph has processed (blocks of 128, counted whether or not the Scope is open) and the CV ADC frames consumed (also always on). `<seq>` counts replies. Integers only. The stamp is taken when the main loop reaches the command byte, not when the byte arrives: expect a few ms of jitter on the host side (`loop_max` is about 6 ms), so a host measuring the crystal against its own clock should send one every few seconds for an hour and fit a line through all of them rather than trust any one pair (`xclock.py` in the calibration skill does this). Counters start at boot, only differences mean anything; the CV frame word is 32 bits in the ISR and wraps after 32 h |
| `%c+0012.3` | set the crystal correction, ppm of the sample rate above nominal: a sign, **four digits**, a point, **one digit**, nothing after it (`%c-0003.5`, `%c+1000.0`). Answers `SFREQ,CLOCK,+12.3,SET,unsaved`. Past +-1000.0 ppm: `SFREQ,ERR,RANGE` and nothing changes; any other shape: `SFREQ,ERR,cancelled`. It is kept in RAM until a save (`g`, or any app-data save) writes it with the Scope's settings; a preset recall never changes it, a factory reset clears it |
| `%cx` | forget the correction (the Scope's line gets its `*` back) |
| `%gf` / `%gs` | fast (0.1 s, what the screen uses) or slow (1 s, for calibration) gate. The reading starts over |

The audio inputs are AC-coupled (1.6 Hz); put the Scope on a CV input for anything slower. The counter runs only while the Scope is open and not on the app menu.

### 259e calibration module, read-only (`=`)
Every letter of the console is taken, so this is `=`. It reads a 259e running the calibration firmware (31.2C4) over the 200e bus; it never writes: **there is no PUT and no COMMIT in `=`** (the raw-frame sender `W` above can send both; only the Cal Routine app is meant to). The bus address is two hex digits (259e `28`-`2B`, the 251e `5C`: **`=5Cs` only**; `=5Cr` and `=5Ca0` are 259e readers, wrong for a 251e's 38-byte record), then one command; no Enter.

| keys | action |
|---|---|
| `=28s` | status: `CALR,259E,28,STATUS,<flags hex>,<fmt>,<seq>` (flags: bit0 record valid, bit2 last commit ok, bit3 refused, bit4 no free slot) |
| `=28r` | the stored record: `CALR,259E,28,REC,<58 hex digits>,inferred_crc_lo=<2 hex digits>` (bytes 0 to 28 as read; byte 29, the CRC low byte, is inferred from the CRC of the first 28 and shown apart), or `REC,NONE` when the module has none |
| `=28a0` / `=28a1` | eight ADC words of channel 0 / 1: `CALR,259E,28,ADC,<ch>,<i>,<word>,<n>,FRESH\|SAME` (`n` is the module's sample counter; `SAME` means no new sample since the previous word) |

A failed request logs `CALR,259E,<aa>,ERR,<why>`: `BUS_BUSY` (the bus stayed busy for 1 s), `NO_REPLY` (a READ unanswered after 3 retries of 60 ms), `BAD_REPLY`, `BUS_ERROR` (the module did not ACK), `ADDRESS` (not a calibratable address), `START`. While the Cal Routine owns the meter, or a request is already running, it refuses with `CALR,259E,ERR,BUSY_CAL` or `CALR,259E,ERR,BUSY`. Any other byte after `=` cancels with `CALR,259E,ERR,cancelled`.

### USB bench DMM (USBTMC)
A USBTMC meter on the USB host port (or the host hub) is claimed by class
(interface `FE/03`), not by VID/PID; verified with a Siglent SDM3065X
(`F4EC:1203`). Only the allowlisted SCPI lines the calibration routine needs
can be sent: `*IDN?`, `*CLS`, `CONF:VOLT:DC <range>`, `VOLT:DC:NPLC <n>`,
`VOLT:DC:AZ <ON|OFF|ONCE>`, `TRIG:SOUR IMM`, `READ?`, `SYST:ERR?` (short or
long forms, any case). Everything else, every `CAL...` above all, is refused
before a byte reaches the meter.

| key | action |
|---|---|
| `e` | **one SCPI line to the meter**: `e<line>⏎`, e.g. `e*IDN?⏎`, `eCONF:VOLT:DC 20⏎`, `eREAD?⏎`. Printable characters up to Enter, at most 64; any control byte cancels (`METER,ERR,cancelled ...`), and an over-long line is cancelled with the rest of it swallowed up to its Enter. A line ending in `?` waits for the reply; any other completes when the meter accepts it. An optional `@<ms> ` prefix sets that query's timeout (1-60000 ms, default 5000), e.g. `e@200 READ?⏎`; it is console syntax and never reaches the meter. Answers, printed when the query ends (the console keeps working meanwhile; 5 s timeout): `METER,DONE,<reply>` (empty for a command), `METER,REFUSED` (not on the allowlist), `METER,TIMEOUT`, or `METER,ERR,<why>` with `<why>` one of `NOT_CONNECTED`, `BUSY`, `DISCONNECT` (unplugged mid-query), `BAD_REPLY`, `OVERFLOW` (reply over 255 chars), `NO_PROGRESS`, `OUT_STUCK` (an earlier command never left the controller), `XFER_ERR_IN` / `XFER_ERR_OUT` (a bulk transfer failed: STALL, babble or transaction errors), `RECOVERING` (the resync below is still running: retry in a moment), `RECOVER_FAILED` (the resync gave up after 3 s; the next query is allowed anyway), `QUEUE`, `FRAME` |

After any query that fails mid-exchange (timeout, garbled reply, bulk
transfer error) the driver resynchronises the meter on its own before it
accepts the next one, without blocking anything: CLEAR_FEATURE(ENDPOINT_HALT)
on whichever bulk endpoint errored, then USBTMC INITIATE_CLEAR and
CHECK_CLEAR_STATUS every 10 ms (reading out anything the meter still had
queued), then a Bulk-OUT halt clear. A late answer to an abandoned query is
recognised by its tag and discarded, so it cannot shift every later reply by
one. No replug is needed.

The plan called this key `U`; `U` is already the audio-graph reorder
(`docs/Timing-Budget.md`), so the meter is on `e`.

**Bench pending (Oren, with the bench owner's agreement): flash `T41_console`
and check, then record the results here.**

- [ ] `u` shows `meter: vid=F4EC pid=1203 connected=1`.
- [ ] `e*IDN?⏎` prints `METER,DONE,Siglent Technologies,SDM3065X,...`.
- [ ] `eCONF:VOLT:DC 20⏎` then `eREAD?⏎` returns a reading.
- [ ] `eCAL:VAL 1⏎` prints `METER,REFUSED`.
- [ ] Unplugging the meter mid-`READ?` gives `TIMEOUT` or `ERR`, and the console keeps working.
- [ ] USB-MIDI on the host hub still works.
- [ ] 10 meter replugs with no module USB drop (design gate 6).
- [ ] Unplug the meter during a pending `eREAD?`, plug it back in, and the next `eREAD?` works without any further replug.
- [ ] Forced timeout: `eVOLT:DC:NPLC 100⏎`, then `e@200 READ?⏎` gives `METER,TIMEOUT`. A query sent within ~3 s may answer `METER,ERR,RECOVERING`. After that, `eVOLT:DC:NPLC 1⏎` and `eREAD?⏎` succeed with a fresh reading (not the late one), with no replug. Repeat 3 times to make sure the replies do not drift off by one.

### Files
| key | action |
|---|---|
| `g` | save globals + app data now |
| `l` | list LittleFS files |
| `s` | list SD card files |
| `h` | LittleFS health: cost of one `open()`, root metadata-log fill, blocks used |
| `r` | print LittleFS `CRASH.LOG` |
| `K` | ButtonWatch — name the physical buttons as you press them |

### DANGER — destructive
Each of these needs the **same key pressed twice within 3 s**; the first press
only arms it and prints what the second one will do. That is a guard against
typos and host probes, not a substitute for care — there is no undo behind any
of them.

| key | action |
|---|---|
| `C` | **clear/reset the config file** (GLOBALS.CFG rewritten empty) |
| `F` | **erase ALL LittleFS files** — presets, config, CRASH.LOG, everything |
| `Z` | **reboot into HalfKay** so a host can flash — stops playing (see "Flashing headlessly") |

## Cal Routine log lines (`CALR`)

While the Cal Routine app runs it logs one line per step on the console. The format is defined by `software/src/CalRoutineFlow.cpp` and the golden file `software/test/fixtures/calr_lines.txt` (its `CALR#` header lines name each kind's fields in order). Lines are `CALR,<KIND>,<field>,...`, comma-separated, outputs and inputs 1-based:

- `MODE` the routine: `OUTPUTS`, `INPUTS` or `SELF`
- `WARN` `NO_MARKER`: Inputs was started on outputs Cal Routine did not calibrate (the owner may go on)
- `METER` the meter's identity
- `STATE` timestamp (ms), stage, current output and input
- `OUT` an output starts
- `NODE` one table entry: target, code, measured value, iterations, rail flag
- `REREAD` a repeat reading of a table entry and its spread
- `SWEEP` one in-between check point and its residual
- `ORES` an output's result: PASS or not, reason, worst error, rail mask
- `PATCH` the patch check, one per output raised alone: the inputs it moved (hex mask) and its own input's shift in raw counts (about 1960 when patched)
- `PRES` the patch check's result: PASS, FAIL or NO_READING (a window gave no samples; masks 00), outputs that moved another input (`crossed`), outputs that did not move their own (`missing`), hex masks
- `IN` an input's span: its source output (output n for input n) and covered span (`lo_mv`, `hi_mv`); all 8 at the start of the sweep
- `IPT` one input measurement point (all 8 inputs are logged for each sweep voltage)
- `IRES` an input's result: verdict, gain, offset, hold-out and line deviation
- `RETRY` a meter query was repeated
- `MERR` the meter's error queue was not empty after its setup (`SETUP`) or an output's autozero (`ZERO`): the reply; the run then fails `METER_CONFIG`
- `LOST` / `BACK` / `REDO` the meter was lost, came back, the output restarts
- `RESULT` can-save flag and the output and input pass masks
- `SAVE` the save result; `DISCARD`; `CANCEL` (stage); `FAIL` (reason)

`tools/hwqa/cvmeter.py` parses them (`parse_calr`).

## Reading the selftest (`t`)

```
=== selftest ===
build: Sep  1 2026 17:53:02 image=624640 etext=0005F718
uptime=123s  reset_cause(SRC_SRSR)=00000001
rtc: 1788285220 s = 17:53:40 on day 20697 since 1970-01-01
watchdog: armed (128s, fed from loop)
loop rate ~4200 Hz
core delta5ms=83 (expect ~83)
heap free: 380000 bytes (RAM2)  holes=0 hole_free=0 largest_run=380000
audio i16 pool: 4 now / 9 max   cpu: 8.4% now / 12.1% max
audio f32 pool: 6 now / 11 max
littlefs: 132KB/4096KB used
fs write-verify: PASS
CRASH.LOG: none (no crashes recorded)
bus rings hw: ev=3 midi_rx=0 midi_tx=0  stuck=0/0
captain: poll_gap_max=1180us echoes=0
=== selftest done ===
```

- **build** — Main.cpp's compile time plus the linked image length and ITCM
  end. The same line heads every CRASH.LOG entry, because a CrashReport's
  code address is only meaningful against the ELF that produced it; match
  this line to know whether an entry came from the firmware that is running.
- **rtc** — what the RTC reads, as a wall-clock time of day. CrashReport
  stamps entries with this clock ("system time"), so this is how to date
  them. The loader sets it from the host at flash time.
- **reset_cause** — raw `SRC_SRSR` hex. A ` [WDOG]` suffix means the last
  reboot was a watchdog timeout, i.e. `loop()` wedged and the module
  self-recovered. Investigate CRASH.LOG and the bus stats.
- **watchdog** — should read `armed` in normal operation (WDOG1, 128 s,
  fed only from `loop()`; armed at the end of `setup()` so interactive
  boot menus can still block).
- **loop rate** — Hz averaged since the *previous* `t`; absent on the
  first press. A collapsing rate means something in loop context is
  blocking.
- **core delta5ms** — core ISR ticks counted across a 5 ms delay.
  The core timer runs at 16.67 kHz, so **~83 = healthy**. Much lower
  means the app ISR is disabled (`I`) or the core timer is being starved.
- **heap free** — RAM2/OCRAM heap headroom: the untouched top of the pool
  only. `holes` / `hole_free` count memory that was malloc'd and freed
  again (below the break); `largest_run` is the biggest single block malloc
  could return now. `hole_free` large with `largest_run` small is
  fragmentation; everything small is true exhaustion. `largest_run` is a
  chunk size: the biggest `malloc(n)` that can succeed is `largest_run - 8`.
  If the walk met an invalid node (the list changed under it) a separate
  `heap walk truncated=1` line follows and the figures are partial.
- **audio pools / cpu** — i16 and F32 block pools, current/high-water,
  plus audio CPU now/peak. A pool `max` at the pool ceiling means
  allocation failures have likely occurred.
- **littlefs** — bytes used against the 4 MB internal partition. Read it as a
  hint, not a budget: `LittleFS_Program` uses **64 KB erase sectors** on T4.1,
  so the partition is only **64 allocation blocks** and *every file costs a
  whole block* however little it holds. A 588-byte file measured on the bench
  occupied 64 KB. Running out of blocks with kilobytes of "free space" showing
  is the expected failure, and it is why a preset slot is now one container
  file (`PB_NN.PBS`) instead of the three-to-five it used to be — 30 slots
  × 3 files is 90 blocks against a 64-block filesystem. Use `l` to count files.
- **fs write-verify** — writes a 64-byte pattern to `SELFTST.TMP`, reads
  it back, compares, deletes. **FAIL** catches the failure mode where
  writes "succeed" as 0-byte files — treat the flash filesystem as
  untrustworthy until reformatted (`F`, after salvaging what `l` shows).
- **CRASH.LOG** — crash forensics. On any crash, CrashReport is captured
  at the next boot and appended to `CRASH.LOG` on LittleFS (rotated at
  8 KB), so unattended crashes leave evidence even when no console was
  watching. `none` is the happy state; if present, the size is shown and
  the file persists until deleted.
- **bus rings hw** — preset-bus ring high-water marks (event ring, MIDI
  RX/TX queues) since boot. `stuck=recovered/detected` counts bus-stuck
  events (SDA/SCL held low ≥3 s with zero RX) and how many the staged
  recovery (engine reset → 9 SCL pulses + STOP → full re-init) cleared.
- **captain** — worst-case gap between Captain MIDI polls since the last
  `t` (resets on read) and the SysEx echo count. Poll gaps in the
  low-millisecond range are normal; tens of ms means loop() is stalling.

## SysEx echo latency probe

Captain MIDI answers an echo command from its normal loop-context SysEx
path, so round-trip time measures real end-to-end MIDI latency:

```
send:  F0 7D 62 4D 01 0B <token: up to 8 data bytes> F7
reply: F0 7D 62 4D 01 4B <same token> F7
```

(`7D 62 4D` = manufacturer/device header, `01` = protocol version,
`0B` = SYX_ECHO, `4B` = SYX_ECHO_R. See `docs/hoc-midi-sysex.md`.)

Each probe also increments the `echoes` counter shown by selftest `t`.
Reference bench numbers over USB MIDI (200 probes): median 0.17 ms,
p95 0.24 ms, max 0.51 ms, 0 drops — ~90 µs one-way.

## Recovery story

1. **Wedged loop()** — the hardware watchdog (WDOG1, 128 s) reboots the
   module instead of leaving it bricked until power-cycle. The timeout is
   long enough that the slowest legitimate blocking op (a full LittleFS
   format, ~45 s) never trips it. After the reboot, boot recall restores
   the last bus preset (app, state, globals) automatically, and the crash
   evidence lands in CRASH.LOG.
2. **Corrupt config** — GLOBALS.CFG is backed up to GLOBALS.BAK once per
   boot after a good load; a corrupt primary restores from the backup
   silently instead of dropping into the factory-reset prompt.
3. **Truly wedged** (pre-watchdog-arm hang, or repeated watchdog loops):
   **power-cycle**. State comes back via boot recall.

## Flashing

**Use `software/flash.sh`.** It builds locally, then does everything else on
the bench rig over ssh (`RIG=orin` by default, overridable). In order: build;
refuse any image that is not linked for slot 0; `scp` it across and stage a
rollback it will not overwrite; SIGTERM the host-side player (`ember.py`, whose
shutdown path drives all six continuous outputs to zero, so a reboot with CVs
patched into a live Buchla is not a transient into someone's speakers); flash;
and verify by enumeration — USB ID plus a `Xenomorpher` ALSA card, retried for
30 s. Note it quiets the *host*, not the firmware: nothing stops the running
app on the module itself.

`./flash.sh` alone builds `T41_audio`; pass an environment name for anything
else, and it is still address-checked.

### The trap it exists to prevent

`platformio.ini` gives each environment its own linker script:

| env | ldscript | base | bootable alone? |
|---|---|---|---|
| `T41` | `slot1.ld` | 0x60100000 | **no** |
| `T41_audio` | `slot0.ld` | 0x60000000 | **yes — this is what the module runs** |
| `T41_MTP` | `slot2.ld` | — | no |

A slot-1 image programs perfectly and reports `Booting`. The RT1062 then
finds nothing at 0x60000000, falls into its ROM downloader, and enumerates as
`1fc9:0135 "SE Blank RT Family"` — which is indistinguishable from a dead
module. On 2026-08-27 that consumed an evening, two full Teensy restores
(which erase EEPROM, so **calibration had to be redone**), and a lot of
pressing a PROGRAM button this board does not expose. Check the first record
of the hex: `:02000004` **`6000`** means slot 0.

## Flashing headlessly

The Teensy's PROGRAM button is **not reachable** once this board is mounted,
so the advice inherited from upstream — "press the program button" — does not
apply, and neither does `teensy_loader_cli -s`: a soft reboot still needs the
running firmware to enter the bootloader for it. Two ways in:

- **Front panel:** Setup/About → **hold encL** past the long-press (the footer
  reads `keep holding: Reflash` while you hold), then **B** on the
  `REFLASH: BOOTLOADER` confirm screen. It ignores a yes for 350 ms after the
  screen appears.
- **From a host:** console **`Z`**, double-press like `C` and `F`. It calls
  `_reboot_Teensyduino_()` and drops straight into HalfKay.

```
# on the rig, with NOTHING else holding the serial port
python3 -c "import os; fd=os.open('/dev/orin/xenomorph', os.O_RDWR); os.write(fd, b'ZZ')"
teensy_loader_cli --mcu=TEENSY41 -w -v firmware.hex
```

**Free the serial port first.** Anything holding `/dev/ttyACM*` — a logger, a
capture script — blocks both the console write and the loader, and the
loader's failure reads "Error opening USB device… press the reset button",
which sends you hunting for a button that does not exist. That is exactly how
an afternoon went missing on 2026-08-27.

## Firmware-card mode: flash a 200e module with no WPM and no USB card

The USB-MIDI SysEx bridge now performs the `H` (load an image), `M` (arm/disarm) and `b` (progress) functions for a host app, so a flash no longer needs this console; see `docs/bus200e-sysex-fw.md`.

A module's bootloader (power on with its Remote / load button held) reads a v1
firmware card image from address 0x50 **once, right after power-up**. A WPM
answers instantly because it is always on; the Xenomorpher used to start
answering ~6 s after reset, far too late.

1. Load the image: `H` (see the key map), or the host tool. The image goes to
   `FWCARD.BIN`, never `PBCARD.BIN`, so bank backups are untouched. The
   Xenomorpher arms itself (`FWCARD.ARM`).
2. **Take any WPM off the bus** (a device already answering 0x50 makes the
   Xenomorpher refuse to serve: two cards on one bus corrupt each other).
3. Power-cycle the case with the module's load button held. While armed, the
   Xenomorpher answers 0x50 from `setup()` before `Serial.begin()` (USB waits up
   to 2 s for a host), `Pinout_Detect()` (~2 s ADC read) and the crash-report
   wait: serving starts ~0.3 s after reset. The boot POLL_DONE is held back.
4. When the module has read the whole image (TOC data end) and the bus has been
   quiet for 30 s the mode disarms itself and the next boot is normal.

`b` prints `fwcard: armed/serving/used`, the boot timeline, and the time of the
module's first read. `M` toggles the arm flag by hand (e.g. to cancel). While
serving, the 0x50 slave is read-only in effect: nothing is flushed to flash.
Verified on a 272e (31.0b1 -> 0.rc.1 community build, 54,186 bytes read).
