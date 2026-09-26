#include "AudioLoopbackProbe.h"

#ifdef XENO_CODEC_AUDIO

#include <Arduino.h>

namespace OC {
namespace LoopbackProbe {

volatile uint8_t  state = IDLE;
volatile uint32_t t_out = 0;
volatile uint32_t t_in  = 0;

void InjectFromIsr(int32_t* dest, int samples) {
  if (state != ARMED) return;
  // The whole half-buffer, both interleaved channels: the bench jumps both
  // outs to both ins, and marking both means the measurement does not
  // depend on which jack was patched.
  for (int i = 0; i < samples; ++i) dest[i] = kLevel;
  t_out = ARM_DWT_CYCCNT;
  state = SENT;
}

void DetectFromIsr(const int32_t* src, int samples) {
  if (state != SENT) return;
  for (int i = 0; i < samples; ++i) {
    const int32_t v = src[i];
    if (v > kThreshold || v < -kThreshold) {
      t_in = ARM_DWT_CYCCNT;
      state = DONE;
      return;
    }
  }
}

}  // namespace LoopbackProbe
}  // namespace OC

#endif  // XENO_CODEC_AUDIO
