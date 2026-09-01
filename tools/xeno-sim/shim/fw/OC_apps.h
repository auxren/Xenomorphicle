#ifndef OC_APP_H_
#define OC_APP_H_
// Host stand-in for software/src/OC_apps.h. PresetBusUI.cpp uses exactly one
// thing from it: OC::APP_EVENT_RESUME, handed to the current app after the
// trigger assignments are persisted. shim/oc_shim.h includes this header too,
// so the app and the overlay agree on the enum.

namespace OC {

enum AppEvent {
  APP_EVENT_SUSPEND,
  APP_EVENT_RESUME,
  APP_EVENT_SCREENSAVER_ON,
  APP_EVENT_SCREENSAVER_OFF,
};

}  // namespace OC

#endif  // OC_APP_H_
