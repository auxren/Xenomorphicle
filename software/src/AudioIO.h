#pragma once

#include <Audio.h>

namespace OC {
  namespace AudioIO {
    // total block size including header is 260 bytes
    // 252 * 260 fits nicely into two 32KB pages
    const int AUDIO_MEMORY = 252;
    // float32 pool (RAM2 heap, ~528 bytes/block); boundary converters and the
    // F32 I2S drivers draw from this until applets migrate to F32
    const int F32_AUDIO_MEMORY = 48;
    AudioStream& InputStream(int interface = 0);
    AudioStream& OutputStream();
    void Init();
  }
}
