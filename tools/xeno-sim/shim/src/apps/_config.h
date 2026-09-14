// The simulator's app manifest: which apps this build instantiates.
//
// Shadows software/src/apps/_config.h (see mkshadow.sh). The firmware's own
// manifest #includes every app header unconditionally, so a host build that
// wanted five apps would still have to make all thirty compile. This lists
// only what the simulator carries; everything else -- the container type, the
// app classes, the app switcher that lists them -- is the real thing, and each
// app below is the firmware's own header, unmodified.
//
// Which apps are here, which are not, and why, is in README.md. The
// const-correctness issue that used to block Captain MIDI, Calibr8or and
// Scale Editor is fixed; each now compiles under clang. They are still absent
// because three unrelated blockers sat behind it -- a 4-vs-5 argument
// usbMIDI.send() in this shim, a real int8_t truncation in ScaleEditor, and
// cursor_countdown being undefined in a NO_HEMISPHERE build. See README.md,
// "Apps that are not simulated".
namespace menu = OC::menu;

// The firmware's redraw flag lives in Main.cpp and is declared by whichever
// app header happens to want it. The manifest's apps do not, so declare it
// here; sim_runtime.cpp defines it.
extern uint_fast8_t MENU_REDRAW;

#ifdef ENABLE_APP_BUS200E
#include "Bus200eApp.h"
#endif
#ifdef ENABLE_APP_TWEIGHTY
#include "TweightyApp.h"
#endif
#include "Backup.h"
#include "SETTINGS.h"

namespace OC {

static AppContainer<void
  , AppSettings
#ifdef ENABLE_APP_BUS200E
  , AppBus200e
#endif
#ifdef ENABLE_APP_TWEIGHTY
  , AppTweighty
#endif
  , AppBackup
> app_container;

// The firmware boots into Captain MIDI on T41; Captain is not in this build
// (see above), so the simulator boots into the 200e app instead. --app picks
// any of them, and the app switcher reaches all of them.
#ifdef ENABLE_APP_BUS200E
static constexpr int DEFAULT_APP_INDEX = 1;
#else
static constexpr int DEFAULT_APP_INDEX = 1;
#endif
static constexpr uint16_t DEFAULT_APP_ID = decltype(app_container)::GetAppIDAtIndex<DEFAULT_APP_INDEX>();
// Position-indexed, like the firmware's, so deleting an app above the boot
// app silently moves it. The firmware has the same assert for the same
// reason; without it the only symptom is selfcheck booting somewhere else.
#ifdef ENABLE_APP_BUS200E
static_assert(DEFAULT_APP_ID == AppBus200e::kAppId,
              "DEFAULT_APP_INDEX must select the 200e app");
#endif

// The app container is TU-local to OC_apps.cpp on target (it is `static` in
// the firmware's manifest too), so the simulator's --app option and its status
// line reach it through these. Declared in sim_runtime.h.
size_t SimAppCount() { return app_container.num_apps(); }
const char *SimAppNameAt(size_t i) { return app_container[i].name(); }
uint16_t SimAppIdAt(size_t i) { return app_container[i].id(); }

}
