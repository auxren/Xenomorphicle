#!/usr/bin/env bash
# Architectural gates for the Xenomorpher hard fork. Cheap grep-based
# checks that fail the build when something deleted in the fork creeps
# back. Run from anywhere; CI runs it in the host-tests job.
#
# Each gate prints its offending lines and the script exits non-zero if
# any gate fails. Add a gate by appending to the list at the bottom.
set -u
cd "$(dirname "$0")/../.."

fails=0

# gate <name> <description> <hits>: hits is the (possibly empty) grep output
gate() {
  local name="$1" desc="$2" hits="$3"
  if [ -n "$hits" ]; then
    echo "FAIL  $name: $desc"
    printf '%s\n' "$hits" | sed 's/^/      /'
    fails=$((fails + 1))
  else
    echo "ok    $name"
  fi
}

# code_lines: drop whole-line comments (// and block-comment bodies that
# start with * or /*, as far as a line filter can tell). Input lines carry
# grep's "path:line:" prefix, so the anchor skips that first.
code_lines() { grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|/\*|\*)' ; }

SRC=software/src

# 1. No preprocessor test of a hardware flag the fork deleted. The runtime
#    NorthernLightModular bool and flip_mode are not flags and are allowed.
hits=$(grep -rnE '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif)\b.*\b(NORTHERNLIGHT(_2OC_LEFTSIDE)?|NLM_(hOC|cardOC|DIY)|VOR|FLIP_180|ARDUINO_TEENSY3[0-9]|ARDUINO_TEENSY40|__MK20DX256__|__MK64FX512__|__MK66FX1M0__)\b' \
  "$SRC" 2>/dev/null)
gate "deleted-hardware-flags" "a #if tests a hardware flag no environment defines" "$hits"

# 2. Deleted apps never reappear (Ponglet.h is a kept applet, not Pong;
#    HemisphereApplet.h is the applet base, not the deleted host).
hits=$(grep -rnE '\b(AppPong|AppScenery|AppUsbDrive|AppWaveformEditor|AppHemisphere)\b|(PongGame|Scenery|UsbDriveApp|WaveformEditor|Hemisphere)\.h\b' \
  "$SRC" software/platformio.ini tools/xeno-sim/Makefile tools/xeno-sim/shim 2>/dev/null \
  | grep -v 'HemisphereApplet\.h')
gate "deleted-apps" "a deleted app or its header is referenced again" "$hits"

# 3. No vestigial build environments in platformio.ini.
hits=$(grep -nE '^\[env:(T3|T4|T40|T32|T32_vor|stock|stock_vor|antihem|custom|oc_dev|T41_dev|T41_audio_blk64|nlm[A-Za-z0-9_]*)\]' \
  software/platformio.ini 2>/dev/null)
gate "deleted-envs" "a deleted PlatformIO environment is back" "$hits"

# 4. No live Serial output in the audio ISR files. A flash-resident string
#    read inside the audio software interrupt is a real-time hazard; count
#    the condition in OC::RT::stats instead.
hits=$(grep -nE '\bSerial\.(print|println|printf|write)\b' \
  "$SRC"/extern/f32/output_i2s2_F32.cpp "$SRC"/extern/f32/input_i2s2_F32.cpp \
  "$SRC"/extern/f32/AudioStream_F32.cpp 2>/dev/null | code_lines)
gate "no-serial-in-audio-isr" "Serial output inside an audio ISR path (use a counter)" "$hits"

# 5. No std::function / std::queue in the ISR defer path.
hits=$(grep -nE 'std::(function|queue)' "$SRC"/OC_core.h "$SRC"/OC_core.cpp "$SRC"/DeferRing.h 2>/dev/null | code_lines)
gate "defer-path-allocation-free" "the ISR->loop defer path must not allocate" "$hits"

echo
if [ "$fails" -ne 0 ]; then
  echo "fork gates: $fails FAILED"
  exit 1
fi
echo "fork gates: all passed"
