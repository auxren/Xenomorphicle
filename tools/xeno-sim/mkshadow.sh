#!/bin/sh
# ---------------------------------------------------------------------------
# Build build/shadow/: a mirror of software/src/ made entirely of symlinks,
# with tools/xeno-sim/shim/src/ laid over the top.
#
# WHY. The firmware's headers are reached with quoted includes -- OC_apps.cpp
# says #include "OC_core.h" -- and a quoted include resolves against the
# directory of the *including file* before it looks at any -I. So no include
# path can shadow a firmware header for a firmware .cpp. Compiling the same
# .cpp through a symlink in a directory we control moves that "directory of
# the including file", and the overlay becomes possible.
#
# The point is that the real sources are compiled: every file here is a symlink
# to software/src/ unless shim/src/ has a file with the same relative path, and
# shim/src/ holds only things that are hardware (the display driver, the DAC,
# EEPROM/SD storage) or the build's own app manifest. Nothing under
# software/src/ is copied or modified.
# ---------------------------------------------------------------------------
set -e

SRC=$1        # software/src
OVER=$2       # shim/src
OUT=$3        # build/shadow

[ -d "$SRC" ] || { echo "mkshadow: no such source dir: $SRC" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"

SRC_ABS=$(cd "$SRC" && pwd)
OVER_ABS=$(cd "$OVER" && pwd)

# The real tree, as symlinks.
(cd "$SRC_ABS" && find . -type d) | while read -r d; do
  mkdir -p "$OUT/$d"
done
(cd "$SRC_ABS" && find . -type f) | while read -r f; do
  ln -sf "$SRC_ABS/${f#./}" "$OUT/$f"
done

# The overlay, replacing whatever it names.
(cd "$OVER_ABS" && find . -type d) | while read -r d; do
  mkdir -p "$OUT/$d"
done
(cd "$OVER_ABS" && find . -type f) | while read -r f; do
  rm -f "$OUT/$f"
  ln -sf "$OVER_ABS/${f#./}" "$OUT/$f"
done
