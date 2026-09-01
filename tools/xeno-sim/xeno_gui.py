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
import json
import os
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "build", "xeno-sim")
PAGE = os.path.join(HERE, "gui.html")

# Only these reach the simulator's key handler. Anything else is dropped here
# rather than shipped to the child, so a malformed request cannot desynchronise
# the line protocol.
KEYS = set("abxylr[],.{}<>nNwtq")
WORD_KEYS = {
    "encl", "encr", "encl-", "encl+", "encr-", "encr+",
    "note", "busnote", "wait",
}


class Sim:
    """The xeno-sim child process and its line protocol."""

    def __init__(self, argv):
        self.lock = threading.Lock()
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
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
            elif name in ("busy", "synthetic"):
                state[name] = value == "1"
            elif name in ("millis", "logtotal"):
                state[name] = int(value or 0)
            else:
                state[name] = value

    def command(self, line):
        with self.lock:
            if self.proc.poll() is not None:
                raise RuntimeError("xeno-sim exited")
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
            return self._read_block()

    def stop(self):
        try:
            self.command("bye")
        except Exception:
            pass
        try:
            self.proc.terminate()
        except Exception:
            pass


class Handler(BaseHTTPRequestHandler):
    server_version = "xeno-gui"
    sim = None

    def log_message(self, fmt, *args):
        pass    # the interesting log is the simulator's, shown in the page

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
