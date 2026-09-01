#!/usr/bin/env python3
"""Browser front end for xeno-sim.

    cd tools/xeno-sim && make && python3 xeno_gui.py
    -> http://127.0.0.1:8731/

This server owns NO simulation and NO UI logic. It spawns `build/xeno-sim
--stdio` -- the same binary the terminal mode runs, compiling the real
Bus200eApp.h, the real weegfx renderer and the real Bus200eMaster FSM -- feeds
it input tokens, and hands the 128x64 framebuffer it gets back to the page.
The browser draws those bits. Every decision about what appears on that screen
is made by firmware C++, exactly as on the module.

Stdlib only, binds 127.0.0.1 only.
"""

import argparse
import atexit
import json
import os
import signal
import subprocess
import sys
import threading
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "build", "xeno-sim")
PAGE = os.path.join(HERE, "gui.html")

# Only these reach the simulator's key handler. Anything else is dropped here
# rather than shipped to the child, so a malformed request cannot desynchronise
# the line protocol.
KEYS = set("abxyzlr[],.{}<>nNwtq1234")
WORD_KEYS = {
    "encl", "encr", "encl-", "encl+", "encr-", "encr+",
    "note", "busnote", "wait",
}
# Buttons that can be held. Seven, including z -- the grey `clock` button. The
# firmware initialises but_mid on pin 20 for this hardware and OC::Ui polls it
# as CONTROL_BUTTON_M, so the simulator drives it like any other button. Its
# binding to the physical grey button has not been confirmed on the bench; see
# README.md.
BUTTONS = {"a", "b", "x", "y", "z", "l", "r", "encl", "encr"}


class Sim:
    """The xeno-sim child process and its line protocol."""

    def __init__(self, argv):
        self.lock = threading.Lock()
        self.dead = None            # the reason, once it is gone
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        # Whatever takes this process down -- a clean ctrl-C, sys.exit, an
        # uncaught exception, SIGTERM -- takes the child with it. Before this,
        # only the ctrl-C path did, and a crash left an orphaned xeno-sim
        # holding its captures open with nothing to talk to.
        atexit.register(self.stop)
        self.greeting = self._read_block()

    def _read_block(self):
        """Read one reply: '<name> <value>' lines terminated by a bare END."""
        state = {"log": []}
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("xeno-sim exited")
            line = line.rstrip("\n")
            if line == "END":
                return state
            name, _, value = line.partition(" ")
            if name == "log":
                state["log"].append(value)
            elif name == "session":
                state.setdefault("session", []).append(value)
            elif name in ("busy", "synthetic", "overlay"):
                state[name] = value == "1"
            elif name in ("millis", "logtotal"):
                state[name] = int(value or 0)
            else:
                state[name] = value

    def command(self, line):
        with self.lock:
            if self.dead:
                raise RuntimeError(self.dead)
            if self.proc.poll() is not None:
                self.dead = "xeno-sim exited (status %s)" % self.proc.poll()
                raise RuntimeError(self.dead)
            try:
                self.proc.stdin.write(line + "\n")
                self.proc.stdin.flush()
                return self._read_block()
            except Exception as e:
                # One desynchronised reply poisons every later one, so the
                # child is written off here rather than left half-talking.
                self.dead = "xeno-sim: %s" % e
                raise

    def stop(self):
        """Take the child down. Safe to call more than once."""
        proc = getattr(self, "proc", None)
        if proc is None or proc.poll() is not None:
            return
        self.dead = self.dead or "stopping"
        try:
            proc.stdin.write("bye\n")
            proc.stdin.flush()
        except Exception:
            pass
        try:
            proc.stdin.close()      # EOF ends its read loop even if "bye" lost
        except Exception:
            pass
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()         # last resort; nothing survives this
                proc.wait(timeout=2)
            except Exception:
                pass


class Handler(BaseHTTPRequestHandler):
    server_version = "xeno-gui"
    sim = None

    def log_message(self, fmt, *args):
        pass    # the interesting log is the simulator's, shown in the page

    def handle_error(self, *args):
        # socketserver's default prints a banner and swallows nothing, but it
        # is easy to lose in a quiet terminal. Say plainly that this is the
        # server surviving a request, and print the traceback: the next crash
        # should leave evidence rather than a silent stop.
        sys.stderr.write("xeno-gui: request handler raised --\n")
        traceback.print_exc()
        sys.stderr.flush()

    def _send(self, code, ctype, body):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, "application/json", json.dumps(obj))

    def do_GET(self):
        # Nothing a single request does may take the server down with it. An
        # unexpected exception becomes a 500 with the traceback on the
        # terminal, so the page reports "lost the simulator" and the operator
        # gets something to read.
        try:
            self._get()
        except Exception:
            sys.stderr.write("xeno-gui: unhandled error serving %s --\n"
                             % self.path)
            traceback.print_exc()
            sys.stderr.flush()
            try:
                self._json({"error": "server error; see the xeno_gui.py "
                                     "terminal for the traceback"}, 500)
            except Exception:
                pass

    def _get(self):
        # A page on the open internet can resolve its own name to 127.0.0.1 and
        # then talk to whatever is listening. Requiring a loopback Host closes
        # that, and costs nothing for the intended use.
        host = self.headers.get("Host", "").rsplit(":", 1)[0].strip("[]")
        if host not in ("localhost", "127.0.0.1", "::1"):
            self._send(403, "text/plain", "loopback Host required\n")
            return

        url = urlparse(self.path)
        q = parse_qs(url.query)

        if url.path in ("/", "/index.html"):
            try:
                with open(PAGE, "rb") as f:
                    self._send(200, "text/html; charset=utf-8", f.read())
            except OSError as e:
                self._send(500, "text/plain", "cannot read gui.html: %s\n" % e)
            return

        if not url.path.startswith("/api/"):
            self._send(404, "text/plain", "not found\n")
            return

        try:
            if url.path == "/api/state":
                st = self.sim.command("state")
            elif url.path == "/api/key":
                k = (q.get("k") or [""])[0]
                if k not in KEYS and k not in WORD_KEYS:
                    self._json({"error": "unknown key %r" % k}, 400)
                    return
                st = self.sim.command("key " + k)
            elif url.path == "/api/btn":
                # The half of a press that makes a chord possible: the button
                # stays held until an "up" arrives, so event.mask can carry two
                # bits and the module's chords dispatch the way they do on the
                # panel.
                k = (q.get("k") or [""])[0]
                s = (q.get("s") or [""])[0]
                if k not in BUTTONS:
                    self._json({"error": "unknown button %r" % k}, 400)
                    return
                if s not in ("down", "up"):
                    self._json({"error": "s must be down or up"}, 400)
                    return
                st = self.sim.command("btn %s %s" % (k, s))
            elif url.path == "/api/session":
                # The recorded session, for the copy/download buttons. Reading
                # it changes nothing.
                st = self.sim.command("session")
            elif url.path == "/api/release-all":
                st = self.sim.command("release-all")
            elif url.path == "/api/enc":
                side = (q.get("side") or ["l"])[0]
                if side not in ("l", "r"):
                    self._json({"error": "side must be l or r"}, 400)
                    return
                try:
                    delta = int((q.get("d") or ["0"])[0])
                except ValueError:
                    self._json({"error": "d must be an integer"}, 400)
                    return
                st = self.sim.command("enc %s %d" % (side, max(-64, min(64, delta))))
            elif url.path == "/api/pump":
                try:
                    ms = int((q.get("ms") or ["100"])[0])
                except ValueError:
                    ms = 100
                st = self.sim.command("pump %d" % max(0, min(500, ms)))
            else:
                self._send(404, "text/plain", "not found\n")
                return
        except Exception as e:
            # Losing the child is expected enough to be a 503 rather than a
            # crash -- but the traceback still goes to the terminal, because a
            # simulator that quietly stops answering is the thing that wasted
            # an afternoon.
            sys.stderr.write("xeno-gui: simulator command failed --\n")
            traceback.print_exc()
            sys.stderr.flush()
            self._json({"error": str(e)}, 503)
            return

        self._json(st)


def main():
    ap = argparse.ArgumentParser(
        description="browser front end for xeno-sim (loopback only)")
    ap.add_argument("--port", type=int, default=8731)
    ap.add_argument("--no-open", action="store_true",
                    help="do not launch a browser")
    ap.add_argument("--real-timing", action="store_true")
    ap.add_argument("--bus-off", action="store_true")
    ap.add_argument("--capture-251e")
    ap.add_argument("--capture-259e")
    args = ap.parse_args()

    if not os.access(EXE, os.X_OK):
        sys.exit("no %s -- run `make` in %s first" % (EXE, HERE))

    argv = [EXE, "--stdio"]
    if args.real_timing:
        argv.append("--real-timing")
    if args.bus_off:
        argv.append("--bus-off")
    if args.capture_251e:
        argv += ["--capture-251e", args.capture_251e]
    if args.capture_259e:
        argv += ["--capture-259e", args.capture_259e]

    Handler.sim = Sim(argv)

    # A signal that would otherwise end the process without unwinding: turn it
    # into a normal exit so atexit runs and the child goes with us.
    def bye(signum, _frame):
        sys.stderr.write("xeno-gui: signal %d, stopping\n" % signum)
        Handler.sim.stop()
        sys.exit(0)

    for sig in (signal.SIGTERM, signal.SIGHUP):
        try:
            signal.signal(sig, bye)
        except (ValueError, OSError, AttributeError):
            pass    # not the main thread, or the platform lacks it

    ThreadingHTTPServer.daemon_threads = True
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = "http://127.0.0.1:%d/" % args.port
    print("xeno-sim GUI on %s   (127.0.0.1 only; ctrl-C to stop)" % url)
    print("SIMULATED BUS - NO HARDWARE - writes go nowhere.")
    if not args.no_open:
        threading.Timer(0.4, webbrowser.open, [url]).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        Handler.sim.stop()


if __name__ == "__main__":
    main()
