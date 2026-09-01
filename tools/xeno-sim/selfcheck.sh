#!/bin/sh
# ---------------------------------------------------------------------------
# The simulator's own checks. `make check` runs them; they take a second or two
# and need no hardware.
#
# What they are for: everything else here is a claim about fidelity, and these
# are the claims that can be checked without a module. Determinism especially --
# session replay is worthless if the same session can produce a different
# frame, so that is checked first and hardest.
# ---------------------------------------------------------------------------
set -e
cd "$(dirname "$0")"

SIM=./build/xeno-sim
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

# The gesture that opens the preset overlay: both encoder buttons, held.
ENTER='l-down,step20,r-down,step80,l-up,r-up,step200'

echo "determinism"
$SIM --dump-fb > "$TMP/d1"
$SIM --dump-fb > "$TMP/d2"
cmp -s "$TMP/d1" "$TMP/d2" && ok "the boot frame is the same twice" \
                           || bad "the boot frame differs between runs"

$SIM --keys "$ENTER,encr+,encr+,encl+" --dump-fb > "$TMP/k1"
$SIM --keys "$ENTER,encr+,encr+,encl+" --dump-fb > "$TMP/k2"
cmp -s "$TMP/k1" "$TMP/k2" && ok "a scripted run is the same twice" \
                           || bad "a scripted run differs between runs"

echo "record and replay"
$SIM --keys "$ENTER,encr+,encr+" --record "$TMP/s.txt" --dump-fb > "$TMP/r1"
$SIM --replay "$TMP/s.txt" --dump-fb > "$TMP/r2"
cmp -s "$TMP/r1" "$TMP/r2" && ok "a replayed session reproduces its frame" \
                           || bad "replay does not reproduce the recorded frame"
$SIM --replay "$TMP/s.txt" --dump-fb > "$TMP/r3"
cmp -s "$TMP/r2" "$TMP/r3" && ok "replay is repeatable" \
                           || bad "replay differs between runs"

# The four cases the RECALL-hold fix (commit 04e9e99c) turns on. The bug was
# that the entry chord itself satisfied the 250ms RECALL hold and fired a
# bus-wide recall; the fix makes both hold timers want a release first.
echo "preset overlay holds"
recall_ran() { grep -q 'PresetEngine: recall' "$1"; }
save_ran()   { grep -q 'PresetEngine: save'   "$1"; }

$SIM --keys "l-down,step20,r-down,step600,l-up,r-up,step400" > "$TMP/h1" 2>&1
recall_ran "$TMP/h1" && bad "the entry chord held 600ms fired a recall" \
                     || ok "the entry chord held 600ms recalls nothing"

$SIM --keys "$ENTER,r-down,step300,r-up,step2000" > "$TMP/h2" 2>&1
recall_ran "$TMP/h2" && ok "a fresh 300ms encR hold recalls" \
                     || bad "a fresh 300ms encR hold did not recall"

$SIM --keys "$ENTER,l-down,step600,l-up,step3000" > "$TMP/h3" 2>&1
save_ran "$TMP/h3" && ok "a fresh 600ms encL hold stores" \
                   || bad "a fresh 600ms encL hold did not store"

$SIM --keys "$ENTER,r-down,step150,r-up,step2000" > "$TMP/h4" 2>&1
recall_ran "$TMP/h4" && bad "a 150ms tap fired a recall" \
                     || ok "a 150ms tap does nothing"

echo "screens are reachable"
reaches() {  # name, keys, expected screen word
  $SIM --keys "$2" 2>&1 | grep -q "^  $3 " \
    && ok "$1" || bad "$1 (expected screen '$3')"
}
reaches "the app switcher (hold A, press encR)" \
        "a-down,step60,r-down,step60,r-up,step60,a-up,step200" "menu"
reaches "the preset-bus overlay (both encoders)" "$ENTER" "preset"
reaches "the screensaver (hold Z, press A)" \
        "z-down,step60,a-down,step60,a-up,step60,z-up,step300" "saver"

echo "app switching"
$SIM --keys "a-down,step60,r-down,step60,r-up,step60,a-up,step200,encr-,encr-,step100,r,step600" 2>&1 \
  | grep -q 'app=Setup/About' \
  && ok "the app switcher changes the running app" \
  || bad "the app switcher did not change the running app"

echo "Z is a working control"
$SIM --keys "z-down,step60,z-up,step60" 2>&1 | grep -q 'held=\[\]' \
  && ok "Z presses and releases" || bad "Z did not release"

[ $fail -eq 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $fail
