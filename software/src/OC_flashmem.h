// Force-included on every T4 translation unit (platformio.ini: -include).
//
// GCC 11 LTO (-flto -fno-fat-lto-objects, the Teensy _LTO opt levels) drops
// the section attribute of any COMDAT function -- every member function
// defined inside its class body, every inline, every template -- so the
// hundreds of in-class `FLASHMEM void View()` / `OnEncoderMove()` in the
// applet headers were landing in ITCM anyway, and only out-of-class
// definitions stayed in flash. Reproduced with a 12-line test (a virtual
// defined in-class vs out-of-class, same attribute: .text vs .flashmem).
// Adding `used` keeps the section through LTO; the cost is that an
// unreferenced FLASHMEM function is now emitted into flash instead of
// discarded, which is flash we have and ITCM we do not.
#pragma once
#include <avr/pgmspace.h>
#undef FLASHMEM
#define FLASHMEM __attribute__((section(".flashmem"), externally_visible))
