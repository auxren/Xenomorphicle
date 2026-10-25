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
suite test_arp_engine
suite test_audio_graph_topo
suite test_fade
suite test_buchla251e_slot_codec    -I host_stubs ../src/Buchla251eSlotCodec.cpp \
                                    ../src/Buchla251eGenerator.cpp ../src/src/extern/bjorklund.cpp
suite test_buchla251e_recorder      ../src/Buchla251eSlotCodec.cpp ../src/Buchla251eRecorder.cpp
suite test_buchla259e_slot_codec    ../src/Buchla259eSlotCodec.cpp
suite test_buchla227e_slot_codec    ../src/Buchla227eSlotCodec.cpp
suite test_buchla200e_module_table  ../src/Buchla200eModuleTable.cpp
suite test_buchla200e_fw_versions   ../src/Buchla200eFwVersions.cpp
suite test_buchla200e_write_guard   ../src/Buchla200eWriteGuard.cpp
suite test_buchla200e_uigate        ../src/Buchla200eUiGate.cpp
suite test_buchla251e_editgenrec    -I host_stubs ../src/Buchla251eSlotCodec.cpp \
                                    ../src/Buchla251eGenerator.cpp ../src/Buchla251eRecorder.cpp \
                                    ../src/src/extern/bjorklund.cpp
suite test_tweighty_transport       ../src/TweightyTransport.cpp
suite test_tweighty_tap_phase       ../src/TweightyTapPhase.cpp
suite test_scope_sources            ../src/ScopeSources.cpp ../src/ScopeSettings.cpp ../src/ScopeRange.cpp
suite test_scope_ring
suite test_scope_range              ../src/ScopeRange.cpp
suite test_scope_trigger            ../src/ScopeTrigger.cpp
suite test_scope_fft                ../src/ScopeFft.cpp
suite test_sampler_math             ../src/SamplerMath.cpp
suite test_sample_convert
suite test_preset_op_queue
suite test_cvmeter_math             ../src/CvMeterMath.cpp
suite test_calroutine_math          ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp
suite test_usbtmc                   ../src/UsbTmc.cpp
suite test_app_folders              ../src/OC_app_folders.cpp
suite test_input_curve_store        ../src/InputCurveStore.cpp
suite test_adc_slot_map             ../src/CvMeterMath.cpp ../src/InputCurveStore.cpp
suite test_input_cal                ../src/InputCal.cpp ../src/CalRoutineMath.cpp \
                                    ../src/CvMeterMath.cpp ../src/InputCurveStore.cpp
suite test_calrec                   ../src/CalRec.cpp
suite test_cal259e_bus              ../src/Cal259eBus.cpp ../src/CalRec.cpp
suite test_calroutine_measure       ../src/CalRoutineMeasure.cpp \
                                    ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
suite test_cal259e_flow             ../src/Cal259eFlow.cpp ../src/Cal259eBus.cpp ../src/CalRec.cpp \
                                    ../src/CalRoutineMeasure.cpp ../src/CalRoutineMath.cpp \
                                    ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
suite test_cal259e_backup           ../src/Cal259eBackup.cpp ../src/Cal259eFlow.cpp ../src/Cal259eBus.cpp \
                                    ../src/CalRec.cpp ../src/CalRoutineMeasure.cpp \
                                    ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
suite test_cal259e_hold
suite test_cal259e_text             ../src/Cal259eText.cpp ../src/Cal259eBackup.cpp ../src/Cal259eFlow.cpp \
                                    ../src/Cal259eBus.cpp ../src/CalRec.cpp ../src/CalRoutineMeasure.cpp \
                                    ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
# The 259e write path is unreachable in the firmware (Cal259eFlow::kAllowWrite is
# false until a real write is proven on a bench 259e and the target scale is proven;
# kAllowCompaction is false until a tested restore path for CAL259BK.BIN exists). Their
# code and tests are kept alive by building the same suite with the constants on. The
# plain build above runs the read-only half and the write-disabled checks.
flow_variant() {  # <label> <define>
  suite_count=$((suite_count + 1))
  printf '\n--- %stest_cal259e_flow (%s) ---\n' "$TAG" "$1"
  # shellcheck disable=SC2086
  $CXX $WARN $OPT "$2" -o "build/${TAG}test_cal259e_flow_$3" \
    test_cal259e_flow.cpp ../src/Cal259eFlow.cpp ../src/Cal259eBus.cpp ../src/CalRec.cpp \
    ../src/CalRoutineMeasure.cpp ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp
  "./build/${TAG}test_cal259e_flow_$3"
}
flow_variant "CAL259E_ALLOW_WRITE=0 (stays read-only)" -DCAL259E_ALLOW_WRITE=0 write0
flow_variant "CAL259E_ALLOW_COMPACTION=0 (stays read-only)" -DCAL259E_ALLOW_COMPACTION=0 compaction0
flow_variant "CAL259E_ALLOW_WRITE=1" -DCAL259E_ALLOW_WRITE=1 write
flow_variant "CAL259E_ALLOW_COMPACTION=1" -DCAL259E_ALLOW_COMPACTION=1 compaction
suite test_calroutine_flow          ../src/CalRoutineFlow.cpp ../src/CalRoutineMeasure.cpp \
                                    ../src/CalRoutineMath.cpp ../src/CvMeterMath.cpp ../src/UsbTmc.cpp

# Counted, not typed: the literal that used to live here said 25 while 26
# suites ran, which is the same hand-maintained-list drift this file exists
# to end.
printf '\n%s%d host suites passed.\n' "$TAG" "$suite_count"
