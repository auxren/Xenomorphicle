#ifndef XENOSIM_SIM_TERM_H_
#define XENOSIM_SIM_TERM_H_

#include <stdint.h>

#include <string>

// Renders the 128x64 monochrome framebuffer as Unicode half-blocks: two pixel
// rows per terminal row, so the whole screen is 32 rows of 128 characters plus
// a border. Pixel aspect ends up roughly square in a normal terminal font.
std::string SimRenderFrame(const uint8_t *frame, const std::string &caption);

// Raw-mode terminal for interactive use. Restores the previous mode on exit
// (including via atexit, so a crash out of the loop does not leave a wedged
// terminal).
bool SimTermRawMode(bool on);

// Reads one key, waiting at most timeout_ms. Returns 0 if nothing arrived.
int SimTermReadKey(int timeout_ms);

// Milliseconds since an arbitrary fixed point; drives --real-timing.
uint64_t SimWallMs();

#endif  // XENOSIM_SIM_TERM_H_
