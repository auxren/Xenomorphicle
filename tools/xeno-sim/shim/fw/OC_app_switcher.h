#ifndef OC_APP_SWITCHER_H_
#define OC_APP_SWITCHER_H_
// Host stand-in for software/src/OC_app_switcher.h. The simulator runs one app
// and has no registry, so this is only what PresetBusUI.cpp's
// persist_assignments() reaches for:
//     app_switcher.current_app()->DispatchAppEvent(OC::APP_EVENT_RESUME)
// The simulator forwards that to the one app it does have (sim_preset.cpp).

#include "OC_apps.h"

namespace OC {

struct SimCurrentApp {
  void DispatchAppEvent(AppEvent e);
};

struct SimAppSwitcher {
  SimCurrentApp *current_app();
};

extern SimAppSwitcher app_switcher;

}  // namespace OC

#endif  // OC_APP_SWITCHER_H_
