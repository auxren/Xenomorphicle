#include "sim_capture.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

static bool HexNibble(char c, int &out) {
  if (c >= '0' && c <= '9') { out = c - '0'; return true; }
  if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
  if (c >= 'a' && c <= 'f') { out = c - 'a' + 10; return true; }
  return false;
}

bool SimLoadHexDump(const std::string &path, std::vector<uint8_t> &out,
                    std::string &err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "cannot open " + path; return false; }

  out.clear();
  std::string line;
  int data_lines = 0;
  while (std::getline(f, line)) {
    // Serial console output: strip the CR of every CRLF before anything else.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) continue;  // banner line

    // Offset field: hex digits only, else this is not a data line.
    unsigned long offset = 0;
    bool ok = true;
    for (size_t i = 0; i < colon; ++i) {
      int n;
      if (!HexNibble(line[i], n)) { ok = false; break; }
      offset = offset * 16 + (unsigned long)n;
    }
    if (!ok) continue;

    std::istringstream rest(line.substr(colon + 1));
    std::string tok;
    size_t at = offset;
    while (rest >> tok) {
      if (tok.size() != 2) { ok = false; break; }
      int hi, lo;
      if (!HexNibble(tok[0], hi) || !HexNibble(tok[1], lo)) { ok = false; break; }
      if (out.size() <= at) out.resize(at + 1, 0);
      out[at++] = (uint8_t)((hi << 4) | lo);
    }
    if (!ok) continue;
    ++data_lines;
  }

  if (data_lines == 0 || out.empty()) {
    err = "no hex data lines parsed from " + path +
          " (CRLF stripped? offset format 'NNNN: bb bb ...'?)";
    return false;
  }
  return true;
}
