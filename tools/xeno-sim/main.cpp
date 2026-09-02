// ---------------------------------------------------------------------------
// xeno-sim: host simulator for the Xenomorpher's UI.
//
// Build:  make            (from tools/xeno-sim/)
// Run:    ./build/xeno-sim
//         ./build/xeno-sim --keys "a-down,r,a-up" --dump-frames
//
// This runs the REAL firmware. The app switcher, the preset-bus overlay, the
// Setup app, the 200e app, the UI event loop with its debounce and encoder
// acceleration, the display pipeline and the 6x8 font are all compiled from
// software/src/, unmodified, through the mirror in build/shadow (see
// mkshadow.sh and the Makefile). Only hardware is replaced.
//
// This file is the driver around that: the command line, the terminal front
// end, the --stdio protocol the browser page speaks, and session record and
// replay. It decides nothing about what any screen looks like.
// ---------------------------------------------------------------------------

#include <unistd.h>     // getppid, for the --stdio orphan guard

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <Arduino.h>
#include <SD.h>          // SimSetCardPresent: --sd-card seats one before boot

#include "OC_apps.h"
#include "OC_app_switcher.h"
#include "OC_ui.h"
#include "PresetBusUI.h"
#include "PresetEngine.h"
#include "src/drivers/display.h"

#include "sim_bus.h"
#include "sim_host.h"
#include "sim_input.h"
#include "sim_runtime.h"
#include "sim_selftest.h"
#include "sim_session.h"
#include "sim_storage.h"
#include "sim_term.h"

namespace {

// The bench captures this simulator prefers. They live in a scratchpad that
// gets reaped; --capture-251e / --capture-259e override, and if they are gone
// the simulator falls back to synthetic banks and says so on screen.
const char *kDefault251e =
    "/private/tmp/claude-501/-Users-oren-Documents-GitHub-Xenomorphicle/"
    "154efe26-0fa2-436c-88b4-7d06faf8a6af/scratchpad/"
    "capture_251e_0x5C_final_after_end_fix.hex";
const char *kDefault259e =
    "/private/tmp/claude-501/-Users-oren-Documents-GitHub-Xenomorphicle/"
    "154efe26-0fa2-436c-88b4-7d06faf8a6af/scratchpad/"
    "capture_259e_0x28_baseline.hex";

uint8_t g_next_note = 60;   // walks up so successive injections are visible

// How long a scripted tap holds the button down. It has to clear UI::Button's
// 8-bit debounce shift register (7 ticks) and stay well under kLongPressTicks
// (500), and it is what a quick real click looks like.
constexpr uint32_t kTapMs = 40;

void Advance(uint32_t ms) { SimRuntimeAdvanceMs(ms); }

// Every input enters the simulator through one of these three, and each one
// records itself, so a session can never be missing an event that was applied.
void ButtonEdge(const std::string &tok, bool down) {
  const uint16_t c = SimInputControlForToken(tok);
  if (!c) return;
  SimSessionRecord("btn " + tok + (down ? " down" : " up"));
  SimInputSetButton(c, down);
}

void EncoderDelta(bool right, int delta) {
  if (!delta) return;
  if (delta > 64) delta = 64;
  if (delta < -64) delta = -64;
  char buf[64];
  snprintf(buf, sizeof(buf), "enc %c %d", right ? 'r' : 'l', delta);
  SimSessionRecord(buf);
  SimInputEncoder(right, delta);
  // A detent is four UI ticks (see sim_input.h); let the queue drain so the
  // caller sees the turn it asked for.
  for (int i = 0; i < 4 * (delta < 0 ? -delta : delta) + 2; ++i) Advance(1);
}

void ReleaseAll() {
  SimSessionRecord("release-all");
  SimInputReleaseAll();
  Advance(kTapMs / 2);
}

void Tap(const std::string &tok) {
  ButtonEdge(tok, true);
  Advance(kTapMs);
  ButtonEdge(tok, false);
  Advance(kTapMs / 2);
}

// Run the clock until the bus goes quiet, or until `cap_ms` of simulated time
// has passed. A scan re-arms itself, so quiet is only believed after several
// consecutive idle ticks.
void Settle(uint32_t cap_ms) {
  int idle = 0;
  for (uint32_t i = 0; i < cap_ms; ++i) {
    Advance(1);
    idle = SimBusBusy() ? 0 : idle + 1;
    if (idle >= 4) return;
  }
}

std::string Caption() {
  std::string c = "XENO-SIM  SIMULATED BUS - NO HARDWARE - writes go nowhere";
  if (SimBusUsingSyntheticBanks()) c += "  [SYNTHETIC BANK DATA]";
  return c;
}

const char *kLegend =
    "a/b/x/y buttons A B X Y   z clock button (CONTROL_BUTTON_M)\n"
    "l/r encoder pushes (encL/encR)   [ ] encL turn -/+   , . encR turn -/+\n"
    "{ } < > the same, x10            1-4 pulse TR1-TR4 (225e last/next jacks)\n"
    "~1-~4 a zero-width spike on TR1-TR4, seen only by the edge latch\n"
    "export / import: the console's E and J, every slot to/from the card\n"
    "nameN=text names slot N (the panel's rename); names lists named slots\n"
    "n note-on -> USB host port 0     N note-on -> 200e bus MIDI\n"
    "w advance 1 simulated second     t toggle fast/real scan pacing   q quit\n"
    "chords need held buttons: a terminal cannot report a key being HELD, so\n"
    "use --keys \"a-down,r,a-up\" or the browser front end for those.";

// Applies one key. Returns false to quit.
//
// "<button>-down" / "<button>-up" hold and release, so a scripted run can
// express a chord ("a-down,r,a-up" opens the app menu) or a long press
// ("l-down,step600,l-up" is a STORE hold). A bare token is still a tap, so
// every existing --keys script means what it always did.
bool ApplyKey(const std::string &k) {
  if (k.empty()) return true;
  if (k == "q" || k == "quit") return false;

  {
    const size_t dash = k.rfind('-');
    if (dash != std::string::npos && dash > 0) {
      const std::string tail = k.substr(dash + 1);
      // "<button>-inN": schedule a tap N simulated ms from now and return at
      // once, without advancing the clock.
      //
      // This is the ONLY way a script can answer a screen the firmware draws
      // from inside its own blocking loop -- Ui::DebugStats(), ConfirmReset(),
      // the calibration wizard. Those never return to the simulator's driver,
      // so the token that would press the exit button is never reached; the
      // press has to already be in flight before the loop is entered. The
      // taps are consumed by SimInputTick(), which runs on the UI-poll
      // schedule inside the pump, so it fires just the same while the
      // firmware is spinning.
      if (tail.rfind("in", 0) == 0 &&
          tail.find_first_not_of("0123456789", 2) == std::string::npos &&
          tail.size() > 2) {
        const std::string btn = k.substr(0, dash);
        const uint16_t c = SimInputControlForToken(btn);
        if (c) {
          const uint32_t at = (uint32_t)strtoul(tail.c_str() + 2, nullptr, 10);
          char buf[64];
          snprintf(buf, sizeof(buf), "sched %s %u %u", btn.c_str(),
                   (unsigned)at, (unsigned)kTapMs);
          SimSessionRecord(buf);
          SimInputScheduleTap(c, at, kTapMs);
          return true;
        }
      }
      if (tail == "down" || tail == "up") {
        if (SimInputControlForToken(k.substr(0, dash))) {
          ButtonEdge(k.substr(0, dash), tail == "down");
          Advance(1);
          return true;
        }
      }
    }
  }

  if (SimInputControlForToken(k)) { Tap(k); return true; }

  if (k == "[" || k == "encl-") { EncoderDelta(false, -1); return true; }
  if (k == "]" || k == "encl+") { EncoderDelta(false, +1); return true; }
  if (k == "," || k == "encr-") { EncoderDelta(true, -1); return true; }
  if (k == "." || k == "encr+") { EncoderDelta(true, +1); return true; }
  if (k == "{") { EncoderDelta(false, -10); return true; }
  if (k == "}") { EncoderDelta(false, +10); return true; }
  if (k == "<") { EncoderDelta(true, -10); return true; }
  if (k == ">") { EncoderDelta(true, +10); return true; }

  // The 225e last/next pulse jacks. A pulse, not a level: the overlay does its
  // own edge detection and that is the code under test.
  if (k.size() == 1 && k[0] >= '1' && k[0] <= '4') {
    const int idx = k[0] - '1';
    SimSessionRecord(std::string("trig ") + k[0]);
    SimInputSetTrigger(idx, true);
    Advance(5);
    SimInputSetTrigger(idx, false);
    Advance(5);
    SimLog("TR%d pulsed", idx + 1);
    return true;
  }
  // The same jack, with a pulse no loop pass could ever sample: high and
  // low again before any time passes. Only the GPIO block's edge latch sees
  // it -- which is the point. A 225e steps on every pulse; so must this.
  if (k.size() == 2 && k[0] == '~' && k[1] >= '1' && k[1] <= '4') {
    const int idx = k[1] - '1';
    SimSessionRecord(std::string("spike ") + k[1]);
    SimInputSetTrigger(idx, true);
    SimInputSetTrigger(idx, false);
    Advance(5);
    SimLog("TR%d spiked", idx + 1);
    return true;
  }

  if (k == "n" || k == "note") {
    SimSessionRecord("note");
    usbHostMIDI[0].Push(midi::NoteOn, g_next_note, 100);
    SimLog("MIDI note %d -> usbHostMIDI[0] (the k-board port)", g_next_note);
    g_next_note = (uint8_t)(g_next_note < 84 ? g_next_note + 2 : 60);
    return true;
  }
  if (k == "N" || k == "busnote") {
    SimSessionRecord("busnote");
    SimBusInjectMidiNote(g_next_note, 100);
    SimLog("MIDI note %d -> 200e bus MIDI RX", g_next_note);
    g_next_note = (uint8_t)(g_next_note < 84 ? g_next_note + 2 : 60);
    return true;
  }
  if (k == "w" || k == "wait") { Advance(1000); return true; }
  // The bench console's E and J: every slot to the card, every slot the card
  // holds back in. The console itself is not compiled here, so the engine
  // calls are made directly and each slot's verdict is logged by name.
  if (k == "export" || k == "import") {
    const bool imp = (k == "import");
    SimSessionRecord(k);
    static const char *const verdict[] = {
      "ok", "no card", "empty", "bad slot", "bad file", "legacy", "FAILED" };
    for (uint8_t i = 0; i < OC::PresetEngine::kNumSlots; ++i) {
      const OC::PresetEngine::ExportResult r =
          imp ? OC::PresetEngine::ImportSlot(i) : OC::PresetEngine::ExportSlot(i);
      if (r == OC::PresetEngine::EXPORT_EMPTY) continue;
      SimLog("%s slot %d: %s", imp ? "import" : "export", i,
             (unsigned)r < 7 ? verdict[r] : "?");
    }
    OC::PresetEngine::FlushSlotNames();   // as the console's J does
    Advance(5);
    return true;
  }
  // The panel's rename, minus the panel: `name3=Drone` names slot 3 (the
  // store is written at once, as SetSlotName does for a real rename);
  // `names` logs every named slot so a check can prove which survived an
  // export/import round trip.
  if (k.size() > 5 && k.compare(0, 4, "name") == 0 && k.find('=') != std::string::npos) {
    const size_t eq = k.find('=');
    const int slot = atoi(k.substr(4, eq - 4).c_str());
    const std::string nm = k.substr(eq + 1);
    SimSessionRecord(k);
    OC::PresetEngine::SetSlotName((uint8_t)slot, nm.c_str());
    SimLog("slot %d named \"%s\"", slot, OC::PresetEngine::SlotName((uint8_t)slot));
    return true;
  }
  if (k == "names") {
    for (uint8_t i = 0; i < OC::PresetEngine::kNumSlots; ++i) {
      const char *nm = OC::PresetEngine::SlotName(i);
      if (*nm) SimLog("name slot %d: \"%s\"", i, nm);
    }
    return true;
  }
  if (k == "t") {
    SimBusSetRealTiming(!SimBusRealTiming());
    SimSessionRecord(std::string("timing ") +
                     (SimBusRealTiming() ? "real" : "fast"));
    SimLog("scan pacing: %s", SimBusRealTiming() ? "real" : "fast");
    return true;
  }
  return true;   // unmapped keys are inert
}

// One refresh's worth of simulated time, with the interactive loop's pacing
// rule: fast pacing compresses only the WAITING, so a scan still redraws.
void Pump(uint32_t budget_ms) {
  if (budget_ms > 500) budget_ms = 500;
  const uint32_t steps =
      (!SimBusRealTiming() && SimBusBusy()) ? 1000 : budget_ms;
  Advance(steps);
}

std::vector<std::string> SplitKeys(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

// The panel as the eye sees it -- the bytes the OLED was last sent, with its
// invert command applied. Nothing here draws: the frame was assembled by the
// firmware's own renderer and handed over page by page.
const uint8_t *VisibleFrame() {
  static uint8_t buf[1024];
  SimPanelVisible(buf);
  return buf;
}

// --full-log: every retained log line under the frame, not the last four.
// A check that greps the --keys output for a line the firmware printed
// EARLY in the script (a save that a later recall then scrolled off) sees
// nothing with the four-line window, and reads that as the save not having
// happened.
static bool g_full_log = false;

void PrintScreen(bool with_chrome) {
  fputs(SimRenderFrame(VisibleFrame(), Caption()).c_str(), stdout);
  if (!with_chrome) return;
  printf("  %s\n", SimRuntimeStatusLine().c_str());
  printf("  %s\n", SimBusStatusLine().c_str());
  const auto &log = SimLogLines();
  const size_t from = (g_full_log || log.size() <= 4) ? 0 : log.size() - 4;
  for (size_t i = from; i < log.size(); ++i) printf("  %s\n", log[i].c_str());
}

// The console's own framebuffer-capture format, byte for byte: 1024 bytes as
// 2048 uppercase hex digits on one line. Main.cpp:1346-1366 prints exactly
// this when an unmapped console key asks for a capture, so a device capture
// and a simulator capture are directly comparable -- see fbdiff.py.
void PrintFrameBufferHex() {
  // --snap-at wins when it fired: the whole point of a deferred capture is
  // that the frame you want is NOT the one on screen at exit. If the capture
  // never fired, fall through to the live frame rather than printing a blank
  // one -- an empty 1024 bytes would decode as an empty screen and read as a
  // layout assertion passing on nothing at all.
  if (SimSnapTaken()) {
    const uint8_t *s = SimSnapBytes();
    for (int i = 0; i < 1024; ++i) printf("%02X", s[i]);
    printf("\n");
    return;
  }
  const uint8_t *f = VisibleFrame();
  for (int i = 0; i < 1024; ++i) printf("%02X", f[i]);
  printf("\n");
}

// ---------------------------------------------------------------------------
// --stdio: a line protocol on stdin/stdout, so a front-end can drive this same
// binary without one line of UI logic leaving C++. xeno_gui.py speaks it.
//
// Requests are one line:
//     key <token>      one of the interactive keys, or a word alias
//     btn <tok> down   hold a button (a b x y z l r) -- it stays held
//     btn <tok> up     release it
//     release-all      release everything (page unload, window blur)
//     enc <l|r> <n>    encoder turn by an arbitrary signed delta
//     trig <1-4>       pulse a trigger input
//     pump <ms>        advance the clock by one refresh, interactive pacing
//     state            report without changing anything
//     session          report, plus the recorded session so far
//     bye              exit
//
// `key` is a tap: down, kTapMs, up. `btn` is the half of it that makes a chord
// possible, and the front end sends it from keydown/keyup and from its
// latches. `state`'s "held" line reports what is held, so a forgotten latch is
// visible rather than mysterious.
//
// Every reply is a block of "<name> <value>" lines ended by a line "END". The
// frame comes back as 1024 bytes of the real page-packed framebuffer, hex --
// the same bytes the panel was sent. The front end draws those bits and
// decides nothing.
// ---------------------------------------------------------------------------

// Log and status text lands on its own protocol line, so anything that could
// break the framing is flattened to a space.
std::string Flatten(const std::string &s) {
  std::string out = s;
  for (char &c : out)
    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7f) c = ' ';
  return out;
}

void EmitState(bool with_session) {
  const uint8_t *f = VisibleFrame();
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(2048);
  for (int i = 0; i < 1024; ++i) {
    hex += kHex[f[i] >> 4];
    hex += kHex[f[i] & 0x0f];
  }
  printf("frame %s\n", hex.c_str());
  printf("caption %s\n", Flatten(Caption()).c_str());
  printf("status %s\n", Flatten(SimBusStatusLine()).c_str());
  printf("millis %lu\n", (unsigned long)SimNowMs());
  printf("busy %d\n", SimBusBusy() ? 1 : 0);
  printf("held %s\n", SimInputHeldTokens().c_str());
  printf("overlay %d\n", OC::PresetBusUI::Active() ? 1 : 0);
  printf("screen %s\n", SimRuntimeScreen());
  printf("app %s\n", SimRuntimeAppName());
  printf("timing %s\n", SimBusRealTiming() ? "real" : "fast");
  printf("synthetic %d\n", SimBusUsingSyntheticBanks() ? 1 : 0);

  const auto &log = SimLogLines();
  printf("logtotal %lu\n", (unsigned long)log.size());
  const size_t from = log.size() > 120 ? log.size() - 120 : 0;
  for (size_t i = from; i < log.size(); ++i)
    printf("log %s\n", Flatten(log[i]).c_str());

  if (with_session) {
    const std::string s = SimSessionText();
    size_t start = 0;
    while (start < s.size()) {
      const size_t nl = s.find('\n', start);
      const size_t end = (nl == std::string::npos) ? s.size() : nl;
      printf("session %s\n", s.substr(start, end - start).c_str());
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }

  fputs("END\n", stdout);
  fflush(stdout);
}

int RunStdio() {
  char buf[512];
  // Orphan guard. Normally the front end's death closes this pipe and fgets
  // returns EOF, which is enough -- but "normally" is doing a lot of work in
  // that sentence, and an orphaned simulator holding its captures open with
  // nothing to talk to has been seen. If our parent is gone, so are we.
  const pid_t parent = getppid();

  EmitState(false);                  // greet with the boot screen
  while (fgets(buf, sizeof(buf), stdin)) {
    if (getppid() != parent) {
      fprintf(stderr, "xeno-sim: parent %d is gone, exiting\n", (int)parent);
      return 0;
    }
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();

    std::vector<std::string> tok;
    {
      std::string cur;
      for (char c : line) {
        if (c == ' ') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
        else cur += c;
      }
      if (!cur.empty()) tok.push_back(cur);
    }
    if (tok.empty()) { EmitState(false); continue; }

    bool with_session = false;
    if (tok[0] == "bye") break;
    if (tok[0] == "key" && tok.size() >= 2) {
      // "q" quits the terminal build; here there is nothing to quit, so it is
      // inert rather than a way for a stray click to kill the server.
      if (tok[1] != "q") ApplyKey(tok[1]);
    } else if (tok[0] == "btn" && tok.size() >= 3) {
      ButtonEdge(tok[1], tok[2] == "down");
      Advance(1);
    } else if (tok[0] == "release-all") {
      ReleaseAll();
    } else if (tok[0] == "enc" && tok.size() >= 3) {
      EncoderDelta(tok[1] == "r", (int)strtol(tok[2].c_str(), nullptr, 10));
    } else if (tok[0] == "trig" && tok.size() >= 2) {
      ApplyKey(tok[1]);
    } else if (tok[0] == "pump" && tok.size() >= 2) {
      Pump((uint32_t)strtoul(tok[1].c_str(), nullptr, 10));
    } else if (tok[0] == "session") {
      with_session = true;
    }
    // "state", and anything unrecognised, just re-report.
    EmitState(with_session);
  }
  return 0;
}

// Names for --write-fault. Returns <0 for an unknown one so a typo cannot
// silently degrade into "faithful" and make a broken write look verified.
int ParseWriteFault(const char *s) {
  const std::string a = s ? s : "";
  if (a == "none" || a == "faithful") return SIM_WRITE_FAITHFUL;
  if (a == "ignore")     return SIM_WRITE_IGNORE;
  if (a == "drop-tail")  return SIM_WRITE_DROP_TAIL;
  if (a == "flip-first") return SIM_WRITE_FLIP_FIRST;
  if (a == "flip-last")  return SIM_WRITE_FLIP_LAST;
  if (a == "short-readback") return SIM_WRITE_SHORT_READBACK;
  return -1;
}

void Usage() {
  printf(
      "xeno-sim -- host simulator for the Xenomorpher's UI\n"
      "\n"
      "  --keys \"a-down,r,a-up\"  apply a key sequence, print the result, exit\n"
      "  --dump-frames        with --keys: print the screen after every key\n"
      "  --dump-fb            print the framebuffer in the console's own\n"
      "                       capture format (2048 uppercase hex digits) and\n"
      "                       nothing else -- compare with fbdiff.py\n"
      "  --test-phzconfig     run the PhzConfig codec checks (serialize,\n"
      "                       load/save, damage) against the RAM volume and\n"
      "                       exit -- no boot; used by selfcheck.sh\n"
      "  --stdio              line protocol on stdin/stdout, for the browser\n"
      "                       front end -- see xeno_gui.py\n"
      "  --record FILE        write the session to FILE on exit\n"
      "  --replay FILE        replay a recorded session, then print the screen\n"
      "  --at N               with --replay: stop after event N\n"
      "  --frames             with --replay: print the screen after each event\n"
      "  --app NAME           boot into an app by name (default: as stored)\n"
      "  --real-timing        pin the virtual clock to wall-clock, so a scan\n"
      "                       takes the ~60s it takes on the module\n"
      "  --bus-off            simulate PresetBus::Enabled() == false\n"
      "  --id-voltage V       the hardware-ID divider the firmware reads at\n"
      "                       boot to pick its pin map (default 0.10)\n"
      "  --reset-settings     boot the first-run/EEPROM-reset path, and\n"
      "                       answer its ConfirmReset prompt OK (erase)\n"
      "  --reset-cancel       the same gesture, answered CANCEL. What a user\n"
      "                       who changed their mind does -- and the only way\n"
      "                       to ask whether a refused reset left storage alone\n"
      "  --snap-at MS         capture the first complete frame drawn at or\n"
      "                       after MS (ABSOLUTE simulated time, so it can\n"
      "                       reach screens drawn during boot itself; inside a\n"
      "                       script prefer the relative \"snapN\" token) and\n"
      "                       let --dump-fb print THAT. The only\n"
      "                       way to see a screen the firmware draws from its\n"
      "                       own blocking loop (DebugStats, ConfirmReset, the\n"
      "                       calibration wizard), which has been redrawn over\n"
      "                       by the time the script gets another turn.\n"
      "  --sd-card            seat an SD card, so SDcard_Ready is true. The\n"
      "                       card is empty unless --state seeds it.\n"
      "  --state FILE         non-volatile memory: read FILE before boot and\n"
      "                       write it back at exit. Two runs sharing one FILE\n"
      "                       are a power cycle -- the only way to ask what a\n"
      "                       SECOND boot sees.\n"
      "  --state-in FILE      read only (leave the image untouched)\n"
      "  --state-out FILE     write only\n"
      "  --dump-fs            list stored files with sizes and CRCs, and\n"
      "                       nothing else\n"
      "  --capture-251e PATH  251e bank hex dump (default: bench capture)\n"
      "  --capture-259e PATH  259e bank hex dump (default: bench capture)\n"
      "  --no-log             omit the status/log lines under the frame\n"
      "  --full-log           every retained log line under the frame, not 4\n"
      "  --write-fault WHAT   make the simulated modules mishandle a RESTORE,\n"
      "                       to exercise the firmware's post-write read-back:\n"
      "                       none | ignore | drop-tail | flip-first |\n"
      "                       flip-last | short-readback\n"
      "                       (default: none, i.e. it works). short-readback\n"
      "                       stores perfectly and truncates the READ-BACK,\n"
      "                       which is the one shape that can fake a VERIFY.\n"
      "  --write-fault-once   apply that fault to the FIRST restore only, so\n"
      "                       the recovery write after it goes out to a module\n"
      "                       that behaves -- which is what makes the pre-write\n"
      "                       snapshot's UNDO checkable end to end.\n"
      "  --help\n"
      "\nKeys:\n%s\n"
      "\nA --keys token may be \"<button>-down\" / \"<button>-up\" (buttons are\n"
      "a b x y z l r), which is how a chord or a long press is scripted:\n"
      "  --keys \"l-down,r-down,step40,l-up,r-up\"  both encoders -> preset bus\n"
      "  --keys \"a-down,r,a-up\"                   A + encR      -> app menu\n"
      "  --keys \"l-down,step600,l-up\"             a 500ms hold  -> STORE\n"
      "and \"stepN\" advances exactly N simulated ms.\n"
      "\n\"<button>-inN\" schedules a tap N ms ahead and returns immediately --\n"
      "the only way to answer a screen the firmware draws from its own\n"
      "blocking loop (DebugStats), which never gives the script another turn:\n"
      "  --keys \"a-down,r,a-up,r-in400,l,step900\"  in and out of debug stats\n"
      "\nThe simulated bus is 0x20 \"210\", 0x28 \"259 A\", 0x5C \"251 A\".\n"
      "NOTHING here touches real hardware. Writes are discarded.\n",
      kLegend);
}

// One scripted token: "stepN" advances exactly N simulated ms, a "+" prefix
// applies the key without settling the bus afterwards, everything else is a
// key. Shared by --keys and --replay so the two can never diverge.
bool ApplyToken(const std::string &tok, bool allow_settle) {
  if (tok.rfind("step", 0) == 0) {
    Advance((uint32_t)strtoul(tok.c_str() + 4, nullptr, 10));
    return true;
  }
  // "qencl+N@T" / "qencr-N@T": queue N detents on an encoder to be delivered
  // T ms from now, and return WITHOUT advancing the clock. "@T" is optional
  // and defaults to immediately.
  //
  // The ordinary turn tokens drain themselves (EncoderDelta advances four ms
  // per detent so the caller sees the turn it asked for), which means that
  // inside a blocking firmware loop they are unusable -- the loop never gives
  // the script another turn, so a turn issued afterwards arrives after the
  // loop has ended. This is the encoder's "<button>-inN": queue it before,
  // consume it during. It is what makes the debug-stats pages past the first
  // reachable at all, since paging there is an encL TURN while an encL PRESS
  // is the exit.
  //
  // The "@T" delay is not optional in practice for that case: detents queued
  // for delivery NOW are consumed by whatever is still on screen while the
  // entry gesture is being held -- the app menu takes them as list scrolling
  // -- and never reach the loop at all. T must put them past the long-press
  // threshold.
  if (tok.rfind("qenc", 0) == 0 && tok.size() > 5) {
    const bool right = tok[4] == 'r';
    const size_t at = tok.find('@');
    const int n = (int)strtol(tok.c_str() + 5, nullptr, 10);
    const uint32_t when = (at == std::string::npos)
                              ? 0
                              : (uint32_t)strtoul(tok.c_str() + at + 1, nullptr, 10);
    if ((tok[4] == 'l' || right) && n) {
      char buf[64];
      snprintf(buf, sizeof(buf), "sched-enc %c %d %u", right ? 'r' : 'l', n,
               (unsigned)when);
      SimSessionRecord(buf);
      SimInputScheduleEncoder(right, n, when);
      return true;
    }
  }
  // "snapN": arm a deferred frame capture N ms from now, and return at once.
  //
  // Relative, like "<button>-inN" and unlike --snap-at, and for the same
  // reason: the interesting moment is always "N ms into the loop I am about to
  // enter", and the absolute clock at that point depends on how long boot took
  // -- a number that moves whenever the splash delay does. A capture armed in
  // absolute time would then quietly sample the wrong screen, and a layout
  // check that passes against the wrong screen is worse than no check.
  if (tok.rfind("snap", 0) == 0 &&
      tok.find_first_not_of("0123456789", 4) == std::string::npos &&
      tok.size() > 4) {
    SimSnapArm(SimNowMs() + (uint32_t)strtoul(tok.c_str() + 4, nullptr, 10));
    return true;
  }
  const bool settle = allow_settle && (tok.empty() || tok[0] != '+');
  const std::string k = settle ? tok : tok.substr(tok.empty() || tok[0] != '+' ? 0 : 1);
  if (!ApplyKey(k)) return false;
  if (settle) Settle(120000);   // a full 61-address scan is ~61 simulated sec
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  SimBusConfig cfg;
  cfg.capture_251e = kDefault251e;
  cfg.capture_259e = kDefault259e;

  std::string keys, record_path, replay_path, boot_app;
  std::string state_in, state_out;
  bool scripted = false, dump_frames = false, show_log = true;
  bool stdio_mode = false, dump_fb = false, reset_settings = false;
  bool card_present = false, dump_fs = false, reset_cancel = false;
  long snap_at = -1;
  bool write_fault_once = false;
  long replay_at = -1;
  int write_fault = SIM_WRITE_FAITHFUL;
  float id_voltage = 0.10f;
  std::vector<std::string> opts;   // what a replay needs to reproduce this run

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1 < argc);
    if (a == "--help" || a == "-h") { Usage(); return 0; }
    else if (a == "--test-phzconfig") return SimPhzConfigSelfTest();
    else if (a == "--keys" && has_next) { keys = argv[++i]; scripted = true; }
    else if (a == "--dump-frames") dump_frames = true;
    else if (a == "--dump-fb") { dump_fb = true; show_log = false; }
    else if (a == "--stdio") stdio_mode = true;
    else if (a == "--record" && has_next) record_path = argv[++i];
    else if (a == "--replay" && has_next) replay_path = argv[++i];
    else if (a == "--at" && has_next) replay_at = strtol(argv[++i], nullptr, 10);
    else if (a == "--frames") dump_frames = true;
    else if (a == "--app" && has_next) { boot_app = argv[++i]; opts.push_back(a + " " + boot_app); }
    else if (a == "--real-timing") { cfg.real_timing = true; opts.push_back(a); }
    else if (a == "--bus-off") { cfg.bus_enabled = false; opts.push_back(a); }
    else if (a == "--reset-settings") { reset_settings = true; opts.push_back(a); }
    else if (a == "--sd-card") { card_present = true; opts.push_back(a); }
    else if (a == "--reset-cancel") { reset_settings = reset_cancel = true; opts.push_back(a); }
    else if (a == "--snap-at" && has_next) { snap_at = strtol(argv[++i], nullptr, 10); }
    else if (a == "--write-fault-once") { write_fault_once = true; opts.push_back(a); }
    else if (a == "--state" && has_next) { state_in = state_out = argv[++i]; }
    else if (a == "--state-in" && has_next) { state_in = argv[++i]; }
    else if (a == "--state-out" && has_next) { state_out = argv[++i]; }
    else if (a == "--dump-fs") { dump_fs = true; show_log = false; }
    else if (a == "--id-voltage" && has_next) {
      id_voltage = strtof(argv[++i], nullptr);
      opts.push_back(a + " " + argv[i]);
    }
    else if (a == "--no-log") show_log = false;
    else if (a == "--full-log") g_full_log = true;
    else if (a == "--capture-251e" && has_next) { cfg.capture_251e = argv[++i]; opts.push_back(a + " " + cfg.capture_251e); }
    else if (a == "--capture-259e" && has_next) { cfg.capture_259e = argv[++i]; opts.push_back(a + " " + cfg.capture_259e); }
    else if (a == "--write-fault" && has_next) {
      write_fault = ParseWriteFault(argv[++i]);
      if (write_fault < 0) { fprintf(stderr, "unknown --write-fault: %s\n", argv[i]); return 2; }
      opts.push_back(a + " " + argv[i]);
    }
    else { fprintf(stderr, "unknown option: %s\n", a.c_str()); Usage(); return 2; }
  }

  // A replay reproduces the run it was recorded from, so its own options win.
  SimSessionFile session;
  if (!replay_path.empty()) {
    session = SimSessionLoad(replay_path.c_str());
    if (!session.error.empty()) {
      fprintf(stderr, "xeno-sim: %s\n", session.error.c_str());
      return 2;
    }
    for (const std::string &o : session.opts) {
      if (o == "--sd-card") card_present = true;
      else if (o == "--reset-cancel") reset_settings = reset_cancel = true;
      else if (o == "--write-fault-once") write_fault_once = true;
      else if (o == "--bus-off") cfg.bus_enabled = false;
      else if (o == "--real-timing") cfg.real_timing = true;
      else if (o == "--reset-settings") reset_settings = true;
      else if (o.rfind("--id-voltage ", 0) == 0) id_voltage = strtof(o.c_str() + 13, nullptr);
      else if (o.rfind("--app ", 0) == 0) boot_app = o.substr(6);
      else if (o.rfind("--capture-251e ", 0) == 0) cfg.capture_251e = o.substr(15);
      else if (o.rfind("--capture-259e ", 0) == 0) cfg.capture_259e = o.substr(15);
      else if (o.rfind("--write-fault ", 0) == 0) write_fault = ParseWriteFault(o.substr(14).c_str());
    }
  }

  SimHostReset();
  SimSetIdVoltage(id_voltage);

  // Order matters and is not arbitrary: SimHostReset() zeroes the EEPROM and
  // the volumes, so the stored image has to be laid down after it and before
  // SimRuntimeBoot() reads any of it. Seating the card is the same boundary --
  // the firmware latches SDcard_Ready once, during boot.
  SimSetCardPresent(card_present);
  if (snap_at >= 0) SimSnapArm((uint32_t)snap_at);
  if (!state_in.empty()) {
    std::string why;
    if (SimStorageLoad(state_in, &why))
      SimLog("--state-in %s: stored state restored", state_in.c_str());
    else
      SimLog("--state-in %s: %s", state_in.c_str(), why.c_str());
  }
  SimBusSetWriteFault((SimWriteFault)(write_fault < 0 ? 0 : write_fault));
  SimBusSetWriteFaultOnce(write_fault_once);

  usbMIDI.set_name("usbMIDI");
  usbHostMIDI[0].set_name("usbHostMIDI[0]");
  usbHostMIDI[1].set_name("usbHostMIDI[1]");
  MIDI1.set_name("MIDI1");

  SimBusInit(cfg);
  if (scripted && cfg.real_timing)
    SimLog("--real-timing has no effect with --keys: scripted mode always "
           "runs the virtual clock flat out.");
  if (!cfg.bus_enabled) SimLog("--bus-off: PresetBus::Enabled() reports false.");

  SimRuntimeBoot(reset_settings, reset_cancel);

  if (!boot_app.empty()) {
    bool found = false;
    for (size_t i = 0; i < OC::SimAppCount(); ++i) {
      if (boot_app == OC::SimAppNameAt(i)) {
        OC::SwitchToApp(i);
        found = true;
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "xeno-sim: no app named \"%s\". Available:\n", boot_app.c_str());
      for (size_t i = 0; i < OC::SimAppCount(); ++i)
        fprintf(stderr, "  %s\n", OC::SimAppNameAt(i));
      return 2;
    }
  }

  Settle(50);
  // Recording starts after boot, so a session is the user's inputs and not the
  // boot sequence -- which a replay reproduces from the options instead.
  SimSessionStart(opts);

  int rc = 0;
  if (!replay_path.empty()) {
    long idx = 0;
    for (const auto &ev : session.events) {
      Advance(ev.first);
      if (ev.second == "end") break;
      // A recorded line is exactly a --stdio request, minus the frame.
      std::vector<std::string> t;
      std::string cur;
      for (char c : ev.second) {
        if (c == ' ') { if (!cur.empty()) { t.push_back(cur); cur.clear(); } }
        else cur += c;
      }
      if (!cur.empty()) t.push_back(cur);
      if (t.empty()) continue;

      if (t[0] == "btn" && t.size() >= 3) { ButtonEdge(t[1], t[2] == "down"); }
      else if (t[0] == "sched" && t.size() >= 4) {
        const uint16_t c = SimInputControlForToken(t[1]);
        if (c)
          SimInputScheduleTap(c, (uint32_t)strtoul(t[2].c_str(), nullptr, 10),
                              (uint32_t)strtoul(t[3].c_str(), nullptr, 10));
      }
      else if (t[0] == "sched-enc" && t.size() >= 4) {
        SimInputScheduleEncoder(t[1] == "r", (int)strtol(t[2].c_str(), nullptr, 10),
                                (uint32_t)strtoul(t[3].c_str(), nullptr, 10));
      }
      else if (t[0] == "release-all") { SimInputReleaseAll(); }
      else if (t[0] == "enc" && t.size() >= 3) {
        SimInputEncoder(t[1] == "r", (int)strtol(t[2].c_str(), nullptr, 10));
      }
      else if (t[0] == "trig" && t.size() >= 2) {
        const int n = atoi(t[1].c_str()) - 1;
        SimInputSetTrigger(n, true); Advance(5);
        SimInputSetTrigger(n, false);
      }
      else if (t[0] == "note") { usbHostMIDI[0].Push(midi::NoteOn, g_next_note, 100); }
      else if (t[0] == "busnote") { SimBusInjectMidiNote(g_next_note, 100); }
      else if (t[0] == "timing" && t.size() >= 2) { SimBusSetRealTiming(t[1] == "real"); }

      ++idx;
      if (dump_frames) {
        printf("--- after event %ld: %s (t=%ums) ---\n", idx, ev.second.c_str(), SimNowMs());
        PrintScreen(show_log);
      }
      if (replay_at >= 0 && idx >= replay_at) break;
    }
    if (dump_fs) SimStorageList(stdout);
    else if (dump_fb) PrintFrameBufferHex();
    else if (!dump_frames) PrintScreen(show_log);
  } else if (stdio_mode) {
    rc = RunStdio();
  } else if (scripted) {
    if (dump_frames) { printf("--- initial ---\n"); PrintScreen(show_log); }
    for (const std::string &tok : SplitKeys(keys)) {
      if (!ApplyToken(tok, true)) break;
      if (dump_frames) {
        printf("--- after '%s' (t=%ums) ---\n", tok.c_str(), SimNowMs());
        PrintScreen(show_log);
      }
    }
    if (dump_fs) SimStorageList(stdout);
    else if (dump_fb) PrintFrameBufferHex();
    else if (!dump_frames) PrintScreen(show_log);
  } else if (dump_fs) {
    SimStorageList(stdout);
  } else if (dump_fb) {
    PrintFrameBufferHex();
  } else {
    if (!SimTermRawMode(true)) {
      fprintf(stderr, "stdin is not a terminal; use --keys for scripted mode.\n");
      return 2;
    }
    uint64_t last_wall = SimWallMs();
    bool running = true;
    while (running) {
      printf("\033[H\033[2J");          // home + clear
      PrintScreen(show_log);
      printf("\n%s\n", kLegend);
      fflush(stdout);

      const int key = SimTermReadKey(SimBusBusy() ? 20 : 120);
      if (key) {
        running = ApplyKey(std::string(1, (char)key));
        if (!running) break;
      }

      // The ONE wall-clock read in the whole simulator, and it only ever
      // decides how much simulated time to advance -- nothing the firmware can
      // observe. Fast pacing compresses only the WAITING: while the bus is
      // busy the clock runs 1 simulated second per refresh, so a progressive
      // scan still redraws and stays abortable.
      const uint64_t now_wall = SimWallMs();
      uint32_t budget = (uint32_t)(now_wall - last_wall);
      last_wall = now_wall;
      if (budget > 500) budget = 500;
      Advance((!SimBusRealTiming() && SimBusBusy()) ? 1000 : budget);
    }
    SimTermRawMode(false);
    printf("\n");
  }

  // The power cycle's other half. Written on every exit path that gets here,
  // so a run that changed nothing still leaves an image -- "the stored set did
  // not change" has to be expressible as two identical files, which means the
  // second run must produce a file at all.
  if (!state_out.empty() && !SimStorageSave(state_out))
    fprintf(stderr, "xeno-sim: cannot write %s\n", state_out.c_str());

  if (!record_path.empty()) {
    FILE *fp = fopen(record_path.c_str(), "w");
    if (!fp) {
      fprintf(stderr, "xeno-sim: cannot write %s\n", record_path.c_str());
      return 2;
    }
    const std::string s = SimSessionText();
    fwrite(s.data(), 1, s.size(), fp);
    fclose(fp);
    fprintf(stderr, "xeno-sim: session written to %s\n", record_path.c_str());
  }
  return rc;
}
