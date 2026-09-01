#!/usr/bin/env python3
"""Flag any TEXT ROW on a captured Xenomorpher frame that is clipped at the
right edge of the 128px panel.

Why this exists, specifically. The 6x8 font gives a row 21 columns: x=0..125.
A 22nd character starts at x=126 and only its first two pixel columns fit, so
it is DRAWN and then cut off. The screen does not look broken -- it looks like
a shorter string. The defect that prompted this printed

    LIVE %lus ago  wire %d

whose fixed overhead is 17 columns, so a 3-digit age (anything past 1m40) plus
a 2-digit slot came to 22 and the last character fell off: "wire 10" rendered
as "wire 1", which is not obviously truncated because it is a perfectly
well-formed slot number. On the screen that precedes a 30-slot rewrite.

Reading the text back (fbtext.py) cannot catch that on its own: it demands an
exact 6x8 glyph match, and two thirds of a glyph matches nothing, so the
clipped character simply does not appear in the decode. The truncation is
INVISIBLE to a text assertion. It is only visible in the pixels.

So this looks at the pixels. In columns 126 and 127 -- the only two a clipped
glyph can reach -- an honest row is uniform: all dark (plain text) or all lit
(a row under invertRect, which this UI uses to shout). Two thirds of a glyph
is neither. Any column that is part-lit in a band where text was found is a
character that ran off the screen.

usage: edgecheck.py FRAME.hex [FRAME.hex ...]
       xeno-sim --dump-fb ... | edgecheck.py -

Prints one line per offending row and exits 1; silent and exits 0 when clean.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbtext

WIDTH, FONT_H = fbtext.WIDTH, fbtext.FONT_H

# The columns a glyph starting at x=126 can occupy. Nothing else can be
# half-drawn here: every earlier column has room for the whole 6px cell.
EDGE_COLUMNS = (126, 127)


def clipped_rows(buf, glyphs):
    """Rows carrying text whose right edge holds a part-drawn glyph."""
    bad = []
    for y in sorted({run[0] for run in fbtext.read_lines(buf, glyphs)}):
        if y + FONT_H > fbtext.HEIGHT:
            continue
        for cx in EDGE_COLUMNS:
            bits = [fbtext.pixel(buf, cx, y + dy) for dy in range(FONT_H)]
            # Uniform is honest: blank panel, or a full-row inversion.
            if len(set(bits)) == 1:
                continue
            bad.append((y, cx, "".join(str(b) for b in bits)))
            break
    return bad


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith("-")] or None
    if not paths and "-" not in sys.argv[1:]:
        sys.exit(__doc__.strip().splitlines()[-3])
    glyphs = fbtext.load_font()
    fail = 0
    for path in (paths or ["-"]):
        for y, cx, bits in clipped_rows(fbtext.load_frame(path), glyphs):
            print("%sy=%-3d CLIPPED: a glyph is cut off at x=%d (column bits %s)"
                  % ("%s: " % path if (paths or []) and len(paths or []) > 1
                     else "", y, cx, bits))
            fail = 1
    return fail


if __name__ == "__main__":
    sys.exit(main())
