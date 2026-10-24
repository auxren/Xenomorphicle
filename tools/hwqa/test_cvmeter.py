#!/usr/bin/env python3
"""Tests for cvmeter.py: line parsing against the firmware's golden lines,
and every operation end to end against the simulated module. No hardware,
no pyserial.

    python3 -m unittest tools/hwqa/test_cvmeter.py      (from the repo root)
    python3 tools/hwqa/test_cvmeter.py
"""

import argparse
import io
import os
import sys
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cvmeter  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "..", "software", "test", "fixtures", "cvmeter_lines.txt")


def golden(tag):
    with open(FIXTURE) as f:
        return [l.rstrip("\n") for l in f if l.startswith(tag + ",")]


class ParseGolden(unittest.TestCase):
    """The same lines test_cvmeter_math.cpp checks the firmware prints."""

    def test_cv_line(self):
        (line,) = golden("CV")
        d = cvmeter.parse_cv(line)
        self.assertIsNotNone(d)
        self.assertEqual(d["ms"], 123456)
        self.assertEqual(d["seq"], 7)
        self.assertEqual(d["dropped"], 2)
        self.assertEqual(d["raw"], [4095.0, 3685.5, 2047.75, 0.0, 409.6, 100.01, 1.0, 4094.0])
        self.assertEqual(d["mv"], [0.0, 1000.2, -3333.3, 9997.6, 0.5, -0.5, 12345.6, -0.1])
        self.assertEqual(d["cal"], "CCSSSSSS")
        self.assertEqual(d["clip"], "H..L...H")

    def test_freq_lines(self):
        lines = golden("FREQ")
        self.assertEqual(len(lines), 4)
        d = [cvmeter.parse_freq(l) for l in lines]
        self.assertEqual(d[0], {"ms": 99, "hz": 440.0, "periods": 110, "note": "A4",
                                "cents": 0.0, "nosig": False})
        self.assertAlmostEqual(d[1]["cents"], 19.56)
        self.assertAlmostEqual(d[2]["cents"], -11.84)
        self.assertEqual(d[3]["note"], "A0")

    def test_csv_row_round_trip(self):
        (line,) = golden("CV")
        d = cvmeter.parse_cv(line)
        row = cvmeter.cv_csv_row(1.5, d)
        self.assertEqual(len(row), len(cvmeter.cv_csv_header()))
        self.assertEqual(row[4:12], ["4095.00", "3685.50", "2047.75", "0.00", "409.60", "100.01",
                                     "1.00", "4094.00"])


class ParseRejects(unittest.TestCase):
    def test_cv_rejects(self):
        (line,) = golden("CV")
        self.assertIsNone(cvmeter.parse_cv(line.rsplit(",", 1)[0]))           # field missing
        self.assertIsNone(cvmeter.parse_cv(line.replace("CCSSSSSS", "CCSS")))  # short flags
        self.assertIsNone(cvmeter.parse_cv(line.replace("CCSSSSSS", "CCSSSSSX")))
        self.assertIsNone(cvmeter.parse_cv(line.replace("H..L...H", "H..L...Q")))
        self.assertIsNone(cvmeter.parse_cv(line.replace("4095.00", "40x5")))
        self.assertIsNone(cvmeter.parse_cv("CV#,ms,seq"))
        self.assertIsNone(cvmeter.parse_cv("0123456789abcdef" * 8))          # a screen capture
        self.assertIsNone(cvmeter.parse_cv(""))

    def test_freq_variants(self):
        self.assertEqual(cvmeter.parse_freq("FREQ,5,NOSIG"), {"ms": 5, "nosig": True})
        d = cvmeter.parse_freq("FREQ,1,0.0000,0,-,-")
        self.assertIsNone(d["cents"])
        self.assertIsNone(cvmeter.parse_freq("FREQ#,jack=TR1,gate_ms=250,a4=440"))
        self.assertIsNone(cvmeter.parse_freq("FREQ,1,abc,0,A4,+0.00"))

    def test_tagged(self):
        sub, pos, kv = cvmeter.parse_tagged(
            "INCAL,RESULT,in=3,out=3,gain_uv=-2441.4,offset_mv=9997.12,resid_mv=0.80,"
            "noise_pp=2.00,verdict=OK", "INCAL")
        self.assertEqual(sub, "RESULT")
        self.assertEqual(pos, [])
        self.assertEqual(kv["verdict"], "OK")
        self.assertEqual(kv["gain_uv"], "-2441.4")
        sub, pos, kv = cvmeter.parse_tagged("INCAL,POINT,in=2,1,1000,3683.40,2.10,3333", "INCAL")
        self.assertEqual((sub, pos, kv), ("POINT", ["1", "1000", "3683.40", "2.10", "3333"], {"in": "2"}))
        self.assertEqual(cvmeter.parse_tagged("DAC,3,HOLD,5000,32700", "DAC"),
                         ("3", ["HOLD", "5000", "32700"], {}))
        self.assertIsNone(cvmeter.parse_tagged("CV,1,2", "DAC"))
        self.assertEqual(cvmeter.parse_cvrange("CVRANGE,1,0,9998,S"),
                         {"input": 1, "lo": 0, "hi": 9998, "cal": "S"})
        self.assertIsNone(cvmeter.parse_cvrange("CVRANGE,1,0,S"))


def args(**kw):
    base = {"csv": None, "seconds": None, "quiet": True, "sim_lines": 40}
    base.update(kw)
    return argparse.Namespace(**base)


class Simulated(unittest.TestCase):
    def setUp(self):
        self.sim = cvmeter.SimModule(seed=3)

    def run_quiet(self, fn, *a, **kw):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = fn(*a, **kw)
        return rc, buf.getvalue()

    def test_stream_to_csv(self):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "cvmeter_test_stream.csv")
        rc, _ = self.run_quiet(cvmeter.cmd_stream, self.sim, args(csv=path))
        self.assertEqual(rc, 0)
        with open(path) as f:
            rows = f.read().splitlines()
        os.remove(path)
        self.assertEqual(rows[0].split(","), cvmeter.cv_csv_header())
        self.assertEqual(len(rows), 41)
        self.assertFalse(self.sim.stream)       # left off afterwards

    def test_stream_already_on_is_left_on_then_off(self):
        self.sim.write(b"v")
        self.sim.drain()
        rc, _ = self.run_quiet(cvmeter.cmd_stream, self.sim, args(sim_lines=5))
        self.assertEqual(rc, 0)
        self.assertFalse(self.sim.stream)

    def test_set_check_release(self):
        chk = args(output=2, input=2, mv=5000, seconds=1, settle=0, tolerance=10.0, sim_lines=60)
        # The simulated input's front end is 0.4 % and a few counts off, like
        # a real one: on the shared scale it misses by tens of mV ...
        rc, out = self.run_quiet(cvmeter.cmd_check, self.sim, chk)
        self.assertEqual(rc, 1, out)
        self.assertIn("calibration S", out)
        self.assertEqual(self.sim.held, {})     # released
        # ... and after the closed-loop calibration it is inside the bound.
        with redirect_stdout(io.StringIO()):
            cvmeter.run_cal(self.sim, 2, 2, ask=lambda p: "y")
        self.sim.patch = {1: 1}
        rc, out = self.run_quiet(cvmeter.cmd_check, self.sim, chk)
        self.assertEqual(rc, 0, out)
        self.assertIn("calibration C", out)
        rc, out = self.run_quiet(cvmeter.cmd_set, self.sim, argparse.Namespace(output=4, mv=2500))
        self.assertEqual(rc, 0)
        self.assertIn("DAC,4,HOLD,2500", out)
        self.assertEqual(self.sim.held, {3: 2500})
        self.run_quiet(cvmeter.cmd_release, self.sim, argparse.Namespace(output=None))
        self.assertEqual(self.sim.held, {})

    def test_out_of_range_set_is_refused(self):
        with self.assertRaises(SystemExit):
            cvmeter.hold(self.sim, 4, 12001)

    def test_calibration_single_and_commit(self):
        prompts = []

        def ask(p):
            prompts.append(p)
            return "y"
        buf = io.StringIO()
        with redirect_stdout(buf):
            outcomes = cvmeter.run_cal(self.sim, 3, 3, ask=ask)
        self.assertEqual(outcomes, [("SAVED", "3", None)])
        self.assertEqual(len(prompts), 2)        # patched? store?
        self.assertIn(2, self.sim.cal)           # physical input 3
        g, off = self.sim.cal[2]
        self.assertAlmostEqual(g, -self.sim.lsb_mv[2], delta=0.01)
        # the stream now marks input 3 as per-input calibrated, and reads true
        self.sim.held[2] = 5000
        self.sim.patch = {2: 2}
        self.sim.write(b"v")
        d = None
        while d is None:
            d = cvmeter.parse_cv(self.sim.readline() or "")
        self.assertEqual(d["cal"][2], "C")
        self.assertAlmostEqual(d["mv"][2], 5000.0, delta=8.0)

    def test_calibration_discard_and_abort(self):
        buf = io.StringIO()
        answers = iter(["", "n"])
        with redirect_stdout(buf):
            out = cvmeter.run_cal(self.sim, 1, 1, ask=lambda p: next(answers))
        self.assertEqual(out, [("DISCARDED", "1", None)])
        self.assertEqual(self.sim.cal, {})
        with redirect_stdout(buf):
            out = cvmeter.run_cal(self.sim, 1, 1, ask=lambda p: "q")
        self.assertEqual(out[0][0], "ABORT")

    def test_calibration_all(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            out = cvmeter.run_cal(self.sim, "a", 5, ask=lambda p: "y")
        self.assertEqual([o[0] for o in out], ["SAVED"] * 8)
        self.assertEqual(sorted(self.sim.cal), list(range(8)))

    def test_print(self):
        rc, out = self.run_quiet(cvmeter.cmd_print, self.sim, args())
        self.assertEqual(rc, 0)
        self.assertEqual(out.count("INCAL,CH,"), 8)

    def test_freq(self):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), "cvmeter_test_freq.csv")
        self.sim.freq_hz = 261.6256
        rc, _ = self.run_quiet(cvmeter.cmd_freq, self.sim, args(csv=path, sim_lines=5))
        self.assertEqual(rc, 0)
        with open(path) as f:
            rows = f.read().splitlines()
        os.remove(path)
        self.assertEqual(len(rows), 6)
        self.assertIn(",C4,", rows[1])
        self.assertFalse(self.sim.freq)

    def test_main_entry_point_sim(self):
        rc, out = self.run_quiet(cvmeter.main, ["sim", "print"])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
