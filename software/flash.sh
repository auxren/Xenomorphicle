#!/usr/bin/env bash
# Build and flash the Xenomorpher, refusing every way this went wrong on
# 2026-08-27. See docs/bench-console.md and Orin_Fun notes/xenomorph-flashing.md.
#
#   ./flash.sh                 build T41_audio and flash it to the rig
#   ./flash.sh T41_audio_dbg   a different environment, still address-checked
#   RIG=orin ./flash.sh        somewhere else
set -euo pipefail

ENV="${1:-T41_audio}"
RIG="${RIG:-orin}"
HERE="$(cd "$(dirname "$0")" && pwd)"
HEX="$HERE/.pio/build/$ENV/firmware.hex"
LOADER="${LOADER:-\$HOME/xeno/teensy_loader_cli_23}"

die() { printf '\033[31mREFUSING: %s\033[0m\n' "$*" >&2; exit 1; }
say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "building $ENV"
pio run -e "$ENV" >/dev/null || die "build failed"
[ -s "$HEX" ] || die "no hex at $HEX"

# ---------------------------------------------------------------------------
# THE CHECK THIS SCRIPT EXISTS FOR.
#
# platformio.ini gives each environment its own linker script:
#   T41       -> slot1.ld -> 0x60100000   NOT independently bootable
#   T41_audio -> slot0.ld -> 0x60000000   bootable
#   T41_MTP   -> slot2.ld
# A slot1 image programs perfectly, reports "Booting", and leaves the RT1062
# finding nothing at 0x60000000 -- so it falls into its ROM downloader and
# enumerates as 1fc9:0135 "SE Blank RT Family", which looks exactly like a
# dead module. That cost an evening, two Teensy restores, and a lot of
# pressing a button that this board does not expose.
#
# An Intel HEX for slot 0 opens with an extended-linear-address record of
# 6000 followed by the FlexSPI config block ("FCFB" = 46 43 46 42).
# ---------------------------------------------------------------------------
say "checking the boot address"
FIRST=$(head -1 "$HEX" | tr -d '\r')
BASE=$(printf '%s' "$FIRST" | sed -n 's/^:02000004\(....\).*/\1/p')
[ -n "$BASE" ] || die "first record is not an extended-address record: $FIRST"
if [ "$BASE" != "6000" ]; then
    die "$ENV links to 0x${BASE}0000, not 0x60000000.
   That image is a SLOT build. It will program, say 'Booting', and leave the
   module looking blank (1fc9:0135). Use an environment whose ldscript is
   slot0.ld -- T41_audio is the one this module runs."
fi
grep -q '^:10000000 *46434642' <(sed 's/\(:10000000\)/\1 /' "$HEX" | head -2) \
    || printf '   note: no FCFB block on line 2; unusual but not fatal\n'
printf '   base 0x60000000, bootable\n'

say "staging"
scp -q "$HEX" "$RIG:/tmp/xeno-new.hex"
# Keep whatever is currently staged as the rollback, and NEVER let the new
# image overwrite it -- on 2026-08-27 the known-good was clobbered by the
# build that then failed, leaving nothing to fall back to.
ssh "$RIG" '[ -f /tmp/xeno-rollback.hex ] || cp -n /tmp/xeno-audio.hex /tmp/xeno-rollback.hex 2>/dev/null || true'
ssh "$RIG" 'md5sum /tmp/xeno-new.hex' | sed 's/^/   /'

say "quieting the instrument"
# A reboot with CVs patched into a live Buchla is a transient into somebody's
# speakers. ember.py's SIGTERM path drives all six continuous outputs to zero.
ssh "$RIG" 'P=$(pgrep -f "ember[.]py" | head -1); [ -n "$P" ] && kill -TERM "$P" && sleep 4 && echo "   ember stopped" || echo "   nothing playing"'

say "flashing"
ssh "$RIG" "$LOADER --mcu=TEENSY41 -w -v /tmp/xeno-new.hex" 2>&1 | tail -3

# ---------------------------------------------------------------------------
# "Booting" is the loader saying it finished SENDING. It is not the module
# saying it is alive. Verify by enumeration or call it a failure.
# ---------------------------------------------------------------------------
say "verifying"
OK=0
for _ in $(seq 1 15); do
    sleep 2
    if ssh "$RIG" 'lsusb | grep -qE "16c0:(048a|04d2)" && grep -q Phazerville /proc/asound/cards'; then
        OK=1; break
    fi
done
if [ "$OK" != 1 ]; then
    ssh "$RIG" 'lsusb | grep -iE "16c0|1fc9" | sed "s/^/   /"' || true
    die "module did not come back with its ALSA card.
   If it shows 1fc9:0135 the flash did not take; /tmp/xeno-rollback.hex is
   the last image known to boot."
fi
ssh "$RIG" 'grep -A1 Phazerville /proc/asound/cards | sed "s/^/   /"'
printf '\n\033[1m== flashed and verified\033[0m\n'
