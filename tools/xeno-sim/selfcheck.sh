#!/bin/sh
# ---------------------------------------------------------------------------
# The simulator's own checks. `make check` runs them; they take about twenty
# seconds and need no hardware. (Most of that is honest simulated time: the
# provenance-row checks age a bank by up to 700 SIMULATED seconds to reach the
# far side of DrawAge's last threshold, and one check deliberately waits out a
# deadline to prove a firmware loop really does block.)
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

# Every simulator run in here is wrapped in a deadline. Not belt-and-braces:
# the firmware has screens it draws from inside its OWN blocking loop
# (Ui::DebugStats, ConfirmReset, the calibration wizard), and a simulator whose
# clock does not advance inside one of those spins forever with nothing on
# stdout. That has already cost a reviewer a hung process; a check that hangs
# the suite is worse than a check that fails it. `timeout` is not on a stock
# macOS, so this is perl's alarm.
#
# Returns the command's status, or 142 if the deadline killed it.
deadline() { perl -e 'alarm shift; exec @ARGV' "$@"; }
BARESIM=$SIM          # no deadline: for the one check that WANTS to be killed
SIM="deadline 60 $SIM"

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

# ---------------------------------------------------------------------------
# Time inside a blocking firmware loop.
#
# Ui::DebugStats (OC_debug.cpp) is a `while (!exit_loop)` whose only pacing is
# delayMicroseconds(10) and whose only exit is a button. The simulator has no
# ISR of its own -- it calls the firmware's from the same loop -- so if a delay
# does not run the background, then inside that loop: the clock stands still,
# the UI poll never runs, no button event is ever queued, and the frame buffer
# is never drained. The simulator wedges with a blank screen and has to be
# killed. delayMicroseconds() was a no-op for exactly that reason, and it cost
# a reviewer a hung process during the audit that produced these checks.
#
# So what is asserted is the property, not the screen: a press scheduled BEFORE
# the loop is entered must be seen from inside it. Nothing else can be -- the
# loop never gives the script another turn, which is why --keys grew
# "<button>-inN" (schedule a tap N ms ahead, return immediately).
echo "blocking firmware loops still see time pass"

MENU='a-down,step60,r-down,step60,r-up,step60,a-up,step200'

# encL LONG press in the app switcher = debug stats; a short press cancels.
$SIM --keys "$MENU,r-in900,l-down,step1400,l-up,step200" 2>&1 \
  | grep -q '^  menu ' \
  && ok "the debug-stats loop is entered, sees a scheduled press, and exits" \
  || bad "the debug-stats loop did not return to the app switcher"

# The other half of the same claim: it really is a blocking loop. Without a
# press to find, it never ends -- so this one is expected to hit its deadline.
# If it ever starts returning on its own, the check above proves less than it
# says and both want re-reading. Run WITHOUT the standard 60s deadline.
if deadline 5 $BARESIM --keys "$MENU,l-down,step1400,l-up" >/dev/null 2>&1; then
  bad "the debug-stats loop returned with no exit press -- it is not blocking"
else
  ok "with no exit press the debug-stats loop blocks, as the firmware's does"
fi

# A short encL press must NOT reach it: DebugStats is a service tool behind a
# deliberate hold, and encL is cancel everywhere else in this instrument.
$SIM --keys "$MENU,l,step200" 2>&1 | grep -q '^  app ' \
  && ok "a short encL press cancels the switcher instead of opening debug stats" \
  || bad "a short encL press did not cancel the app switcher"

# ---------------------------------------------------------------------------
# The 200e write path. A write rewrites all 30 presets and has no undo, so what
# is checked here is not that it works -- it is that it TELLS THE TRUTH when it
# does not. --write-fault makes the simulated 251e mishandle the RESTORE; the
# firmware is supposed to notice on its own read-back and say so.
#
# The screen text is read off the framebuffer with the firmware's own font
# (fbtext.py), so these assert what a person would actually see.
# ---------------------------------------------------------------------------
echo "200e write verification"

# scan -> enter 0x5C -> Read -> Gen -> apply -> back -> Save -> confirm -> settle
#
# Two tokens in here look like padding and are not:
#
#   the `l,step10` after the APPLY press -- Gen STAYS on its own screen after
#   APPLY (it draws the stage-strip preview the home screen used to be the only
#   place to see), so the encL that goes back to the module home is a real step,
#   not transport this script can skip;
#
#   the step400 before the final `r` -- the write-confirm screen ignores a yes
#   for kConfirmDeadMs (350 ms) after it appears, so that a fumbled
#   app-switcher chord cannot commit a 30-slot rewrite. A shorter gap is
#   correctly refused and leaves the confirm screen up, which reads on the
#   status row as nothing at all.
#
# Both failure modes look identical from here -- an empty y=46 row -- and both
# have already sent someone hunting for a firmware regression that was not
# there.
W251='l,step200,r,step10,r,step3000,],],r,step10,r,step10,l,step10,],],r,step400,r,step6000'

# Everything up to and including the Read, for checks that need a live bank
# without a write behind it.
W251_READ='l,step200,r,step10,r,step3000'
# ...and up to the APPLY that leaves an unsaved edit in the working buffer.
W251_EDIT="$W251_READ,],],r,step10,r,step10,l,step200"

status_says() {  # fault, expected status-line text
  $SIM --app "200e Modules" --write-fault "$1" --keys "$W251" --dump-fb 2>/dev/null \
    | python3 fbtext.py - | grep '^y=46' | sed 's/^y=46 *x=0 *//'
}
says() {  # description, fault, expected
  got=$(status_says "$2")
  [ "$got" = "$3" ] && ok "$1" || bad "$1 (said: '$got')"
}

says "a good write reads back and says VERIFIED" none "WROTE + VERIFIED"
says "a module that stored nothing is caught" ignore "[inv] BAD: 6 bytes wrong"
says "one wrong byte in the edited slot is caught" \
     flip-first "[inv] BAD: 1 byte wrong"
says "a byte changed in ANOTHER preset is caught" \
     flip-last "[inv] BAD: OTHER PRESETS!"
# --- the two truncations, which are NOT the same failure --------------------
#
# These two look alike in a one-line summary and are opposites. Anyone tempted
# to "simplify" them into one case should read this paragraph first.
#
#   drop-tail       a bad STORE seen through a GOOD read-back. The module drops
#                   the last record -- one this edit never touched -- so all
#                   63120 bytes come back and they genuinely match the intent.
#                   VERIFIED is the TRUE answer. There is nothing wrong with
#                   the module's contents to report, and inventing an alarm
#                   here would teach the user to ignore the real ones.
#
#   short-readback  a good STORE seen through a BAD read-back. The module holds
#                   the bank perfectly, then serves 2104 of 63120 bytes on the
#                   verify BACKUP and reports DONE. VERIFIED would be a LIE --
#                   not because the module is wrong, but because nothing was
#                   checked.
#
# The second one is why the verify pass requires the whole bank. The read-back
# lands in the SAME card image the restore was sourced from, so every byte the
# module did not send still holds the value we intended: the whole-bank hash
# agrees with itself, happily, on 3% of the evidence. The old code asked only
# that the read-back covered the edited slot's 2104-byte window -- which a
# 2104-byte read-back does exactly -- so it said WROTE + VERIFIED and then
# stamped read_hash_ with the poisoned image, so every later diff was against
# a baseline that was two-thirds wishful thinking.
says "truncation away from the edit is not a false alarm" \
     drop-tail "WROTE + VERIFIED"

# Asserted as a property, not a string: what matters is that this can never
# read as success, whatever wording the app settles on for it. (At the time of
# writing it says "BAD: OTHER PRESETS!", which overstates what is known -- see
# the note in the report; the app cannot actually tell whether the other 29 are
# intact, only that it did not see them.)
short_row=$(status_says short-readback)
case "$short_row" in
  ""|*VERIFIED*)
    bad "a truncated read-back earned VERIFIED on 3% of the bank (said: '$short_row')" ;;
  *BAD*|*FAIL*)
    ok "a truncated read-back is refused as proof, however good the write was" ;;
  *)
    bad "a truncated read-back reported neither success nor failure (said: '$short_row')" ;;
esac

# The edit must SURVIVE a bad write -- the working copy is then the only place
# the user's intent exists. Arming Save again is the honest way to ask: it
# re-diffs against the module's real contents, so a retry that still has work
# to do proves nothing was discarded, and a good write having nothing left to
# do proves the first one really landed.
rearm_says() {  # fault, screen row
  $SIM --app "200e Modules" --write-fault "$1" --keys "$W251,r,step10" \
       --dump-fb 2>/dev/null | python3 fbtext.py - | grep "^y=$2" \
    | sed "s/^y=$2 *x=0 *//"
}
[ "$(rearm_says ignore 26)" = "6 bytes change" ] \
  && ok "a failed write keeps the edit, ready to retry" \
  || bad "a failed write lost the edit (re-arm said: '$(rearm_says ignore 26)')"
[ "$(rearm_says none 36)" = "No changes to write" ] \
  && ok "a verified write leaves nothing to rewrite" \
  || bad "a verified write still shows changes (re-arm said: '$(rearm_says none 36)')"

# ...and the other side of the short read-back: the image it leaves behind is
# a blend of what the module sent and what we intended, so it is not a usable
# baseline for anything. The read must be INVALIDATED, not merely reported --
# otherwise the next Save diffs the user's edit against two thirds of a wish
# and cheerfully reports the number of bytes it would change. "Read the module
# first" is the guard refusing on have_read, which is what forces a real
# re-Read before anything else reaches the wire.
[ "$(rearm_says short-readback 36)" = "Read the module first" ] \
  && ok "a truncated read-back invalidates the read, so the next Save must re-Read" \
  || bad "a truncated read-back left a poisoned baseline armable (re-arm said: '$(rearm_says short-readback 36)')"

# ---------------------------------------------------------------------------
# Three ways this app has claimed something that was not so. Each of these was
# a real defect, each fix is in apps/Bus200eApp.h, and each screen below is the
# one a person is looking at while deciding whether to rewrite 30 presets.
# ---------------------------------------------------------------------------
echo "200e state is not misreported"

row_at() {  # y, then simulator args
  y=$1; shift
  $SIM --app "200e Modules" "$@" --dump-fb 2>/dev/null \
    | python3 fbtext.py - | grep "^y=$y" | head -1 | sed "s/^y=$y *x=[0-9]* *//"
}

# 1. A finished write must not outrank a live edit.
#
# write_state_ used to outrank everything on the status row and nothing ever
# cleared it, so WROTE + VERIFIED stayed up for the rest of the session: the
# screen asserted a verified match against the module while arming Save
# simultaneously reported "6 bytes change". It also froze the staleness clock,
# so LIVE Ns ago never came back after the first write. Editing after a write
# is the normal loop in this app, not a corner.
edit_after_write=$(row_at 46 --keys "$W251,[,step50,[,step50,r,step10,r,step10,l,step200")
[ "$edit_after_write" = "[inv] EDITED*" ] \
  && ok "a new edit after a verified write says EDITED*, not the stale verdict" \
  || bad "a new edit after a verified write said '$edit_after_write'"

# 2. The write-confirm screen ignores a yes for kConfirmDeadMs (350 ms).
#
# The app switcher is "hold A, press encR". On the module home A ALONE arms the
# write and encR commits it here, so the two halves of the most common
# navigation chord are, in order, arm and commit -- a fumbled chord committed
# 63,120 bytes with the confirm screen visible for 51 ms. A two-step
# confirmation whose two steps are the same button is a one-step confirmation.
W251_ARMED='l,step200,r,step10,r,step3000,],],r,step10,r,step10,l,step10,],],r'
early=$(row_at 13 --keys "$W251_ARMED,step10,r,step6000")
[ "$early" = "[inv] WRITE to 5C slot 1" ] \
  && ok "a confirm 10ms after arming does not commit -- the prompt is still up" \
  || bad "a confirm 10ms after arming left the confirm screen (y=13 said: '$early')"
early_status=$(row_at 46 --keys "$W251_ARMED,step10,r,step6000")
case "$early_status" in
  *VERIFIED*|*WRIT*)
    bad "a confirm inside the dead window committed the write ('$early_status')" ;;
  *) ok "a confirm inside the dead window puts nothing on the wire" ;;
esac
late=$(row_at 46 --keys "$W251_ARMED,step400,r,step6000")
[ "$late" = "WROTE + VERIFIED" ] \
  && ok "a confirm 400ms after arming does commit -- the guard is a delay, not a wall" \
  || bad "a confirm 400ms after arming did not commit (said: '$late')"

# 3. Turning the slot encoder with an unsaved edit is refused, not obeyed.
#
# Changing slot re-decodes working_slot_ from the card image, which destroys
# the edit. One detent of the encoder you also PUSH to confirm -- the most
# likely accidental gesture on this screen -- used to erase an edit silently,
# and left edited_ set, so the screen went on claiming EDITED* over bytes
# identical to the module's.
refusal=$(row_at 46 --keys "$W251_EDIT,.,step200")
[ "$refusal" = "[inv] edited: Save or Read" ] \
  && ok "the slot encoder is refused with an unsaved edit, and names both ways out" \
  || bad "the slot encoder with an unsaved edit said '$refusal'"
$SIM --app "200e Modules" --keys "$W251_EDIT,.,step200" --dump-fb 2>/dev/null \
  | python3 fbtext.py - | grep -q '^y=13 *x=80 *Slot 1$' \
  && ok "a refused slot turn leaves the slot where it was" \
  || bad "a refused slot turn moved the slot anyway"
survived=$(row_at 26 --keys "$W251_EDIT,.,step200,],],r,step10")
[ "$survived" = "6 bytes change" ] \
  && ok "the working buffer survives the refusal, with the edit still in it" \
  || bad "the edit did not survive a refused slot turn (Save said: '$survived')"

# APPLY on the generator stays on the generator, and the preview it stays for
# is really drawn. That round trip was two presses of pure transport per lap,
# ~40 in a session, and it existed only because the preview lived elsewhere --
# so "APPLY returns home" and "the Gen screen has no preview" are one fact, and
# either regressing alone brings the cost straight back.
gen_legend=$(row_at 56 --keys "$W251_READ,],],r,step10,r,step10")
[ "$gen_legend" = "encR:APPLY encL:back" ] \
  && ok "APPLY stays on the Gen screen instead of bouncing home" \
  || bad "APPLY left the Gen screen (y=56 said: '$gen_legend')"
band_lit() {  # frame on stdin: lit pixels in rows $1..$2
  python3 -c 'import sys,re
h = re.sub(r"[^0-9A-Fa-f]", "", sys.stdin.read())[:2048]
b = bytes.fromhex(h)
y0, y1 = int(sys.argv[1]), int(sys.argv[2])
print(sum((b[(y >> 3) * 128 + x] >> (y & 7)) & 1
          for y in range(y0, y1 + 1) for x in range(128)))' "$1" "$2"
}
gen_preview=$($SIM --app "200e Modules" --keys "$W251_READ,],],r,step10,r,step10" \
  --dump-fb 2>/dev/null | band_lit 48 54)
[ "$gen_preview" -gt 20 ] \
  && ok "the Gen screen draws the stage preview it stays put for" \
  || bad "the Gen screen's preview band is empty ($gen_preview lit pixels)"

# ---------------------------------------------------------------------------
# The display cannot lie by truncation.
#
# The row is 21 columns: x=0..125. A 22nd character starts at x=126, two of its
# six pixel columns fit, and the rest is gone -- and it does not LOOK cut off,
# it looks like a shorter string. "LIVE %lus ago  wire %d" had 17 columns of
# fixed overhead, so a 3-digit age plus a 2-digit slot overflowed and turned
# "wire 10" into "wire 1": a perfectly well-formed slot number, on the screen
# that precedes a 30-slot rewrite. It is now DrawAge(), capped at 12 columns.
#
# Reading the text back cannot catch this -- fbtext.py demands an exact glyph
# match, and two thirds of a glyph matches nothing, so the clipped character
# simply is not in the decode. edgecheck.py looks at the pixels instead: in
# columns 126-127 an honest row is uniform (all dark, or all lit under an
# invertRect), and a part-drawn glyph is neither.
# ---------------------------------------------------------------------------
echo "nothing on screen is clipped at the right edge"

# The provenance row at ages that reach each of DrawAge's three forms, plus one
# far past the last boundary. 100 s and 600 s are the thresholds; 100 s is also
# exactly where the old format grew its third digit.
#
# The last case is the original defect's own shape and the only one that
# actually reproduced it: a 3-digit age AND a 2-digit slot. `>` walks the slot
# encoder to the end of the bank (slot 30), so under the old format the row was
# "LIVE 105s ago  wire 29" -- 22 columns, one over -- and the '9' was cut off,
# leaving the well-formed and entirely wrong "wire 2". A 1-digit slot fits and
# proves nothing.
age_row() {  # keys, label
  $SIM --app "200e Modules" --keys "$1" --dump-fb 2>/dev/null > "$TMP/age.hex"
  said=$(python3 fbtext.py "$TMP/age.hex" | grep '^y=46' | sed 's/^y=46 *x=0 *//')
  case "$said" in
    "LIVE "*" ago") : ;;
    *) bad "the provenance row $2 is not an age at all: '$said'" ;;
  esac
  python3 edgecheck.py "$TMP/age.hex" >"$TMP/edge.txt" 2>&1 \
    && ok "the provenance row survives $2 intact ($said)" \
    || bad "the provenance row is clipped $2: $(cat "$TMP/edge.txt")"
}
for age in 2000 9000 105000 700000; do
  age_row "$W251_READ,step$age" "an age of ${age}ms"
done
age_row "$W251_READ,>,step105000" "a 3-digit age on a 2-digit slot"

# The generic half, which is the part worth keeping: no screen anywhere may
# have a character running off the right edge. Cheap enough to sweep, and it
# costs nothing to add a screen to the list.
sweep() {  # name, then simulator args
  name=$1; shift
  $SIM "$@" --dump-fb 2>/dev/null > "$TMP/sweep.hex"
  python3 edgecheck.py "$TMP/sweep.hex" >"$TMP/edge.txt" 2>&1 \
    || { bad "$name has a character clipped at the right edge: $(cat "$TMP/edge.txt")"; return; }
  printf '%s ' "$name"
}
printf '  ok    no clipped text on: '
sweep boot
sweep app-switcher --keys "$MENU"
sweep preset-overlay --keys "$ENTER"
sweep 200e-select --app "200e Modules"
sweep 200e-home --app "200e Modules" --keys "$W251_READ"
sweep 200e-gen --app "200e Modules" --keys "$W251_READ,],],r,step10"
sweep 200e-confirm --app "200e Modules" --keys "$W251_ARMED,step10"
sweep 200e-verified --app "200e Modules" --keys "$W251"
sweep 200e-bad --app "200e Modules" --write-fault ignore --keys "$W251"
sweep setup --app "Setup/About"
sweep scenery --app Scenery
sweep screensaver --keys "z-down,step60,a-down,step60,a-up,step60,z-up,step300"
echo

[ $fail -eq 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $fail
