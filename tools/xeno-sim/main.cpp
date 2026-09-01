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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sim_bus.h"
#include "sim_term.h"

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

void Button(uint16_t control) {
  const UI::Event e(UI::EVENT_BUTTON_PRESS, control, 0, control);
  g_app.HandleButtonEvent(e);
}

void Encoder(uint16_t control, int value) {
  const UI::Event e(UI::EVENT_ENCODER, control, (int16_t)value, 0);
  g_app.HandleEncoderEvent(e);
}

// One simulated millisecond: the ISR path, the bus, then the main loop --
// the same three contexts the firmware runs these in.
void Tick(uint32_t dt_ms) {
  SimAdvanceMs(dt_ms);
  g_app.Controller();
  SimBusTask();
  g_app.Loop();
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

void Redraw() {
  graphics.Begin(SimFrameBuffer(), weegfx::CLEAR_FRAME_ENABLE);
  g_app.DrawMenu();
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
    "w advance 1 simulated second    t toggle fast/real scan pacing    q quit";

// Applies one key. Returns false to quit.
bool ApplyKey(const std::string &k) {
  if (k.empty()) return true;
  if (k == "q" || k == "quit") return false;

  if (k == "a") { Button(OC::CONTROL_BUTTON_UP); return true; }
  if (k == "b") { Button(OC::CONTROL_BUTTON_DOWN); return true; }
  if (k == "x") { Button(OC::CONTROL_BUTTON_UP2); return true; }
  if (k == "y") { Button(OC::CONTROL_BUTTON_DOWN2); return true; }
  if (k == "l" || k == "encl") { Button(OC::CONTROL_BUTTON_L); return true; }
  if (k == "r" || k == "encr") { Button(OC::CONTROL_BUTTON_R); return true; }

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

void Usage() {
  printf(
      "xeno-sim -- host simulator for the Xenomorpher's 200e Modules app\n"
      "\n"
      "  --keys \"r,x,x,a\"     apply a key sequence, print the result, exit\n"
      "  --dump-frames        with --keys: print the screen after every key\n"
      "  --real-timing        pin the virtual clock to wall-clock, so a scan\n"
      "                       takes the ~60s it takes on the module\n"
      "  --bus-off            simulate PresetBus::Enabled() == false\n"
      "  --capture-251e PATH  251e bank hex dump (default: bench capture)\n"
      "  --capture-259e PATH  259e bank hex dump (default: bench capture)\n"
      "  --no-log             omit the status/log lines under the frame\n"
      "  --help\n"
      "\nKeys:\n%s\n"
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
  bool scripted = false, dump_frames = false, show_log = true;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1 < argc);
    if (a == "--help" || a == "-h") { Usage(); return 0; }
    else if (a == "--keys" && has_next) { keys = argv[++i]; scripted = true; }
    else if (a == "--dump-frames") dump_frames = true;
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

  SimBusInit(cfg);
  if (scripted && cfg.real_timing)
    SimLog("--real-timing has no effect with --keys: scripted mode always "
           "runs the virtual clock flat out.");
  if (!cfg.bus_enabled) SimLog("--bus-off: PresetBus::Enabled() reports false.");
  g_app.Init();
  Settle(50);

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
