#!/usr/bin/env python3
"""Decode a sequence of framebuffers, one per line, as hwctl.py --keys emits.

hwctl.py prints one 2048-hex-character frame per line so that a single session
can capture several screens -- which it has to, because the screens most worth
checking are modal and close between invocations. This turns that stream into
labelled text using the same glyph decoder the simulator's checks use, so a
hardware check reads like a simulator check.

    hwctl.py --keys 'a+r,300,~,+,200,~' | fbseq.py - "switcher" "scrolled"

Labels are optional and positional; unlabelled frames are numbered.
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "xeno-sim"))
import fbtext  # noqa: E402


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "-"
    labels = sys.argv[2:]
    stream = sys.stdin if src == "-" else open(src)
    lines = [l.strip() for l in stream if l.strip()]

    glyphs = fbtext.load_font()
    for i, line in enumerate(lines):
        label = labels[i] if i < len(labels) else "frame %d" % (i + 1)
        print("--- %s ---" % label)
        hexonly = "".join(c for c in line if c in "0123456789abcdefABCDEF")
        if len(hexonly) < 2048:
            print("    (short frame: %d/2048 hex chars)" % len(hexonly))
            continue
        packed = hexonly[:2048]
        buf = bytes(int(packed[j:j + 2], 16) for j in range(0, 2048, 2))
        for y, x, inv, text in fbtext.read_lines(buf, glyphs):
            print("y=%-3d x=%-4d %s%s" % (y, x, "[inv] " if inv else "", text))


if __name__ == "__main__":
    main()
