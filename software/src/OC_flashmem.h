// Force-included on every T4 translation unit (platformio.ini: -include).
//
// GCC 11 LTO (-flto -fno-fat-lto-objects, the Teensy _LTO opt levels) drops
// the section attribute of any COMDAT function -- every member function
// defined inside its class body, every inline, every template -- so the
// hundreds of in-class `FLASHMEM void View()` / `OnEncoderMove()` in the
// applet headers were landing in ITCM anyway, and only out-of-class
// definitions stayed in flash. Reproduced with a 12-line test (a virtual
// defined in-class vs out-of-class, same attribute: .text vs .flashmem).
// `externally_visible` keeps the section through LTO: the function is no
// longer a candidate for the whole-program privatisation that loses the
// attribute, and unlike `used` it does not force dead functions to be
// emitted (`used` grew every image by ~1.8 KB and made T41_MTP link
// HS::DrawAppletList into a NO_HEMISPHERE build). `retain`, `noipa`,
// -flto-partition=none/one and -fno-ipa-icf were tried and do not help.
#pragma once
#include <avr/pgmspace.h>
#undef FLASHMEM
#define FLASHMEM __attribute__((section(".flashmem"), externally_visible))
