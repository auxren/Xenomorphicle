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
    AudioStream& InputStream(int interface = 0);
    AudioStream& OutputStream();
    void Init();
  }
}
