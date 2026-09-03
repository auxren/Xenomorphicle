// See sim_storage.h for why the simulator has persistent storage at all.
//
// The image format, one record per line:
//
//   xeno-sim-storage 1
//   eeprom <hex>
//   file <lfs|sd> <name> <hex>
//
// Text, and sorted by name, for two reasons. A failing check prints something
// a person can read; and two images of the same instrument state are
// byte-identical files, so "the preset set did not change" is `cmp`, not a
// parser. Empty files are recorded with an empty hex field, which is a state
// PresetEngine explicitly guards against and therefore one worth being able to
// round-trip.

#include "sim_storage.h"

#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include <EEPROM.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>

#include "PhzConfig.h"

namespace {

// The two volumes the firmware can write, by the name used in the image.
// PhzConfig::myfs is internal flash; SD is the card. Nothing else persists --
// the calibration data the firmware keeps lives in the EEPROM array.
struct Volume { const char *tag; FS *fs; };

std::vector<Volume> Volumes() {
  return { {"lfs", (FS *)&PhzConfig::myfs}, {"sd", (FS *)&SD} };
}

uint32_t Crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k)
      c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return ~c;
}

void AppendHex(std::string &out, const uint8_t *p, size_t n) {
  static const char *kHex = "0123456789ABCDEF";
  out.reserve(out.size() + n * 2);
  for (size_t i = 0; i < n; ++i) {
    out += kHex[p[i] >> 4];
    out += kHex[p[i] & 0xF];
  }
}

bool ParseHex(const std::string &s, std::vector<uint8_t> &out) {
  if (s.size() & 1) return false;
  out.clear();
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    int hi = -1, lo = -1;
    for (int k = 0; k < 16; ++k) {
      const char d = "0123456789ABCDEF"[k];
      if (s[i] == d || s[i] == (char)(d | 0x20)) hi = k;
      if (s[i + 1] == d || s[i + 1] == (char)(d | 0x20)) lo = k;
    }
    if (hi < 0 || lo < 0) return false;
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  return true;
}

}  // namespace

bool SimStorageLoad(const std::string &path, std::string *why) {
  FILE *fp = fopen(path.c_str(), "r");
  if (!fp) {
    if (why) *why = "no image (a virgin module)";
    return false;
  }

  std::vector<char> line(1 << 16);
  bool header_seen = false;
  bool ok = true;
  while (fgets(line.data(), (int)line.size(), fp)) {
    std::string s(line.data());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty()) continue;

    // Split into at most four space-separated fields; the last (hex) may be
    // empty, which is how a zero-byte file is spelled.
    std::vector<std::string> f;
    size_t i = 0;
    while (i < s.size() && f.size() < 3) {
      size_t j = s.find(' ', i);
      if (j == std::string::npos) { f.push_back(s.substr(i)); i = s.size(); break; }
      f.push_back(s.substr(i, j - i));
      i = j + 1;
    }
    if (i <= s.size() && f.size() == 3) f.push_back(s.substr(i));

    if (f[0] == "xeno-sim-storage") { header_seen = true; continue; }
    if (!header_seen) { ok = false; break; }

    if (f[0] == "eeprom" && f.size() >= 2) {
      std::vector<uint8_t> b;
      if (!ParseHex(f[1], b)) { ok = false; break; }
      const size_t n = b.size() < SimEepromSize() ? b.size() : SimEepromSize();
      memcpy(SimEepromBytes(), b.data(), n);
      continue;
    }
    if (f[0] == "file" && f.size() >= 3) {
      const std::string hex = (f.size() >= 4) ? f[3] : std::string();
      std::vector<uint8_t> b;
      if (!ParseHex(hex, b)) { ok = false; break; }
      for (const Volume &v : Volumes())
        if (f[1] == v.tag) v.fs->volume().files[f[2]] = b;
      continue;
    }
    ok = false;
    break;
  }
  fclose(fp);
  if (!ok && why) *why = "malformed image";
  return ok && header_seen;
}

bool SimStorageSave(const std::string &path) {
  std::string out = "xeno-sim-storage 1\n";

  out += "eeprom ";
  AppendHex(out, SimEepromBytes(), SimEepromSize());
  out += "\n";

  // std::map iterates sorted, and the volumes are already std::map, so the
  // image is deterministic without sorting anything by hand. Two runs that
  // stored the same thing produce identical files.
  for (const Volume &v : Volumes()) {
    for (const auto &f : v.fs->volume().files) {
      out += "file ";
      out += v.tag;
      out += " ";
      out += f.first;
      out += " ";
      AppendHex(out, f.second.data(), f.second.size());
      out += "\n";
    }
  }

  FILE *fp = fopen(path.c_str(), "w");
  if (!fp) return false;
  const size_t n = fwrite(out.data(), 1, out.size(), fp);
  fclose(fp);
  return n == out.size();
}

void SimStorageList(FILE *out) {
  for (const Volume &v : Volumes())
    for (const auto &f : v.fs->volume().files)
      fprintf(out, "fs %s %s %zu %08X\n", v.tag, f.first.c_str(),
              f.second.size(), Crc32(f.second.data(), f.second.size()));
  fprintf(out, "eeprom %zu %08X\n", SimEepromSize(),
          Crc32(SimEepromBytes(), SimEepromSize()));
}
