#!/usr/bin/env bash
# Every host suite, in one place.
#
# This exists because CI and the bench had drifted apart. The bench ran a
# hand-maintained copy of the list, the two disagreed about both the suites
# and the warnings, and two real bugs went to main green because of it: a
# sign-compare GCC rejects and clang does not, and a test overrunning its own
# array, which only corrupted anything under GCC's layout. CI, the sanitizer
# job and whoever is sitting at the module now run this same file.
#
#   ./run-host-tests.sh              normal build, what CI gates on
#   SAN=1 ./run-host-tests.sh        AddressSanitizer + UBSan, aborts on any
#
# CXX overrides the compiler. Exits non-zero on the first failure.
set -e
cd "$(dirname "$0")"
mkdir -p build

CXX=${CXX:-g++}
# -Wsign-compare is named explicitly: GCC has it in -Wall for C++, Apple clang
# only in -Wextra. Without it the bench cannot reproduce a CI failure.
WARN="-std=c++17 -Wall -Wsign-compare -Werror"

if [ "${SAN:-0}" = "1" ]; then
  OPT="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
  TAG="san_"
else
  OPT="-O2"
  TAG=""
fi

# suite <name> [extra sources / flags ...]
suite_count=0
suite() {
  local name="$1"; shift
  suite_count=$((suite_count + 1))
  printf '\n--- %s%s ---\n' "$TAG" "$name"
  # shellcheck disable=SC2086
  $CXX $WARN $OPT -o "build/${TAG}${name}" "${name}.cpp" "$@"
  "./build/${TAG}${name}"
}

suite test_bus200e                  ../src/PresetBus200e.cpp
suite test_bus200e_master           ../src/Bus200eMaster.cpp ../src/PresetBus200e.cpp
suite test_bus200e_sysex            ../src/Bus200eSysEx.cpp
suite test_bus200e_bridge           ../src/Bus200eBridge.cpp ../src/Bus200eSysEx.cpp
suite test_buscard                  ../src/PresetBusCard.cpp
suite test_midi_types
suite test_miditxring
suite test_rtstats
suite test_defer_ring
suite test_preset_stage
suite test_card_sectors
suite test_audio_graph_order
suite test_f32_int_convert
suite test_audio_graph_topo
suite test_fade
suite test_buchla251e_slot_codec    -I host_stubs ../src/Buchla251eSlotCodec.cpp \
                                    ../src/Buchla251eGenerator.cpp ../src/src/extern/bjorklund.cpp
suite test_buchla251e_recorder      ../src/Buchla251eSlotCodec.cpp ../src/Buchla251eRecorder.cpp
suite test_buchla259e_slot_codec    ../src/Buchla259eSlotCodec.cpp
suite test_buchla200e_module_table  ../src/Buchla200eModuleTable.cpp
suite test_buchla200e_write_guard   ../src/Buchla200eWriteGuard.cpp
suite test_buchla200e_uigate        ../src/Buchla200eUiGate.cpp
suite test_buchla251e_editgenrec    -I host_stubs ../src/Buchla251eSlotCodec.cpp \
                                    ../src/Buchla251eGenerator.cpp ../src/Buchla251eRecorder.cpp \
                                    ../src/src/extern/bjorklund.cpp
suite test_tweighty_transport       ../src/TweightyTransport.cpp
suite test_tweighty_tap_phase       ../src/TweightyTapPhase.cpp
suite test_scope_math               ../src/ScopeMath.cpp
suite test_sampler_math             ../src/SamplerMath.cpp
suite test_sample_convert
suite test_preset_op_queue

# Counted, not typed: the literal that used to live here said 25 while 26
# suites ran, which is the same hand-maintained-list drift this file exists
# to end.
printf '\n%s%d host suites passed.\n' "$TAG" "$suite_count"
