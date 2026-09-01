#ifndef XENOSIM_FS_H_
#define XENOSIM_FS_H_
// ---------------------------------------------------------------------------
// A RAM-backed Teensy FS/File.
//
// The firmware keeps its whole-module state in files: GLOBALS.CFG through
// PhzConfig, BANK_nnn.DAT through PresetEngine, PBCARD.BIN through PresetBus.
// Excluding those paths would mean excluding the Setup app, the preset overlay
// and half of what the preset bus does, so instead the file system is real
// enough to round-trip: writes land in a std::map that lives as long as the
// process.
//
// It is NOT persistent. Nothing the simulator stores survives the process, and
// nothing here says anything about LittleFS's actual behaviour -- the wear,
// the erase timing, the 0-byte-file failure mode PresetEngine.cpp guards
// against. See the README.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <Arduino.h>

#define FILE_READ 0
#define FILE_WRITE 1
#define FILE_WRITE_BEGIN 2

// One store per FS instance, keyed by file name.
struct SimFsVolume {
  std::map<std::string, std::vector<uint8_t>> files;
  uint64_t capacity = 1024u * 1024u;
};

class File : public SimPrint {
public:
  File() {}
  File(SimFsVolume *vol, const std::string &name, int mode)
      : vol_(vol), name_(name), mode_(mode) {
    auto it = vol->files.find(name);
    if (mode == FILE_READ) {
      if (it == vol->files.end()) { vol_ = nullptr; return; }
      data_ = it->second;
    } else {
      if (mode == FILE_WRITE_BEGIN || it == vol->files.end())
        vol->files[name] = {};
      data_ = vol->files[name];
      pos_ = (mode == FILE_WRITE) ? data_.size() : 0;
    }
    open_ = true;
  }

  ~File() { close(); }

  // Teensy's File is a copyable handle; PhzConfig's directory walk passes one
  // by value. A copy carries the same bytes and commits the same content, so
  // the duplicate write-back is harmless.
  File(const File &) = default;
  File &operator=(const File &) = default;
  File(File &&o) { move_from(o); }
  File &operator=(File &&o) { close(); move_from(o); return *this; }

  explicit operator bool() const { return open_; }

  size_t write(const uint8_t *b, size_t n) override {
    if (!open_ || mode_ == FILE_READ) return 0;
    if (pos_ + n > data_.size()) data_.resize(pos_ + n);
    memcpy(data_.data() + pos_, b, n);
    pos_ += n;
    dirty_ = true;
    return n;
  }
  using SimPrint::write;

  int read() {
    if (!open_ || pos_ >= data_.size()) return -1;
    return data_[pos_++];
  }
  int peek() const {
    return (open_ && pos_ < data_.size()) ? data_[pos_] : -1;
  }
  size_t read(void *dst, size_t n) {
    if (!open_) return 0;
    if (pos_ + n > data_.size()) n = data_.size() - pos_;
    memcpy(dst, data_.data() + pos_, n);
    pos_ += n;
    return n;
  }
  size_t readBytes(char *dst, size_t n) { return read(dst, n); }
  String readStringUntil(char term) {
    std::string out;
    while (pos_ < data_.size()) {
      const char c = (char)data_[pos_++];
      if (c == term) break;
      out += c;
    }
    return String(out);
  }
  int available() const { return open_ ? (int)(data_.size() - pos_) : 0; }
  size_t size() const { return data_.size(); }
  size_t position() const { return pos_; }
  bool seek(size_t p) { if (p > data_.size()) return false; pos_ = p; return true; }
  bool isDirectory() const { return false; }
  File openNextFile() { return File(); }
  const char *name() const { return name_.c_str(); }
  void flush() { commit(); }

  void close() {
    commit();
    open_ = false;
    vol_ = nullptr;
  }

private:
  void commit() {
    if (open_ && dirty_ && vol_) { vol_->files[name_] = data_; dirty_ = false; }
  }
  void move_from(File &o) {
    vol_ = o.vol_; name_ = o.name_; data_ = std::move(o.data_);
    pos_ = o.pos_; mode_ = o.mode_; open_ = o.open_; dirty_ = o.dirty_;
    o.vol_ = nullptr; o.open_ = false; o.dirty_ = false;
  }

  SimFsVolume *vol_ = nullptr;
  std::string name_;
  std::vector<uint8_t> data_;
  size_t pos_ = 0;
  int mode_ = FILE_READ;
  bool open_ = false;
  bool dirty_ = false;
};

class FS {
public:
  virtual ~FS() {}
  File open(const char *name, int mode = FILE_READ) {
    return File(&vol_, name, mode);
  }
  bool exists(const char *name) { return vol_.files.count(name) != 0; }
  bool remove(const char *name) { return vol_.files.erase(name) != 0; }
  bool rename(const char *from, const char *to) {
    auto it = vol_.files.find(from);
    if (it == vol_.files.end()) return false;
    vol_.files[to] = it->second;
    vol_.files.erase(it);
    return true;
  }
  bool mkdir(const char *) { return true; }
  bool format() { vol_.files.clear(); return true; }
  bool quickFormat() { vol_.files.clear(); return true; }
  bool rmdir(const char *) { return true; }
  uint64_t totalSize() { return vol_.capacity; }
  uint64_t usedSize() {
    uint64_t n = 0;
    for (const auto &f : vol_.files) n += f.second.size();
    return n;
  }
  SimFsVolume &volume() { return vol_; }

protected:
  SimFsVolume vol_;
};

#endif  // XENOSIM_FS_H_
