// Session record and replay. See sim_session.h.

#include "sim_session.h"

#include <stdio.h>
#include <stdlib.h>

#include "sim_host.h"

namespace {
bool g_recording = false;
uint32_t g_last_ms = 0;
std::vector<std::string> g_opts;
std::vector<std::string> g_lines;
}  // namespace

void SimSessionStart(const std::vector<std::string> &opts) {
  g_recording = true;
  g_opts = opts;
  g_lines.clear();
  g_last_ms = SimNowMs();
}

bool SimSessionRecording() { return g_recording; }

void SimSessionRecord(const std::string &line) {
  if (!g_recording) return;
  const uint32_t now = SimNowMs();
  char buf[64];
  snprintf(buf, sizeof(buf), "%u ", (unsigned)(now - g_last_ms));
  g_last_ms = now;
  g_lines.push_back(std::string(buf) + line);
}

std::string SimSessionText() {
  std::string out = "xeno-sim-session 1\n";
  for (const auto &o : g_opts) out += "opt " + o + "\n";
  for (const auto &l : g_lines) out += l + "\n";
  // A trailing `end` pins the simulated time after the last input, so a bug
  // that only appears while a hold is still running still replays.
  char buf[64];
  snprintf(buf, sizeof(buf), "%u end\n", (unsigned)(SimNowMs() - g_last_ms));
  out += buf;
  return out;
}

SimSessionFile SimSessionLoad(const char *path) {
  SimSessionFile f;
  FILE *fp = fopen(path, "r");
  if (!fp) { f.error = std::string("cannot open ") + path; return f; }

  char line[512];
  bool seen_header = false;
  while (fgets(line, sizeof(line), fp)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    // Tolerate what a chat client does to pasted text: leading spaces, and
    // the quoting characters people's clients add.
    size_t b = s.find_first_not_of(" \t>");
    if (b == std::string::npos) continue;
    s = s.substr(b);
    if (s.empty() || s[0] == '#') continue;

    if (!seen_header) {
      if (s.rfind("xeno-sim-session", 0) != 0) {
        f.error = "not a session file (expected an 'xeno-sim-session' header)";
        fclose(fp);
        return f;
      }
      seen_header = true;
      continue;
    }
    if (s.rfind("opt ", 0) == 0) { f.opts.push_back(s.substr(4)); continue; }

    char *endp = nullptr;
    const unsigned long dt = strtoul(s.c_str(), &endp, 10);
    if (endp == s.c_str()) { f.error = "bad event line: " + s; break; }
    while (*endp == ' ') ++endp;
    f.events.emplace_back((uint32_t)dt, std::string(endp));
  }
  fclose(fp);
  if (!seen_header && f.error.empty())
    f.error = "empty session file";
  return f;
}
