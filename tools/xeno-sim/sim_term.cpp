#include "sim_term.h"

#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kW = 128;
constexpr int kH = 64;

inline bool Px(const uint8_t *f, int x, int y) {
  return (f[(y >> 3) * kW + x] >> (y & 7)) & 1;
}

struct termios g_saved;
bool g_raw = false;

void RestoreTerm() {
  if (g_raw) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
    g_raw = false;
  }
}

}  // namespace

std::string SimRenderFrame(const uint8_t *frame, const std::string &caption) {
  std::string out;
  out.reserve(kW * kH / 2 * 4 + 1024);

  // Top border carries the caption, so a captured frame can never be mistaken
  // for a photo of the module.
  out += "\xe2\x94\x8c";  // U+250C
  {
    std::string cap = " " + caption + " ";
    if ((int)cap.size() > kW) cap = cap.substr(0, kW);
    out += cap;
    for (int i = (int)cap.size(); i < kW; ++i) out += "\xe2\x94\x80";  // U+2500
  }
  out += "\xe2\x94\x90\n";  // U+2510

  for (int row = 0; row < kH; row += 2) {
    out += "\xe2\x94\x82";  // U+2502
    for (int x = 0; x < kW; ++x) {
      const bool top = Px(frame, x, row);
      const bool bot = Px(frame, x, row + 1);
      if (top && bot)      out += "\xe2\x96\x88";  // full block
      else if (top)        out += "\xe2\x96\x80";  // upper half
      else if (bot)        out += "\xe2\x96\x84";  // lower half
      else                 out += ' ';
    }
    out += "\xe2\x94\x82\n";
  }

  out += "\xe2\x94\x94";  // U+2514
  for (int i = 0; i < kW; ++i) out += "\xe2\x94\x80";
  out += "\xe2\x94\x98\n";  // U+2518
  return out;
}

bool SimTermRawMode(bool on) {
  if (on) {
    if (g_raw) return true;
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return false;
    struct termios raw = g_saved;
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
    g_raw = true;
    atexit(RestoreTerm);
    return true;
  }
  RestoreTerm();
  return true;
}

int SimTermReadKey(int timeout_ms) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return 0;
  unsigned char c = 0;
  if (read(STDIN_FILENO, &c, 1) != 1) return 0;
  return (int)c;
}

uint64_t SimWallMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000u + (uint64_t)(tv.tv_usec / 1000);
}
