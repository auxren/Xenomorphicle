#!/usr/bin/env python3
"""Compare two 128x64 OLED framebuffer captures, byte for byte.

The module's console dumps its framebuffer as 2048 hex digits on one line
(Main.cpp's capture path, reached by pressing an unmapped console key). The
simulator prints the same 1024 bytes in the same page-packed order:

    ./build/xeno-sim --keys "..." --dump-fb > sim.hex

So a device capture and a simulator capture of the same screen are directly
comparable, and "it looks about right" becomes a number.

    python3 fbdiff.py device.hex sim.hex          # counts + a visual delta
    python3 fbdiff.py device.hex sim.hex --quiet  # counts only, exit 1 on diff

Input is forgiving: any whitespace, any case, an optional 0x prefix, and any
lines that are not hex (console chatter around the capture) are skipped. The
first run of >= 2048 hex characters in the file is the frame.

Packing: bit (y & 7) of buf[(y >> 3) * 128 + x]. Both sides produce it; this
script is the only place it is unpacked, and it draws nothing else.
"""

import re
import sys

WIDTH, HEIGHT = 128, 64
FRAME_BYTES = WIDTH * HEIGHT // 8


def load(path):
    text = open(path, "r", errors="replace").read()
    # Console captures arrive surrounded by other output; take the longest run
    # of hex, which is the frame.
    best = ""
    for run in re.findall(r"[0-9A-Fa-f\s]{%d,}" % (FRAME_BYTES * 2), text):
        packed = re.sub(r"\s+", "", run)
        if len(packed) >= len(best):
            best = packed
    if len(best) < FRAME_BYTES * 2:
        sys.exit("%s: no %d-byte frame found (got %d hex digits)"
                 % (path, FRAME_BYTES, len(best)))
    packed = best[:FRAME_BYTES * 2]
    return bytes(int(packed[i:i + 2], 16) for i in range(0, len(packed), 2))


def pixel(buf, x, y):
    return (buf[(y >> 3) * WIDTH + x] >> (y & 7)) & 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in sys.argv[1:]
    if len(args) != 2:
        sys.exit("usage: fbdiff.py A.hex B.hex [--quiet]")

    a, b = load(args[0]), load(args[1])

    diff_bytes = sum(1 for i in range(FRAME_BYTES) if a[i] != b[i])
    diff_pixels = sum(bin(a[i] ^ b[i]).count("1") for i in range(FRAME_BYTES))
    on_a = sum(bin(v).count("1") for v in a)
    on_b = sum(bin(v).count("1") for v in b)

    print("A: %s  (%d lit pixels)" % (args[0], on_a))
    print("B: %s  (%d lit pixels)" % (args[1], on_b))
    print("differing bytes:  %d / %d" % (diff_bytes, FRAME_BYTES))
    print("differing pixels: %d / %d  (%.3f%%)"
          % (diff_pixels, WIDTH * HEIGHT, 100.0 * diff_pixels / (WIDTH * HEIGHT)))

    if diff_pixels == 0:
        print("IDENTICAL")
        return 0

    if not quiet:
        # One character per pixel pair (two rows), so 64 lines become 32.
        #   ' ' both off in both     '#' lit in both
        #   'a' only A lit           'b' only B lit
        # Any letter is a difference; a screen of '#' and ' ' with a few
        # letters is a near miss, a screen of letters is a different screen.
        print()
        print("    " + "".join(str((x // 10) % 10) if x % 10 == 0 else " "
                               for x in range(WIDTH)))
        for y in range(HEIGHT):
            row = []
            for x in range(WIDTH):
                pa, pb = pixel(a, x, y), pixel(b, x, y)
                if pa and pb:
                    row.append("#")
                elif pa:
                    row.append("a")
                elif pb:
                    row.append("b")
                else:
                    row.append(".")
            print("%3d " % y + "".join(row))
        print()
        print("'#' lit in both, '.' dark in both, 'a' only in %s, 'b' only in %s"
              % (args[0], args[1]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
