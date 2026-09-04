#!/usr/bin/env python3
"""Drive the Xenomorpher's panel over USB serial and read its screen back.

This is xeno-sim's --keys/--dump-fb, aimed at the real module. The console
build (T41_console) already carries both halves: 'j' plus one character presses
a control, and any unrecognised byte asks for a framebuffer capture, which
arrives as 2048 hex characters -- byte for byte the same format the simulator
dumps, so tools/xeno-sim/fbtext.py decodes either one.

What it is for: checking on hardware the things the simulator cannot reach.
The simulator builds six apps and neither Hemisphere nor Quadrants; the module
builds all of them. A check that passes in the simulator and a check that
passes here are different claims, and only the second one is about the
instrument.

TWO THINGS TO KNOW BEFORE USING IT:

The console ignores every byte until it receives the literal "pew!", and
re-locks on every reboot and every flash. Silence is the locked state, not a
dead module -- hosts probe new CDC ports with AT/MBIM byte soup and real
commands were being hit by it ('D' froze the display, '(' fired preset saves).

Capture is triggered by the DEFAULT branch of the console's command switch, so
the capture byte must be one the firmware does not recognise. '~' is used here.
Do not substitute a letter without checking Main.cpp: 'C' and 'F' are
destructive (arm-and-confirm), 'f' starts a file transfer that then eats the
next 11 bytes as a header, and 'A' eats the next two as an app index.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial required: python3 -m venv venv && venv/bin/pip install pyserial")

# 'j' takes one more character naming the control. Lower case taps, upper case
# holds; the brackets turn the left encoder and the signs turn the right one.
# Straight from the firmware's own help line (Main.cpp, case 'j').
PRESS_CHARS = set("lrabzxy") | set("LRABZXY") | set("[]-+")

FRAME_BYTES = 1024          # 128x64 / 8, the panel's framebuffer
FRAME_HEXCHARS = FRAME_BYTES * 2
CAPTURE_BYTE = b"~"         # must NOT be a command the firmware knows


class Panel:
    def __init__(self, port, baud=115200, verbose=False):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.verbose = verbose
        # Opening the port asserts DTR and can reset some hosts' view of it;
        # give the CDC endpoint a moment before the unlock goes out or the
        # first bytes land in nothing.
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def log(self, *a):
        if self.verbose:
            print(*a, file=sys.stderr)

    def unlock(self):
        """Send the console's unlock token. Required after every boot/flash."""
        self.ser.write(b"pew!")
        self.ser.flush()
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.log("unlocked")

    def chord(self, mod, key):
        """Press `key` while `mod` is held -- the shape of every global gesture.

        The console's 'o' command, added for exactly this: injection could only
        express complete single presses before, so no chord-opened screen (the
        app switcher, the I/O menus, the preset-bus overlay, the screensaver)
        could be reached from the bench at all.
        """
        if mod not in "lrabzxy" or key not in "lrabzxy":
            raise ValueError("chord takes two of l r a b z x y: modifier then key")
        self.ser.write(b"o" + mod.encode() + key.encode())
        self.ser.flush()
        self.log(f"chord {mod}+{key}")

    def press(self, spec):
        """Press one control. `spec` is one character from PRESS_CHARS."""
        if spec not in PRESS_CHARS:
            raise ValueError(f"unknown control {spec!r}; valid: {''.join(sorted(PRESS_CHARS))}")
        self.ser.write(b"j" + spec.encode())
        self.ser.flush()
        self.log(f"press {spec}")

    def keys(self, script, settle=0.12):
        """Run a comma-separated script of presses, e.g. 'a,],],r'.

        A bare number is a pause in milliseconds, so a script can wait out a
        hold threshold (the chord card's is 700ms) without the caller sleeping
        around the call.
        """
        for tok in [t.strip() for t in script.split(",") if t.strip()]:
            if tok.isdigit():
                time.sleep(int(tok) / 1000.0)
                continue
            if "+" in tok and len(tok) == 3:      # "a+r": hold a, press r
                self.chord(tok[0], tok[2])
                time.sleep(settle)
                continue
            self.press(tok)
            time.sleep(settle)

    def capture(self, timeout=6.0):
        """Ask for the framebuffer and return it as a 2048-char hex string.

        The firmware sends it in 32-byte chunks paced ~950us apart and ends the
        frame with a newline, so this accumulates hex until it has a full frame
        rather than reading a fixed number of bytes.
        """
        self.ser.reset_input_buffer()
        self.ser.write(CAPTURE_BYTE)
        self.ser.flush()

        buf = []
        got = 0
        deadline = time.time() + timeout
        while got < FRAME_HEXCHARS and time.time() < deadline:
            chunk = self.ser.read(256).decode("ascii", "ignore")
            if not chunk:
                continue
            # The console prints other lines too (debug output, command
            # acknowledgements). Keep only hex, which is what the frame is.
            hexonly = "".join(c for c in chunk if c in "0123456789abcdefABCDEF")
            buf.append(hexonly)
            got += len(hexonly)
        out = "".join(buf)
        if len(out) < FRAME_HEXCHARS:
            raise TimeoutError(
                f"short frame: {len(out)}/{FRAME_HEXCHARS} hex chars "
                f"(is the console locked? it re-locks on every flash)"
            )
        return out[:FRAME_HEXCHARS]

    def close(self):
        self.ser.close()


def main():
    ap = argparse.ArgumentParser(description="Drive the Xenomorpher panel over serial.")
    ap.add_argument("--port", default="/dev/cu.usbmodem192573201")
    ap.add_argument("--keys", default="", help="press script, e.g. 'a+r,],],r' (a+r = chord; bare number = ms pause)")
    ap.add_argument("--dump-fb", action="store_true", help="print the framebuffer as hex")
    ap.add_argument("--settle", type=float, default=0.12, help="seconds between presses")
    ap.add_argument("--no-unlock", action="store_true", help="skip the pew! unlock")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    p = Panel(args.port, verbose=args.verbose)
    try:
        if not args.no_unlock:
            p.unlock()
        if args.keys:
            p.keys(args.keys, settle=args.settle)
            time.sleep(0.25)
        if args.dump_fb:
            print(p.capture())
    finally:
        p.close()


if __name__ == "__main__":
    main()
