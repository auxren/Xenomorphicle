# Flashing the Xenomorpher from the Orin

Teensy 4.1 Buchla 4U module on the 200e preset bus. Written by the
Xenomorphicle session; any session on this machine can follow it.

## Device map

| what | where |
|---|---|
| Xeno console (CDC) | `/dev/ttyACM0` — verify via `/dev/serial/by-id/usb-Teensyduino_Phazerville_*` |
| ESP32 (unrelated) | `/dev/ttyACM1` — do not touch |
| running firmware | USB `16c0:048a` |
| bootloader | USB `16c0:0478` (HalfKay) |
| audio / MIDI | ALSA card 2 (`hw:2,0`), `/dev/snd/midiC2D0` |

## Build (on the Mac)

```
cd /Users/oren/Documents/GitHub/Xenomorphicle/software
pio run -e T41_audio_dbg          # NOT plain T41 - the module runs the dbg env
scp .pio/build/T41_audio_dbg/firmware.hex orin:/tmp/xeno_new.hex
```

On the Orin keep a rollback:
`cp /tmp/xeno.hex /tmp/xeno_prev.hex; mv /tmp/xeno_new.hex /tmp/xeno.hex`

## Flash

```bash
# 1. the serial logger holds the port - kill it BY PID first
LOG=$(pgrep -f "[x]eno_capture" | head -1); [ -n "$LOG" ] && kill $LOG

# 2. bootloader touch: open the CDC port at 134 baud
python3 -c 'import serial; s=serial.Serial("/dev/ttyACM0",134); s.close()'
sleep 3

# 3. flash
teensy_loader_cli --mcu=TEENSY41 -v /tmp/xeno.hex

# 4. the FIRST attempt often fails with "error writing to Teensy".
#    Retry ONLY if HalfKay is still present:
sleep 2
lsusb | grep -q 16c0:0478 && teensy_loader_cli --mcu=TEENSY41 -v /tmp/xeno.hex

# 5. restart the logger
sleep 10
setsid nohup python3 /home/auxren/xeno/xeno_capture.py >/dev/null 2>&1 < /dev/null &
```

**Never put `-w` on an unconditional retry.** If attempt 1 actually
succeeded, `-w` blocks forever waiting for a bootloader that already
booted. That hang is the most common way this looks broken.

## Verify (console, 115200)

The console is LOCKED after boot until it receives the literal `pew!`
(ModemManager-style probe bytes were firing real commands).

**Do not blindly send `pew!`** — if it is already unlocked those four
bytes are typed as COMMANDS, and `p` toggles a preset overlay on screen.
Correct probe: send `t` and read; if there is no `=== selftest ===`,
THEN send `pew!` and retry `t`.

Keys: `t` selftest, `b` preset-bus dump, `u` USB host devices + profiles,
`a` switch to Captain MIDI, `k` toggle 0x50 card serving. Any unmapped key
dumps the framebuffer as 2048 hex chars (SSD1306, 8 pages x 128 cols,
LSB = top pixel).

Healthy selftest: `watchdog: armed`, `core delta5ms` 83-85,
`fs write-verify: PASS`.

## Traps

- **pgrep/pkill self-match.** Over ssh the remote shell command line
  contains your pattern, so `pkill -f xeno_capture` kills the ssh command
  too (exit 255, truncated output). Bracket it: `"[x]eno_capture"`, or kill
  by numeric PID from a separate call.
- **Heredocs over ssh.** Apostrophes in the text terminate the outer
  single-quoted ssh command. Write the file locally and `scp` it.
- **panel.py holds the audio capture device** (hw:2,0) and respawns
  arecord. For exclusive audio: SIGSTOP panel.py PID, kill its arecord
  child, work, SIGCONT in a `finally`. Working implementation: `AudioClaim`
  in `~/xeno/bench_regression.py` — copy it, do not rewrite.
- **Wedged (not crashing)**: enumerates, but console AND the 134-baud touch
  are both dead, with no re-enumeration cycling. Recovery: arm a loop that
  watches for `16c0:0478` and flashes the instant it appears, then have
  Oren power-cycle the case (boot window is about 2s). KILL any
  bootloader-toucher loop right after recovery, or it knocks the good
  firmware straight back into HalfKay.
- **LittleFS survives reflashing** — presets and device profiles persist.
  Never reformat to "fix" a flash.

## Safety

Live Buchla 200e bus with the user's instrument attached. Never format the
filesystem, never factory-reset, never touch the WPM config. Flash only
compile-verified builds. If the device goes unresponsive, stop and report
rather than retrying blind.
