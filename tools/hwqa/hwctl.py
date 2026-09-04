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

THREE THINGS TO KNOW BEFORE USING IT:

The console ignores every byte until it receives the literal "pew!", and
re-locks on every reboot and every flash. Silence is the locked state, not a
dead module -- hosts probe new CDC ports with AT/MBIM byte soup and real
commands were being hit by it ('D' froze the display, '(' fired preset saves).

THE UNLOCK TOKEN IS ITSELF MADE OF COMMANDS, so it must never be sent to a
console that is already unlocked. Main.cpp only feeds the unlock shift register
while LOCKED; once unlocked, every byte dispatches through the command switch,
and "pew!" then reads as four commands:

    'p'  toggle the preset-bus overlay        (Main.cpp:1521)
    'e'  unrecognised -> framebuffer capture
    'w'  begin "patch a byte in the resident card image", which then eats
         the next 6 bytes as hex digits       (Main.cpp:1550)
    '!'  not a hex digit, so it cancels the pending patch

The net effect is that a second unlock silently TOGGLES THE PRESET-BUS OVERLAY
over whatever app is running, and any bytes arriving between 'w' and a non-hex
byte are swallowed as patch digits instead of being obeyed. Observed on
hardware 2026-09-03: three consecutive invocations returned three different
screens (an app screen, the preset-bus overlay, then a frame short enough to
decode as blank) purely from re-unlocking. No bus traffic results -- 'p' is
display-only and 'w' patches a RAM image and says so -- but every panel script
run through a re-unlocked console is measuring the wrong thing.

So unlock() PROBES FIRST and only sends the token if the console is actually
locked. Do not "simplify" it back into an unconditional write.

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

    def _settle_argument_state(self):
        """Cancel any half-finished multi-byte command argument.

        Several console commands read their argument from the following bytes
        ('w' wants 6 hex digits, 'm'/'q'/'x' want 2, 'A' wants 2, 'j' and 'o'
        want 1 or 2 control characters). A script that died mid-argument, or a
        stray unlock's 'w', leaves the console waiting -- and the next thing we
        send is eaten as an argument rather than obeyed.

        '!' is not a command and not a hex digit, so every one of those pending
        readers cancels on it, and the command switch's default branch treats
        it as a capture request. Sending a few is therefore always safe and
        always clears the decks.
        """
        self.ser.write(b"!!!")
        self.ser.flush()
        time.sleep(0.15)
        self.ser.reset_input_buffer()

    def is_unlocked(self, timeout=1.5):
        """True if the console answers a capture request.

        A locked console answers nothing at all, so a frame coming back is
        proof of an unlocked one. This is the whole reason unlock() is safe to
        call repeatedly -- see the module docstring on why sending the token
        twice toggles the preset-bus overlay.
        """
        self.ser.reset_input_buffer()
        self.ser.write(CAPTURE_BYTE)
        self.ser.flush()
        got = 0
        deadline = time.time() + timeout
        while got < FRAME_HEXCHARS and time.time() < deadline:
            chunk = self.ser.read(256).decode("ascii", "ignore")
            got += sum(1 for c in chunk if c in "0123456789abcdefABCDEF")
        self.ser.reset_input_buffer()
        return got >= FRAME_HEXCHARS

    def unlock(self):
        """Unlock the console, but ONLY if it is actually locked.

        Required after every boot and every flash. Sending the token to an
        already-unlocked console executes it as commands and toggles the
        preset-bus overlay over whatever app is running; see the module
        docstring. So: clear any pending argument, ask for a frame, and send
        the token only if nothing came back.
        """
        self._settle_argument_state()
        if self.is_unlocked():
            self.log("already unlocked -- token not sent")
            return
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

        A '~' token captures the screen AT THAT POINT and is returned in the
        list this yields. That exists because the screens most worth testing
        are modal and do not survive the gap between two invocations: the app
        switcher was observed closing on its own between one run and the next,
        so "open the switcher" and "now turn the encoder" could not be
        expressed as two commands. Everything a check needs to see has to
        happen inside one session.
        """
        frames = []
        for tok in [t.strip() for t in script.split(",") if t.strip()]:
            if tok.isdigit():
                time.sleep(int(tok) / 1000.0)
                continue
            if tok == "~":
                frames.append(self.capture())
                continue
            if "+" in tok and len(tok) == 3:      # "a+r": hold a, press r
                self.chord(tok[0], tok[2])
                time.sleep(settle)
                continue
            self.press(tok)
            time.sleep(settle)
        return frames

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
    ap.add_argument("--keys", default="", help="press script, e.g. 'a+r,300,~,+,~' "
                    "(a+r = chord; bare number = ms pause; ~ = capture the screen here)")
    ap.add_argument("--dump-fb", action="store_true", help="print the framebuffer as hex")
    ap.add_argument("--settle", type=float, default=0.12, help="seconds between presses")
    ap.add_argument("--no-unlock", action="store_true", help="skip the pew! unlock")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    p = Panel(args.port, verbose=args.verbose)
    try:
        if not args.no_unlock:
            p.unlock()
        frames = []
        if args.keys:
            frames = p.keys(args.keys, settle=args.settle)
            time.sleep(0.25)
        if args.dump_fb:
            frames.append(p.capture())
        # One frame per line, so a caller can split them and hand each to
        # fbtext.py. A single frame prints exactly as it always did.
        for f in frames:
            print(f)
    finally:
        p.close()


if __name__ == "__main__":
    main()
