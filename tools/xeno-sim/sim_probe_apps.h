#ifndef XENOSIM_SIM_PROBE_APPS_H_
#define XENOSIM_SIM_PROBE_APPS_H_
// ---------------------------------------------------------------------------
// Test stand-ins for Quadrants and Captain MIDI, built only into the
// app-switch check (-DXENO_SIM_APPSWITCH, see the Makefile).
//
// The real two cannot be compiled here (they are Hemisphere apps, and this
// build is NO_HEMISPHERE). What the app-switch check needs from them is not
// their applets but their contract with the shared config map: each keeps a
// file in PhzConfig's one map, saves into it from Suspend, and has to own it
// again when something else has rewritten it. These do exactly that, with the
// same app ids (PresetEngine keys the bank extract on Quadrants' id), the same
// event cases as Quadrants.h / CaptainMIDI.h, and a single value of "live
// state" each, so a lost bank shows up as a changed number.
// ---------------------------------------------------------------------------

#include "PhzConfig.h"
#include "PresetEngine.h"

namespace probe {

constexpr uint16_t kLiveKey = 1;          // the one value each app keeps
constexpr uint16_t kGlobKey = 100;        // a Quadrants global: only Store persists it
constexpr uint16_t kQuadLivePresetKey = 253;  // what Quadrants leaves for the extractor

// The call counts are what the check reads to see which event arrived.
struct Counts {
  int resume = 0, suspend = 0, reload = 0;
};

// APP_EVENT_CONFIG_RELOAD by value, or -1 where the firmware does not have it
// (this check also has to build, and fail, against a tree from before it).
template <class E>
constexpr auto ReloadEvent(int) -> decltype((int)E::APP_EVENT_CONFIG_RELOAD) {
  return (int)E::APP_EVENT_CONFIG_RELOAD;
}
template <class E> constexpr int ReloadEvent(long) { return -1; }
constexpr int kReloadEvent = ReloadEvent<OC::AppEvent>(0);

}  // namespace probe

// Quadrants: a bank file of 32 presets, preset p at key (p << 11) | 1. Live
// state is preset 0's value plus one global (like the quantizers, MIDI maps or
// patterns). Suspend stores both into the map and saves the bank file, as
// store_to_preset() does -- into whatever the map holds. A RESUME reapplies the
// file's global to the live one (LoadGlobals); CONFIG_RELOAD only takes the map
// back, which is what the real handler does. Building with
// -DPROBE_RELOAD_LOADS_GLOBALS models the handler as it was before that was
// fixed (the reload also ran LoadGlobals), to show the check going red.
OC_APP_CLASS(AppProbeQuad, TWOCCS("QS"), "Quadrants", "4x Applets") {
public:
  OC_APP_INTERFACE_DECLARE(AppProbeQuad, 0);
  static constexpr const char *kBank = "BANK_000.DAT";
  uint64_t live = 0;
  uint64_t glob = 0;
  int preset_id = -1;
  probe::Counts n;
  // The dial moves without loading anything; a RESUME (the confirm) loads it.
  uint8_t bank_dial = 0;
  uint8_t loaded_bank = 0;
  char file[16] = "BANK_000.DAT";
  void LoadBankMap(uint8_t id) {
    loaded_bank = id;
    snprintf(file, sizeof(file), "BANK_%03d.DAT", id);
    PhzConfig::load_config(file);
  }
  void LoadGlobals() { PhzConfig::getValue(probe::kGlobKey, glob); }
  void SetBank() { LoadBankMap(bank_dial); LoadGlobals(); }
  void Store() {
    PhzConfig::setValue((uint16_t)((preset_id << 11) | probe::kLiveKey), live);
    PhzConfig::setValue(probe::kGlobKey, glob);
    PhzConfig::save_config(file);
  }
};

// Captain: CAPTAIN.DAT. Live state is the active setup's value.
OC_APP_CLASS(AppProbeCaptain, TWOCCS("MI"), "Captain MIDI", "MIDI I/O") {
public:
  OC_APP_INTERFACE_DECLARE(AppProbeCaptain, 0);
  static constexpr const char *kFile = "CAPTAIN.DAT";
  uint64_t live = 0;
  probe::Counts n;
  // Called from a recall's Suspend, to damage something behind the engine's
  // back between its first and second look at the slot.
  void (*on_recall_suspend)() = nullptr;
  void Store() {
    PhzConfig::load_config(kFile);  // own the map before writing, as StoreData does
    PhzConfig::setValue(probe::kLiveKey, live);
    PhzConfig::save_config(kFile);
  }
};

// The interface every app has to supply; none of it draws or reads anything.
#define PROBE_APP_COMMON(clazz) \
  inline void clazz::Init() {} \
  inline size_t clazz::SaveAppData(util::StreamBufferWriter &) const { return 0; } \
  inline size_t clazz::RestoreAppData(util::StreamBufferReader &) { return 0; } \
  inline void clazz::Loop() {} \
  inline void clazz::DrawMenu() const {} \
  inline void clazz::DrawScreensaver() const {} \
  inline void clazz::HandleButtonEvent(const UI::Event &) {} \
  inline void clazz::HandleEncoderEvent(const UI::Event &) {} \
  inline void clazz::Process(OC::IOFrame *) {} \
  inline void clazz::GetIOConfig(OC::IOConfig &) const {} \
  inline void clazz::DrawDebugInfo() const {}

PROBE_APP_COMMON(AppProbeQuad)
PROBE_APP_COMMON(AppProbeCaptain)

// The same cases, in the same order of effect, as AppQuadrants::HandleAppEvent.
inline void AppProbeQuad::HandleAppEvent(OC::AppEvent event) {
  if ((int)event == probe::kReloadEvent) {
    ++n.reload;
#ifdef PROBE_RELOAD_USES_DIAL
    LoadBankMap(bank_dial);
#else
    LoadBankMap(loaded_bank);
#endif
#ifdef PROBE_RELOAD_LOADS_GLOBALS
    LoadGlobals();
#endif
    return;
  }
  switch (event) {
    case OC::APP_EVENT_RESUME:
      ++n.resume;
      SetBank();
      if (preset_id < 0) {  // first open: take preset 0's value as the live one
        preset_id = 0;
        PhzConfig::getValue((uint16_t)((0 << 11) | probe::kLiveKey), live);
      }
      break;
    case OC::APP_EVENT_SCREENSAVER_ON:
    case OC::APP_EVENT_SUSPEND:
      ++n.suspend;
      if (preset_id >= 0) Store();
      break;
    case OC::APP_EVENT_FLUSH:
      if (preset_id >= 0) {
        Store();
        PhzConfig::setValue(probe::kQuadLivePresetKey, (uint64_t)preset_id);
      }
      break;
    default: break;
  }
}

inline void AppProbeCaptain::HandleAppEvent(OC::AppEvent event) {
  if ((int)event == probe::kReloadEvent) { ++n.reload; PhzConfig::load_config(kFile); return; }
  switch (event) {
    case OC::APP_EVENT_RESUME:
      ++n.resume;
      PhzConfig::load_config(kFile);
      PhzConfig::getValue(probe::kLiveKey, live);
      break;
    case OC::APP_EVENT_SUSPEND:
      ++n.suspend;
      if (on_recall_suspend &&
          (OC::PresetEngine::RecallReplacing() & OC::PresetEngine::RECALL_SUSPEND))
        on_recall_suspend();
      Store();
      break;
    case OC::APP_EVENT_FLUSH:
      Store();
      break;
    default: break;
  }
}

#endif  // XENOSIM_SIM_PROBE_APPS_H_
