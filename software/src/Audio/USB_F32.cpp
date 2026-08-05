#include "usb_desc.h" // defines AUDIO_INTERFACE for USB audio builds

#if defined(ARDUINO_TEENSY41) && defined(AUDIO_INTERFACE)

#include "USB_F32.h"
#include <arm_math.h>

audio_block_f32_t *AudioInputUSB_F32::rxBuffer[USBAudioInInterface::ringRxBufferSize][USB_AUDIO_MAX_NO_CHANNELS];
audio_block_f32_t *AudioOutputUSB_F32::txBuffer[USBAudioOutInterface::ringTxBufferSize][USB_AUDIO_MAX_NO_CHANNELS];

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
  if (bIdx != -1) {
    for (uint16_t i = 0; i < noChannels; i++) {
      AudioStream_F32::transmit(rxBuffer[bIdx][i], i);
      AudioStream_F32::release(rxBuffer[bIdx][i]);
      rxBuffer[bIdx][i] = NULL;
    }
    _usbInterface.incrementBufferIndex();
  }
}

#if AUDIO_SUBSLOT_SIZE == 2
void AudioInputUSB_F32::copy_to_buffers(const uint8_t *src, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  const int16_t *src16 = (const int16_t *)src;
  for (uint32_t i = 0; i < len; i++)
    for (uint16_t j = 0; j < noChannels; j++)
      rxBuffer[bIdx][j]->data[count + i] = (float)(*src16++) * (1.0f / 32768.0f);
}
#elif AUDIO_SUBSLOT_SIZE == 3
void AudioInputUSB_F32::copy_to_buffers(const uint8_t *src, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  for (uint32_t i = 0; i < len; i++) {
    for (uint16_t j = 0; j < noChannels; j++) {
      // 24-bit little-endian -> sign-extended int32 -> float, full fidelity
      int32_t v = (int32_t)((uint32_t)src[0] << 8 | (uint32_t)src[1] << 16 | (uint32_t)src[2] << 24) >> 8;
      src += 3;
      rxBuffer[bIdx][j]->data[count + i] = (float)v * (1.0f / 8388608.0f);
    }
  }
}
#else
#error "AudioInputUSB_F32: unsupported AUDIO_SUBSLOT_SIZE"
#endif

bool AudioInputUSB_F32::setBlockQuite(uint16_t bIdx, uint16_t channel) {
  if (!rxBuffer[bIdx][channel]) rxBuffer[bIdx][channel] = AudioStream_F32::allocate_f32();
  if (rxBuffer[bIdx][channel]) {
    memset(rxBuffer[bIdx][channel]->data, 0, AUDIO_BLOCK_SAMPLES * sizeof(rxBuffer[bIdx][channel]->data[0]));
    return true;
  }
  return false;
}

void AudioInputUSB_F32::releaseBlock(uint16_t bIdx, uint16_t channel) {
  if (rxBuffer[bIdx][channel]) {
    AudioStream_F32::release(rxBuffer[bIdx][channel]);
    rxBuffer[bIdx][channel] = NULL;
  }
}

bool AudioInputUSB_F32::allocateBlock(uint16_t bIdx, uint16_t channel) {
  if (!rxBuffer[bIdx][channel]) rxBuffer[bIdx][channel] = AudioStream_F32::allocate_f32();
  return rxBuffer[bIdx][channel] != NULL;
}

bool AudioInputUSB_F32::areBlocksReady(uint16_t bIdx, uint16_t noChannels) {
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
  if (bIdx < 0) {
    // transport not running; drain inputs
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

#if AUDIO_SUBSLOT_SIZE == 2
void AudioOutputUSB_F32::copy_from_buffers(uint8_t *dst, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  int16_t *dst16 = (int16_t *)dst;
  for (uint32_t i = 0; i < len; i++) {
    for (uint16_t j = 0; j < noChannels; j++) {
      int32_t v = (int32_t)lrintf(txBuffer[bIdx][j]->data[count + i] * 32768.0f);
      *dst16++ = (int16_t)__SSAT(v, 16);
    }
  }
}
#elif AUDIO_SUBSLOT_SIZE == 3
void AudioOutputUSB_F32::copy_from_buffers(uint8_t *dst, uint16_t bIdx, uint16_t noChannels, unsigned int count, unsigned int len) {
  for (uint32_t i = 0; i < len; i++) {
    for (uint16_t j = 0; j < noChannels; j++) {
      int32_t v = (int32_t)lrintf(txBuffer[bIdx][j]->data[count + i] * 8388608.0f);
      v = __SSAT(v, 24);
      *dst++ = (uint8_t)(v & 255);         // 24-bit little-endian
      *dst++ = (uint8_t)((v >> 8) & 255);
      *dst++ = (uint8_t)((v >> 16) & 255);
    }
  }
}
#else
#error "AudioOutputUSB_F32: unsupported AUDIO_SUBSLOT_SIZE"
#endif

void AudioOutputUSB_F32::releaseBlocks(uint16_t bIdx, uint16_t noChannels) {
  for (uint16_t i = 0; i < noChannels; i++) {
    if (txBuffer[bIdx][i]) {
      AudioStream_F32::release(txBuffer[bIdx][i]);
      txBuffer[bIdx][i] = NULL;
    }
  }
}

bool AudioOutputUSB_F32::isBlockReady(uint16_t bIdx, uint16_t channel) {
  return txBuffer[bIdx][channel] != NULL;
}

#endif // ARDUINO_TEENSY41 && AUDIO_INTERFACE
