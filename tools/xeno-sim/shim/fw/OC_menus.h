#ifndef OC_MENUS_H
#define OC_MENUS_H
// Host stand-in for software/src/OC_menus.h, which pulls the SH1106 driver,
// OC_DAC.h and the settings machinery. PresetBusUI.cpp uses none of that -- it
// draws with the `graphics` global, which is the REAL weegfx::Graphics,
// compiled from src/drivers/weegfx.cpp by shim/weegfx_host.cpp and rendering
// into the same 128x64 framebuffer the SH1106 driver would be handed.

#include "src/drivers/weegfx.h"

extern weegfx::Graphics graphics;

#endif  // OC_MENUS_H
