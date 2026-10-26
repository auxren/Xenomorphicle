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
cp test_scope_freq.cpp test_scope_fmt.cpp test_scope_fft.cpp test_scope_decim.cpp test_scope_freq_service.cpp test_scope_freq_line.cpp \
   test_scope_freq_console.cpp test_scope_sources.cpp "$W/test/"
mkdir -p "$W/orig" && cp "$W"/src/* "$W/orig/"

# suite name -> test file + sources (under src/) + flags
suite_cmd() {
  case "$1" in
    service)  echo "test_scope_freq_service.cpp -pthread ScopeFreqService.cpp ScopeFreqLine.cpp ScopeSources.cpp" ;;
    line)     echo "test_scope_freq_line.cpp @NOFLOAT@ ScopeFreqLine.cpp" ;;
    fmt)      echo "test_scope_fmt.cpp" ;;
    fft)      echo "test_scope_fft.cpp ScopeFft.cpp" ;;
    decim)    echo "test_scope_decim.cpp ScopeFft.cpp" ;;
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
 "service@@ScopeFreqService.h@@CV rate back to nominal 37.5 kHz@@constexpr int kCvTimerTicksPerCompare = kCvTimerComp1 + 1;   // COMP1 + 1: verified on the hardware (see above)@@constexpr int kCvTimerTicksPerCompare = kCvTimerComp1;"
 "service@@ScopeFreqService.cpp@@gate rounded down, a 2 s gate 5 ppm short@@  return (double)k < n ? k + 1 : k;@@  return k;"
 "line@@ScopeFreqLine.cpp@@unverified rate not marked@@  else if (rate_unverified) mark[0] = '#';@@"
 "line@@ScopeFreqLine.cpp@@SFREQ drops the rate field@@clock_calibrated ? 1 : 0, rate_verified ? 1 : 0);\n}@@clock_calibrated ? 1 : 0, 1);\n}"
 "console@@ScopeFreqConsole.cpp@@a cancel leaks the rest of the burst@@    else { last_ms_ = now_ms; return Make(Kind::PENDING); }@@    else { state_ = IDLE; return Action(); }"
 "console@@ScopeFreqConsole.cpp@@unsigned clock value accepted@@      if (c == '+' || c == '-') {@@      if (c == '+' || c == '-' || (c >= '0' && c <= '9')) {"
 "fmt@@ScopeFmt.h@@Fixed rounds down@@const uint64_t q = (uint64_t)(a * (double)sc + 0.5);@@const uint64_t q = (uint64_t)(a * (double)sc);"
 "fmt@@ScopeFmt.h@@Fixed prints -0.00@@      if (q == 0) neg = false;@@"
 "fmt@@ScopeFmt.h@@Fixed drops the zero padding of the fraction@@          t[k++] = (char)('0' + (int)((fp / p) % 10));@@          if (fp / p || i == 0) t[k++] = (char)('0' + (int)((fp / p) % 10));"
 "fmt@@ScopeFmt.h@@peak text switches to kHz at 1000 Hz, not 999.5@@  } else if (hz < 999.5) {@@  } else if (hz < 1000.0) {"
 "fft@@ScopeFft.cpp@@spectrum keeps the DC@@    re_[r] = (in[i] - mean) * win_[i];@@    re_[r] = (in[i] - mean * 0.f) * win_[i];"
 "line@@ScopeFreqLine.cpp@@line formats with %f again (blank on the module)@@    k += ScopeFmt::Fixed(out + k, sizeof(out) - (size_t)k, r.hz, hz_dec);@@    k += snprintf(out + k, sizeof(out) - (size_t)k, \"%.*f\", hz_dec, r.hz);"
 "line@@ScopeFreqLine.cpp@@SFREQ formats hz with %f again@@  ScopeFmt::Fixed(hz, sizeof(hz), num ? r->hz : 0.0, 5);@@  snprintf(hz, sizeof(hz), \"%.5f\", num ? r->hz : 0.0);"
 "decim@@ScopeDecim.h@@long filter centre tap off by one (DC gain)@@    int32_t acc = 16384 * Tap(lbuf_@@    int32_t acc = 16385 * Tap(lbuf_"
 "decim@@ScopeDecim.h@@last stage uses the short filter (aliases pass)@@      v = last ? Long() : Short(s);@@      v = Short(s);"
 "decim@@ScopeDecim.h@@output on every input (no decimation)@@      phase_[s] ^= 1;\n      if (phase_[s]) return false;@@      phase_[s] = 0;"
 "decim@@ScopeDecim.h@@ring read starts at the wrong sample@@      const uint32_t start = (c0 - (uint32_t)kRingLen) % kRingCap;@@      const uint32_t start = (c0 - (uint32_t)kRingLen + 1u) % kRingCap;"
 "decim@@ScopeDecim.h@@a short-filter tap sign flipped@@    SCOPEDECIM_PAIR(b, p, 31, 9, -2403, 3);@@    SCOPEDECIM_PAIR(b, p, 31, 9, 2403, 3);"
 "fft@@ScopeFft.cpp@@low end back to the nearest bin@@      m = DbAt(db, HzAt((float)c, rate_hz) / df);   // no bin in this column: interpolate@@      { int kk = (int)lroundf(HzAt((float)c, rate_hz) / df); if (kk < 1) kk = 1; m = db[kk]; }"
 "fft@@ScopeFft.cpp@@linear axis scaled by 128 not 127@@      m = DbAt(db, span_hz * c / 127.f / df);@@      m = DbAt(db, span_hz * c / 128.f / df);"
 "fft@@ScopeFft.cpp@@window never regenerated@@  if (w.win_kind == (uint8_t)kind) return;@@  if (w.win_kind != 255) return;"
 "fft@@ScopeFft.cpp@@peak search ignores the upper limit@@  if (k1 > kN / 2 - 1) k1 = kN / 2 - 1;@@  k1 = kN / 2 - 1;"
 "fft@@ScopeFft.cpp@@averaging: power spectrum is magnitude not power@@    pow_out[k] = m * m;@@    pow_out[k] = m;"
 "fft@@ScopeFft.cpp@@window gain from a wrong sum@@    sum += v;@@    sum += 1.0;"
 "decim@@ScopeDecim.h@@a snapshot that overlapped a push is accepted@@      if (count_ == c0) return true;@@      return true;"
 "decim@@ScopeDecim.h@@no spare slot in the ring (a push in progress tears the window)@@constexpr int kRingCap = kRingLen + 1;@@constexpr int kRingCap = kRingLen;"
 "decim@@ScopeDecim.h@@overshoot wraps instead of clamping@@inline int16_t Clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }@@inline int16_t Clamp16(int32_t v) { return (int16_t)v; }"
 "decim@@ScopeDecim.h@@Reset keeps the stage phase@@    for (auto &p : phase_) p = 0;@@"
 "decim@@ScopeDecim.h@@Reset keeps the long filter's history@@    for (auto &v : lbuf_) v = 0;@@"
 "fft@@ScopeFft.h@@averaging advances on every redraw@@inline bool AvgDue(uint32_t count, uint32_t last) { return (uint32_t)(count - last) >= kAvgFreshSamples; }@@inline bool AvgDue(uint32_t count, uint32_t last) { (void)count; (void)last; return true; }"
 "fft@@ScopeFft.h@@shown span reaches the decimated Nyquist (the transition band is drawn)@@decimated_rate_hz * 4.0 / 10.0@@decimated_rate_hz * 5.0 / 10.0"
 "fft@@ScopeFft.h@@zoom peak search floor five bins@@inline float PeakFloorHz(float bin_hz) { return 3.f * bin_hz < 20.f ? 3.f * bin_hz : 20.f; }@@inline float PeakFloorHz(float bin_hz) { return 5.f * bin_hz; }"
 "fmt@@ScopeFmt.h@@span label always in kHz@@  if (hz < 999.5) {\n    Fixed(num, sizeof(num), hz, 0);\n    for (const char *c = num; *c; ++c) t[k++] = *c;\n  } else {\n    Fixed(num, sizeof(num), hz / 1000.0, 1);\n    for (const char *c = num; *c; ++c) t[k++] = *c;\n    t[k++] = 'k';\n  }\n  t[k] = 0;\n  if (out && n) {\n    size_t c = (size_t)k < n - 1 ? (size_t)k : n - 1;\n    for (size_t i = 0; i < c; ++i) out[i] = t[i];\n    out[c] = 0;\n  }\n  return k;\n}\n\n}  // namespace ScopeFmt@@  if (false) {\n  } else {\n    Fixed(num, sizeof(num), hz / 1000.0, 1);\n    for (const char *c = num; *c; ++c) t[k++] = *c;\n    t[k++] = 'k';\n  }\n  t[k] = 0;\n  if (out && n) {\n    size_t c = (size_t)k < n - 1 ? (size_t)k : n - 1;\n    for (size_t i = 0; i < c; ++i) out[i] = t[i];\n    out[c] = 0;\n  }\n  return k;\n}\n\n}  // namespace ScopeFmt"
 "settings@@ScopeSources.cpp@@CV rate back to the nominal 37.5 kHz@@    case KIND_CV_IN: return (float)ScopeFreqSvc::kCvFsHz;@@    case KIND_CV_IN: return 37500.f;"
 "decim@@ScopeDecim.h@@short filter centre tap off by one@@    int32_t acc = 16386 * Tap(b, p, 31, 9);@@    int32_t acc = 16387 * Tap(b, p, 31, 9);"
 "decim@@ScopeDecim.h@@first stage back to the 11-tap coefficients (folds 19.5 kHz at -44 dB)@@    SCOPEDECIM_PAIR(b, p, 31, 9, 10022, 1);\n    SCOPEDECIM_PAIR(b, p, 31, 9, -2403, 3);\n    SCOPEDECIM_PAIR(b, p, 31, 9, 706, 5);\n    SCOPEDECIM_PAIR(b, p, 31, 9, -141, 7);\n    SCOPEDECIM_PAIR(b, p, 31, 9, 7, 9);@@    SCOPEDECIM_PAIR(b, p, 31, 9, 9600, 1);\n    SCOPEDECIM_PAIR(b, p, 31, 9, -1600, 3);\n    SCOPEDECIM_PAIR(b, p, 31, 9, 192, 5);"
 "decim@@ScopeDecim.h@@only one snapshot attempt@@constexpr int kReadAttempts = 4;@@constexpr int kReadAttempts = 1;"
 "decim@@ScopeDecim.h@@a snapshot with the count moved by one is accepted@@      if (count_ == c0) return true;@@      if (count_ - c0 <= 1u) return true;"
 "fft@@ScopeFft.h@@a leakage peak is read as a frequency (no level gate)@@constexpr float kPeakMinDb = -70.f;@@constexpr float kPeakMinDb = -200.f;"
 "decim@@ScopeDecim.h@@last stage filter cut to eight tap pairs (the ghost peak at 1.5x the span)@@    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 124, 17);\n    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -70, 19);\n    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 37, 21);\n    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -17, 23);\n    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 7, 25);\n    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -2, 27);\n@@"
 "decim@@ScopeDecim.h@@short filter overshoot wraps (no clamp in Short)@@    SCOPEDECIM_PAIR(b, p, 31, 9, 7, 9);\n    return Clamp16((acc + 16384) >> 15);@@    SCOPEDECIM_PAIR(b, p, 31, 9, 7, 9);\n    return (int16_t)((acc + 16384) >> 15);"
 "decim@@ScopeDecim.h@@last stage filter taps shifted (a tap sign flipped)@@    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, -3322, 3);@@    SCOPEDECIM_PAIR(lbuf_, p, 63, 27, 3322, 3);"
 "est@@ScopeFreq.h@@Log2 series cut short@@for (int k = 1; k <= 21; k += 2) {@@for (int k = 1; k <= 3; k += 2) {"
 "est@@ScopeFreq.h@@Result.pp not recorded@@    r.pp = hi_ >= lo_ ? (int32_t)hi_ - (int32_t)lo_ : 0;\n    r.crossings = line_.n;@@    r.crossings = line_.n;"
 "est@@ScopeFreq.h@@note names shifted by a semitone@@kNames[3 * n];@@kNames[3 * ((n + 1) % 12)];"
)

for s in service line console settings est fmt fft decim; do
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
