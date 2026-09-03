#ifndef XENOSIM_SIM_SESSION_H_
#define XENOSIM_SIM_SESSION_H_
// ---------------------------------------------------------------------------
// Session record and replay.
//
// THE POINT: when something looks wrong in the browser, the session that got
// there is a dozen lines of text that can be pasted into a chat message, and
// replaying them here reproduces the same frames, byte for byte.
//
// Timing is part of the repro and is recorded as part of it. Every line
// carries the SIMULATED milliseconds since the previous line, because the
// gestures worth reproducing are timed ones: a 500 ms STORE hold, a 250 ms
// RECALL hold, a 500-tick long press, a screensaver timeout. A recording that
// kept only the order of events could not reproduce any of them.
//
// FORMAT -- one line per event, "<dt_ms> <verb> [args]":
//
//     xeno-sim-session 1
//     opt --bus-off
//     0 btn l down
//     2 btn r down
//     14 btn l up
//     3 btn r up
//     612 enc r 1
//     40 end
//
// `opt` lines carry the command-line options the run started with, so a replay
// starts from the same configuration. Everything after the header is an event
// with its elapsed simulated time.
//
// DETERMINISM. Replay is only meaningful if it is exact, so: the virtual clock
// is the only time source any firmware code can observe (see sim_host.h),
// random() is seeded to a fixed value, and every simulator-owned static is
// reset before boot. The one thing that is NOT captured is the wall-clock
// pacing of the interactive terminal and the GUI -- but that only ever decides
// how much simulated time to advance, and how much it advanced is exactly what
// the dt column records.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <string>
#include <vector>

// --- recording -------------------------------------------------------------
void SimSessionStart(const std::vector<std::string> &opts);
bool SimSessionRecording();

// Record one event at the current simulated time. Called from the one place
// each input enters the simulator, so nothing can be applied without being
// recorded.
void SimSessionRecord(const std::string &line);

// The session so far, as the text above.
std::string SimSessionText();

// --- replay ----------------------------------------------------------------
struct SimSessionFile {
  std::vector<std::string> opts;
  std::vector<std::pair<uint32_t, std::string>> events;  // dt_ms, "verb args"
  std::string error;
};

SimSessionFile SimSessionLoad(const char *path);

#endif  // XENOSIM_SIM_SESSION_H_
