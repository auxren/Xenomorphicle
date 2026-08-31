#include "usb_desc.h" // defines AUDIO_INTERFACE for USB audio builds

#if defined(ARDUINO_TEENSY41) && defined(AUDIO_INTERFACE)

#include "USB_F32.h"
#include <arm_math.h>
#include "sample_convert.h"

audio_block_f32_t *AudioInputUSB_F32::rxBuffer[USBAudioInInterface::ringRxBufferSize][USB_AUDIO_MAX_NO_CHANNELS];
audio_block_f32_t *AudioOutputUSB_F32::txBuffer[USBAudioOutInterface::ringTxBufferSize][USB_AUDIO_MAX_NO_CHANNELS];

volatile UsbAudioF32Guards usb_audio_f32_guards;

// ---------------------------------------------------------------------------
// Why every callback below bounds-checks its arguments.
//
// These are ISR callbacks. The transport (the core's usb_audio_interface.cpp)
// picks the ring index and we index rxBuffer/txBuffer with it -- so a bad
// index from the transport is an unchecked wild pointer inside the USB
// interrupt, and that is exactly what took the module down after every preset
// Store. The CrashReport from the fixed firmware named the faulting PC as
// AudioInputUSB_F32::copy_to_buffers; disassembly puts it on the `vstr` that
// writes rxBuffer[bIdx][j]->data[count+i], with DACCVIOL at 0x1B4 on one run
// and 0x80000C on another -- i.e. NULL+4+4*108 (a released block) and a word
// of unrelated .bss read back as a block pointer.
//
// The transport's bug: USBAudioInInterface::resetBuffer() recomputes the write
// index after a stream stall as
//
//     resetSamples     = (TARGET_RX_BUFFER_TIME_S + blockDuration - dt) * Fs
//     incoming_rx_bIdx = resetSamples / AUDIO_BLOCK_SAMPLES      // no modulo!
//
// where dt is a *smoothed extrapolation* of "time since the last USB receive
// ISR", accepted anywhere in [-0.5s, +1.5*bInterval]. ringRxBufferSize is 3
// here, so dt only has to come out about -4ms negative for the index to leave
// the array; the accepted -0.5s floor allows indices up to ~172. rxBuffer is
// 3*4 pointers = 48 bytes at the head of .bss, so bIdx==3 aliases
// AudioOutputUSB_F32::txBuffer[0] and bIdx==8 lands on systick_millis_count --
// which is how a store instruction in an audio ISR ends up dereferencing a
// millisecond counter.
//
// A preset Store blocks loop() for seconds across LittleFS program-flash
// writes (which hold __disable_irq() over each erase/program), so update() and
// the receive ISR both stall, the DWT cycle counter that dt is built from
// wraps every 7.16s at 600MHz, and dt comes back meaningless. That is why the
// crash was reliable and always landed just after Captain's CAPTAIN.DAT save
// -- the longest write in the chain -- with a fault address that moved around.
//
// We cannot patch the core package, and we should not have to: rxBuffer and
// txBuffer are ours. Refusing an out-of-range index (allocateBlock/
// setBlockQuite/areBlocksReady returning false) makes the transport treat the
// reset as an allocation failure and retry it on the next update() with fresh
// timing, which self-heals within one audio block. The NULL checks in the copy
// routines are the belt to that suspenders: resetBuffer() runs inside a
// "must all happen in one un-interrupted block" region that allocate_f32()
// silently reopens with its unconditional __enable_irq(), so the receive ISR
// can land mid-reset with only some channels allocated.
// ---------------------------------------------------------------------------

namespace {
constexpr uint16_t kRxRing = USBAudioInInterface::ringRxBufferSize;
constexpr uint16_t kTxRing = USBAudioOutInterface::ringTxBufferSize;
constexpr uint16_t kChans = USB_AUDIO_MAX_NO_CHANNELS;
}  // namespace

// ---------- input (host -> device) ----------

AudioInputUSB_F32::AudioInputUSB_F32(float kp, float ki)
  : AudioStream_F32(0, NULL),
    _usbInterface(setBlockQuite, releaseBlock, allocateBlock, areBlocksReady, copy_to_buffers, kp, ki) {
  for (uint16_t i = 0; i < USBAudioInInterface::ringRxBufferSize; i++)
    for (uint16_t j = 0; j < USB_AUDIO_MAX_NO_CHANNELS; j++)
      rxBuffer[i][j] = NULL;
  _usbInterface.begin();
}

void AudioInputUSB_F32::update(void) {
  int16_t bIdx = -1;
  uint16_t noChannels;
  _usbInterface.update(bIdx, noChannels);
  if (bIdx < 0) return;
  if (bIdx >= (int16_t)kRxRing || noChannels > kChans) {
    usb_audio_f32_guards.rx_bad_index++;
    return;
  }
  for (uint16_t i = 0; i < noChannels; i++) {
    audio_block_f32_t *b = rxBuffer[bIdx][i];
    if (!b) continue;   // released mid-reset; nothing to hand downstream
    AudioStream_F32::transmit(b, i);
    AudioStream_F32::release(b);
    rxBuffer[bIdx][i] = NULL;
  }
  _usbInterface.incrementBufferIndex();
}

// Fetch the row of block pointers for one ring slot, refusing out-of-range
// indices outright. Returns the number of channels safe to touch, 0 if the
// index is bad. NULL entries stay NULL in `out` and are the caller's problem.
bool AudioInputUSB_F32::rxRow(uint16_t bIdx, uint16_t noChannels, audio_block_f32_t **out) {
  if (bIdx >= kRxRing || noChannels > kChans) {
    usb_audio_f32_guards.rx_bad_index++;
    return false;
  }
  bool all = true;
  for (uint16_t j = 0; j < noChannels; j++) {
    out[j] = rxBuffer[bIdx][j];
    if (!out[j]) all = false;
  }
  if (!all) usb_audio_f32_guards.rx_null_block++;
  return true;
}

#if AUDIO_SUBSLOT_SIZE == 2
void AudioInputUSB_F32::copy_to_buffers(const uint8_t *src, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  audio_block_f32_t *blk[kChans];
  if (!rxRow(bIdx, noChannels, blk)) return;
  if (count >= (unsigned)AUDIO_BLOCK_SAMPLES) return;
  if (count + len > (unsigned)AUDIO_BLOCK_SAMPLES) len = AUDIO_BLOCK_SAMPLES - count;
  const int16_t *src16 = (const int16_t *)src;
  for (uint32_t i = 0; i < len; i++)
    for (uint16_t j = 0; j < noChannels; j++) {
      const float s = samplefmt::f32_from_i16(*src16++);
      if (blk[j]) blk[j]->data[count + i] = s;
    }
}
#elif AUDIO_SUBSLOT_SIZE == 3
void AudioInputUSB_F32::copy_to_buffers(const uint8_t *src, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  audio_block_f32_t *blk[kChans];
  if (!rxRow(bIdx, noChannels, blk)) return;
  if (count >= (unsigned)AUDIO_BLOCK_SAMPLES) return;
  if (count + len > (unsigned)AUDIO_BLOCK_SAMPLES) len = AUDIO_BLOCK_SAMPLES - count;
  for (uint32_t i = 0; i < len; i++) {
    for (uint16_t j = 0; j < noChannels; j++) {
      if (blk[j]) blk[j]->data[count + i] = samplefmt::f32_from_i24le(src);
      src += 3;
    }
  }
}
#else
#error "AudioInputUSB_F32: unsupported AUDIO_SUBSLOT_SIZE"
#endif

bool AudioInputUSB_F32::setBlockQuite(uint16_t bIdx, uint16_t channel) {
  if (bIdx >= kRxRing || channel >= kChans) {
    usb_audio_f32_guards.rx_bad_index++;
    return false;   // transport treats this as an allocation failure and retries
  }
  if (!rxBuffer[bIdx][channel]) rxBuffer[bIdx][channel] = AudioStream_F32::allocate_f32();
  if (rxBuffer[bIdx][channel]) {
    memset(rxBuffer[bIdx][channel]->data, 0, AUDIO_BLOCK_SAMPLES * sizeof(rxBuffer[bIdx][channel]->data[0]));
    return true;
  }
  return false;
}

void AudioInputUSB_F32::releaseBlock(uint16_t bIdx, uint16_t channel) {
  if (bIdx >= kRxRing || channel >= kChans) {
    usb_audio_f32_guards.rx_bad_index++;
    return;
  }
  if (rxBuffer[bIdx][channel]) {
    AudioStream_F32::release(rxBuffer[bIdx][channel]);
    rxBuffer[bIdx][channel] = NULL;
  }
}

bool AudioInputUSB_F32::allocateBlock(uint16_t bIdx, uint16_t channel) {
  if (bIdx >= kRxRing || channel >= kChans) {
    usb_audio_f32_guards.rx_bad_index++;
    return false;   // fails allocateChannels() -> resetBuffer() -> retry
  }
  if (!rxBuffer[bIdx][channel]) rxBuffer[bIdx][channel] = AudioStream_F32::allocate_f32();
  return rxBuffer[bIdx][channel] != NULL;
}

bool AudioInputUSB_F32::areBlocksReady(uint16_t bIdx, uint16_t noChannels) {
  if (bIdx >= kRxRing || noChannels > kChans) {
    usb_audio_f32_guards.rx_bad_index++;
    return false;   // reads as a buffer underflow -> reset is retried
  }
  for (uint16_t i = 0; i < noChannels; i++)
    if (!rxBuffer[bIdx][i]) return false;
  return true;
}

// ---------- output (device -> host) ----------

AudioOutputUSB_F32::AudioOutputUSB_F32(void)
  : AudioStream_F32(USB_AUDIO_MAX_NO_CHANNELS, inputQueueArray_f32),
    _usbInterface(releaseBlocks, isBlockReady, copy_from_buffers) {
  for (uint16_t i = 0; i < USBAudioOutInterface::ringTxBufferSize; i++)
    for (uint16_t j = 0; j < USB_AUDIO_MAX_NO_CHANNELS; j++)
      txBuffer[i][j] = NULL;
  _usbInterface.begin();
}

AudioOutputUSB_F32::AudioOutputUSB_F32(int nch)
  : AudioStream_F32(nch, inputQueueArray_f32),
    _usbInterface(releaseBlocks, isBlockReady, copy_from_buffers) {
  for (uint16_t i = 0; i < USBAudioOutInterface::ringTxBufferSize; i++)
    for (uint16_t j = 0; j < USB_AUDIO_MAX_NO_CHANNELS; j++)
      txBuffer[i][j] = NULL;
  _usbInterface.begin();
}

void AudioOutputUSB_F32::update(void) {
  int16_t bIdx = -1;
  uint16_t noChannels;
  _usbInterface.update(bIdx, noChannels);
  if (bIdx >= (int16_t)kTxRing || noChannels > kChans) {
    usb_audio_f32_guards.tx_bad_index++;
    bIdx = -1;
  }
  if (bIdx < 0) {
    // transport not running (or handed us a bad slot); drain inputs
    for (uint16_t i = 0; i < USB_AUDIO_MAX_NO_CHANNELS; i++) {
      audio_block_f32_t *b = receiveReadOnly_f32(i);
      if (b) AudioStream_F32::release(b);
    }
    return;
  }
  for (uint16_t i = 0; i < noChannels; i++) {
    if (txBuffer[bIdx][i]) AudioStream_F32::release(txBuffer[bIdx][i]);
    txBuffer[bIdx][i] = receiveReadOnly_f32(i);
    if (!txBuffer[bIdx][i]) {
      txBuffer[bIdx][i] = AudioStream_F32::allocate_f32();
      if (txBuffer[bIdx][i]) {
        memset(txBuffer[bIdx][i]->data, 0, AUDIO_BLOCK_SAMPLES * sizeof(txBuffer[bIdx][i]->data[0]));
      } else {
        // out of F32 audio memory
        releaseBlocks(bIdx, noChannels);
        break;
      }
    }
  }
  _usbInterface.incrementBufferIndex();
}

// Same contract as rxRow(): refuse an out-of-range slot, report a partially
// allocated one. The transport only ever probes channel 0 with isBlockReady()
// before calling us, so channels 1..n-1 must be checked here.
bool AudioOutputUSB_F32::txRow(uint16_t bIdx, uint16_t noChannels, audio_block_f32_t **out) {
  if (bIdx >= kTxRing || noChannels > kChans) {
    usb_audio_f32_guards.tx_bad_index++;
    return false;
  }
  bool all = true;
  for (uint16_t j = 0; j < noChannels; j++) {
    out[j] = txBuffer[bIdx][j];
    if (!out[j]) all = false;
  }
  if (!all) usb_audio_f32_guards.tx_null_block++;
  return true;
}

#if AUDIO_SUBSLOT_SIZE == 2
void AudioOutputUSB_F32::copy_from_buffers(uint8_t *dst, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  audio_block_f32_t *blk[kChans];
  int16_t *dst16 = (int16_t *)dst;
  if (!txRow(bIdx, noChannels, blk) || count >= (unsigned)AUDIO_BLOCK_SAMPLES) {
    memset(dst, 0, len * noChannels * AUDIO_SUBSLOT_SIZE);
    return;
  }
  if (count + len > (unsigned)AUDIO_BLOCK_SAMPLES) {
    const unsigned keep = AUDIO_BLOCK_SAMPLES - count;
    memset(dst + keep * noChannels * AUDIO_SUBSLOT_SIZE, 0,
           (len - keep) * noChannels * AUDIO_SUBSLOT_SIZE);
    len = keep;
  }
  for (uint32_t i = 0; i < len; i++) {
    for (uint16_t j = 0; j < noChannels; j++) {
      *dst16++ = blk[j] ? samplefmt::f32_to_i16(blk[j]->data[count + i]) : 0;
    }
  }
}
#elif AUDIO_SUBSLOT_SIZE == 3
void AudioOutputUSB_F32::copy_from_buffers(uint8_t *dst, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  audio_block_f32_t *blk[kChans];
  if (!txRow(bIdx, noChannels, blk) || count >= (unsigned)AUDIO_BLOCK_SAMPLES) {
    memset(dst, 0, len * noChannels * AUDIO_SUBSLOT_SIZE);
    return;
  }
  if (count + len > (unsigned)AUDIO_BLOCK_SAMPLES) {
    const unsigned keep = AUDIO_BLOCK_SAMPLES - count;
    memset(dst + keep * noChannels * AUDIO_SUBSLOT_SIZE, 0,
           (len - keep) * noChannels * AUDIO_SUBSLOT_SIZE);
    len = keep;
  }
  for (uint32_t i = 0; i < len; i++) {
    for (uint16_t j = 0; j < noChannels; j++) {
      samplefmt::f32_to_i24le(blk[j] ? blk[j]->data[count + i] : 0.f, dst);
      dst += 3;
    }
  }
}
#else
#error "AudioOutputUSB_F32: unsupported AUDIO_SUBSLOT_SIZE"
#endif

void AudioOutputUSB_F32::releaseBlocks(uint16_t bIdx, uint16_t noChannels) {
  if (bIdx >= kTxRing || noChannels > kChans) {
    usb_audio_f32_guards.tx_bad_index++;
    return;
  }
  for (uint16_t i = 0; i < noChannels; i++) {
    if (txBuffer[bIdx][i]) {
      AudioStream_F32::release(txBuffer[bIdx][i]);
      txBuffer[bIdx][i] = NULL;
    }
  }
}

bool AudioOutputUSB_F32::isBlockReady(uint16_t bIdx, uint16_t channel) {
  if (bIdx >= kTxRing || channel >= kChans) {
    usb_audio_f32_guards.tx_bad_index++;
    return false;
  }
  return txBuffer[bIdx][channel] != NULL;
}

#endif // ARDUINO_TEENSY41 && AUDIO_INTERFACE
