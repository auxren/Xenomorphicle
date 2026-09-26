#pragma once
// Round-trip latency measurement, for a bench where AUDIO OUT is jumped
// back to AUDIO IN.
//
// The graph-order report (AudioGraphOrder.h) says how many audio blocks the
// *graph* adds. This measures the whole path in wall-clock: a marker is
// written into the outgoing I2S buffer from the output DMA ISR, and the
// input DMA ISR timestamps the first sample above a threshold. The
// difference therefore covers the TX DMA buffer, the codec's DAC, the patch
// cable, the codec's ADC and the RX DMA buffer -- everything between
// "software wrote it" and "software saw it".
//
// Both ends are taken in the hardware DMA ISRs rather than in update(),
// because those are the earliest and latest points at which the samples
// exist in software; taking them in update() would fold the audio software
// ISR's own scheduling into the number.
//
// Deliberately not a continuous meter: it fires once per arming, so the
// marker is a single burst rather than something audible.
#include <stdint.h>

#ifdef XENO_CODEC_AUDIO

namespace OC {
namespace LoopbackProbe {

enum State : uint8_t { IDLE = 0, ARMED, SENT, DONE };

extern volatile uint8_t  state;
extern volatile uint32_t t_out;     // DWT cycles when the marker was written
extern volatile uint32_t t_in;      // DWT cycles when it came back

// ~70% of full scale for the 32-bit I2S word. Loud enough to clear any
// plausible noise floor by a wide margin, quiet enough that neither the DAC
// nor the ADC is asked to clip.
constexpr int32_t kLevel     = 1500000000;
// ~14% of full scale. The measured floor with a cable attached was under
// 0.3%, so this cannot trigger on anything but the marker.
constexpr int32_t kThreshold =  300000000;

// Called from the output DMA ISR with the half-buffer it is about to hand
// to the DMA. Writes the marker and timestamps it, once, when armed.
void InjectFromIsr(int32_t* dest, int samples);
// Called from the input DMA ISR. Timestamps the first sample over
// threshold. Costs one predictable branch per half-block when idle.
void DetectFromIsr(const int32_t* src, int samples);

}  // namespace LoopbackProbe
}  // namespace OC

#endif  // XENO_CODEC_AUDIO
