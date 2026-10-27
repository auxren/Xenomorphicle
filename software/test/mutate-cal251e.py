#!/usr/bin/env python3
"""Mutation check for the Cal Routine's 251e target (docs/specs/cal-251e.md).

Each mutation edits a COPY of the sources, rebuilds the 251e suites there and expects at least
one of them to go red. A mutation that leaves every suite green is reported SURVIVED and the
script exits non-zero; a mutation that does not compile is BUILD (a broken mutation, not a proof).
The unmutated copy must be green first.

    python3 -I mutate-cal251e.py            # all mutations, 4 at a time
    python3 -I mutate-cal251e.py M07 M12    # some

The mutations are deliberate faults of the kinds the design guards against: a wrong physical
scale, a missing physical check, a write that gets through, an override left on the bus, a
COMMIT that is resent, a verdict believed from a stale status bit.
"""
import concurrent.futures, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
CXX = os.environ.get('CXX', 'g++')
WARN = ['-std=c++17', '-Wall', '-Wsign-compare', '-Werror', '-O1']

FLOW_SRCS = ['Cal251eFlow.cpp', 'Cal251eBus.cpp', 'CalRec.cpp', 'CalRec251e.cpp', 'CalRoutineFlow.cpp',
             'CalRoutineMeasure.cpp', 'CalRoutineMath.cpp', 'CvMeterMath.cpp', 'UsbTmc.cpp']
SUITES = [
    ('calrec251e', 'test_calrec251e.cpp', ['CalRec251e.cpp', 'CalRec.cpp'], []),
    ('cal251e_bus', 'test_cal251e_bus.cpp', ['Cal251eBus.cpp', 'CalRec.cpp', 'CalRec251e.cpp'], []),
    ('cal251e_flow', 'test_cal251e_flow.cpp', FLOW_SRCS, []),
    ('cal251e_flow+write', 'test_cal251e_flow.cpp', FLOW_SRCS, ['-DCAL251E_ALLOW_WRITE=1']),
    ('cal251e_text', 'test_cal251e_text.cpp', ['Cal251eText.cpp'] + FLOW_SRCS, []),
]

# (id, what it breaks, file in src/ (or test/), old text, new text)
MUTATIONS = [
 ('M01', 'fit target scale 4096 codes per 10 V (the 259e mistake) in the one constant',
  'CalRec251e.h', 'constexpr double kMvPerCode = kStockTopMv / double(kStockCodeAtTop);',
  'constexpr double kMvPerCode = kStockTopMv / 4096.0;'),
 ('M02', 'absolute-anchor check dropped from the flow',
  'Cal251eFlow.cpp', '      if (first == F_NONE) first = F_RECORD;', ''),
 ('M03', 'hardware scale window opened to 9000..11000 mV',
  'CalRec251e.h', 'constexpr double kScaleLoMv = 9600.0, kScaleHiMv = 10400.0;',
  'constexpr double kScaleLoMv = 9000.0, kScaleHiMv = 11000.0;'),
 ('M04', 'the write compiled in by default',
  'Cal251eFlow.h', '  static constexpr bool kAllowWrite = false;', '  static constexpr bool kAllowWrite = true;'),
 ('M05', 'verify points held at the uncorrected code instead of the corrected one',
  'Cal251eFlow.cpp', 'codes[k] = R::Apply(sc, result_.ch[k].gain, result_.ch[k].offset);', 'codes[k] = sc;'),
 ('M06', 'loopback verdict dropped (a bad measurement chain is trusted)',
  'Cal251eFlow.cpp', '  if (loop_bad_ > kLoopbackMv) { Fail(F_LOOPBACK); return; }', ''),
 ('M07', 'identity check always passes (crossed or missing cable accepted)',
  'Cal251eFlow.cpp', '  if (!ok) {\n    id_bad_ch_ = ch;', '  if (!ok && false) {\n    id_bad_ch_ = ch;'),
 ('M08', 'RELEASE never scheduled (overrides left to the 5 s timer)',
  'Cal251eFlow.cpp', '  if (ovr_live_) { rel_pending_ = true; rel_started_ = false; }', ''),
 ('M09', 'overrides refreshed once per point, not before every window',
  'Cal251eFlow.cpp', '        if (acq_bus_) { acq_i_ = 0; acq_ = A_SEND; }  // refresh the overrides before every window\n        else win_.Start(*in_, *clock_);',
  '        win_.Start(*in_, *clock_);'),
 ('M10', 'COMMIT wire error treated as "nothing went out" (would be resent upstream)',
  'Cal251eBus.cpp', '    if (rc == E_BUS_ERROR && last_send_rc_ != -1) {', '    if (false) {'),
 ('M11', 'COMMIT verdict believed without the record sequence number',
  'Cal251eBus.cpp', '(fl & kStatusValid) && put_seq_valid_ && status_.seq == put_seq_)', '(fl & kStatusValid))'),
 ('M12', 'overlapping record bytes not compared',
  'Cal251eBus.cpp', '      if ((seen_ >> off & 1u) && record_[off] != last_data_[i]) {', '      if (false) {'),
 ('M13', 'drift repeat dropped',
  'Cal251eFlow.cpp', '      if (fabs(mv_[k] - fit_[k][0].mv) > kDriftMv) { Fail(F_DRIFT); return; }', ''),
 ('M14', 'results never expire (write build)',
  'Cal251eFlow.cpp', '  if (now_ - results_ms_ >= kResultsMaxAgeMs) { Fail(F_EXPIRED); return; }', ''),
 ('M15', 'loopback drives outputs A-D instead of E-H',
  'Cal251eFlow.h', 'static constexpr int kFirstLoopOut = 4;', 'static constexpr int kFirstLoopOut = 0;'),
 ('M16', 'offset sign flipped in the fit',
  'CalRec251e.cpp', 'const double o_real = -line.b / line.a + 0.5;', 'const double o_real = line.b / line.a + 0.5;'),
 ('M17', 'integer refinement of the fit removed',
  'CalRec251e.cpp', '      const double g = g0 + dg, o = o0 + dof;', '      const double g = g0, o = o0;'),
 ('M18', 'status bit1 (override active) not required',
  'Cal251eFlow.cpp', '    if (!(bus_.status().flags & Cal251eBus::kStatusOverride)) { Fail(F_NO_OVERRIDE); return; }', ''),
 ('M19', 'PUT chunk offsets wrong (last frame at 30)',
  'Cal251eBus.cpp', 'static const uint8_t kPutOff[5] = {0, 8, 16, 24, 32};', 'static const uint8_t kPutOff[5] = {0, 8, 16, 24, 30};'),
 ('M20', 'a nonlinear DAC accepted (non-linearity limit removed)',
  'Cal251eFlow.cpp', '      if (first == F_NONE) first = F_NONLIN;', ''),
 ('M21', 'input near its top accepted',
  'Cal251eFlow.cpp', '        if (mv > top - kTopGuardMv) { Fail(F_INPUT_RANGE); return false; }', '        (void)top;'),
 ('M23', 'the zero-floor limit |b| <= 100 mV dropped',
  'CalRec251e.cpp', 'return v >= kScaleLoMv && v <= kScaleHiMv && fabs(an.b) <= kZeroMaxMv;',
  'return v >= kScaleLoMv && v <= kScaleHiMv;'),
 ('M24', 'the flow\'s fit target constant set to 4096 codes per 10 V (the flow only)',
  'Cal251eFlow.h', 'double fit_k_ = CalRec251e::kMvPerCode;', 'double fit_k_ = 10000.0 / 4096.0;'),
 ('M25', 'Failed screen says "Nothing was written." after an unknown COMMIT outcome',
  'Cal251eText.cpp', '      snprintf(r3, sizeof(r3), "Written: UNKNOWN.");', '      snprintf(r3, sizeof(r3), "Nothing was written.");'),
 ('M26', 'Fail() no longer lets go of the outputs (every failure path)',
  'Cal251eFlow.cpp', 'C251_CODE void Cal251eFlow::Fail(FailCode f) {\n  ReleaseHolds();', 'C251_CODE void Cal251eFlow::Fail(FailCode f) {'),
 ('M27', 'per-power-up COMMIT cap not enforced (write build)',
  'Cal251eFlow.cpp', '  if (commits_power_up_ >= kWriteCap) { Fail(F_WRITE_CAP); return; }\n', ''),
 ('M28', 'a refused bit believed without a clean baseline (stale bit on a lost COMMIT)',
  'Cal251eBus.cpp', 'if ((fl & kStatusRefused) && !base_refused_) res', 'if ((fl & kStatusRefused)) res'),
 ('M29', 'warn-first policy lost: non-linearity over the write limit fails a measuring-only run',
  'Cal251eFlow.cpp', '(kAllowWrite && nl > kNonlinMv)) {\n      if (first', '(nl > kNonlinMv)) {\n      if (first'),
 ('M30', 'fit refinement window narrowed to the old +-2 / +-3',
  'CalRec251e.cpp', 'constexpr int kGainWindow = 4, kOffsetWindow = 6;', 'constexpr int kGainWindow = 2, kOffsetWindow = 3;'),
 ('M31', 'COMMIT counted but never logged per COMMIT (write build)',
  'Cal251eFlow.cpp', '    C251_LOG(Commit, commits_power_up_, unsigned(sent.seq));\n', ''),
 ('M32', 'Worst() skips the top stage code 3831',
  'CalRec251e.cpp', '  for (int c = 0; c <= kStockCodeAtTop; c++) {\n    const double ideal', '  for (int c = 0; c < kStockCodeAtTop; c++) {\n    const double ideal'),
 ('M33', 'the reported noise is doubled',
  'Cal251eFlow.cpp', 'noise_ss_[k] += 0.5 * d * d;', 'noise_ss_[k] += 2.0 * d * d;'),
 ('M34', 'gross non-linearity limit raised from 12 to 30 mV',
  'Cal251eFlow.h', 'kNonlinGrossMv = 12.0;', 'kNonlinGrossMv = 30.0;'),
 ('M35', 'a failed status read before the COMMIT is ignored (write build)',
  'Cal251eFlow.cpp', '  if (sub_ == S_PRESTAT) {\n    if (rc < 0) { Fail(F_BUS); return; }', '  if (sub_ == S_PRESTAT) {'),
 ('M36', 'the END line no longer carries the warning word',
  'Cal251eFlow.cpp', 'snprintf(t, sizeof(t), ",warn=%X", warn);', 'snprintf(t, sizeof(t), ",warn=%X", warn & 0u);'),
 ('M37', 'the fit stops at the first failing channel',
  'Cal251eFlow.cpp', 'if (!R::ScaleOk(an)) { if (first == F_NONE) first = F_SCALE; continue; }', 'if (!R::ScaleOk(an)) { Fail(F_SCALE); return; }'),
 ('M38', 'loopback limit back at the old 2.0 mV',
  'Cal251eFlow.h', 'kLoopbackMv = 2.5;', 'kLoopbackMv = 2.0;'),
 ('M39', 'loopback comparison made inclusive (a reading exactly at the limit fails)',
  'Cal251eFlow.cpp', '  if (loop_bad_ > kLoopbackMv) { Fail(F_LOOPBACK); return; }', '  if (loop_bad_ >= kLoopbackMv) { Fail(F_LOOPBACK); return; }'),
 ('M22', 'the write ignores its own anchor re-check (write build)',
  'Cal251eFlow.cpp', '    if (!R::AnchorsOk(an_[k], result_.ch[k], nullptr)) { Fail(F_RECORD); return; }', ''),
]


def build_and_run(tmp, name, test, srcs, flags):
    exe = os.path.join(tmp, 'build', name.replace('+', '_'))
    cmd = [CXX] + WARN + flags + ['-o', exe, test] + [os.path.join('..', 'src', s) for s in srcs]
    b = subprocess.run(cmd, cwd=os.path.join(tmp, 'test'), capture_output=True, text=True)
    if b.returncode != 0:
        return 'BUILD', b.stderr.strip().splitlines()[:3]
    r = subprocess.run([exe], cwd=os.path.join(tmp, 'test'), capture_output=True, text=True, timeout=900)
    if r.returncode != 0:
        fl = [l.strip() for l in r.stdout.splitlines() if 'FAIL' in l]
        return 'RED', fl[:2] or ['exit %d' % r.returncode]
    return 'GREEN', []


def run_one(mid, desc, fname, old, new):
    tmp = tempfile.mkdtemp(prefix='mut251_')
    try:
        os.makedirs(os.path.join(tmp, 'src'))
        os.makedirs(os.path.join(tmp, 'test', 'fixtures'))
        os.makedirs(os.path.join(tmp, 'build'))
        for f in os.listdir(SRC):
            if f.endswith(('.h', '.cpp')):
                shutil.copy(os.path.join(SRC, f), os.path.join(tmp, 'src', f))
        for f in os.listdir(HERE):
            if f.endswith(('.h', '.cpp')):
                shutil.copy(os.path.join(HERE, f), os.path.join(tmp, 'test', f))
        for f in os.listdir(os.path.join(HERE, 'fixtures')):
            shutil.copy(os.path.join(HERE, 'fixtures', f), os.path.join(tmp, 'test', 'fixtures', f))
        if mid != 'M00':
            path = os.path.join(tmp, 'src', fname)
            text = open(path).read()
            if text.count(old) != 1:
                return mid, desc, 'BADMUT', ['old text found %d times' % text.count(old)]
            open(path, 'w').write(text.replace(old, new))
        reds, builds = [], []
        for name, test, srcs, flags in SUITES:
            st, det = build_and_run(tmp, name, test, srcs, flags)
            if st == 'RED':
                reds.append((name, det[0] if det else ''))
            elif st == 'BUILD':
                builds.append((name, det))
        if builds:
            return mid, desc, 'BUILD', [str(b) for b in builds]
        if reds:
            return mid, desc, 'RED', ['%s: %s' % r for r in reds]
        return mid, desc, 'GREEN', []
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    want = set(sys.argv[1:])
    jobs = [('M00', 'unmutated control (must be GREEN)', 'CalRec251e.h', '', '')] + MUTATIONS
    jobs = [j for j in jobs if not want or j[0] in want]
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        for mid, desc, st, det in ex.map(lambda j: run_one(*j), jobs):
            ok = (st == 'GREEN') if mid == 'M00' else (st == 'RED')
            bad += 0 if ok else 1
            tag = {'RED': 'red', 'GREEN': 'GREEN' if mid == 'M00' else 'SURVIVED', 'BUILD': 'BUILD ERROR',
                   'BADMUT': 'BAD MUTATION'}[st]
            print('%-4s %-11s %s' % (mid, tag, desc))
            for d in det:
                print('       ' + d)
    print('%d of %d as expected' % (len(jobs) - bad, len(jobs)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
