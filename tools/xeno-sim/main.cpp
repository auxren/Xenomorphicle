// ---------------------------------------------------------------------------
// xeno-sim: host simulator for the Xenomorpher's "200e Modules" app.
//
// Build:  make            (from tools/xeno-sim/)
// Run:    ./build/xeno-sim
//         ./build/xeno-sim --keys "r,x,x,a" --dump-frames
//
// This runs the REAL app. src/apps/Bus200eApp.h is compiled unmodified behind
// the shims in shim/oc_shim.h, it draws through the REAL weegfx renderer and
// the real 6x8 font, and its bus work goes through the REAL Bus200eMaster FSM.
// Only the hardware itself is fake: see README.md for exactly where the
// simulation stops being faithful.
// ---------------------------------------------------------------------------

#include "shim/oc_shim.h"   // must precede the app: defines its whole environment

#include "apps/Bus200eApp.h"

#include <unistd.h>     // getppid, for the --stdio orphan guard

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "PresetBusUI.h"
#include "sim_bus.h"
#include "sim_preset.h"
#include "sim_term.h"
#include "sim_ui.h"

namespace {

AppBus200e g_app;

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

// One simulated millisecond: the ISR path, the bus, then the main loop --
// the same three contexts the firmware runs these in. OC::Ui::Poll() is the
// first of those: on the module it is a 1kHz timer interrupt, and running it
// once per simulated ms is what makes kLongPressTicks (500) mean 500 ms here
// too.
void Tick(uint32_t dt_ms) {
  SimAdvanceMs(dt_ms);
  SimUiPoll();              // ISR: buttons -> events, with the held mask
  g_app.Controller();
  SimBusTask();
  SimPresetTask();
  OC::PresetBusUI::Task();  // real: times the overlay's STORE/RECALL holds
  SimUiDispatch(&g_app);    // loop: the queue -> chords, overlay, app
  g_app.Loop();
}

// A press and release of one button, the way a click or a one-shot key token
// means it. Two ticks, because the state machine needs to see both edges --
// and because that is what the hardware does too.
void Tap(uint16_t control) {
  SimUiSetButton(control, true);
  Tick(1);
  SimUiSetButton(control, false);
  Tick(1);
}

void Encoder(uint16_t control, int value) {
  SimUiEncoder(control, value);
  Tick(1);
}

// Run the clock until the bus goes quiet, or until `cap_ms` of simulated time
// has passed. A scan re-arms itself inside Loop(), so quiet is only believed
// after several consecutive idle ticks.
void Settle(uint32_t cap_ms) {
  int idle = 0;
  for (uint32_t i = 0; i < cap_ms; ++i) {
    Tick(1);
    idle = SimBusBusy() ? 0 : idle + 1;
    if (idle >= 4) return;
  }
}

// The simulator's stand-in for the two screens it has no registry to draw.
// Marked as a stand-in on the glass itself: the whole promise of this thing is
// that what you see is firmware output, so the one place it is not has to say
// so where you are looking.
void DrawStandIn(const char *what, const char *why) {
  graphics.drawFrame(0, 0, 128, 64);
  graphics.setPrintPos(4, 4);
  graphics.print("SIMULATOR STAND-IN");
  graphics.drawHLine(1, 14, 126);
  graphics.setPrintPos(4, 20);
  graphics.print(what);
  graphics.setPrintPos(4, 34);
  graphics.print(why);
  graphics.setPrintPos(4, 52);
  graphics.print("any button = back");
}

void Redraw() {
  graphics.Begin(SimFrameBuffer(), weegfx::CLEAR_FRAME_ENABLE);
  if (OC::PresetBusUI::Active()) {
    // The real overlay, compiled from src/PresetBusUI.cpp. See shim/fw/.
    OC::PresetBusUI::Draw();
  } else if (SimUiMode() == OC::UI_MODE_APP_SETTINGS) {
    DrawStandIn("app menu opened", "no app registry here");
  } else if (SimUiMode() == OC::UI_MODE_SCREENSAVER) {
    DrawStandIn("screensaver", "no screensaver here");
  } else {
    g_app.DrawMenu();
  }
  graphics.End();
}

std::string Caption() {
  std::string c = "XENO-SIM  SIMULATED BUS - NO HARDWARE - writes go nowhere";
  if (SimBusUsingSyntheticBanks()) c += "  [SYNTHETIC BANK DATA]";
  return c;
}

const char *kLegend =
    "a/b/x/y buttons A B X Y   l/r encoder pushes (encL/encR)\n"
    "[ ] encL turn -/+   , . encR turn -/+   { } < > same, x10\n"
    "n note-on -> USB host port 0    N note-on -> 200e bus MIDI\n"
    "w advance 1 simulated second    t toggle fast/real scan pacing    q quit\n"
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
      if (tail == "down" || tail == "up") {
        const uint16_t c = SimUiControlForToken(k.substr(0, dash));
        if (c) {
          SimUiSetButton(c, tail == "down");
          Tick(1);
          return true;
        }
      }
    }
  }

  {
    const uint16_t c = SimUiControlForToken(k);
    if (c) { Tap(c); return true; }
  }

  if (k == "[" || k == "encl-") { Encoder(OC::CONTROL_ENCODER_L, -1); return true; }
  if (k == "]" || k == "encl+") { Encoder(OC::CONTROL_ENCODER_L, +1); return true; }
  if (k == "," || k == "encr-") { Encoder(OC::CONTROL_ENCODER_R, -1); return true; }
  if (k == "." || k == "encr+") { Encoder(OC::CONTROL_ENCODER_R, +1); return true; }
  if (k == "{") { Encoder(OC::CONTROL_ENCODER_L, -10); return true; }
  if (k == "}") { Encoder(OC::CONTROL_ENCODER_L, +10); return true; }
  if (k == "<") { Encoder(OC::CONTROL_ENCODER_R, -10); return true; }
  if (k == ">") { Encoder(OC::CONTROL_ENCODER_R, +10); return true; }

  if (k == "n" || k == "note") {
    usbHostMIDI[0].Push(HEM_MIDI_NOTE_ON, g_next_note, 100);
    SimLog("MIDI note %d -> usbHostMIDI[0] (the k-board port)", g_next_note);
    g_next_note = (uint8_t)(g_next_note < 84 ? g_next_note + 2 : 60);
    return true;
  }
  if (k == "N" || k == "busnote") {
    SimBusInjectMidiNote(g_next_note, 100);
    SimLog("MIDI note %d -> 200e bus MIDI RX", g_next_note);
    g_next_note = (uint8_t)(g_next_note < 84 ? g_next_note + 2 : 60);
    return true;
  }
  if (k == "w" || k == "wait") {
    for (int i = 0; i < 1000; ++i) Tick(1);
    return true;
  }
  if (k == "t") {
    SimBusSetRealTiming(!SimBusRealTiming());
    SimLog("scan pacing: %s", SimBusRealTiming() ? "real" : "fast");
    return true;
  }
  return true;   // unmapped keys are inert, exactly like Z on the panel
}

// Arbitrary-delta encoder turn, for the GUI's wheel and drag gestures. The
// scripted key tokens only reach +/-1 and +/-10; a drag needs the in-between.
void EncoderDelta(bool right, int delta) {
  if (delta > 64) delta = 64;
  if (delta < -64) delta = -64;
  if (!delta) return;
  Encoder(right ? OC::CONTROL_ENCODER_R : OC::CONTROL_ENCODER_L, delta);
}

// Hold or release one button. This is the whole point of the down/up split:
// event.mask carries the set of buttons held, and the module's three chords
// (both encoders -> preset bus, A + encR -> app menu, A + B -> screen flip)
// are dispatched on that mask, not on the button that moved.
void ButtonEdge(const std::string &tok, bool down) {
  const uint16_t c = SimUiControlForToken(tok);
  if (!c) return;
  SimUiSetButton(c, down);
  Tick(1);
}

// One refresh's worth of simulated time, with the interactive loop's pacing
// rule: fast pacing compresses only the WAITING, so a scan still redraws.
void Pump(uint32_t budget_ms) {
  if (budget_ms > 500) budget_ms = 500;
  const uint32_t steps =
      (!SimBusRealTiming() && SimBusBusy()) ? 1000 : budget_ms;
  for (uint32_t i = 0; i < steps; ++i) Tick(1);
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

void PrintScreen(bool with_chrome) {
  Redraw();
  fputs(SimRenderFrame(SimFrameBuffer(), Caption()).c_str(), stdout);
  if (!with_chrome) return;
  printf("  %s\n", SimBusStatusLine().c_str());
  const auto &log = SimLogLines();
  const size_t from = log.size() > 4 ? log.size() - 4 : 0;
  for (size_t i = from; i < log.size(); ++i) printf("  %s\n", log[i].c_str());
}

// ---------------------------------------------------------------------------
// --stdio: a line protocol on stdin/stdout, so a front-end can drive this same
// binary without one line of UI logic leaving C++. xeno_gui.py speaks it.
//
// Requests are one line:
//     key <token>      one of the interactive keys, or a word alias
//     btn <tok> down   hold a button (a b x y z l r) -- it stays held
//     btn <tok> up     release it
//     enc <l|r> <n>    encoder turn by an arbitrary signed delta
//     pump <ms>        advance the clock by one refresh, interactive pacing
//     state            report without changing anything
//     bye              exit
//
// `key` is a tap: down, one tick, up. `btn` is the half of it that makes a
// chord possible, and the front end sends it from keydown/keyup and from its
// latches. `state`'s "held" line reports what is held, so a forgotten latch is
// visible rather than mysterious.
//
// Every reply is a block of "<name> <value>" lines ended by a line "END". The
// frame comes back as 1024 bytes of the real vertically-packed framebuffer,
// hex, exactly as the SH1106 driver would be handed it -- the front end draws
// those bits and decides nothing.
// ---------------------------------------------------------------------------

// Log and status text lands on its own protocol line, so anything that could
// break the framing is flattened to a space.
std::string Flatten(const std::string &s) {
  std::string out = s;
  for (char &c : out)
    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7f) c = ' ';
  return out;
}

void EmitState() {
  Redraw();
  const uint8_t *f = SimFrameBuffer();
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(2048);
  for (int i = 0; i < 128 * 64 / 8; ++i) {
    hex += kHex[f[i] >> 4];
    hex += kHex[f[i] & 0x0f];
  }
  printf("frame %s\n", hex.c_str());
  printf("caption %s\n", Flatten(Caption()).c_str());
  printf("status %s\n", Flatten(SimBusStatusLine()).c_str());
  printf("millis %lu\n", (unsigned long)SimNowMs());
  printf("busy %d\n", SimBusBusy() ? 1 : 0);
  printf("held %s\n", SimUiHeldTokens().c_str());
  printf("overlay %d\n", OC::PresetBusUI::Active() ? 1 : 0);
  printf("timing %s\n", SimBusRealTiming() ? "real" : "fast");
  printf("synthetic %d\n", SimBusUsingSyntheticBanks() ? 1 : 0);

  const auto &log = SimLogLines();
  printf("logtotal %lu\n", (unsigned long)log.size());
  const size_t from = log.size() > 120 ? log.size() - 120 : 0;
  for (size_t i = from; i < log.size(); ++i)
    printf("log %s\n", Flatten(log[i]).c_str());

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

  EmitState();                       // greet with the boot screen
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
    if (tok.empty()) { EmitState(); continue; }

    if (tok[0] == "bye") break;
    if (tok[0] == "key" && tok.size() >= 2) {
      // "q" quits the terminal build; here there is nothing to quit, so it is
      // inert rather than a way for a stray click to kill the server.
      if (tok[1] != "q") ApplyKey(tok[1]);
    } else if (tok[0] == "btn" && tok.size() >= 3) {
      ButtonEdge(tok[1], tok[2] == "down");
    } else if (tok[0] == "release-all") {
      // The front end sends this on page unload and on window blur, so a
      // latch or a held key cannot survive into a session nobody is watching.
      SimUiReleaseAll();
      Tick(1);
    } else if (tok[0] == "enc" && tok.size() >= 3) {
      EncoderDelta(tok[1] == "r", (int)strtol(tok[2].c_str(), nullptr, 10));
    } else if (tok[0] == "pump" && tok.size() >= 2) {
      Pump((uint32_t)strtoul(tok[1].c_str(), nullptr, 10));
    }
    // "state", and anything unrecognised, just re-report.
    EmitState();
  }
  return 0;
}

void Usage() {
  printf(
      "xeno-sim -- host simulator for the Xenomorpher's 200e Modules app\n"
      "\n"
      "  --keys \"r,x,x,a\"     apply a key sequence, print the result, exit\n"
      "  --dump-frames        with --keys: print the screen after every key\n"
      "  --stdio              line protocol on stdin/stdout, for the browser\n"
      "                       front end -- see xeno_gui.py\n"
      "  --real-timing        pin the virtual clock to wall-clock, so a scan\n"
      "                       takes the ~60s it takes on the module\n"
      "  --bus-off            simulate PresetBus::Enabled() == false\n"
      "  --capture-251e PATH  251e bank hex dump (default: bench capture)\n"
      "  --capture-259e PATH  259e bank hex dump (default: bench capture)\n"
      "  --no-log             omit the status/log lines under the frame\n"
      "  --help\n"
      "\nKeys:\n%s\n"
      "\nA --keys token may be \"<button>-down\" / \"<button>-up\" (buttons are\n"
      "a b x y z l r), which is how a chord or a long press is scripted:\n"
      "  --keys \"l-down,r,l-up\"           both encoders -> preset-bus overlay\n"
      "  --keys \"a-down,r,a-up\"           A + encR      -> app menu\n"
      "  --keys \"l-down,step600,l-up\"     a 500ms hold  -> STORE\n"
      "\nThe simulated bus is 0x20 \"210\", 0x28 \"259 A\", 0x5C \"251 A\".\n"
      "NOTHING here touches real hardware. Writes are discarded.\n",
      kLegend);
}

}  // namespace

int main(int argc, char **argv) {
  SimBusConfig cfg;
  cfg.capture_251e = kDefault251e;
  cfg.capture_259e = kDefault259e;

  std::string keys;
  bool scripted = false, dump_frames = false, show_log = true, stdio_mode = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1 < argc);
    if (a == "--help" || a == "-h") { Usage(); return 0; }
    else if (a == "--keys" && has_next) { keys = argv[++i]; scripted = true; }
    else if (a == "--dump-frames") dump_frames = true;
    else if (a == "--stdio") stdio_mode = true;
    else if (a == "--real-timing") cfg.real_timing = true;
    else if (a == "--bus-off") cfg.bus_enabled = false;
    else if (a == "--no-log") show_log = false;
    else if (a == "--capture-251e" && has_next) cfg.capture_251e = argv[++i];
    else if (a == "--capture-259e" && has_next) cfg.capture_259e = argv[++i];
    else { fprintf(stderr, "unknown option: %s\n", a.c_str()); Usage(); return 2; }
  }

  usbMIDI.set_name("usbMIDI");
  usbHostMIDI[0].set_name("usbHostMIDI[0]");
  usbHostMIDI[1].set_name("usbHostMIDI[1]");
  MIDI1.set_name("MIDI1");

  OC::ui.Init();
  SimPresetInit(&g_app);
  SimBusInit(cfg);
  if (scripted && cfg.real_timing)
    SimLog("--real-timing has no effect with --keys: scripted mode always "
           "runs the virtual clock flat out.");
  if (!cfg.bus_enabled) SimLog("--bus-off: PresetBus::Enabled() reports false.");
  g_app.Init();
  Settle(50);

  if (stdio_mode) return RunStdio();

  if (scripted) {
    if (dump_frames) {
      printf("--- initial ---\n");
      PrintScreen(show_log);
    }
    for (const std::string &tok : SplitKeys(keys)) {
      // "stepN" advances exactly N simulated ms; a "+" prefix applies the key
      // without settling afterwards. Together they reach the states that only
      // exist mid-job -- pressing Read while a scan is walking, say.
      if (tok.rfind("step", 0) == 0) {
        const long n = strtol(tok.c_str() + 4, nullptr, 10);
        for (long i = 0; i < n; ++i) Tick(1);
      } else {
        const bool settle = tok.empty() || tok[0] != '+';
        const std::string k = settle ? tok : tok.substr(1);
        if (!ApplyKey(k)) break;
        if (settle) Settle(120000);  // a full 61-address scan is ~61 sim. sec
      }
      if (dump_frames) {
        printf("--- after '%s' ---\n", tok.c_str());
        PrintScreen(show_log);
      }
    }
    if (!dump_frames) PrintScreen(show_log);
    return 0;
  }

  if (!SimTermRawMode(true)) {
    fprintf(stderr,
            "stdin is not a terminal; use --keys for scripted mode.\n");
    return 2;
  }

  uint64_t last_wall = SimWallMs();
  bool running = true;
  while (running) {
    printf("\033[H\033[2J");          // home + clear
    PrintScreen(show_log);
    printf("\n%s\n", kLegend);
    fflush(stdout);

    // One screen refresh's worth of simulated time. In fast pacing the clock
    // runs as fast as the loop will carry it; in real pacing it tracks the
    // wall clock, so scan pacing feels the way it does on the module.
    const int key = SimTermReadKey(SimBusBusy() ? 20 : 120);
    if (key) {
      const std::string k(1, (char)key);
      running = ApplyKey(k);
      if (!running) break;
    }

    const uint64_t now_wall = SimWallMs();
    uint32_t budget = (uint32_t)(now_wall - last_wall);
    last_wall = now_wall;
    if (budget > 500) budget = 500;
    // Fast pacing compresses only the WAITING: while the bus is busy the clock
    // runs 1 simulated second per refresh, so a progressive scan still redraws
    // and stays abortable. Idle time passes at wall-clock speed in both modes,
    // so the provenance line's "LIVE 12s ago" counts the way it really counts.
    const uint32_t steps =
        (!SimBusRealTiming() && SimBusBusy()) ? 1000 : budget;
    for (uint32_t i = 0; i < steps; ++i) Tick(1);
  }

  SimTermRawMode(false);
  printf("\n");
  return 0;
}
