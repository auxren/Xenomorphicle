#pragma once
// Host stand-in for software/src/HSUtils.h, which pulls OC_core.h, OC_ADC.h,
// OC_scales.h and (under those) util_math.h's ARM inline assembly. The one
// thing PresetBusUI.cpp takes from it is the popup used for a trigger-driven
// recall that happens with the overlay closed.
//
// PopupType's enumerators are in the real header's order, so the value is the
// same one the firmware would pass.

namespace HS {

enum PopupType {
  POPUP_NONE,
  MENU_POPUP,
  CLOCK_POPUP,
  PRESET_POPUP,
  QUANTIZER_POPUP,
  MIDI_POPUP,
  MESSAGE_POPUP,

  POPUP_TYPE_COUNT
};

void PokePopup(PopupType pop, const char *msg);

}  // namespace HS
