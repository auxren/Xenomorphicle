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

...with one exception, which is NOT a loophole. Text right-aligned with
print_right(127) legitimately ends at x=126: "R:exit" on the debug-stats page
starts at 91 and its last glyph occupies 121..126, so column 126 is part-lit
and the row is perfectly honest. The rule that separates the two cases is not
"is the column uniform" but "is every lit pixel here accounted for by a glyph
that DECODED". A whole glyph decodes; two thirds of one matches nothing, which
is the very property that makes it invisible to fbtext -- so the pixels it
leaves behind are exactly the unexplained ones.

That keeps the original defect caught. "LIVE 105s ago  wire 29" put its 22nd
character at x=126, where no cell can even be read (a cell needs six columns
and the last readable one starts at 122), so those pixels belong to no decoded
run and are still flagged. Only fully-formed glyphs are forgiven.

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


def decoded_columns(runs):
    """Per row y, the x columns that a fully-decoded glyph accounts for.

    A run reported at (y, x) with n characters occupies x .. x+6n-1. Those
    pixels have an explanation, so they are not evidence of a truncation.
    """
    cov = {}
    for y, x, _inv, text in runs:
        cov.setdefault(y, set()).update(range(x, x + fbtext.FONT_W * len(text)))
    return cov


def clipped_rows(buf, glyphs):
    """Rows carrying text whose right edge holds a part-drawn glyph."""
    runs = fbtext.read_lines(buf, glyphs)
    cov = decoded_columns(runs)
    bad = []
    for y in sorted({run[0] for run in runs}):
        if y + FONT_H > fbtext.HEIGHT:
            continue
        for cx in EDGE_COLUMNS:
            bits = [fbtext.pixel(buf, cx, y + dy) for dy in range(FONT_H)]
            # Uniform is honest: blank panel, or a full-row inversion.
            if len(set(bits)) == 1:
                continue
            # ...and so is a column inside a glyph that decoded -- see the
            # print_right note in the module docstring.
            if cx in cov.get(y, ()):
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
