#!/usr/bin/env python3
"""Second implementation of the 251e fit (src/CalRec251e.cpp), written from the spec
(docs/specs/cal-251e.md section 4) and independent of the C++ code. It produces the vectors in
test_calrec251e.cpp and the statistics quoted in the spec.

    python3 -I ref_cal251e_fit.py            # print the four vectors
    python3 -I ref_cal251e_fit.py --stats N  # N random stages: error and rejection statistics

Algorithm: least squares of mV on the raw code (doubles, summed in input order), then
gain = round(16384 K / a), offset = round(-b / a + 0.5) (round half to even), then the best pair in
gain +-4, offset +-6 around that by the smallest worst predicted error over every reachable stage
code 0..3831 (ties to the pair nearest the rounded one, distance |dg| * 8 + |do|).
"""
import math, random, sys

K = 10000.0 / 3831.0
GW, OW = 4, 6


def apply(code, g, o):
    return max(0, min(4095, ((code * g) >> 14) + o))


def line(pts):
    n = float(len(pts)); sx = sy = sxx = sxy = 0.0
    for x, y in pts:
        x = float(x); sx += x; sy += y; sxx += x * x; sxy += x * y
    a = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    return a, (sy - a * sx) / n


def worst(a, b, g, o, k, cutoff=1e300):
    w = 0.0
    for c in range(0, 3832):
        ideal = (c * k - b) / a
        if ideal < 0.0 or ideal > 4095.0:
            continue
        e = abs(a * apply(c, g, o) + b - c * k)
        if e > w:
            w = e
            if w > cutoff:
                return w
    return w


def fit(pts, k=K):
    a, b = line(pts)
    g0 = round(16384 * k / a); o0 = round(-b / a + 0.5)
    best = None
    for dg in range(-GW, GW + 1):
        for do in range(-OW, OW + 1):
            g = g0 + dg; o = o0 + do
            if g < 1 or g > 65535 or o < -32768 or o > 32767:
                continue
            w = worst(a, b, g, o, k, 1e300 if best is None else best[0] + 1e-9)
            dist = abs(dg) * 8 + abs(do)
            if best is None or w < best[0] - 1e-9 or (w < best[0] + 1e-9 and dist < best[3]):
                best = (w, g, o, dist)
    return a, b, best[1], best[2], best[0]


def stage_mv(c, ge, zero, bow, inl, scale=1.0):
    x = c / 4095.0
    return scale * K * (1 + ge) * c + zero + bow * math.sin(math.pi * x) + inl * math.sin(8 * math.pi * x)


def mkpts(ge, zero, bow, inl, scale=1.0):
    out = []
    for mv in (1200, 2400, 3600, 4800, 6000, 7200, 8400, 9600):
        c = round(mv * 3831 / 10000)
        out.append((c, stage_mv(c, ge, zero, bow, inl, scale)))
    return out


VECTORS = (('A', (0.012, 6.0, 1.2, 0.5)), ('B', (-0.015, 2.0, 1.0, 0.3)),
           ('C', (0.0, 0.0, 0.0, 0.0)), ('D', (0.03, -40.0, 2.0, 1.0)))


def stats(n):
    """Random stages: how big the module's own error is, and how often the provisional limits
    (non-linearity 3.0 mV about the fit line, verify 4.0 mV) would reject a sound stage. The
    readings carry 0.3 mV noise. Verify measures the corrected code on the same chain."""
    rnd = random.Random(251)
    for label, bow_max, inl_max in (('bow<=2, INL<=1', 2.0, 1.0), ('bow<=5, INL<=2.6', 5.0, 2.6)):
        pred, nl_rej, ver_rej = [], 0, 0
        for _ in range(n):
            ge = rnd.uniform(-0.03, 0.03); zero = rnd.uniform(-30, 30)
            bow = rnd.uniform(0, bow_max); inl = rnd.uniform(0, inl_max)
            pts = [(c, v + rnd.gauss(0, 0.3)) for c, v in mkpts(ge, zero, bow, inl)]
            a, b = line(pts)
            _, _, g, o, w = fit(pts)
            pred.append(w)
            nl = max(abs(v - (a * c + b)) for c, v in pts)
            nl_rej += nl > 3.0
            worstv = 0.0
            for mv in (300, 3000, 5400, 7800, 9600):
                sc = round(mv * 3831 / 10000)
                r = apply(sc, g, o)
                v = stage_mv(r, ge, zero, bow, inl) + rnd.gauss(0, 0.3)
                worstv = max(worstv, abs(v - sc * K))
            ver_rej += worstv > 4.0
        pred.sort()
        print('%-18s predicted worst: median %.2f p95 %.2f max %.2f mV   NONLIN rejects %.1f %%   VERIFY rejects %.1f %%'
              % (label, pred[len(pred) // 2], pred[int(len(pred) * .95)], pred[-1], 100.0 * nl_rej / n, 100.0 * ver_rej / n))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--stats':
        stats(int(sys.argv[2]) if len(sys.argv) > 2 else 100)
    else:
        for name, args in VECTORS:
            a, b, g, o, w = fit(mkpts(*args))
            print('%s %s a=%.9f b=%.9f g=%d o=%d worst=%.6f' % (name, args, a, b, g, o, w))
