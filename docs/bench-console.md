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
| `a` | activate Captain MIDI (suspend/switch/resume, same path as the app menu; for headless benches where boot recall restored a different app) |
| `I` | toggle app ISR |
| `D` | toggle display redraw |
| `L` | toggle app loop |
| `i` | scan all I2C addresses |
| `u` | dump USB host port device identities (VID/PID/product) + the Captain MIDI per-device profile table |

### Preset engine / bus
| key | action |
|---|---|
| `(` / `)` | **local** save / recall, slot 0 |
| `{` / `}` | **local** save / recall, slot 1 |
| `>` / `<` | **broadcast** save / recall, slot 0 — hits every module on the 200e bus |
| `.` / `,` | **broadcast** save / recall, slot 1 |
| `p` | toggle the preset-bus overlay UI (remote inspection of the front-panel preset manager) |
| `b` | preset-bus debug dump |
| `B` | toggle preset-bus verbose logging |
| `k` | toggle 0x50 card serving (slave-TX card emulator; hard-gated off when a live WPM is present) |

### Files
| key | action |
|---|---|
| `g` | save globals + app data now |
| `l` | list LittleFS files |
| `s` | list SD card files |

### DANGER — destructive, no confirmation
| key | action |
|---|---|
| `C` | **clear/reset the config file** (GLOBALS.CFG rewritten empty) |
| `F` | **erase ALL LittleFS files** — presets, config, CRASH.LOG, everything |
| `Z` | **reboot into HalfKay** so a host can flash — stops playing (see "Flashing headlessly") |

## Reading the selftest (`t`)

```
=== selftest ===
uptime=123s  reset_cause(SRC_SRSR)=00000001
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

**Use `software/flash.sh`.** It builds, refuses any image that is not linked
for slot 0, stages a rollback it will not overwrite, stops the running app so
no CV transient reaches the Buchla, flashes, and verifies by enumeration.

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

- **Front panel:** SETTINGS → **Reflash** ("Flash Upgrade Mode").
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
