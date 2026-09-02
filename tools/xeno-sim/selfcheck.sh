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

echo "the PhzConfig codec (the real PhzConfig.cpp against the RAM volume)"
if $SIM --test-phzconfig > "$TMP/phz" 2>&1; then
  ok "$(tail -1 "$TMP/phz")"
else
  bad "PhzConfig codec checks failed:"; grep FAIL "$TMP/phz"
fi

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

# The same tap one millisecond later. The hold timer used to be stamped
# `millis() | 1`, which rounds an even reading UP; the next loop pass in the
# same millisecond then saw now - stamp wrap to 0xFFFFFFFF and the 250 ms
# hold was "over" -- a plain tap fired a bus-wide RECALL whenever the press
# landed on an even millisecond. The simulated clock is deterministic, so
# the check above only ever tried one parity; this one tries the other.
$SIM --keys "l-down,step20,r-down,step80,l-up,r-up,step201,r-down,step150,r-up,step2000" > "$TMP/h5" 2>&1
recall_ran "$TMP/h5" && bad "a 150ms tap on the other millisecond parity fired a recall" \
                     || ok "a 150ms tap does nothing on either millisecond parity"

# The 225e pulse jacks against what the engine REFUSES. Slot 1 is stored by
# the hold above; slots 2 and 3 are empty, so every recall below is refused
# (EMPTY SLOT) -- and refused is the case that matters. The bus RECALL has
# already gone out, the rest of the case is on that slot, and the panel has
# to be on it too. The overlay used to follow LastSlot (the slot still
# LOADED here) after every finished op, so a NEXT pulse into an empty slot
# bounced straight back: 1 -> 2, refused, back to 1, and the next pulse
# tried 2 again. Two pulses must reach slot 3.
echo "preset overlay follows the case, not the loaded slot"
STORED="$ENTER,l-down,step600,l-up,step3000"          # slot 1 stored, overlay open
NEXT_TR1="encl+,encl+,encr+,step50"                    # cursor to NEXT, assign TR1
$SIM --keys "$STORED,$NEXT_TR1,1,step1500,1,step1500" > "$TMP/p1" 2>&1
grep -q 'recall slot 2' "$TMP/p1" \
  && ok "two NEXT pulses from slot 1 reach slot 3 (bus slot 2), empty or not" \
  || bad "two NEXT pulses did not reach slot 3 -- the panel bounced off the empty slot"
# The same with the overlay closed: cycling runs whether or not it is open.
$SIM --keys "$STORED,$NEXT_TR1,a,step300,1,step1500,1,step1500" > "$TMP/p2" 2>&1
grep -q 'recall slot 2' "$TMP/p2" \
  && ok "the same with the overlay closed" \
  || bad "with the overlay closed, two NEXT pulses did not reach slot 3"
# A pulse no loop pass could sample (~1: high and low again with no time
# passing). The trigger path used to compare pin levels between loop
# passes, so a narrow pulse -- or any pulse landing during a 250 ms flash
# write, when loop() is not running -- was dropped while the rest of the
# case stepped. The GPIO edge latch sees it; the overlay must step on it.
$SIM --keys "$STORED,$NEXT_TR1,~1,step1500,~1,step1500" > "$TMP/p2s" 2>&1
grep -q 'recall slot 2' "$TMP/p2s" \
  && ok "two zero-width NEXT spikes step just like two pulses" \
  || bad "a NEXT pulse too narrow for loop() to sample was dropped"
# ...and an edge latched BEFORE the assignment existed must not fire once
# it does: the latch is drained every pass, assigned or not.
$SIM --keys "$STORED,~1,step200,$NEXT_TR1,step1500" > "$TMP/p2e" 2>&1
grep -q 'recall slot' "$TMP/p2e" \
  && bad "a spike from before the NEXT assignment fired a recall after it" \
  || ok "an edge from before the assignment does not fire once assigned"

# A rename is bound to the slot it was opened on. A pulse mid-edit moves the
# selection (it is a performance event; it cannot wait for a menu), and the
# name typed for slot 1 must still land on slot 1 -- it used to commit to
# whatever `sel` had become by the time DONE was clicked.
RENAME="encl-,l,step100,encr+,encl+,encr+,step50"      # cursor to name, EDIT, type
$SIM --keys "$STORED,$NEXT_TR1,$RENAME,1,step1500,l,step100,encl-,encr-,step50" \
     --dump-fb 2>/dev/null | python3 fbtext.py - | grep '^y=44' > "$TMP/p3"
grep -q 'AB$' "$TMP/p3" \
  && ok "a rename interrupted by a NEXT pulse still lands on the slot it was opened on" \
  || bad "a rename interrupted by a NEXT pulse landed elsewhere (slot 1 row: $(cat "$TMP/p3"))"

# A factory reset from Setup (encR arms, B commits) re-runs AppSwitcher::Init
# live. It used to switch to the default app with the ISR still running and
# never sent it RESUME; now it goes through the same stop/switch/resume
# choreography as the app menu, and the default app must be up and drawing.
echo "a runtime factory reset lands in the default app, resumed"
$SIM --app "Setup/About" --keys "r,step500,b,step2000" > "$TMP/fr1" 2>&1
grep -q 'Filesystem formatted' "$TMP/fr1" \
  && ok "encR then B in Setup erases storage" \
  || bad "encR then B in Setup did not reach the erase"
$SIM --app "Setup/About" --keys "r,step500,b,step2000" --dump-fb 2>/dev/null \
  | python3 fbtext.py - | grep -q '^y=1 .*200e Modules' \
  && ok "and the default app is drawing afterwards" \
  || bad "the default app is not drawing after the reset"

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
#
# Re-arming is `a`, not encR, and that is not a stylistic choice. After a write
# that ends BAD with a pre-write snapshot on hand, the action row is replaced
# by the recovery prompt ("encR:UNDO  encL:keep"), so encR arms the undo -- the
# row it used to run is not on screen to run. A is the module home's primary
# action in every state, so it is also the one gesture that states this
# property without depending on which row happens to be drawn.
rearm_says() {  # fault, screen row
  $SIM --app "200e Modules" --write-fault "$1" --keys "$W251,a,step200" \
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
# NON-VOLATILE MEMORY.
#
# Everything below this line asks what a SECOND boot sees, which until --state
# existed could not be asked at all: the file system is a std::map and the
# EEPROM an array, both of which die with the process. `--state FILE` reads an
# image before boot and writes it back at exit, so two runs sharing one file
# are a power cycle. `--sd-card` seats a card, because half of these questions
# are about whether the two machines agree.
#
# This is still not LittleFS -- no wear, no erase timing, no 0-byte-file
# failure mode -- so a clean round-trip here says nothing about those. What it
# does say is which BYTES the firmware chose to keep, and on which volume.
# ---------------------------------------------------------------------------
IMG="$TMP/img"          # scratch state images live here
mkdir -p "$IMG"

echo "presets live on internal flash, card or no card"

# The bug: with a card inserted every previously-saved preset read as "Empty
# preset", and pulling the card brought them back. Nothing was lost, but a
# musician cannot tell that from the front panel. It was `SDcard_Ready ? SD :
# myfs` deciding where containers live, so the instrument's preset memory
# depended on whether an accessory happened to be seated.
#
# The property, stated so that no wording or file name can disarm it: a preset
# stored in one card state must RECALL in the other. Recall either reports
# "done" or refuses with EMPTY SLOT, and those are the two answers worth
# telling apart.
stored_then_recalled() {  # store-flags, recall-flags, label
  rm -f "$IMG/xfer"
  $SIM $1 --state-out "$IMG/xfer" \
       --keys "$ENTER,l-down,step600,l-up,step4000" >/dev/null 2>&1
  $SIM $2 --state-in "$IMG/xfer" \
       --keys "$ENTER,r-down,step300,r-up,step3000" 2>&1 \
    | grep -q 'recall slot 0 done' \
    && ok "a preset stored $3 recalls after a power cycle" \
    || bad "a preset stored $3 did not recall after a power cycle"
}
stored_then_recalled ""          "--sd-card"  "with no card, with one inserted"
stored_then_recalled "--sd-card" ""           "with a card, with it pulled"

# The control for both of the above. Without it they would pass just as well
# against a recall that says "done" unconditionally, which is precisely the
# failure being guarded against -- the old bug was a recall that lied about
# what it found.
$SIM --keys "$ENTER,r-down,step300,r-up,step3000" 2>&1 \
  | grep -q 'refused (EMPTY SLOT)' \
  && ok "recalling a slot nothing was ever stored in is refused, not faked" \
  || bad "an empty slot did not refuse a recall -- the two checks above prove nothing"

# ...and the containers are on internal flash in BOTH cases, which is the
# mechanism behind the property above. Asserted on the volume, not the path:
# the file may be renamed, but it may not move to the card.
$SIM --sd-card --state-in "$IMG/xfer" --dump-fs 2>/dev/null > "$TMP/fs.txt"
grep -q '^fs lfs PB_' "$TMP/fs.txt" \
  && ok "a preset container is on internal flash with a card seated" \
  || bad "no preset container on internal flash: $(cat "$TMP/fs.txt")"
grep -q '^fs sd PB_' "$TMP/fs.txt" \
  && bad "a preset container was written to the CARD" \
  || ok "no preset container is written to the card"

# Merely seating or pulling a card must not CHANGE the stored set either. Two
# boots from one image, one with a card and one without; the containers they
# leave behind must be byte-identical. A boot that quietly rewrote, migrated or
# dropped a preset because the card state moved would show up here and nowhere
# else on this screen.
cp "$IMG/xfer" "$IMG/nocard" && cp "$IMG/xfer" "$IMG/card"
$SIM            --state "$IMG/nocard" --keys "step300" >/dev/null 2>&1
$SIM --sd-card  --state "$IMG/card"   --keys "step300" >/dev/null 2>&1
# (process substitution is not POSIX sh, and this script runs under /bin/sh)
# `|| true` is load-bearing: this script runs under `set -e`, and a grep that
# matches nothing exits 1. Without it a build where the presets went to the
# CARD -- exactly the regression this section exists to catch -- killed the
# suite here instead of failing this check, taking every later section with it.
# A check that can abort the run is worse than a check that fails it.
grep '^file lfs PB_' "$IMG/nocard" > "$TMP/pb_nocard" 2>/dev/null || true
grep '^file lfs PB_' "$IMG/card"   > "$TMP/pb_card"   2>/dev/null || true
if [ ! -s "$TMP/pb_nocard" ]; then
  bad "no preset survived the no-card boot, so the comparison below is vacuous"
elif cmp -s "$TMP/pb_nocard" "$TMP/pb_card"; then
  ok "the stored preset set is the same whether or not a card is present"
else
  bad "the stored preset set changed with the card state"
fi

# The card's actual job: export puts a container on it, import brings one
# back. And an import must never retire a good local slot on the strength
# of a card file it has not verified: ImportSlot used to check the 16-byte
# header and nothing else, so a card copy damaged past that point replaced
# the local slot and the next recall said BAD PRESET with the good copy
# already gone. Damage the CARD copy (corrupt_slot.py --sd) and import.
cp "$IMG/xfer" "$IMG/exp"
$SIM --sd-card --state "$IMG/exp" --keys "export,step100" 2>&1 | grep -q 'export slot 0: ok' \
  && ok "a stored slot exports to the card" \
  || bad "the slot did not export"
grep -q '^file sd PB_00.PBS ' "$IMG/exp" \
  && ok "...and the container is on the card afterwards" \
  || bad "no PB_00.PBS on the card after export"
python3 corrupt_slot.py "$IMG/exp" "$IMG/expbad" 0 G --sd
cp "$IMG/expbad" "$IMG/imp"
$SIM --sd-card --state "$IMG/imp" --keys "import,step100" 2>&1 | grep -q 'import slot 0: bad file' \
  && ok "a damaged card copy is refused on import" \
  || bad "a damaged card copy was not refused on import"
grep '^file lfs PB_00.PBS ' "$IMG/exp" > "$TMP/pb_before" || true
grep '^file lfs PB_00.PBS ' "$IMG/imp" > "$TMP/pb_after"  || true
if [ ! -s "$TMP/pb_before" ]; then
  bad "no local container to protect, so the check below is vacuous"
elif cmp -s "$TMP/pb_before" "$TMP/pb_after"; then
  ok "...and the local slot is byte-identical to before"
else
  bad "the refused import still changed the local slot"
fi
# Control: the undamaged card copy imports.
cp "$IMG/exp" "$IMG/impok"
$SIM --sd-card --state "$IMG/impok" --keys "import,step100" 2>&1 | grep -q 'import slot 0: ok' \
  && ok "an undamaged card copy imports (control)" \
  || bad "the undamaged card copy did not import"

echo "a damaged preset is refused, whichever layer sees the damage"

# One bit flipped inside the stored G section. The container's own section
# checksum catches that first; with the checksum recomputed (corrupt_slot.py
# --fix-sum) the damage reaches PhzConfig's reader, which used to accept a
# G section whose config half failed as long as its data half passed --
# and recalled it, app id and all, from the garbage. The control is the
# undamaged image, which must still recall.
$SIM --state-out "$IMG/good" --keys "$ENTER,l-down,step600,l-up,step4000" >/dev/null 2>&1
$SIM --state-in "$IMG/good" --keys "$ENTER,r-down,step300,r-up,step3000" 2>&1 \
  | grep -q 'recall slot 0 done' \
  && ok "the undamaged slot recalls (control)" \
  || bad "the undamaged slot did not recall, so the two refusals below prove nothing"
python3 corrupt_slot.py "$IMG/good" "$IMG/bad1" 0 G
python3 corrupt_slot.py "$IMG/good" "$IMG/bad2" 0 G --fix-sum
$SIM --state-in "$IMG/bad1" --keys "$ENTER,r-down,step300,r-up,step3000" 2>&1 \
  | grep -q 'refused (BAD PRESET)' \
  && ok "a flipped bit in the G section is refused by the container checksum" \
  || bad "a flipped bit in the G section was not refused"
$SIM --state-in "$IMG/bad2" --keys "$ENTER,r-down,step300,r-up,step3000" 2>&1 \
  | grep -q 'refused (BAD PRESET)' \
  && ok "...and with the container checksum patched, by PhzConfig's own" \
  || bad "a G section with a bad PhzConfig chunk checksum was recalled"
$SIM --state-in "$IMG/bad2" --keys "$ENTER,r-down,step300,r-up,step3000,b,step500" --dump-fb 2>/dev/null \
  | python3 fbtext.py - | grep -q '^y=1 ' \
  && ok "the refused recall leaves an app drawing" \
  || bad "nothing drawn after the refused recall"

# The file-backed sections (B/S/C) used to be checksummed only as they were
# streamed out to their live files in step 4 -- after validation, with the
# app world frozen -- and a failure there was silent: the recall reported
# "done" with the previous slot's file still in place. Scenery gives the sim
# a slot with an S section to damage. Control first, again.
$SIM --app Scenery --state-out "$IMG/goodS" --keys "$ENTER,l-down,step600,l-up,step4000" >/dev/null 2>&1
$SIM --app Scenery --state-in "$IMG/goodS" --keys "$ENTER,r-down,step300,r-up,step3000" 2>&1 \
  | grep -q 'recall slot 0 done' \
  && ok "the undamaged Scenery slot recalls (control)" \
  || bad "the undamaged Scenery slot did not recall"
python3 corrupt_slot.py "$IMG/goodS" "$IMG/badS" 0 S
$SIM --app Scenery --state-in "$IMG/badS" --keys "$ENTER,r-down,step300,r-up,step3000" 2>&1 \
  | grep -q 'refused (BAD PRESET)' \
  && ok "a flipped bit in a file-backed (S) section is refused before anything is applied" \
  || bad "a damaged S section was recalled as done"

echo "the pre-write bank snapshot"

# Before this existed the write path could DETECT that it had damaged a preset
# the user never touched and could never repair one: CommitWrite kept only
# hashes of the other 29 slots, and the verify read-back overwrote the card
# image with the module's now-corrupt contents -- so the last good copy was
# destroyed by the act of checking. One 64 KB block holds the pre-write bank.

snap_line() { $SIM --app "200e Modules" "$@" --dump-fs 2>/dev/null | grep '^fs lfs PBSNAP'; }

# Two halves of one claim. A snapshot must exist after a write, and must NOT
# exist after a read -- it is a write's doing, and a file that were always
# there would satisfy every other check here while proving nothing.
[ -n "$(snap_line --keys "$W251")" ] \
  && ok "an armed and confirmed write leaves a bank snapshot behind" \
  || bad "no snapshot after a write"
[ -z "$(snap_line --keys "$W251_READ")" ] \
  && ok "a read with no write behind it leaves no snapshot" \
  || bad "a read alone left a snapshot: $(snap_line --keys "$W251_READ")"

# It holds the WHOLE bank, not the edited slot. A 251e bank is 63,120 bytes and
# the container adds a 16-byte header; a snapshot of one 2,104-byte slot would
# be the same mistake the short read-back makes -- a safety net woven over 3%
# of the hole.
snap_bytes=$(snap_line --keys "$W251" | awk '{print $4}')
[ "$snap_bytes" = "63136" ] \
  && ok "the snapshot is the whole 63120-byte bank plus its header" \
  || bad "the snapshot is $snap_bytes bytes, not a whole bank (63136)"

# --- the load-bearing one: PRE-write, not post-write ------------------------
#
# The file existing proves nothing about WHEN it was taken. Taken after the
# wire -- or worse, after the verify read-back, which is where the corrupt
# bytes land -- it would faithfully preserve the damage it exists to undo.
#
# So ask the module. Write an edit through a 251e that mishandles that one
# restore (--write-fault-once, so the recovery write afterwards goes out to a
# module that behaves), UNDO, then re-Read and build the SAME edit again:
#
#   snapshot = the bank BEFORE the write  ->  the edit is once more a change
#                                             ("6 bytes change")
#   snapshot = the bank AFTER the write   ->  the edit is already in the module
#                                             ("No changes to write")
#
# Both answers are well-formed screens, so this cannot pass by accident, and
# the control below runs the identical script with the UNDO left out to prove
# the two are actually distinguishable.
# Reaching the undo is three deliberate acts, and that is the design: the
# recovery row is a cursor row in the app's ordinary grammar (encL turn moves,
# encR runs), and its cursor starts on `keep`. So: one encL detent to UNDO,
# encR to arm, and encR again past the 350ms dead window to commit.
UNDO_ARM='],step100,r,step400,r,step8000'
REDO='{,r,step3000,],],r,step10,r,step10,l,step10,],],r,step400'  # re-Read, same edit, arm

undone=$($SIM --app "200e Modules" --write-fault flip-last --write-fault-once \
              --keys "$W251,$UNDO_ARM,$REDO" --dump-fb 2>/dev/null \
         | python3 fbtext.py - | grep '^y=26' | sed 's/^y=26 *x=0 *//')
[ "$undone" = "6 bytes change" ] \
  && ok "the snapshot holds the bank from BEFORE the write, so an undo really undoes" \
  || bad "after an undo the same edit was not a change again (said: '$undone')"

kept=$($SIM --app "200e Modules" --write-fault flip-last --write-fault-once \
            --keys "$W251,r,step300,$REDO" --dump-fb 2>/dev/null \
       | python3 fbtext.py - | grep '^y=36' | sed 's/^y=36 *x=0 *//')
[ "$kept" = "No changes to write" ] \
  && ok "...and without the undo the same edit is no change, so that told us something" \
  || bad "the control case did not report an already-written edit (said: '$kept')"

# An undo is a real write -- 63,120 bytes back on the wire, all 30 slots
# rewritten -- so it must be read back and judged like any other. An undo that
# could not be verified would be no better than the damage it repairs.
undo_verdict=$($SIM --app "200e Modules" --write-fault flip-last --write-fault-once \
                    --keys "$W251,$UNDO_ARM" --dump-fb 2>/dev/null \
               | python3 fbtext.py - | grep '^y=46' | sed 's/^y=46 *x=0 *//')
[ "$undo_verdict" = "WROTE + VERIFIED" ] \
  && ok "an undo is read back and verified like any other write" \
  || bad "an undo produced no verdict of its own (said: '$undo_verdict')"

# The recovery prompt is an OFFER, and an offer that appears when it cannot be
# honoured is worse than none. It replaces the action row only when the module
# is known bad AND a snapshot for THIS target exists.
# The whole row, joined -- not its first run. The recovery row is two entries
# ("keep" and "UNDO") and fbtext reports each as its own run, so a `head -1`
# here answered "[inv] keep" to the question "is UNDO offered?" and got it
# exactly backwards.
row56() { $SIM --app "200e Modules" "$@" --dump-fb 2>/dev/null \
            | python3 fbtext.py - | grep '^y=56' | sed 's/^y=56 *x=[0-9]* *//' \
            | tr '\n' ' '; }
case "$(row56 --write-fault flip-last --keys "$W251")" in
  *UNDO*) ok "a BAD write offers the recovery prompt in place of the action row" ;;
  *) bad "a BAD write did not offer recovery (y=56 said: '$(row56 --write-fault flip-last --keys "$W251")')" ;;
esac
case "$(row56 --keys "$W251")" in
  *UNDO*) bad "a GOOD write offered an undo nobody needs" ;;
  *) ok "a verified write leaves the ordinary action row alone" ;;
esac

# The snapshot is tagged with the address it came from, and LoadSnapshot
# refuses one whose address does not match the target. Restoring a 251e's
# 63,120 bytes into whatever answers at some other address is the single worst
# thing this engine could do, so the offer must not even appear there. `,` and
# `.` walk the address; 0x28 is the 259e.
retarget=$(row56 --write-fault flip-last --keys "$W251,l,step200,l,step200,{,step200,r,step2000")
case "$retarget" in
  *UNDO*) bad "the undo was offered for a module the snapshot did not come from" ;;
  *) ok "the recovery prompt is not offered for another module's bank" ;;
esac

# Both sides of the undo confirm's dead window. It is the same kConfirmDeadMs
# (350 ms) as the write confirm, and for the same reason -- but it is a second
# constant in a second place, so a change to one that misses the other has to
# be caught here. Inside: the prompt is still up. Outside: it commits.
undo_y13() { $SIM --app "200e Modules" --write-fault flip-last --write-fault-once \
                  --keys "$W251,],step100,r,step$1,r,step8000" --dump-fb 2>/dev/null \
             | python3 fbtext.py - | grep '^y=13' | head -1 | sed 's/^y=13 *x=[0-9]* *//'; }
case "$(undo_y13 10)" in
  *UNDO*) ok "a confirm 10ms after arming an undo does nothing -- the prompt is still up" ;;
  *) bad "a confirm inside the undo dead window left the prompt (y=13: '$(undo_y13 10)')" ;;
esac
# Asserted on the VERDICT, not merely on the prompt being gone. "no longer
# showing the undo prompt" is also true of a build that has no undo at all, so
# on its own this half would pass vacuously against the very regression the
# half above catches. A committed undo is a write, and a write leaves a verdict.
undo_late=$($SIM --app "200e Modules" --write-fault flip-last --write-fault-once \
                 --keys "$W251,],step100,r,step400,r,step8000" --dump-fb 2>/dev/null \
            | python3 fbtext.py - | grep '^y=46' | sed 's/^y=46 *x=0 *//')
case "$undo_late" in
  *WROTE*|*VERIFIED*) ok "a confirm 400ms after arming an undo commits and reports a verdict" ;;
  *) bad "a confirm 400ms after arming an undo left no verdict (y=46: '$undo_late')" ;;
esac


# --- the two properties that make the recovery row safe to have at all ------
#
# The row replaces the action row on the screen a user reaches by being told
# their module is damaged, and one of its entries rewrites 63,120 bytes. Both
# assertions below are about the ARRANGEMENT rather than the wording, because
# the arrangement is the safety.

# 1. The cursor starts on the harmless end, every single time a verdict lands.
#    A cursor that remembered UNDO would turn the next reflex encR -- the most
#    practised press in the instrument -- into a whole-bank write.
cursor_on() {  # returns the entry that is inverted, i.e. where the cursor is
  $SIM --app "200e Modules" "$@" --dump-fb 2>/dev/null | python3 fbtext.py - \
    | sed -n 's/^y=56 *x=[0-9]* *\[inv\] *//p' | head -1
}
[ "$(cursor_on --write-fault flip-last --keys "$W251")" = "keep" ] \
  && ok "the recovery cursor starts on the harmless entry" \
  || bad "the recovery cursor did not start on keep (on: '$(cursor_on --write-fault flip-last --keys "$W251")')"

# ...and it is put back there on EVERY verdict, not just the first. The case
# that matters is an undo that itself ends BAD: the cursor was deliberately
# moved to UNDO to get there, a second verdict has just landed on the same
# screen, and the reflex answer to a verdict is encR. If the cursor stayed
# where the user last put it, that reflex would fire another 63,120-byte write
# at a module already known to be wrong. (--write-fault without -once, so the
# module keeps mishandling and the recovery write fails too.)
undone_bad="$W251,],step100,r,step400,r,step8000"
[ "$(cursor_on --write-fault flip-last --keys "$undone_bad")" = "keep" ] \
  && ok "...and is back on the harmless entry after an undo that itself ends BAD" \
  || bad "the recovery cursor stayed on UNDO through a second verdict"

# The control: the cursor CAN be moved, so the two checks above are not simply
# observing a row that never changes.
[ "$(cursor_on --write-fault flip-last --keys "$W251,],step200")" = "UNDO" ] \
  && ok "one encL detent moves the recovery cursor to UNDO, so it really is a cursor" \
  || bad "the recovery cursor did not move on an encL detent"

# 2. A reflex encR in the BAD state puts NOTHING on the wire. This is the whole
#    reason the cursor defaults to `keep`, and it is the assertion most worth
#    surviving someone later "simplifying" the row back to a single button.
#    Asserted on the recovery write's own log line rather than on the screen:
#    a redraw can hide a write, and the firmware says "undo -- restoring N
#    snapshot bytes" exactly when the snapshot goes out. (Not counted against
#    the module's RESTORE lines -- the displayed log is capped, so the first
#    write of a long run scrolls off and a count there measures the cap.)
sent_undo() {  # 1 if the recovery write reached the wire, 0 if not
  $SIM --app "200e Modules" --write-fault flip-last --write-fault-once \
       --keys "$W251,$1" 2>&1 | grep -c 'undo -- restoring'
}
[ "$(sent_undo 'r,step300')" = "0" ] \
  && ok "a reflex encR after a BAD verdict puts nothing on the wire" \
  || bad "a reflex encR after a BAD verdict sent the recovery write"

# Two of them, spaced past the confirm's dead window, which is the gesture that
# actually reaches the wire when the cursor is pointed the wrong way. One press
# is safe on its own merely because the confirm screen exists -- so the single
# press above does NOT test the cursor's default, and only this pair does. It
# is the exact shape of "verdict lands, user taps encR twice to make the screen
# go away", and it must still cost nothing.
[ "$(sent_undo 'r,step400,r,step8000')" = "0" ] \
  && ok "two reflex encR presses after a BAD verdict still put nothing on the wire" \
  || bad "two reflex encR presses committed a 63,120-byte recovery write"

# ...and the deliberate three-step gesture DOES send it, so the check above is
# measuring a real difference and not simply a marker that never appears.
[ "$(sent_undo "$UNDO_ARM")" = "1" ] \
  && ok "...while the deliberate encL-to-UNDO gesture does send it" \
  || bad "the deliberate undo gesture never reached the wire"

echo "small app state is not a whole-EEPROM rewrite"

# The 200e scan result is ~11 bytes. It used to reach storage through
# OC::SaveAppData(), which is a sledgehammer: it calls SaveGlobalSettings
# (loading GLOBALS.CFG into PhzConfig's shared map and never handing it back),
# rewrites 000.SCL-003.SCL on the card, and holds __disable_irq() across flash
# windows -- which masks the I2C slave on a module whose whole job is being on
# a bus. The scan set now has a PhzConfig key of its own.
#
# A scan is ~57 SIMULATED seconds of honest QUERY timeouts, so these are the
# slowest checks in the file; they are worth it, because "does it come back
# after a power cycle" has no cheaper form.
SCAN='l,step70000'

# Start from an image with valid stored settings, so a second boot is an
# ordinary boot rather than a first run -- otherwise every property below is
# confounded by the first-run path.
rm -f "$IMG/base"
$SIM --keys "$MENU,r-down,step900,r-up,step600" --state-out "$IMG/base" >/dev/null 2>&1

cp "$IMG/base" "$IMG/scan"
$SIM --app "200e Modules" --state "$IMG/scan" --keys "$SCAN" >/dev/null 2>&1

# The property, phrased so it cannot be satisfied by re-scanning: read the
# module list on a fresh boot WITHOUT scanning. "B:probe N" counts what the app
# believes it found, so N>0 with no scan in the script is memory, not a scan.
found_after_cycle=$($SIM --app "200e Modules" --state-in "$IMG/scan" \
                         --keys "step2000" --dump-fb 2>/dev/null \
                    | python3 fbtext.py - | grep -o 'B:probe [0-9]*' | head -1)
[ "$found_after_cycle" = "B:probe 3" ] \
  && ok "the scan set survives a power cycle without re-scanning" \
  || bad "the scan set did not survive a power cycle (said: '$found_after_cycle')"

# The control: the same screen on a module that never scanned. Without this the
# check above would pass against an app that hard-codes three responders.
found_virgin=$($SIM --app "200e Modules" --state-in "$IMG/base" \
                    --keys "step2000" --dump-fb 2>/dev/null \
               | python3 fbtext.py - | grep -o 'B:probe [0-9]*' | head -1)
[ "$found_virgin" = "B:probe 0" ] \
  && ok "...and a module that never scanned still reports nothing found" \
  || bad "an unscanned module already claims responders (said: '$found_virgin')"

# The sledgehammer itself, and the sharpest evidence of it. OC::SaveAppData()
# calls SaveGlobalSettings(), which rewrites 000.SCL-003.SCL on the card -- so
# an 11-byte scan result, on a module with a card seated, wrote four scale
# files it has nothing to do with. Asserted as "the card gained nothing",
# which is a property of the whole write rather than of those four names.
$SIM --sd-card --app "200e Modules" --state-in "$IMG/base" --keys "$SCAN" \
     --dump-fs 2>/dev/null > "$TMP/scanfs.txt"
grep -q '^fs sd ' "$TMP/scanfs.txt" \
  && bad "persisting the scan set wrote to the card: $(grep '^fs sd ' "$TMP/scanfs.txt" | tr '\n' ' ')" \
  || ok "persisting the scan set writes nothing to the card"

# The EEPROM is where calibration lives on this target -- app data does not,
# it is in GLOBALS.CFG through PhzConfig. So this is not a re-test of the line
# above; it is the standing invariant that scanning a bus never disturbs the
# module's calibration, compared as a CRC over the whole array so it holds
# whatever the layout becomes.
ee() { grep '^eeprom ' "$1" | awk '{print $2}'; }   # the image's eeprom hex
[ -n "$(ee "$IMG/base")" ] && [ "$(ee "$IMG/base")" = "$(ee "$IMG/scan")" ] \
  && ok "persisting the scan set leaves the calibration EEPROM untouched" \
  || bad "persisting the scan set rewrote the EEPROM"

# Another app's stored state must survive it. The app the switcher saved is the
# nearest thing to "somebody else's setting" this build can express.
app_before=$($SIM --state-in "$IMG/base" --keys "step300" 2>&1 | grep -Eo 'app=[^ ]+' | tail -1)
app_after=$($SIM --state-in "$IMG/scan" --keys "step300" 2>&1 | grep -Eo 'app=[^ ]+' | tail -1)
[ -n "$app_before" ] && [ "$app_before" = "$app_after" ] \
  && ok "persisting the scan set does not disturb another app's stored state" \
  || bad "the stored app changed across a scan ('$app_before' -> '$app_after')"

# PhzConfig's shared map must be handed back afterwards. The failure this
# guards is not visible on the 200e screen at all: SaveGlobalSettings leaves
# GLOBALS.CFG resident in the shared map, so the NEXT thing to write a config
# file writes the wrong map. A preset STORE is that next thing, and it is the
# one whose corruption would be silent and permanent.
$SIM --app "200e Modules" --state-in "$IMG/scan" \
     --keys "$SCAN,$ENTER,l-down,step600,l-up,step4000" 2>&1 \
  | grep -q 'PresetEngine: save' \
  && ok "a preset still stores after a scan persists, so the config map was handed back" \
  || bad "a preset store failed after a scan persisted"

echo "the boot path decides before it mutates"

# AppSwitcher::Init() used to run InitDefaults() on every app and zero the user
# Turing machines BEFORE ConfirmReset() asked, so every app's live state was
# already gone by the time anything sought permission. The decision now
# precedes the mutation.

# A boot with valid stored settings must restore them rather than take the
# first-run path. Asserted on a SPECIFIC app, chosen with the switcher's own
# long click (switch and save) so that the value restored is one this script
# put there -- "some app came up" is true of a first run too, and would have
# been a check that could not fail.
rm -f "$IMG/setup"
$SIM --keys "$MENU,encr-,encr-,step100,r-down,step900,r-up,step600" \
     --state-out "$IMG/setup" >/dev/null 2>&1
restored=$($SIM --state-in "$IMG/setup" --keys "step300" 2>&1 \
           | sed -n 's/^  [a-z][a-z]*  *app=\(.*\)  held=.*/\1/p' | tail -1)
[ "$restored" = "Setup/About" ] \
  && ok "a boot with valid stored settings restores the app that was saved" \
  || bad "the saved app did not come back after a power cycle (got: '$restored')"
$SIM --state-in "$IMG/base" --keys "step300" 2>&1 | grep -q 'ConfirmReset answered' \
  && bad "a boot with valid stored settings still opened the reset prompt" \
  || ok "a boot with valid stored settings does not ask about a reset at all"

# First run still works: nothing stored, the prompt appears, OK erases and the
# instrument comes up.
$SIM --reset-settings --keys "step300" 2>&1 | grep -q 'ConfirmReset answered OK' \
  && ok "the first-run/EEPROM-reset gesture reaches its prompt" \
  || bad "--reset-settings did not reach the ConfirmReset prompt"
$SIM --reset-settings --keys "step300" 2>&1 | grep -Eq '^  [a-z]+ +app=' \
  && ok "an accepted reset leaves a running instrument" \
  || bad "an accepted reset left no running app"

# The one that matters: a reset the user BACKS OUT OF must leave storage as it
# was. This is the whole "intent before mutation" claim, and it is a claim
# about bytes, so it is checked as bytes -- the image before and the image
# after must be identical. --reset-cancel performs the module's own A+B gesture
# and then answers the prompt CANCEL.
cp "$IMG/base" "$IMG/cancelled"
$SIM --reset-cancel --state "$IMG/cancelled" --keys "step300" >/dev/null 2>&1
cmp -s "$IMG/base" "$IMG/cancelled" \
  && ok "a CANCELLED reset leaves stored settings byte-identical" \
  || bad "a cancelled reset changed stored settings"

# ...and the control, without which the above passes on any run that writes
# nothing at all: an ACCEPTED reset must change them.
cp "$IMG/base" "$IMG/accepted"
$SIM --reset-settings --state "$IMG/accepted" --keys "step300" >/dev/null 2>&1
cmp -s "$IMG/base" "$IMG/accepted" \
  && bad "an ACCEPTED reset changed nothing -- the cancel check above proves nothing" \
  || ok "...and an accepted reset does change them, so that was a real comparison"

echo "a chord-opened screen ignores the chord that opened it"

# Every global gesture in this instrument is an unlabelled chord, and the hand
# does not release a chord's two buttons at the same instant. The proven case:
# the both-encoder chord left encR held, and the preset overlay's 250ms RECALL
# hold timer -- which reads the button directly rather than waiting for an
# event -- fired on it. A bus-wide recall nobody asked for.
#
# The rule is now global (Ui::IgnoreUntilRelease), so the check is too, and it
# is stated as a property with no strings in it: entering a screen with the
# chord's second button STILL HELD must leave exactly the frame that a clean
# entry leaves. Anything the stray hold did -- moving a cursor, activating a
# row, arming a timer, dismissing the screen -- changes pixels.
absorbs() {  # name, expected-screen (or "-"), clean keys, fumbled keys, extra args
  name=$1; want=$2; clean=$3; fumbled=$4; shift 4
  $SIM "$@" --keys "$clean"   --dump-fb 2>/dev/null > "$TMP/clean.hex"
  $SIM "$@" --keys "$fumbled" --dump-fb 2>/dev/null > "$TMP/fumbled.hex"

  # A clean entry must actually have drawn something, or two blank frames
  # would compare equal and the check would pass on nothing.
  if ! grep -q '[1-9A-Fa-f]' "$TMP/clean.hex"; then
    bad "$name: the clean entry drew an empty frame"; return
  fi

  # The screen must still be OPEN. This is the half that catches the loudest
  # form of the leak, and it is the half a frame comparison alone misses: the
  # chord's second button is released a moment after the screen opens in the
  # CLEAN case too, so with the guard removed both entries leak and both end up
  # back at the app -- identical frames, and a comparison that agrees the
  # instrument is fine. Naming the screen the gesture opened is what makes the
  # two cases distinguishable. ("-" for a screen the simulator has no word for:
  # the IO settings menu is drawn by the app, so g_ui_mode never changes.)
  if [ "$want" != "-" ]; then
    got=$($SIM "$@" --keys "$fumbled" 2>&1 \
          | sed -n 's/^  \([a-z][a-z]*\)  *app=.*/\1/p' | tail -1)
    if [ "$got" != "$want" ]; then
      bad "$name: the held chord button closed the screen (screen '$got', wanted '$want')"
      return
    fi
  fi

  # ...and nothing subtler happened either: no cursor moved, no row activated,
  # no timer armed. Anything the stray hold did changes pixels.
  cmp -s "$TMP/clean.hex" "$TMP/fumbled.hex" \
    && ok "$name absorbs a fumbled entry gesture" \
    || bad "$name acted on the chord button that was still held"
}
absorbs "the app switcher (A+encR, encR held)" menu \
        "a-down,step60,r-down,step60,r-up,step60,a-up,step900" \
        "a-down,step60,r-down,step60,a-up,step900,r-up,step60"
absorbs "the screensaver (Z+A, A held)" saver \
        "z-down,step60,a-down,step60,a-up,step60,z-up,step900" \
        "z-down,step60,a-down,step60,z-up,step900,a-up,step60"
absorbs "the preset overlay (both encoders, encR held)" preset \
        "l-down,step20,r-down,step80,l-up,r-up,step900" \
        "l-down,step20,r-down,step80,l-up,step900,r-up,step60"
absorbs "the IO settings screen (A+encL, encL held)" - \
        "a-down,step60,l-down,step60,l-up,step60,a-up,step900" \
        "a-down,step60,l-down,step60,a-up,step900,l-up,step60" --app Scenery

# The rule is "until released", not "for a while", so a press AFTER the release
# must work normally. Without this pair the guard could be a permanent mute and
# every check above would still pass.
$SIM --keys "$MENU,encr-,encr-,step100,r,step600" 2>&1 | grep -q 'app=Setup/About' \
  && ok "a deliberate press after the chord is released still acts" \
  || bad "the release-first guard swallowed a deliberate press too"

# The hold-sampled gestures are the ones the guard had to be careful with:
# STORE (500ms) and RECALL (250ms) are timed against ui.read_immediate(), i.e.
# by reading the pin, not by waiting for an event -- which is exactly why the
# entry chord could satisfy one. The four cases above under "preset overlay
# holds" pin both sides of both thresholds; this asserts the guard did not
# quietly break the mechanism they rely on by making read_immediate always
# false. A 600ms hold after a clean entry must still store.
$SIM --keys "$ENTER,l-down,step600,l-up,step3000" 2>&1 | grep -q 'PresetEngine: save' \
  && ok "the release-first rule leaves the hold-sampled STORE working" \
  || bad "the release-first rule broke the 500ms STORE hold"

echo "hold A reveals the chord list"

# Every global gesture here is an unlabelled chord, which is fine once and
# unguessable the first time. Holding the modifier alone now lists them.
# kChordHintDelayTicks is 700 UI ticks: long enough that the first half of a
# real chord never flashes a card at you, short enough to find by accident.
hint_rows() { $SIM "$@" --dump-fb 2>/dev/null | python3 fbtext.py -; }

hint_rows --keys "a-down,step400" | grep -q 'HOLD A' \
  && bad "the chord list appeared 400ms in, inside its own 700-tick delay" \
  || ok "holding A for 400ms shows nothing -- the delay is real"
hint_rows --keys "a-down,step900" | grep -q 'HOLD A' \
  && ok "holding A past the delay reveals the chord list" \
  || bad "holding A for 900ms revealed no chord list"

# It is a hint, not a mode. Releasing without a second press must do nothing at
# all -- not switch app, not open anything, not leave a mark.
$SIM --keys "a-down,step900,a-up,step300" --dump-fb 2>/dev/null > "$TMP/afterhint.hex"
$SIM --keys "step1200" --dump-fb 2>/dev/null > "$TMP/nohint.hex"
cmp -s "$TMP/afterhint.hex" "$TMP/nohint.hex" \
  && ok "releasing the hold without a second press does nothing at all" \
  || bad "revealing the chord list and letting go left the instrument changed"

# ...and the chords still work THROUGH it: the overlay must not eat the second
# press it exists to advertise.
$SIM --keys "a-down,step900,r-down,step60,r-up,step60,a-up,step200" 2>&1 \
  | grep -q '^  menu ' \
  && ok "a chord completed through the revealed list still works" \
  || bad "the chord list swallowed the chord it was advertising"

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
sweep 200e-recovery-row --app "200e Modules" --write-fault ignore --keys "$W251"
sweep 200e-undo-confirm --app "200e Modules" --write-fault ignore \
      --keys "$W251,],step100,r,step10"
sweep setup --app "Setup/About"
sweep scenery --app Scenery
sweep screensaver --keys "z-down,step60,a-down,step60,a-up,step60,z-up,step300"
echo


# ---------------------------------------------------------------------------
# The debug-stats page, which until now had no layout coverage at all.
#
# It is a `while (!exit_loop)` that owns the process, and by the time it ends
# the app menu has redrawn over it -- so --dump-fb showed the menu and the page
# itself was unverifiable. `snapN` fixes that: it arms a capture N ms ahead,
# and the capture fires from the SH1106 shim's page-7 write, so what it keeps
# is one WHOLE frame drawn from inside the loop rather than a torn blend.
#
# Getting past page 1 needs the other half: paging is an encL TURN while an
# encL PRESS is the exit, and detents queued before the loop are eaten by the
# app menu while the entry gesture is still being held. "qencl+N@T" delivers
# them T ms later, i.e. inside the loop.
#
# Worth checking because an exit hint was recently added here whose geometry
# nothing checked, on the one screen in the instrument that looks like a crash
# if you cannot tell how to leave it.
echo "the debug-stats page has a layout, and it is checked"

# encL held long = enter; the r-inN tap is the exit press, in flight before the
# loop starts because the loop never gives the script another turn.
ds_page() {  # detents to page forward
  $SIM --keys "$MENU,r-in3000,qencl+$1@700,l-down,snap1400,step3500,l-up,step200" \
       --dump-fb 2>/dev/null > "$TMP/ds.hex"
}

ds_page 0
python3 fbtext.py "$TMP/ds.hex" > "$TMP/dstext"
# The page must actually have been captured -- if the timing ever drifts past
# the loop, the capture would hold the app menu instead and every assertion
# below would be about the wrong screen. The page number is the proof, and it
# is a property of the page rather than a string anyone will reword.
grep -Eq '^y=2 +x=2 +1/[0-9]+ ' "$TMP/dstext" \
  && ok "a frame can be captured from inside the debug-stats loop" \
  || bad "the capture did not land on the debug-stats page: $(head -2 "$TMP/dstext")"

# The exit hint. This page's only way out used to be a press it never
# mentioned, which reads as a crash. It is right-aligned to x=127, so it also
# happens to be the one string in the instrument that legitimately occupies the
# last pixel column -- see the print_right note in edgecheck.py.
grep -q 'R:exit' "$TMP/dstext" \
  && ok "the debug-stats page says how to leave it" \
  || bad "the debug-stats page advertises no exit"

# Title and hint must not collide. The title is drawn with drawStrClipX into
# the space before the hint; if that clip window is ever widened past kHintX
# the two overlap and BOTH become unreadable, which no text assertion can see
# (an overlap matches no glyph, so it simply vanishes from the decode). So
# assert they are separate runs on the same row -- the title starting at the
# left, the hint at the right.
title_x=$(grep -E '^y=2 ' "$TMP/dstext" | head -1 | sed -E 's/^y=2 +x=([0-9]+).*/\1/')
hint_x=$(grep 'R:exit' "$TMP/dstext" | sed -E 's/^y=[0-9]+ +x=([0-9]+).*/\1/')
if [ -n "$title_x" ] && [ -n "$hint_x" ] && [ "$title_x" -lt "$hint_x" ]; then
  ok "the title and the exit hint are separate runs, title first"
else
  bad "title/hint geometry is wrong (title x=$title_x, hint x=$hint_x)"
fi

# ...and the generic rule, now that the page can be reached: nothing clipped,
# on every page of it. The pages are walked with real detents through the real
# decoder, so encoder ACCELERATION applies and N detents is not N pages -- the
# sweep does not care which pages it lands on, only that none of them clips.
printf '  ok    no clipped text on debug-stats pages: '
for n in 0 1 2 3 5 8; do
  ds_page $n
  python3 edgecheck.py "$TMP/ds.hex" > "$TMP/edge.txt" 2>&1 \
    || { printf '\n'; bad "a debug-stats page is clipped: $(cat "$TMP/edge.txt")"; break; }
  printf '%s ' "$(python3 fbtext.py "$TMP/ds.hex" | sed -nE 's/^y=2 +x=2 +([0-9]+\/[0-9]+).*/\1/p' | head -1)"
done
echo

[ $fail -eq 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $fail
