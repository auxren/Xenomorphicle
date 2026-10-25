#pragma once
// The audio I/O routing constants and OutputStream(), for the app-switch check
// only (the real header drags in the codec graph and the NVIC). Shadows
// software/src/AudioIO.h; the numbers are the firmware's.
#include <Audio.h>
namespace OC {
namespace AudioIO {
constexpr uint8_t kOutputRouteChannels = 2;
constexpr uint8_t kOutputRouteSources = 4;
constexpr uint8_t kOutputRouteTweightySlot = 1;
constexpr uint8_t kOutputRouteSamplerSlot = 2;
constexpr uint8_t kOutputRouteEffectSlot = 3;
AudioStream &InputStream(int interface = 0);
AudioStream &OutputStream();
}  // namespace AudioIO
}  // namespace OC
