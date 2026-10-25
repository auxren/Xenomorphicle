#ifndef XENOSIM_SIM_APPSWITCH_H_
#define XENOSIM_SIM_APPSWITCH_H_
// --test-app-switch SCENARIO: what an app switch does to the Scope's lazily
// taken arena and to the config map the Quadrants and Captain stand-ins own.
// Needs the build-switch/ binary (-DXENO_SIM_APPSWITCH). Each scenario is one
// process: some of them reboot from a stored image, which a second scenario in
// the same process could not do.

#include <string>

// Before SimRuntimeBoot: the boot-time gesture a scenario needs (holding the
// right encoder through the splash opens the app menu at power-up).
void SimAppSwitchPreBoot(const std::string &scenario);

// After boot. Returns the process exit status: 0 when every check held.
int SimAppSwitchTest(const std::string &scenario);

#endif  // XENOSIM_SIM_APPSWITCH_H_
