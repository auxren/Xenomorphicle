#!/usr/bin/env bash
# Proves test_scope_freq can fail: applies one deliberate bug at a time to a
# private copy of src/ScopeFreq.h, runs the suite against it, and requires red.
# A mutation that leaves the suite green is a hole in the tests; this script
# exits non-zero if there is one. Nothing in the tree is modified.
#   ./mutate-scope-freq.sh
set -u
cd "$(dirname "$0")"
CXX=${CXX:-g++}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/src" "$W/test"
cp ../src/ScopeFreq.h "$W/src/ScopeFreq.h.orig"
cp test_scope_freq.cpp "$W/test/"

# name @@ exact text in ScopeFreq.h @@ replacement
MUTS=(
 "nearest-sample timestamps@@(double)(ls_ - a2) / (double)(b2 - a2)@@0.0"
 "midpoint timestamps@@(double)(ls_ - a2) / (double)(b2 - a2)@@0.5"
 "gate quartered@@if (t >= (double)cfg_.gate_samples) return Finish();@@if (t >= (double)cfg_.gate_samples * 0.25) return Finish();"
 "gate counts n crossings, not n-1 periods@@return n >= 2 ? ckt / Ckk() : 0.0;@@return n >= 2 ? ckt / Ckk() * (double)n / (double)(n - 1) : 0.0;"
 "hysteresis collapsed@@const int32_t hi2 = (int32_t)ceil(h);@@const int32_t hi2 = 0;"
 "Welford mean wrong@@mean_t += dt / (double)n;@@mean_t += dt / (double)(n + 1);"
 "clock ratio ignored@@r.hz = cfg_.fs * cfg_.clock_ratio / P;@@r.hz = cfg_.fs / P;"
 "LOST watchdog removed@@} else if (idle_ > cfg_.max_period) {@@} else if (idle_ > 0x7fffffffu) {"
 "clip flag removed@@if (x >= 32760 || x <= -32760) clip_ = 1;@@"
 "level pinned to 0@@ls_ = cfg_.auto_center ? (int32_t)hi_ + (int32_t)lo_ :@@ls_ = 0 ? 0 :"
 "interval-ratio check removed@@r.interval_ratio > cfg_.max_interval_ratio || @@"
 "jitter check removed@@ || r.jitter > cfg_.max_jitter@@"
 "32-bit wrap not extended@@if (started_ && c < last_) hi_ += 1ull << 32;@@"
 "level not re-learned per gate@@if (hi_ >= lo_ && (int)hi_ - (int)lo_ >= (int)(cfg_.min_pp_hyst * cfg_.hyst_min)) SetLevels();@@"
  "hysteresis re-arm at the level (one-sided)@@} else if (b2 < hlo_) {@@} else if (b2 < ls_) {"
 "cents sigma zeroed@@r.cents_sigma = kCentsPerOctave / 0.69314718055994531 * (line_.SlopeSigma() / P);@@r.cents_sigma = 0;"
 "edge min interval reports max@@r.min_interval = imin_;@@r.min_interval = imax_;"
)

# Sanity: the unmutated copy must be green, or every red below means nothing.
cp "$W/src/ScopeFreq.h.orig" "$W/src/ScopeFreq.h"
$CXX -std=c++17 -O2 -o "$W/base" "$W/test/test_scope_freq.cpp" || exit 2
"$W/base" >/dev/null 2>&1 || { echo "baseline is RED: fix the tests first"; exit 2; }
echo "baseline: green"

survivors=0
for m in "${MUTS[@]}"; do
  name=${m%%@@*}; rest=${m#*@@}; from=${rest%%@@*}; to=${rest#*@@}
  MFROM=$from MTO=$to python3 - "$W/src/ScopeFreq.h.orig" "$W/src/ScopeFreq.h" <<'PY' || { echo "MUTATION DID NOT APPLY: $name"; survivors=$((survivors+1)); continue; }
import os, sys
s = open(sys.argv[1]).read()
f, t = os.environ["MFROM"], os.environ["MTO"]
f, t = f.replace("\\n", "\n"), t.replace("\\n", "\n")
if s.count(f) != 1:
    sys.exit(1)
open(sys.argv[2], "w").write(s.replace(f, t))
PY
  if cmp -s "$W/src/ScopeFreq.h.orig" "$W/src/ScopeFreq.h"; then echo "MUTATION IS A NO-OP: $name"; survivors=$((survivors+1)); continue; fi
  if ! $CXX -std=c++17 -O2 -o "$W/mut" "$W/test/test_scope_freq.cpp" 2>"$W/cc.log"; then
    echo "does not compile: $name"; survivors=$((survivors+1)); continue
  fi
  out=$("$W/mut" 2>&1); rc=$?
  fails=$(grep -c "FAIL" <<<"$out")
  if [ $rc -ne 0 ]; then printf 'RED   %-42s %3d failed checks\n' "$name" "$fails"
  else printf 'GREEN %-42s <-- SURVIVED\n' "$name"; survivors=$((survivors+1)); fi
done
echo "survivors: $survivors"
[ $survivors -eq 0 ]
