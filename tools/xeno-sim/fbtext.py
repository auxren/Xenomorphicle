#!/usr/bin/env python3
"""Read the TEXT off a captured Xenomorpher framebuffer.

The simulator and the module's console both emit the same 1024-byte frame as
hex (xeno-sim --dump-fb). Comparing two of those tells you THAT they differ
(fbdiff.py); this tells you what the screen actually SAYS, which is what a
regression check wants to assert and what a bug report wants to quote.

It is not OCR in the guessing sense: it renders every glyph of the firmware's
own 6x8 font (src/extern/gfx_font_6x8.h, parsed at run time -- never copied
here, so it cannot drift) and demands an exact 6x8 pixel match. A cell that
matches nothing is a space; a cell whose INVERSE matches is reported as text
too, because invertRect is how this UI shouts.

Text is not on a grid -- setPrintPos takes arbitrary pixel coordinates -- so
every top row y and every horizontal phase is tried, and only runs of two or
more real glyphs are kept. Output is one line per run:

    y=45  x=0    [inv] WROTE + VERIFIED

usage: fbtext.py FRAME.hex [FRAME.hex ...]
       xeno-sim --dump-fb ... | fbtext.py -
"""

import os
import re
import sys

WIDTH, HEIGHT = 128, 64
FRAME_BYTES = WIDTH * HEIGHT // 8
FONT_W, FONT_H = 6, 8

FONT_H_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "software", "src", "src", "extern", "gfx_font_6x8.h")


def load_font():
    """The firmware's font table: 6 column bytes per glyph, starting at ' '."""
    text = open(FONT_H_PATH, "r", errors="replace").read()
    body = text[text.index("ssd1306xled_font6x8"):]
    body = body[body.index("{") + 1:body.index("};")]
    body = re.sub(r"//[^\n]*", "", body)
    vals = [int(v, 0) for v in re.findall(r"0[xX][0-9A-Fa-f]+|\d+", body)]
    glyphs = {}
    for i in range(0, len(vals) // FONT_W):
        ch = chr(32 + i)
        if ch > "\x7e":
            break
        glyphs[ch] = vals[i * FONT_W:(i + 1) * FONT_W]
    return glyphs


def load_frame(path):
    text = sys.stdin.read() if path == "-" else open(path, "r",
                                                     errors="replace").read()
    best = ""
    for run in re.findall(r"[0-9A-Fa-f\s]{%d,}" % (FRAME_BYTES * 2), text):
        packed = re.sub(r"\s+", "", run)
        if len(packed) >= len(best):
            best = packed
    if len(best) < FRAME_BYTES * 2:
        sys.exit("%s: no %d-byte frame found" % (path, FRAME_BYTES))
    packed = best[:FRAME_BYTES * 2]
    return bytes(int(packed[i:i + 2], 16) for i in range(0, len(packed), 2))


def pixel(buf, x, y):
    return (buf[(y >> 3) * WIDTH + x] >> (y & 7)) & 1


def cell_columns(buf, x, y):
    """The 6 column bytes under (x, y), in the font's own bit order."""
    cols = []
    for cx in range(FONT_W):
        byte = 0
        for cy in range(FONT_H):
            if pixel(buf, x + cx, y + cy):
                byte |= 1 << cy
        cols.append(byte)
    return cols


def match_cell(glyphs, cols, inverted):
    if inverted:
        cols = [(~c) & 0xFF for c in cols]
    if not any(cols):
        return " "
    for ch, g in glyphs.items():
        if ch != " " and g == cols:
            return ch
    return None


def read_lines(buf, glyphs):
    """Every run of >=2 glyphs on the frame, as (y, x, inverted, text)."""
    out = []
    for inverted in (False, True):
        for y in range(HEIGHT - FONT_H + 1):
            x = 0
            while x <= WIDTH - FONT_W:
                # A run must START on a real glyph, never on a space, or every
                # blank stretch of screen would seed one.
                ch = match_cell(glyphs, cell_columns(buf, x, y), inverted)
                if ch is None or ch == " ":
                    x += 1
                    continue
                run, cx, gaps = "", x, 0
                while cx <= WIDTH - FONT_W:
                    c = match_cell(glyphs, cell_columns(buf, cx, y), inverted)
                    if c is None:
                        break
                    # Trailing blanks are not part of the run, but a single
                    # space inside one is (this UI pads columns with them).
                    if c == " ":
                        gaps += 1
                        if gaps > 3:
                            break
                    else:
                        gaps = 0
                    run += c
                    cx += FONT_W
                run = run.rstrip()
                if len(run.replace(" ", "")) >= 2:
                    out.append((y, x, inverted, run))
                    x = cx
                else:
                    x += 1
    # A glyph run found at y is also "found" at y+-0 in the other polarity only
    # by coincidence; keep both, but sort so reading order is reading order.
    return sorted(out)


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith("-")] or None
    if not paths and "-" not in sys.argv[1:]:
        sys.exit(__doc__.strip().splitlines()[-2])
    glyphs = load_font()
    for path in (paths or ["-"]):
        if len(paths or []) > 1:
            print("== %s" % path)
        for y, x, inv, text in read_lines(load_frame(path), glyphs):
            print("y=%-3d x=%-4d %s%s" % (y, x, "[inv] " if inv else "", text))


if __name__ == "__main__":
    main()
