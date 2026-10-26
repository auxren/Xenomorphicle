#!/usr/bin/env bash
# Proves the frequency counter's service, line, console and settings suites can
# fail: applies one deliberate bug at a time to a private copy of the sources,
# runs the suite that owns that code, and requires red. A mutation that leaves
# the suite green is a hole in the tests; this script exits non-zero if there is
# one. Nothing in the tree is modified. (The estimator has its own,
# mutate-scope-freq.sh; the screen/layout and settings-migration checks in
# tools/xeno-sim are proven by hand against the same kind of edit, see the spec.)
#   ./mutate-freq-wire.sh
set -u
cd "$(dirname "$0")"
CXX=${CXX:-g++}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/src" "$W/test"
cp ../src/Scope*.h ../src/Scope*.cpp "$W/src/"
cp host_stubs/nofloat_printf.h "$W/nofloat_printf.h"
cp test_scope_freq.cpp test_scope_fmt.cpp test_scope_fft.cpp test_scope_freq_service.cpp test_scope_freq_line.cpp \
   test_scope_freq_console.cpp test_scope_sources.cpp "$W/test/"
mkdir -p "$W/orig" && cp "$W"/src/* "$W/orig/"

# suite name -> test file + sources (under src/) + flags
suite_cmd() {
  case "$1" in
    service)  echo "test_scope_freq_service.cpp -pthread ScopeFreqService.cpp ScopeFreqLine.cpp ScopeSources.cpp" ;;
    line)     echo "test_scope_freq_line.cpp @NOFLOAT@ ScopeFreqLine.cpp" ;;
    fmt)      echo "test_scope_fmt.cpp" ;;
    fft)      echo "test_scope_fft.cpp ScopeFft.cpp" ;;
    console)  echo "test_scope_freq_console.cpp ScopeFreqConsole.cpp" ;;
    settings) echo "test_scope_sources.cpp ScopeSources.cpp ScopeSettings.cpp ScopeRange.cpp" ;;
    est)      echo "test_scope_freq.cpp" ;;
  esac
}
build_run() {  # suite -> prints output, returns the test's status (99 = did not compile)
  local spec; spec=$(suite_cmd "$1")
  local t=${spec%% *}; local rest=${spec#* }; local args=() f
  [ "$rest" = "$spec" ] && rest=""
  for f in $rest; do case "$f" in @NOFLOAT@) args+=(-include "$W/nofloat_printf.h");; -*) args+=("$f");; *) args+=("$W/src/$f");; esac; done
  $CXX -std=c++17 -Wall -Wsign-compare -Werror -O2 -o "$W/bin" "$W/test/$t" ${args[@]+"${args[@]}"} 2>"$W/cc.log" || return 99
  "$W/bin" 2>&1
}

# suite @@ file @@ name @@ exact text @@ replacement
MUTS=(
 "service@@ScopeFreqService.cpp@@Select is not idempotent@@  if (src + 1 == (int)g_src1) return;@@"
 "service@@ScopeFreqService.cpp@@clock sign flipped@@c.clock_ratio = 1.0 + (double)clock_ppm10 * 1e-7;@@c.clock_ratio = 1.0 - (double)clock_ppm10 * 1e-7;"
 "service@@ScopeFreqService.cpp@@hand-off reader accepts a torn copy@@    if (s1 == g_pub_seq) return true;@@    return true;"
 "service@@ScopeFreqService.cpp@@CV word index not remapped@@detail::g_cv_index1 = (uint8_t)((cv < 4 ? cv + 4 : cv - 4) + 1);@@detail::g_cv_index1 = (uint8_t)(cv + 1);"
 "service@@ScopeFreqService.cpp@@clock range one too wide@@if (calibrated && (ppm10 > kClockMaxPpm10 || ppm10 < -kClockMaxPpm10)) return false;@@if (calibrated && (ppm10 > kClockMaxPpm10 + 1 || ppm10 < -kClockMaxPpm10)) return false;"
 "service@@ScopeFreqService.cpp@@idle CV input counts as moving@@const bool moving = r.pp > kMovingLsb;@@const bool moving = true;"
 "service@@ScopeFreqService.cpp@@Select keeps the old reading@@  if (!keep_meter) g_meter.Reset();@@"
 "service@@ScopeFreqService.cpp@@CV_LOW never returns for an audio-rate tone@@r.hz > 30.0@@r.hz > 3000000.0"
 "service@@ScopeFreqService.cpp@@fast gate is the slow one@@c.gate_samples = Samples(slow ? 1.0 : 0.1, kCvFsHz);@@c.gate_samples = Samples(1.0, kCvFsHz);"
 "service@@ScopeFreqService.cpp@@Stop leaves the audio tap armed@@  detail::g_audio_tap1 = 0;\n  detail::g_cv_index1 = 0;\n  g_src1 = 0;@@  detail::g_cv_index1 = 0;\n  g_src1 = 0;"
 "service@@ScopeFreqService.h@@audio feed ignores the tap@@  if (tap + 1 != (int)detail::g_audio_tap1) return;@@  if (detail::g_audio_tap1 == 0) return;"
 "line@@ScopeFreqLine.cpp@@acceptance at 2 sigma@@kAgreeSigmas * comb@@2.0 * comb"
 "line@@ScopeFreqLine.h@@acceptance floor removed@@constexpr double kAgreeFloorCents = 0.01;@@constexpr double kAgreeFloorCents = 0.0;"
 "line@@ScopeFreqLine.cpp@@gate confirms itself@@n.confirmed = prev_ok_ && Agree(prev_, r);@@n.confirmed = prev_ok_;"
 "line@@ScopeFreqLine.cpp@@decimals ignore sigma@@int hz_dec = DecimalsForSigma(r.hz * r.sigma_cents / kCentsPerLn, 3);@@int hz_dec = DecimalsForSigma(r.hz * r.sigma_cents / kCentsPerLn, 3) * 0 + 3;"
 "line@@ScopeFreqLine.cpp@@line not cut back to 21 columns@@    if (strlen(out) <= (size_t)kLineChars) break;@@    break;"
 "line@@ScopeFreqLine.cpp@@uncalibrated mark is the tilde (not in the font)@@mark[0] = '*';@@mark[0] = '~';"
 "line@@ScopeFreqLine.cpp@@unstable not marked@@if (r.status == Status::UNSTABLE) mark[0] = '!';@@if (false) mark[0] = '!';"
 "console@@ScopeFreqConsole.cpp@@range limit twice too wide@@if (value_ > kMaxPpm10) return Make(Kind::RANGE);@@if (value_ > 2 * kMaxPpm10) return Make(Kind::RANGE);"
 "console@@ScopeFreqConsole.cpp@@a bad byte is not swallowed@@  state_ = DRAIN;\n  return Make(Kind::CANCEL);@@  state_ = IDLE;\n  return Action();"
 "console@@ScopeFreqConsole.cpp@@no timeout@@if (state_ != IDLE && (uint32_t)(now_ms - last_ms_) > kTimeoutMs) state_ = IDLE;@@"
 "console@@ScopeFreqConsole.cpp@@three digits instead of four@@      if (n_ == 4) {@@      if (n_ == 3) {"
 "settings@@ScopeSettings.cpp@@layout version not checked@@  if (n == kScopeSettingsBytes && in[16] != kScopeSettingsVersion) return false;@@"
 "settings@@ScopeSettings.cpp@@clock range not checked@@if (v >= -kScopeClockMaxPpm10 && v <= kScopeClockMaxPpm10) {@@if (true) {"
 "settings@@ScopeSettings.h@@padding byte not recognised@@if (stored == k || stored == k + 1) return k;@@if (stored == k) return k;"
 "settings@@ScopeSettings.cpp@@16-byte layout refused@@  if (n != kScopeSettingsBytes && n != kScopeSettingsBytesV1) return false;@@  if (n != kScopeSettingsBytes) return false;"
 "settings@@ScopeSettings.cpp@@uncalibrated value saved@@  out[18] = (uint8_t)((uint16_t)(cal ? s.clock_ppm10 : 0));@@  out[18] = (uint8_t)((uint16_t)(s.clock_ppm10));"
 "settings@@ScopeSettings.cpp@@clock bytes moved into the old layout@@  out[15] = s.screensaver_trace;@@  out[15] = s.clock_cal;"
 "service@@ScopeFreqService.h@@negative CV word not clamped@@  if (v < 0) v = 0;@@"
 "service@@ScopeFreqService.h@@CV rate back to nominal 37.5 kHz@@constexpr int kCvTimerTicksPerCompare = kCvTimerComp1 + 1;   // UNVERIFIED (see above)@@constexpr int kCvTimerTicksPerCompare = kCvTimerComp1;"
 "service@@ScopeFreqService.cpp@@gate rounded down, a 2 s gate 5 ppm short@@  return (double)k < n ? k + 1 : k;@@  return k;"
 "line@@ScopeFreqLine.cpp@@unverified rate not marked@@  else if (rate_unverified) mark[0] = '#';@@"
 "line@@ScopeFreqLine.cpp@@SFREQ drops the rate field@@clock_calibrated ? 1 : 0, rate_verified ? 1 : 0);\n}@@clock_calibrated ? 1 : 0, 1);\n}"
 "console@@ScopeFreqConsole.cpp@@a cancel leaks the rest of the burst@@    else { last_ms_ = now_ms; return Make(Kind::PENDING); }@@    else { state_ = IDLE; return Action(); }"
 "console@@ScopeFreqConsole.cpp@@unsigned clock value accepted@@      if (c == '+' || c == '-') {@@      if (c == '+' || c == '-' || (c >= '0' && c <= '9')) {"
 "fmt@@ScopeFmt.h@@Fixed rounds down@@const uint64_t q = (uint64_t)(a * (double)sc + 0.5);@@const uint64_t q = (uint64_t)(a * (double)sc);"
 "fmt@@ScopeFmt.h@@Fixed prints -0.00@@      if (q == 0) neg = false;@@"
 "fmt@@ScopeFmt.h@@Fixed drops the zero padding of the fraction@@          t[k++] = (char)('0' + (int)((fp / p) % 10));@@          if (fp / p || i == 0) t[k++] = (char)('0' + (int)((fp / p) % 10));"
 "fmt@@ScopeFmt.h@@peak text switches to kHz at 1000 Hz, not 999.5@@  if (hz < 999.5) {@@  if (hz < 1000.0) {"
 "fft@@ScopeFft.cpp@@spectrum keeps the DC@@    re_[r] = (in[i] - mean) * win_[i];@@    re_[r] = (in[i] - mean * 0.f) * win_[i];"
 "line@@ScopeFreqLine.cpp@@line formats with %f again (blank on the module)@@    k += ScopeFmt::Fixed(out + k, sizeof(out) - (size_t)k, r.hz, hz_dec);@@    k += snprintf(out + k, sizeof(out) - (size_t)k, \"%.*f\", hz_dec, r.hz);"
 "line@@ScopeFreqLine.cpp@@SFREQ formats hz with %f again@@  ScopeFmt::Fixed(hz, sizeof(hz), num ? r->hz : 0.0, 5);@@  snprintf(hz, sizeof(hz), \"%.5f\", num ? r->hz : 0.0);"
 "est@@ScopeFreq.h@@Log2 series cut short@@for (int k = 1; k <= 21; k += 2) {@@for (int k = 1; k <= 3; k += 2) {"
 "est@@ScopeFreq.h@@Result.pp not recorded@@    r.pp = hi_ >= lo_ ? (int32_t)hi_ - (int32_t)lo_ : 0;\n    r.crossings = line_.n;@@    r.crossings = line_.n;"
 "est@@ScopeFreq.h@@note names shifted by a semitone@@kNames[3 * n];@@kNames[3 * ((n + 1) % 12)];"
)

for s in service line console settings est fmt fft; do
  cp "$W/orig"/* "$W/src/"
  out=$(build_run $s); rc=$?
  [ $rc -eq 0 ] || { echo "baseline $s is RED (rc=$rc): fix the tests first"; echo "$out" | tail -5; exit 2; }
done
echo "baselines: green (service, line, console, settings, estimator)"

survivors=0
for m in "${MUTS[@]}"; do
  suite=${m%%@@*}; rest=${m#*@@}
  file=${rest%%@@*}; rest=${rest#*@@}
  name=${rest%%@@*}; rest=${rest#*@@}
  from=${rest%%@@*}; to=${rest#*@@}
  cp "$W/orig"/* "$W/src/"
  MFROM=$from MTO=$to python3 - "$W/orig/$file" "$W/src/$file" <<'PY' || { echo "MUTATION DID NOT APPLY: $name"; survivors=$((survivors+1)); continue; }
import os, sys
s = open(sys.argv[1]).read()
f, t = os.environ["MFROM"], os.environ["MTO"]
f, t = f.replace("\\n", "\n"), t.replace("\\n", "\n")
if s.count(f) != 1:
    sys.exit(1)
open(sys.argv[2], "w").write(s.replace(f, t))
PY
  if cmp -s "$W/orig/$file" "$W/src/$file"; then echo "MUTATION IS A NO-OP: $name"; survivors=$((survivors+1)); continue; fi
  out=$(build_run "$suite"); rc=$?
  if [ $rc -eq 99 ]; then echo "does not compile: $name"; survivors=$((survivors+1)); continue; fi
  fails=$(grep -c "FAIL" <<<"$out")
  if [ $rc -ne 0 ]; then printf 'RED   %-9s %-52s %3d failed checks\n' "$suite" "$name" "$fails"
  else printf 'GREEN %-9s %-52s <-- SURVIVED\n' "$suite" "$name"; survivors=$((survivors+1)); fi
done
echo "survivors: $survivors"
[ $survivors -eq 0 ]
