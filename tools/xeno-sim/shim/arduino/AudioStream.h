#ifndef XENOSIM_AUDIOSTREAM_H_
#define XENOSIM_AUDIOSTREAM_H_
// ---------------------------------------------------------------------------
// Stand-in for cores/teensy4/AudioStream.h -- the stock int16 AudioStream/
// AudioConnection pair. This is NOT part of software/src; on target it comes
// from framework-arduinoteensy (a git dependency, not vendored in this repo),
// so there is nothing real to mirror through the shadow tree. It exists here
// solely so software/src/extern/f32/AudioStream_F32.{h,cpp} -- real firmware,
// compiled unmodified through the shadow -- has a base class to derive from
// and links.
//
// Nothing here ever runs: the simulator has no audio-rate ISR at all (see
// Audio.h), so allocate()/release()/transmit()/receiveReadOnly()/
// receiveWritable() are never called by anything the sim actually drives --
// AudioTweightyF32::update() is reachable only from an audio callback this
// build doesn't have. They are given honest, uninstrumented bodies (a bare
// heap block, a real singly-linked destination list) rather than left
// unimplemented, purely so the *type-checking* on real firmware call sites
// (AudioConvertI16toF32Multi, AudioConnection) has real methods to resolve
// against -- not because any of it is exercised.
//
// `friend class AudioConnection_F32;` below is this stand-in's one deliberate
// invention: on target, extern/f32/AudioStream_F32.cpp's AudioConnection_F32
// reaches into AudioStream_F32's inherited (from this class) protected
// `active` flag, which only compiles there because the real firmware build
// passes -fpermissive (see software/platformio.ini) -- GCC downgrades that
// access-control violation to a warning. The host compiler here does not
// support -fpermissive for access control (clang rejects it outright), so
// this stand-in grants the access it needs directly instead of trying to
// reproduce a cross-compiler leniency quirk.
// ---------------------------------------------------------------------------
#include <stdint.h>
#include <cstddef>
#include <cstring>
// Real cores/teensy4/AudioStream.h pulls Arduino.h in too (for __disable_irq/
// __enable_irq, among other things) -- extern/f32/AudioStream_F32.cpp (real
// firmware) uses both directly and only reaches this header via <Audio.h>/
// <AudioStream.h>, never including Arduino.h itself.
#include "Arduino.h"

#ifndef AUDIO_SAMPLE_RATE_EXACT
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f
#endif
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE AUDIO_SAMPLE_RATE_EXACT
#endif
#ifndef AUDIO_BLOCK_SAMPLES
#define AUDIO_BLOCK_SAMPLES 128
#endif

struct audio_block_t {
  uint8_t ref_count = 0;
  uint8_t reserved1 = 0;
  uint16_t memory_pool_index = 0;
  int16_t data[AUDIO_BLOCK_SAMPLES] = {0};
};

class AudioStream;
class AudioConnection_F32;  // real type: extern/f32/AudioStream_F32.h

class AudioConnection {
public:
  AudioConnection()
    : src(nullptr), dst(nullptr), src_index(0), dest_index(0),
      next_dest(nullptr), isConnected(false) {}
  AudioConnection(AudioStream &source, AudioStream &destination)
    : AudioConnection() { connect(source, 0, destination, 0); }
  AudioConnection(AudioStream &source, unsigned char sourceOutput,
                   AudioStream &destination, unsigned char destinationInput)
    : AudioConnection() { connect(source, sourceOutput, destination, destinationInput); }
  inline void connect(void);
  inline void connect(AudioStream &source, unsigned char sourceOutput,
                       AudioStream &destination, unsigned char destinationInput);
  inline void disconnect(void);
  friend class AudioStream;
protected:
  AudioStream *src;
  AudioStream *dst;
  unsigned char src_index;
  unsigned char dest_index;
  AudioConnection *next_dest;
  bool isConnected;
};

class AudioStream {
public:
  AudioStream(unsigned char ninput, audio_block_t **iqueue)
    : num_inputs(ninput), inputQueue(iqueue) {
    active = false;
    destination_list = nullptr;
    for (unsigned char i = 0; i < num_inputs; i++) inputQueue[i] = nullptr;
  }
  virtual ~AudioStream() {}
  bool isActive() { return active; }
protected:
  bool active;
  // How many AudioConnections currently reference this stream, real Teensy
  // AudioStream state (cores/teensy4/AudioStream.h) that extern/f32/
  // AudioStream_F32.cpp's AudioConnection_F32::connect()/disconnect() now
  // reaches through the friend declaration below to decide whether `active`
  // should actually drop back to false again -- not just whether it was
  // ever set. AudioConnection::connect()/disconnect() just below track it
  // the same way, for the same reason.
  uint8_t numConnections = 0;
  unsigned char num_inputs;
  static inline audio_block_t *allocate();
  static inline void release(audio_block_t *block);
  inline void transmit(audio_block_t *block, unsigned char index = 0);
  inline audio_block_t *receiveReadOnly(unsigned int index = 0);
  inline audio_block_t *receiveWritable(unsigned int index = 0);
  friend class AudioConnection;
  // See file header: the real firmware relies on -fpermissive for this same
  // reach-through on target, not on a friend declaration that exists there.
  friend class AudioConnection_F32;
private:
  virtual void update(void) = 0;
  AudioConnection *destination_list;
  audio_block_t **inputQueue;
};

// --- bodies ------------------------------------------------------------
// Real (if minimal) block lifetime, not stubs: a plain heap block and a
// ref count, no static pool. Nothing the simulator drives ever calls any
// of this (no audio-rate ISR exists here at all -- see Audio.h), so there
// is no throughput or pool-exhaustion concern to model.
inline audio_block_t *AudioStream::allocate() {
  audio_block_t *b = new audio_block_t();
  b->ref_count = 1;
  return b;
}

inline void AudioStream::release(audio_block_t *block) {
  if (!block) return;
  if (block->ref_count > 1) block->ref_count--;
  else delete block;
}

inline void AudioStream::transmit(audio_block_t *block, unsigned char index) {
  for (AudioConnection *c = destination_list; c != nullptr; c = c->next_dest) {
    if (c->src_index == index && c->dst->inputQueue[c->dest_index] == nullptr) {
      c->dst->inputQueue[c->dest_index] = block;
      block->ref_count++;
    }
  }
}

inline audio_block_t *AudioStream::receiveReadOnly(unsigned int index) {
  if (index >= num_inputs) return nullptr;
  audio_block_t *in = inputQueue[index];
  inputQueue[index] = nullptr;
  return in;
}

inline audio_block_t *AudioStream::receiveWritable(unsigned int index) {
  if (index >= num_inputs) return nullptr;
  audio_block_t *in = inputQueue[index];
  inputQueue[index] = nullptr;
  if (in && in->ref_count > 1) {
    audio_block_t *p = AudioStream::allocate();
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) p->data[i] = in->data[i];
    in->ref_count--;
    in = p;
  }
  return in;
}

inline void AudioConnection::connect(void) {
  if (isConnected || !src || !dst) return;
  if (dest_index > dst->num_inputs) return;
  AudioConnection *p = src->destination_list;
  if (p == nullptr) {
    src->destination_list = this;
  } else {
    while (p->next_dest) p = p->next_dest;
    p->next_dest = this;
  }
  src->numConnections++;
  src->active = true;
  dst->numConnections++;
  dst->active = true;
  isConnected = true;
}

inline void AudioConnection::connect(AudioStream &source, unsigned char sourceOutput,
                                      AudioStream &destination, unsigned char destinationInput) {
  if (isConnected) disconnect();
  src = &source;
  dst = &destination;
  src_index = sourceOutput;
  dest_index = destinationInput;
  connect();
}

inline void AudioConnection::disconnect(void) {
  if (!isConnected || !src || !dst) return;
  if (dest_index > dst->num_inputs) return;
  AudioConnection *p = src->destination_list;
  if (p == this) {
    src->destination_list = next_dest;
  } else {
    while (p && p->next_dest != this) p = p->next_dest;
    if (p) p->next_dest = next_dest;
  }
  next_dest = nullptr;
  if (dst->inputQueue[dest_index]) {
    AudioStream::release(dst->inputQueue[dest_index]);
    dst->inputQueue[dest_index] = nullptr;
  }
  if (src->numConnections > 0) src->numConnections--;
  if (src->numConnections == 0) src->active = false;
  if (dst->numConnections > 0) dst->numConnections--;
  if (dst->numConnections == 0) dst->active = false;
  isConnected = false;
}

#endif
