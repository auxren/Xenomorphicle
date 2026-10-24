#!/usr/bin/env python3
"""Bench CV meter, input calibration and frequency counter over the console.

Host side of the console commands in software/src/CvMeter.cpp (T41_console
build). The procedure, the line formats and the accuracy limits are in
docs/CV-Meter.md; the key map is in docs/bench-console.md.

    cvmeter.py PORT stream [--csv FILE] [--seconds S]   log all 8 inputs (50 Hz)
    cvmeter.py PORT cal --input N|all --output M        guided input calibration
    cvmeter.py PORT print                               show the input calibration
    cvmeter.py PORT freq [--csv FILE] [--seconds S]     frequency counter on TR1
    cvmeter.py PORT check --output M --input N --mv X   set a voltage, read it back
    cvmeter.py PORT set --output M --mv X               hold an output
    cvmeter.py PORT release [--output M]                release one / every output

Also parsed here: the Cal Routine app's CALR,... log lines (parse_calr,
summarize_calr; fields from software/test/fixtures/calr_lines.txt) and the
console 'e' meter answers (parse_meter). "sim" has a simulated USBTMC meter
behind 'e' and start_cal_routine(), a whole all-pass run's CALR lines.

PORT is a serial device (/dev/ttyACM0, /dev/cu.usbmodem...), or "sim" for a
simulated module that needs no hardware and no pyserial -- what the tests and
a dry run use.

Only pyserial is needed beyond the standard library, and only for a real port.

THE CONSOLE IS LOCKED after every boot until it receives "pew!", and that
token is itself four commands on an unlocked console (see hwctl.py's module
docstring). So unlock() probes with a screen capture first and only sends the
token when nothing answers -- the same rule hwctl.py follows.
"""

import argparse
import csv
import math
import random
import re
import sys
import time

CHANNELS = 8
CAPTURE_BYTE = b"~"          # not a command: the console answers with a frame
FRAME_HEXCHARS = 2048

# ---------------------------------------------------------------------------
# Line parsing. These are the formats CvMeterMath.cpp prints; the golden
# examples live in software/test/fixtures/cvmeter_lines.txt and both the
# firmware test and test_cvmeter.py check them.


def parse_cv(line):
    """CV,<ms>,<seq>,<dropped>,<raw1..8>,<mv1..8>,<cal>,<clip> -> dict or None."""
    if not line.startswith("CV,"):
        return None
    f = line.strip().split(",")
    if len(f) != 4 + 2 * CHANNELS + 2:
        return None
    try:
        out = {
            "ms": int(f[1]),
            "seq": int(f[2]),
            "dropped": int(f[3]),
            "raw": [float(x) for x in f[4:4 + CHANNELS]],
            "mv": [float(x) for x in f[4 + CHANNELS:4 + 2 * CHANNELS]],
            "cal": f[4 + 2 * CHANNELS],
            "clip": f[5 + 2 * CHANNELS],
        }
    except ValueError:
        return None
    if len(out["cal"]) != CHANNELS or len(out["clip"]) != CHANNELS:
        return None
    if set(out["cal"]) - set("CS") or set(out["clip"]) - set(".LH"):
        return None
    return out


def parse_cvrange(line):
    """CVRANGE,<input>,<lo_mv>,<hi_mv>,<C|S> -> dict or None."""
    f = line.strip().split(",")
    if len(f) != 5 or f[0] != "CVRANGE":
        return None
    try:
        return {"input": int(f[1]), "lo": int(f[2]), "hi": int(f[3]), "cal": f[4]}
    except ValueError:
        return None


def parse_freq(line):
    """FREQ,<ms>,<hz>,<periods>,<note>,<cents> or FREQ,<ms>,NOSIG -> dict or None."""
    if not line.startswith("FREQ,"):
        return None
    f = line.strip().split(",")
    try:
        if len(f) == 3 and f[2] == "NOSIG":
            return {"ms": int(f[1]), "nosig": True}
        if len(f) != 6:
            return None
        cents = None if f[5] == "-" else float(f[5])
        return {"ms": int(f[1]), "hz": float(f[2]), "periods": int(f[3]),
                "note": f[4], "cents": cents, "nosig": False}
    except ValueError:
        return None


def parse_tagged(line, tag):
    """TAG,SUB,a,b,k=v,... -> (SUB, [positional], {k: v}) or None.

    INCAL, DAC and the '#' header lines all use this shape.
    """
    f = line.strip().split(",")
    if not f or f[0] != tag or len(f) < 2:
        return None
    pos, kv = [], {}
    for x in f[2:]:
        if "=" in x:
            k, v = x.split("=", 1)
            kv[k] = v
        else:
            pos.append(x)
    return f[1], pos, kv


# CALR lines: the Cal Routine app's run log (software/src/CalRoutineFlow.cpp).
# The field names and order are the CALR# headers of the golden file
# software/test/fixtures/calr_lines.txt; test_cvmeter.py checks this table
# against that file. kind: i int, f float, h hex int, s string.
CALR_FIELDS = {
    "METER": ("identity",),
    "STATE": ("ms", "stage", "out", "in"),
    "OUT": ("out",),
    "NODE": ("out", "k", "target_mv", "code", "measured_mv", "iters", "rail"),
    "REREAD": ("out", "target_mv", "code", "spread_mv"),
    "SWEEP": ("out", "target_mv", "code", "measured_mv", "resid_mv", "counted"),
    "ORES": ("out", "result", "reason", "worst_mv", "worst_at_mv", "rail_mask"),
    "IN": ("in", "src_out", "lo_mv", "hi_mv"),
    "IPT": ("in", "mv", "code", "mean_raw", "pp_raw", "samples", "used"),
    "IRES": ("in", "result", "verdict", "gain_uv", "offset_mv", "holdout_mv",
             "line_dev_mv", "points"),
    "RETRY": ("scpi", "attempt"),
    "MERR": ("step", "reply"),
    "LOST": ("out",),
    "BACK": ("out",),
    "REDO": ("out",),
    "RESULT": ("can_save", "out_pass", "in_pass"),
    "SAVE": ("result",),
    "DISCARD": (),
    "CANCEL": ("stage",),
    "FAIL": ("reason",),
    "MODE": ("mode",),
    "WARN": ("reason",),
    "PATCH": ("out", "moved", "own_delta_raw"),
    "PRES": ("result", "crossed", "missing"),
}
_CALR_KIND = {"ms": "i", "out": "i", "in": "i", "k": "i", "target_mv": "i", "code": "i",
              "measured_mv": "f", "iters": "i", "rail": "i", "spread_mv": "f",
              "resid_mv": "f", "counted": "i", "worst_mv": "f", "worst_at_mv": "i",
              "rail_mask": "h", "src_out": "i", "lo_mv": "i", "hi_mv": "i", "mv": "i",
              "mean_raw": "f", "pp_raw": "f", "samples": "i", "used": "i",
              "gain_uv": "f", "offset_mv": "f", "holdout_mv": "f", "line_dev_mv": "f",
              "points": "i", "attempt": "i", "can_save": "i", "out_pass": "h", "in_pass": "h",
              "moved": "h", "own_delta_raw": "f", "crossed": "h", "missing": "h"}


def parse_calr(line):
    """CALR,<SUB>,... -> {"sub": SUB, field: value, ...} or None.

    None for a '#' header, another tag, an unknown SUB, a wrong field count
    or a field that does not parse. Masks (rail_mask, out_pass, in_pass,
    moved, crossed, missing) are hex in the line and ints here.
    """
    f = line.strip().split(",")
    if len(f) < 2 or f[0] != "CALR" or f[1] not in CALR_FIELDS:
        return None
    names = CALR_FIELDS[f[1]]
    vals = f[2:]
    if f[1] == "METER":
        vals = [",".join(vals)] if vals else []   # an identity may hold commas
    if len(vals) != len(names):
        return None
    d = {"sub": f[1]}
    try:
        for n, v in zip(names, vals):
            k = _CALR_KIND.get(n, "s")
            d[n] = int(v) if k == "i" else float(v) if k == "f" else int(v, 16) if k == "h" else v
    except ValueError:
        return None
    return d


def summarize_calr(parsed):
    """Fold parsed CALR dicts into the figures an operator reads: the mode,
    per output and input results, the patch check (the last one wins: a
    failed check is followed by another once the patch is fixed), the
    RESULT masks, and whether the run saved."""
    res = {"outputs": {}, "inputs": {}, "saved": False, "failed": None, "cancelled": None,
           "out_pass": None, "in_pass": None, "can_save": None, "identity": None,
           "mode": None, "warn": None, "patch": None}
    moved = {}
    for d in parsed:
        s = d["sub"]
        if s == "MODE":
            res["mode"] = d["mode"]
        elif s == "WARN":
            res["warn"] = d["reason"]
        elif s == "PATCH":
            moved[d["out"]] = d["moved"]
        elif s == "PRES":
            res["patch"] = {"result": d["result"], "crossed": d["crossed"],
                            "missing": d["missing"], "moved": moved}
            moved = {}
        elif s == "ORES":
            res["outputs"][d["out"]] = d      # the last result of an output wins (retry)
        elif s == "IRES":
            res["inputs"][d["in"]] = d
        elif s == "RESULT":
            res["can_save"], res["out_pass"], res["in_pass"] = d["can_save"], d["out_pass"], d["in_pass"]
        elif s == "SAVE":
            res["saved"] = d["result"] == "OK"
        elif s == "FAIL":
            res["failed"] = d["reason"]
        elif s == "CANCEL":
            res["cancelled"] = d["stage"]
        elif s == "METER":
            res["identity"] = d["identity"]
    return res


def parse_meter(line):
    """METER,DONE,<reply> | REFUSED | TIMEOUT | ERR,<why> -> (kind, value) or None.

    Console 'e' answers. The reply of DONE may hold commas; value is None for
    REFUSED and TIMEOUT, the reason for ERR (NOT_CONNECTED, BUSY, BUSY_CAL, ...).
    """
    f = line.strip().split(",", 2)
    if len(f) < 2 or f[0] != "METER":
        return None
    if f[1] == "DONE":
        return "DONE", (f[2] if len(f) > 2 else "")
    if f[1] in ("REFUSED", "TIMEOUT") and len(f) == 2:
        return f[1], None
    if f[1] == "ERR" and len(f) == 3:
        return "ERR", f[2]
    return None


def cv_csv_header():
    return (["host_s", "ms", "seq", "dropped"]
            + [f"raw{i}" for i in range(1, CHANNELS + 1)]
            + [f"mv{i}" for i in range(1, CHANNELS + 1)]
            + ["cal", "clip"])


def cv_csv_row(host_s, d):
    return ([f"{host_s:.3f}", d["ms"], d["seq"], d["dropped"]]
            + [f"{x:.2f}" for x in d["raw"]] + [f"{x:.1f}" for x in d["mv"]]
            + [d["cal"], d["clip"]])


# ---------------------------------------------------------------------------
# Transport


class Console:
    """A real console on a serial port."""

    def __init__(self, port, baud=115200, verbose=False):
        try:
            import serial  # noqa: WPS433 (only needed for real hardware)
        except ImportError:
            sys.exit("pyserial required for a real port: python3 -m pip install pyserial")
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.verbose = verbose
        self._buf = b""
        time.sleep(0.4)   # DTR settles before the first byte goes out
        self.ser.reset_input_buffer()

    def log(self, *a):
        if self.verbose:
            print(*a, file=sys.stderr)

    def write(self, data):
        self.log(">>", data)
        self.ser.write(data)
        self.ser.flush()

    def readline(self, timeout=1.0):
        """One decoded line without its ending, or None on timeout."""
        deadline = time.time() + timeout
        while True:
            i = self._buf.find(b"\n")
            if i >= 0:
                line, self._buf = self._buf[:i], self._buf[i + 1:]
                text = line.decode("ascii", "replace").rstrip("\r")
                self.log("<<", text)
                return text
            if time.time() >= deadline:
                return None
            self._buf += self.ser.read(512)

    def drain(self, quiet=0.15):
        time.sleep(quiet)
        self.ser.reset_input_buffer()
        self._buf = b""

    def unlock(self):
        # '!' cancels any half-typed argument; it is not a command.
        self.write(b"!!!")
        self.drain()
        self.write(CAPTURE_BYTE)
        got, deadline = 0, time.time() + 1.5
        while got < FRAME_HEXCHARS and time.time() < deadline:
            chunk = self.ser.read(256).decode("ascii", "ignore")
            got += sum(1 for c in chunk if c in "0123456789abcdefABCDEF")
        self.drain()
        if got >= FRAME_HEXCHARS:
            self.log("already unlocked -- token not sent")
            return
        self.write(b"pew!")
        self.drain(0.3)

    def close(self):
        self.ser.close()


class SimModule:
    """A simulated module for --dry-run use and the tests.

    It speaks the same lines as CvMeter.cpp and models a Xenomorpher: 1.2 V
    per DAC table step and per pitch octave, 0 V near the raw-4095 rail.
    Output o drives input o (the patch the calibration prompts for) unless
    `patch` says otherwise; each input has its own front-end gain/offset
    error and a little noise. Time is simulated, so nothing here sleeps.
    """

    SHARED_GAIN = 2.9296875       # mV/count: default scale, 1.2 V octaves
    DAC_MAX_MV = 12000            # table entries 0..10 at 1.2 V
    CAL_POINTS = (1200, 8400, 4800)

    def __init__(self, seed=1, patch=None, freq_hz=440.0, noise_counts=0.6):
        self.rng = random.Random(seed)
        self.t_ms = 1000
        self.patch = dict(patch) if patch else {i: i for i in range(CHANNELS)}
        # true front end: raw = zero - mv / lsb_mv
        self.zero = [4093.0 - 1.3 * i for i in range(CHANNELS)]
        self.lsb_mv = [2.9297 * (1.0 + 0.004 * (i - 3)) for i in range(CHANNELS)]
        self.noise = noise_counts
        self.held = {}
        self.cal = {}           # input -> (gain mV/count, offset mV)
        self.stream = False
        self.freq = False
        self.freq_hz = freq_hz
        self.out = []
        self.pending = ""
        self.flow = None
        self.seq = 0
        # simulated USBTMC meter behind console 'e'
        self.meter = "ok"              # None = not connected
        self.meter_output = 0          # the output the meter lead is on (0-based)
        self.meter_timeouts = 0        # next n queries time out
        self.meter_line = ""           # None when not collecting an 'e' line
        self.meter_collect = False
        self.calr = []                 # queued CALR lines of a running Cal Routine

    # -- the device side -------------------------------------------------
    def _raw(self, i):
        mv = 0.0
        for o, inp in self.patch.items():
            if inp == i and o in self.held:
                mv = self.held[o]
        raw = self.zero[i] - mv / self.lsb_mv[i] + self.rng.gauss(0.0, self.noise)
        return min(4095.0, max(0.0, raw))

    def _mv(self, i, raw):
        if i in self.cal:
            g, off = self.cal[i]
            return g * raw + off, "C"
        return (4095.0 - raw) * self.SHARED_GAIN, "S"

    def _say(self, s):
        self.out.append(s)

    def _cv_line(self):
        raws = [self._raw(i) for i in range(CHANNELS)]
        mvs, cal = [], ""
        for i, r in enumerate(raws):
            m, c = self._mv(i, r)
            mvs.append(m)
            cal += c
        clip = "".join("L" if r < 2 else "H" if r >= 4094 else "." for r in raws)
        line = (f"CV,{self.t_ms},{self.seq},0,"
                + ",".join(f"{r:.2f}" for r in raws) + ","
                + ",".join(f"{m:.1f}" for m in mvs) + f",{cal},{clip}")
        self.seq += 1
        return line

    def _freq_line(self):
        hz = self.freq_hz * (1.0 + self.rng.gauss(0.0, 2e-6))
        semis = 12.0 * math.log2(hz / 440.0)
        n = math.floor(semis + 0.5)
        cents = 100.0 * (semis - n)
        names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        midi = 69 + n
        note = f"{names[midi % 12]}{midi // 12 - 1}"
        sign = "-" if cents < 0 else "+"
        return f"FREQ,{self.t_ms},{hz:.4f},{max(1, int(hz / 4))},{note},{sign}{abs(cents):.2f}"

    def _flow_points(self):
        return list(self.CAL_POINTS)

    def _run_flow_measure(self):
        i, o = self.flow["in"], self.flow["out"]
        pts = self._flow_points()
        means = []
        for k, mv in enumerate(pts):
            self.held[o] = mv
            xs = [self._raw(i) for _ in range(200)]
            mean = sum(xs) / len(xs)
            means.append(mean)
            self._say(f"INCAL,POINT,in={i + 1},{k + 1},{mv},{mean:.2f},{max(xs) - min(xs):.2f},{len(xs)}")
        self.held.pop(o, None)
        g = (pts[1] - pts[0]) / (means[1] - means[0])
        off = pts[0] - g * means[0]
        resid = g * means[2] + off - pts[2]
        verdict = "OK" if abs(resid) <= 10.0 and -6 < g < -1 else "RESIDUAL"
        self.flow.update(gain=g, offset=off, verdict=verdict)
        self._say(f"INCAL,RESULT,in={i + 1},out={o + 1},gain_uv={g * 1000:.1f},"
                  f"offset_mv={off:.2f},resid_mv={resid:.2f},noise_pp=2.00,verdict={verdict}")
        if verdict == "OK":
            self._say("  Press y to store this calibration; any other key discards it.")
            self.flow["state"] = "commit"
        else:
            self._next_input()

    def _start_input(self):
        f = self.flow
        pts = ",".join(str(p) for p in self.CAL_POINTS)
        self._say(f"INCAL,PATCH,in={f['in'] + 1},out={f['out'] + 1},points={pts}")
        self._say(f"  Patch OUTPUT {f['out'] + 1} to INPUT {f['in'] + 1} ...")
        f["state"] = "patch"

    def _next_input(self):
        f = self.flow
        if f["all"] and f["in"] < CHANNELS - 1:
            f["in"] += 1
            self.patch = {f["out"]: f["in"]}
            self._start_input()
            return
        if f["all"]:
            self._say("INCAL,DONE")
        self.flow = None

    def _key(self, c):
        if self.flow:
            st = self.flow["state"]
            if st == "patch":
                if c == "y":
                    self._run_flow_measure()
                else:
                    self._say(f"INCAL,ABORT,in={self.flow['in'] + 1},cancelled at the patch prompt")
                    self.flow = None
            elif st == "commit":
                i = self.flow["in"]
                if c == "y":
                    self.cal[i] = (self.flow["gain"], self.flow["offset"])
                    self._say(f"INCAL,SAVED,in={i + 1}")
                else:
                    self._say(f"INCAL,DISCARDED,in={i + 1}")
                self._next_input()
            return
        if self.meter_collect:
            if c in "\r\n":
                self.meter_collect = False
                self._meter_query(self.meter_line)
            else:
                self.meter_line += c
            return
        p = self.pending + c
        if p == "e":
            self.meter_collect, self.meter_line, self.pending = True, "", ""
        elif p == "v":
            self.stream = not self.stream
            if self.stream:
                self._say("CV#,ms,seq,dropped," + ",".join(cv_csv_header()[4:]))
                for i in range(CHANNELS):
                    self._say(f"CVRANGE,{i + 1},0,11997,{'C' if i in self.cal else 'S'}")
            else:
                self._say(f"CV#,stopped,lines={self.seq},dropped=0")
            self.pending = ""
        elif p == "n":
            self.freq = not self.freq
            self._say("FREQ#,jack=TR1,gate_ms=250,a4=440" if self.freq else "FREQ#,stopped,dropped=0")
            self.pending = ""
        elif p in ("d", "Q") or (p.startswith("d") and not p.endswith(("\r", "\n", "x"))
                                  and re.fullmatch(r"d[1-8a-h][+-]?\d*", p)) \
                or re.fullmatch(r"Q[1-8a]", p):
            self.pending = p
        elif p == "dx":
            self.held.clear()
            self._say("DAC,ALL,RELEASED")
            self.pending = ""
        elif re.fullmatch(r"d[1-8]x", p):
            self.held.pop(int(p[1]) - 1, None)
            self._say(f"DAC,{p[1]},RELEASED")
            self.pending = ""
        elif re.fullmatch(r"d[1-8][+-]?\d+[\r\n ]", p):
            o, mv = int(p[1]) - 1, int(p[2:-1])
            if 0 <= mv <= self.DAC_MAX_MV:
                self.held[o] = mv
                self._say(f"DAC,{o + 1},HOLD,{mv},{200 + mv * 5}")
            else:
                self._say(f"DAC,ERR,{o + 1},{mv} mV is outside this output's calibrated range "
                          f"0..{self.DAC_MAX_MV} mV")
            self.pending = ""
        elif p == "Qp":
            self._say("INCAL#,version=1,mask=%02X,stored=yes,scale=15360,octave_mv=1200"
                      % sum(1 << i for i in self.cal))
            for i in range(CHANNELS):
                if i in self.cal:
                    g, off = self.cal[i]
                    self._say(f"INCAL,CH,{i + 1},C,gain_uv={g * 1000:.1f},offset_mv={off:.2f},"
                              f"verify_mv=0.3,src={i + 1},range=0..12000,apps=line")
                else:
                    self._say(f"INCAL,CH,{i + 1},S,adc_offset=4095,range=0..11997,apps=shared")
            self.pending = ""
        elif re.fullmatch(r"Q[1-8a][1-8]", p):
            a = p[1] == "a"
            i = 0 if a else int(p[1]) - 1
            o = int(p[2]) - 1
            self.patch = {o: i}
            self.flow = {"in": i, "out": o, "all": a}
            self._start_input()
            self.pending = ""
        else:
            self.pending = ""   # unknown: a screen capture on the real thing

    # -- the simulated meter (console 'e') -------------------------------
    METER_ID = "Siglent Technologies,SDM3065X,SDM36GBX000000,1.01.01.25"
    METER_ALLOW = ("*IDN?", "*RST", "*CLS", "SYST:ERR?", "READ?", "CONF:VOLT:DC",
                   "VOLT:DC:", "SENS:", "TRIG:", "INIT", "FETC?", "MEAS:VOLT:DC?")

    def _meter_query(self, line):
        if self.cal_busy:
            return self._say("METER,ERR,BUSY_CAL")
        m = re.fullmatch(r"@(\d+) (.*)", line)
        if line.startswith("@"):
            if not m or not 1 <= int(m.group(1)) <= 60000:
                return self._say("METER,ERR,cancelled (expected @<1-60000 ms> then a space)")
            line = m.group(2)
        if not line.upper().startswith(self.METER_ALLOW):
            return self._say("METER,REFUSED")
        if self.meter is None:
            return self._say("METER,ERR,NOT_CONNECTED")
        if self.meter_timeouts > 0:
            self.meter_timeouts -= 1
            return self._say("METER,TIMEOUT")
        u = line.upper()
        if u == "*IDN?":
            reply = self.METER_ID
        elif u == "SYST:ERR?":
            reply = '+0,"No error"'
        elif u.endswith("?"):
            volts = self.held.get(self.meter_output, 0.0) / 1000.0
            volts += self.rng.gauss(0.0, 2e-5)
            reply = f"{volts:+.5E}"
        else:
            reply = ""
        self._say(f"METER,DONE,{reply}")

    # -- a simulated Cal Routine run (the app's CALR log) -----------------
    cal_busy = False

    def start_cal_routine(self):
        """Queue the CALR lines of a whole, all-pass Self run (8 outputs
        against the meter, then outputs patched A->1 ... H->8: the patch
        check and one all-8 input sweep) and own the meter until they have
        been read."""
        r = self.rng
        L = ["CALR#,version=2"]
        L += ["CALR#," + sub + ("," + ",".join(f) if f else "") for sub, f in CALR_FIELDS.items()]
        L.append("CALR,MODE,SELF")
        L.append("CALR,METER," + self.METER_ID.replace(",", ";"))
        ms = 1000

        def line(sub, *vals):
            L.append("CALR," + sub + "".join("," + str(v) for v in vals))

        for o in range(1, 9):
            ms += 20000
            line("STATE", ms, "CAL_OUT", o, 0)
            line("OUT", o)
            for k in range(11):
                t = k * 1200
                code = round(t * 5.4613)
                line("NODE", o, k, t, code, f"{t + r.gauss(0, 0.1):.3f}", 3, 0)
            line("REREAD", o, 3600, 19661, f"{abs(r.gauss(0, 0.4)):.3f}")
            for k in range(10):                       # 2 verify points per node gap
                for t in (k * 1200 + 400, k * 1200 + 800):
                    line("SWEEP", o, t, round(t * 5.4613), f"{t + r.gauss(0, 0.2):.3f}",
                         f"{r.gauss(0, 0.2):.3f}", 1)
            line("ORES", o, "PASS", "OK", f"{abs(r.gauss(0, 0.2)):.3f}", 10560, 0)
        ms += 5000
        line("STATE", ms, "PROMPT_PATCH", 0, 0)
        line("STATE", ms + 2000, "CHECK_PATCH", 0, 0)
        for o in range(1, 9):
            line("PATCH", o, f"{1 << (o - 1):02X}", f"{1963 + r.gauss(0, 20):.2f}")
        line("PRES", "PASS", "00", "00")
        ms += 10000
        line("STATE", ms, "CAL_IN", 0, 0)
        for i in range(1, 9):
            line("IN", i, i, 600, 10800)
        for mv in range(600, 10201, 600):                # all 8 inputs per window
            for i in range(1, 9):
                raw = 3640.0 - mv / 2.75
                line("IPT", i, mv, round(mv * 5.4613), f"{raw:.2f}", "2.25", 3333, 1)
        for i in range(1, 9):
            line("IRES", i, "PASS", "OK", f"{-2750.0 + r.gauss(0, 2):.1f}",
                 f"{10010 + r.gauss(0, 0.5):.2f}", f"{abs(r.gauss(0, 0.5)):.2f}",
                 f"{abs(r.gauss(0, 0.8)):.2f}", 17)
        line("RESULT", 1, "FF", "FF")
        line("SAVE", "OK")
        self.calr = L
        self.cal_busy = True

    # -- the Console interface -------------------------------------------
    def write(self, data):
        for c in data.decode("ascii"):
            self._key(c)

    def readline(self, timeout=1.0):
        if self.out:
            return self.out.pop(0)
        if self.calr:
            l = self.calr.pop(0)
            if not self.calr:
                self.cal_busy = False
            return l
        if self.stream or self.freq:
            self.t_ms += 20
            if self.freq and self.t_ms % 260 == 0:
                return self._freq_line()
            if self.stream:
                return self._cv_line()
            return self.readline(timeout)
        return None

    def drain(self, quiet=0.0):
        self.out.clear()

    def unlock(self):
        pass

    def close(self):
        pass


def open_console(port, verbose=False, unlock=True):
    con = SimModule() if port == "sim" else Console(port, verbose=verbose)
    if unlock:
        con.unlock()
    return con


# ---------------------------------------------------------------------------
# Operations


def wait_for(con, pred, timeout=3.0):
    """Read lines until pred(line) is truthy; return (line, value) or (None, None)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = con.readline(timeout=max(0.05, deadline - time.time()))
        if line is None:
            continue
        v = pred(line)
        if v:
            return line, v
    return None, None


def toggle_on(con, key, started_prefix, stopped_prefix, err_prefix=None):
    """Send a toggle key and make sure it ended up ON (it may have been on)."""
    for _ in range(2):
        con.write(key)

        def which(line):
            if err_prefix and line.startswith(err_prefix):
                return "err"
            if line.startswith(stopped_prefix):
                return "stopped"
            if line.startswith(started_prefix):
                return "started"
            return None
        line, state = wait_for(con, which)
        if state == "started":
            return True
        if state == "err":
            sys.exit(f"refused: {line}")
        if state is None:
            sys.exit(f"no answer to {key!r}: is the console unlocked and is this a T41_console build?")
    return False


def toggle_off(con, key, stopped_prefix):
    con.write(key)
    wait_for(con, lambda line: line.startswith(stopped_prefix), timeout=2.0)


def cmd_stream(con, args):
    toggle_on(con, b"v", "CV#,ms", "CV#,stopped")
    out = open(args.csv, "w", newline="") if args.csv else None
    w = csv.writer(out) if out else None
    if w:
        w.writerow(cv_csv_header())
    t0 = time.time()
    n = 0
    last_print = 0.0
    try:
        while args.seconds is None or time.time() - t0 < args.seconds:
            line = con.readline(timeout=0.5)
            if line is None:
                continue
            r = parse_cvrange(line)
            if r:
                print(f"input {r['input']}: reads {r['lo']}..{r['hi']} mV ({r['cal']})")
                continue
            d = parse_cv(line)
            if not d:
                continue
            n += 1
            if w:
                w.writerow(cv_csv_row(time.time() - t0, d))
            if time.time() - last_print >= 0.5 and not args.quiet:
                last_print = time.time()
                cells = " ".join(f"{m / 1000:8.4f}{c}{'!' if k != '.' else ' '}"
                                 for m, c, k in zip(d["mv"], d["cal"], d["clip"]))
                print(f"{d['ms']:>10} {cells}  drop={d['dropped']}")
            if args.sim_lines and n >= args.sim_lines:
                break
    except KeyboardInterrupt:
        pass
    finally:
        toggle_off(con, b"v", "CV#,stopped")
        if out:
            out.close()
    print(f"{n} lines" + (f" -> {args.csv}" if args.csv else ""))
    return 0


def cmd_freq(con, args):
    toggle_on(con, b"n", "FREQ#,jack", "FREQ#,stopped", err_prefix="FREQ#,ERR")
    out = open(args.csv, "w", newline="") if args.csv else None
    w = csv.writer(out) if out else None
    if w:
        w.writerow(["host_s", "ms", "hz", "periods", "note", "cents"])
    t0 = time.time()
    n = 0
    try:
        while args.seconds is None or time.time() - t0 < args.seconds:
            line = con.readline(timeout=0.5)
            if line is None:
                continue
            if line.startswith("FREQ#,stopped"):
                print(line)
                break
            d = parse_freq(line)
            if not d:
                continue
            n += 1
            if d["nosig"]:
                if not args.quiet:
                    print(f"{d['ms']:>10}  no signal on TR1")
                continue
            if w:
                w.writerow([f"{time.time() - t0:.3f}", d["ms"], f"{d['hz']:.4f}", d["periods"],
                            d["note"], d["cents"]])
            if not args.quiet:
                print(f"{d['ms']:>10}  {d['hz']:12.4f} Hz  {d['note']:>4} {d['cents']:+7.2f} c"
                      f"  ({d['periods']} periods)")
            if args.sim_lines and n >= args.sim_lines:
                break
    except KeyboardInterrupt:
        pass
    finally:
        toggle_off(con, b"n", "FREQ#,stopped")
        if out:
            out.close()
    return 0


def cmd_print(con, args):
    con.write(b"Qp")
    got = 0
    while got < CHANNELS:
        line = con.readline(timeout=2.0)
        if line is None:
            sys.exit("no answer to Qp")
        if line.startswith("INCAL"):
            print(line)
            if line.startswith("INCAL,CH,"):
                got += 1
    return 0


def run_cal(con, inp, out, ask=input, say=print):
    """Drive one 'Q' run. inp: 1..8 or 'a'. Returns the INCAL outcomes seen."""
    con.write(b"Q" + str(inp).encode() + str(out).encode())
    outcomes = []
    single = inp != "a"
    while True:
        line = con.readline(timeout=5.0)
        if line is None:
            say("timed out waiting for the module")
            break
        if line.startswith("  "):
            continue
        t = parse_tagged(line, "INCAL")
        if not t:
            continue
        sub, pos, kv = t
        if sub == "PATCH":
            say(f"\nPatch OUTPUT {kv['out']} -> INPUT {kv['in']} (nothing else on either jack).")
            # points=a,b,c: the comma split leaves b and c positional
            pts = ", ".join([kv.get("points", "?")] + pos)
            say(f"Output {kv['out']} will be driven to {pts} mV.")
            ans = ask("Press Enter when patched (q aborts): ").strip().lower()
            con.write(b"x" if ans.startswith("q") else b"y")
        elif sub == "POINT":
            say(f"  point {pos[0]}: set {pos[1]} mV, raw {pos[2]}, spread {pos[3]} counts, n={pos[4]}")
        elif sub == "RESULT":
            say(f"  input {kv['in']}: gain {kv['gain_uv']} uV/count, offset {kv['offset_mv']} mV, "
                f"verify residual {kv['resid_mv']} mV, noise {kv['noise_pp']} counts p-p "
                f"-> {kv['verdict']}")
            if kv["verdict"] == "OK":
                ans = ask("  Store it? [y/N]: ").strip().lower()
                con.write(b"y" if ans == "y" else b"n")
            else:
                outcomes.append(("REJECTED", kv["in"], kv["verdict"]))
                if single:
                    break
        elif sub in ("SAVED", "DISCARDED"):
            say(f"  input {kv['in']}: {sub.lower()}")
            outcomes.append((sub, kv["in"], None))
            if single:
                break
        elif sub in ("ABORT", "ERR"):
            say(f"  {line}")
            outcomes.append((sub, kv.get("in"), None))
            break
        elif sub == "DONE":
            break
    return outcomes


def cmd_cal(con, args):
    inp = "a" if args.input == "all" else int(args.input)
    if inp != "a" and not 1 <= inp <= CHANNELS:
        sys.exit("--input is 1..8 or all")
    ask = (lambda prompt: "y") if args.yes else input
    outcomes = run_cal(con, inp, args.output, ask=ask)
    saved = sum(1 for o in outcomes if o[0] == "SAVED")
    print(f"\n{saved} input(s) stored.")
    return 0 if saved or not outcomes else 1


def hold(con, out, mv):
    con.write(f"d{out}{mv}\r".encode())
    line, t = wait_for(con, lambda l: parse_tagged(l, "DAC"))
    if not t or t[0] == "ERR":
        sys.exit(f"DAC set failed: {line}")
    return line


def release(con, out=None):
    con.write(b"dx" if out is None else f"d{out}x".encode())
    wait_for(con, lambda l: l.startswith("DAC,") and "RELEASED" in l, timeout=2.0)


def cmd_set(con, args):
    print(hold(con, args.output, args.mv))
    return 0


def cmd_release(con, args):
    release(con, args.output)
    print("released")
    return 0


def mean_input(con, inp, seconds, settle=0.5, max_lines=None):
    """Average one input's mV over `seconds` of the CV stream, after `settle`."""
    toggle_on(con, b"v", "CV#,ms", "CV#,stopped")
    vals, raws, cal = [], [], "?"
    t0 = time.time()
    n = 0
    try:
        while True:
            line = con.readline(timeout=0.5)
            d = parse_cv(line) if line else None
            if not d:
                continue
            n += 1
            if time.time() - t0 < settle and not max_lines:
                continue
            vals.append(d["mv"][inp - 1])
            raws.append(d["raw"][inp - 1])
            cal = d["cal"][inp - 1]
            if max_lines and n >= max_lines:
                break
            if not max_lines and time.time() - t0 >= settle + seconds:
                break
    finally:
        toggle_off(con, b"v", "CV#,stopped")
    if not vals:
        sys.exit("no CV lines arrived")
    m = sum(vals) / len(vals)
    sd = math.sqrt(sum((v - m) ** 2 for v in vals) / len(vals))
    return m, sd, sum(raws) / len(raws), cal, len(vals)


def cmd_check(con, args):
    print(hold(con, args.output, args.mv))
    try:
        m, sd, raw, cal, n = mean_input(con, args.input, args.seconds, settle=args.settle,
                                        max_lines=args.sim_lines)
    finally:
        release(con, args.output)
    print(f"output {args.output} set {args.mv} mV -> input {args.input} reads {m:.1f} mV "
          f"(sd {sd:.2f} mV, raw {raw:.2f}, {n} lines, calibration {cal}): "
          f"error {m - args.mv:+.1f} mV")
    return 0 if abs(m - args.mv) <= args.tolerance else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Bench CV meter / input calibration / frequency counter.")
    ap.add_argument("port", help="serial port, or 'sim' for a simulated module")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--no-unlock", action="store_true", help="skip the probe-then-unlock step")
    ap.add_argument("--sim-lines", type=int, default=None, help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("stream", help="log the CV stream")
    p.add_argument("--csv")
    p.add_argument("--seconds", type=float)
    p.add_argument("--quiet", action="store_true")

    p = sub.add_parser("cal", help="guided input calibration")
    p.add_argument("--input", required=True, help="1..8 or all")
    p.add_argument("--output", type=int, required=True, choices=range(1, CHANNELS + 1))
    p.add_argument("--yes", action="store_true", help="answer every prompt yes (scripted runs)")

    sub.add_parser("print", help="print the per-input calibration")

    p = sub.add_parser("freq", help="frequency counter on TR1")
    p.add_argument("--csv")
    p.add_argument("--seconds", type=float)
    p.add_argument("--quiet", action="store_true")

    p = sub.add_parser("check", help="hold an output, read an input, compare")
    p.add_argument("--output", type=int, required=True, choices=range(1, CHANNELS + 1))
    p.add_argument("--input", type=int, required=True, choices=range(1, CHANNELS + 1))
    p.add_argument("--mv", type=int, required=True)
    p.add_argument("--seconds", type=float, default=2.0)
    p.add_argument("--settle", type=float, default=0.5)
    p.add_argument("--tolerance", type=float, default=10.0, help="mV; exit 1 beyond it")

    p = sub.add_parser("set", help="hold an output at a voltage")
    p.add_argument("--output", type=int, required=True, choices=range(1, CHANNELS + 1))
    p.add_argument("--mv", type=int, required=True)

    p = sub.add_parser("release", help="release one output, or all")
    p.add_argument("--output", type=int, choices=range(1, CHANNELS + 1))

    args = ap.parse_args(argv)
    con = open_console(args.port, verbose=args.verbose, unlock=not args.no_unlock)
    try:
        return {"stream": cmd_stream, "cal": cmd_cal, "print": cmd_print, "freq": cmd_freq,
                "check": cmd_check, "set": cmd_set, "release": cmd_release}[args.cmd](con, args)
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
