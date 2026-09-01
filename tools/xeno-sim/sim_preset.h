#ifndef XENOSIM_SIM_PRESET_H_
#define XENOSIM_SIM_PRESET_H_

#include <stdint.h>

class SimAppBase;

// ---------------------------------------------------------------------------
// The environment the REAL software/src/PresetBusUI.cpp needs.
//
// The overlay itself is firmware -- shim/fw/ compiles that file unmodified, so
// every pixel of the preset screen, the 7-segment window, the STORE and RECALL
// hold bars and the confirmation banners are the module's own code. What is
// faked here is what sits behind it:
//
//   OC::PresetEngine   30 slots on the SD card. The simulator keeps them in
//                      RAM, pre-populates a few so both the stored and the
//                      EMPTY paths are visible, and completes a save or recall
//                      after a plausible delay so the pending/banner path and
//                      the "EMPTY SLOT n" refusal both run for real.
//   OC::DigitalInputs  the 225e last/next pulse jacks: always low, see
//                      shim/fw/OC_digital_inputs.h.
//   HS::PokePopup      logged.
//   OC::app_switcher   forwarded to the one app the simulator runs.
//
// NOTHING here writes anything anywhere.
// ---------------------------------------------------------------------------

void SimPresetInit(SimAppBase *app);

// Completes whatever save/recall is in flight. Call once per simulated ms.
void SimPresetTask();

// What PresetBus::BroadcastSave / BroadcastRecall land on in the simulator.
void SimPresetRequestSave(uint8_t slot);
void SimPresetRequestRecall(uint8_t slot);

#endif  // XENOSIM_SIM_PRESET_H_
