#ifndef XENOSIM_TEENSYVARIABLEPLAYBACK_H_
#define XENOSIM_TEENSYVARIABLEPLAYBACK_H_
// ---------------------------------------------------------------------------
// Stand-in for software/teensy-variable-playback (AudioPlaySdResmp), built only
// into the app-switch check, where the real Sampler app runs against it. The
// vendored player is not compiled here (it needs the SD card and the Audio
// ISR); this models just what bears on memory and on the stop/close/reload
// semantics the Sampler depends on, copied from the vendored sources:
//
//   play(name)   close(), then open the file and preload 7 blocks of 4096
//                bytes (BUFFER_COUNT_SD x BUFFER_SIZE_SD x 2) plus a file
//                object, state PAUSED (startPaused)       ResamplingReader.h play()
//   stop()       the player's stop(): reader stop (STOPPED) THEN reset(),
//                and reset() is reload(): the 7 blocks are read again from the
//                SD and the state goes to PAUSED           playresmp.h, reset()
//   close()      stop(); _file_samples = 0; free the blocks ResamplingSdReader.h
//   available()  state != STOPPED && file_samples > 0
//
// sim_sd_block_reads counts the SD block reads, so a test can see a release
// that re-reads what it is about to free.
// ---------------------------------------------------------------------------
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "AudioStream.h"

enum loop_type { looptype_none, looptype_repeat, looptype_pingpong };

extern long sim_sd_block_reads;   // 4096-byte blocks read from the card
extern long sim_open_files;       // files currently held open
extern long sim_reentrant_events; // queued player events run while a file was closing

namespace simtvp {

// AudioEventResponder::disableResponse()/enableResponse() nest on this count.
extern int g_response_disable;

class Reader {
 public:
  enum State { STOPPED, PLAYING, PAUSED };
  static constexpr int kBlocks = 7;
  static constexpr size_t kBlockBytes = 2048 * 2;

  ~Reader() { close(); }

  bool Play(const char *name) {
    close();
    if (!name || !*name) return false;
    file_ = malloc(96);  // the SD File object and its filename copy
    for (int i = 0; i < kBlocks; ++i) blocks_[i] = calloc(1, kBlockBytes);
    ++sim_open_files;
    file_samples_ = 100000;
    Reload();
    state_ = PAUSED;
    return true;
  }
  void Stop() { state_ = STOPPED; }
  void Reset() {
    if (file_samples_ == 0) return;
    if (Reload()) state_ = PAUSED;
  }
  bool Reload() {
    if (!file_) return false;
    sim_sd_block_reads += kBlocks;
    return true;
  }
  // A playing voice queues an evReload every audio block (playresmp.h:248).
  bool event_pending = false;
  void close() {
    Stop();
    file_samples_ = 0;
    if (file_) {
      // File::close() syncs the volume cache and SdFat's SDIO driver can
      // yield() there, which runs a queued event unless responses are masked.
      if (event_pending && g_response_disable == 0) {
        ++sim_reentrant_events;
        event_pending = false;
      }
      for (int i = 0; i < kBlocks; ++i) { free(blocks_[i]); blocks_[i] = nullptr; }
      free(file_);
      file_ = nullptr;
      --sim_open_files;
    }
  }
  bool Available() const { return state_ != STOPPED && file_samples_ > 0; }
  bool IsPlaying() const { return state_ == PLAYING; }
  void StartPlaying() {
    if (IsPlaying()) Stop();
    if (Available()) { Reset(); state_ = PLAYING; event_pending = true; }
  }
  bool Open() const { return file_ != nullptr; }

 private:
  void *file_ = nullptr;
  void *blocks_[kBlocks] = {};
  State state_ = STOPPED;
  uint32_t file_samples_ = 0;
};

}  // namespace simtvp

class AudioPlaySdResmp : public AudioStream {
 public:
  AudioPlaySdResmp() : AudioStream(0, nullptr), reader(new simtvp::Reader()) {}
  virtual ~AudioPlaySdResmp() { delete reader; }
  void enableInterpolation(bool) {}
  void setBufferInPSRAM(bool) {}
  bool playWav(const char *filename) { stop(); return reader->Play(filename); }
  static void disableResponse() { ++simtvp::g_response_disable; }
  static void enableResponse() { if (--simtvp::g_response_disable < 0) simtvp::g_response_disable = 0; }
  void clearEvent() { reader->event_pending = false; }
  void stop() {
    disableResponse();
    clearEvent();
    reader->Stop();
    reader->Reset();
    enableResponse();
  }
  void setLoopType(loop_type) {}
  void setPlaybackRate(float) {}
  bool isPlaying() const { return reader->IsPlaying(); }
  bool available() const { return reader->Available(); }
  void play() { if (reader->Available()) reader->StartPlaying(); }
  void retrigger() {}
  void update() override {}

 protected:
  simtvp::Reader *reader;
};

// ResamplingSdReader::close() as SamplerVoice::CloseFile reaches it.
#endif
