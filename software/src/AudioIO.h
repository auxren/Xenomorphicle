#pragma once

#include <Audio.h>

namespace OC {
  namespace AudioIO {
    // total block size including header is 260 bytes
    // 252 * 260 fits nicely into two 32KB pages
    const int AUDIO_MEMORY = 252;
    // float32 pool (RAM2 heap, ~528 bytes/block); boundary converters, the
    // F32 I2S drivers, and F32-native applets draw from this. The Delay
    // applet alone can hold ~10 blocks per channel in flight (8 taps + I/O).
    const int F32_AUDIO_MEMORY = 80;

    // OutputStream()'s destination is a summing point (AudioSummingRoute,
    // Audio/AudioMixer.h), not a plain relay: Quadrants' own audio-applet
    // chain-tail (AudioAppletSubapp.h), Tweighty's background engine output
    // (apps/TweightyApp.h), and Sampler's 8-voice mix (apps/SamplerApp.h)
    // are ALL wired to it (Quadrants and Sampler permanently at Init(),
    // Tweighty lazily on its own first RESUME) and all need to be audible
    // simultaneously -- see AudioIO.cpp's output_route comment.
    // Source-major input layout: source `s`'s channel `c` is at index
    // `s * kOutputRouteChannels + c`. Quadrants' chain-tail predates this
    // and keeps targeting index == channel unchanged (source slot 0);
    // Tweighty and Sampler each claim their own slot explicitly.
    constexpr uint8_t kOutputRouteChannels = 2;
    constexpr uint8_t kOutputRouteSources = 3;
    constexpr uint8_t kOutputRouteTweightySlot = 1;
    constexpr uint8_t kOutputRouteSamplerSlot = 2;

    AudioStream& InputStream(int interface = 0);
    AudioStream& OutputStream();
    void Init();
  }
}
