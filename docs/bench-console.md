# Bench console cheat sheet

Operator reference for the USB serial debug console (T4.1 builds with
`PRINT_DEBUG`). Open the CDC port at any baud rate; output is plain text.

## Unlock: type `pew!`

The console ignores **all** input until the literal 4-character sequence
`pew!` arrives (once per boot). This is deliberate: Linux hosts
(ModemManager on the Jetson) AT/MBIM-probe every new CDC port, and that
byte soup has hit real commands (`D` froze the display, `C`/`F` are far
worse). You get `-=[ console unlocked ]=-` back when it lands.

After unlocking, `z` prints the live key map (with current toggle states).
**Any key not listed below triggers a framebuffer screen capture** — so
stray typing is harmless but will spew capture bytes.

## Key map

### System
| key | action |
|---|---|
| `z` | print this key map with live toggle states |
| `t` | selftest — one-shot health report (see below) |
| `T` | real-time budget report: every counter with PASS/FAIL per ceiling (docs/Timing-Budget.md); `T` again within 3 s resets the counters |
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
| `y` | USB bridge status; toggles the fallback `usbMIDI` poll (leave OFF under any app that reads USB MIDI itself) |

`w` and `x` together are a whole-bank write with none of the 200e app's guards
in front of them. `x` rewrites all 30 slots of the target module. A second `x`
with a **different** address inside the window is a fresh arm, not a
confirmation — it used to fire at the new address unconfirmed.

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
| `e` | **one SCPI line to the meter**: `e<line>⏎`, e.g. `e*IDN?⏎`, `eCONF:VOLT:DC 20⏎`, `eREAD?⏎`. Printable characters up to Enter, at most 64; any control byte cancels (`METER,ERR,cancelled ...`), and an over-long line is cancelled with the rest of it swallowed up to its Enter. A line ending in `?` waits for the reply; any other completes when the meter accepts it. Answers, printed when the query ends (the console keeps working meanwhile; 5 s timeout): `METER,DONE,<reply>` (empty for a command), `METER,REFUSED` (not on the allowlist), `METER,TIMEOUT`, or `METER,ERR,<why>` with `<why>` one of `NOT_CONNECTED`, `BUSY`, `DISCONNECT` (unplugged mid-query), `BAD_REPLY`, `OVERFLOW` (reply over 255 chars), `NO_PROGRESS`, `OUT_STUCK` (an earlier command never left the controller: replug the meter), `QUEUE`, `FRAME` |

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

## Reading the selftest (`t`)

```
=== selftest ===
build: Sep  1 2026 17:53:02 image=624640 etext=0005F718
uptime=123s  reset_cause(SRC_SRSR)=00000001
rtc: 1788285220 s = 17:53:40 on day 20697 since 1970-01-01
watchdog: armed (128s, fed from loop)
loop rate ~4200 Hz
core delta5ms=83 (expect ~83)
heap free: 380000 bytes (RAM2)
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
- **heap free** — RAM2/OCRAM heap headroom.
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
