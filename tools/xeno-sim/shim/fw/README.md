# `shim/fw/` — the include shadow that compiles the real `PresetBusUI.cpp`

`software/src/PresetBusUI.cpp` is the firmware's preset-bus overlay: the screen
both encoder pushes open. The simulator compiles and links **that file, verbatim**
so the overlay in the browser is the firmware's own pixels and its own STORE /
RECALL hold logic, not a drawing of them.

Getting it to compile on the host needed one trick. `PresetBusUI.cpp` includes
`"OC_ui.h"`, `"OC_apps.h"`, `"OC_menus.h"` and friends, and for a quoted include
the preprocessor searches **the directory of the file containing the directive**
first — which is `software/src/`, so no `-I` can shadow them. Several of those
headers are unreachable on a host build: `OC_menus.h` pulls the display driver,
`HSUtils.h` pulls `OC_core.h` and `OC_ADC.h`, and `util_math.h` under them is
ARM inline assembly (`ssat` / `usat`).

So `PresetBusUI.cpp` here is a **symlink** to the real file, and the symlink's
own directory is this one. GCC and clang both take "the directory of the current
file" from the path they opened, not from the symlink target, so the quoted
includes land in *this* directory first. The small stand-ins below answer them;
anything not present here (`PresetBus.h`, `PresetEngine.h`, `PhzConfig.h`,
`PresetBusUI.h`) falls through to `-I../../software/src` and is the real header.

| file | why it is here |
|---|---|
| `Arduino.h` | the real one is Teensyduino; `software/test/host_stubs/Arduino.h` is deliberately near-empty and this TU needs `constrain`, `millis`, `FLASHMEM` |
| `OC_ui.h` | the real one drags `OC_config.h`, `ui_button.h`, the ADC. Declares the simulator's `OC::Ui` — the button state machine that mirrors `OC_ui.cpp`'s `Poll()`, implemented in `../../sim_ui.cpp` |
| `OC_apps.h` | only `OC::AppEvent` is used (by `persist_assignments`) |
| `OC_app_switcher.h` | only `app_switcher.current_app()->DispatchAppEvent()` is used |
| `OC_menus.h` | only the `graphics` global is used |
| `OC_digital_inputs.h` | the 225e last/next pulse jacks; the simulator has no trigger inputs, so they read low forever |
| `HSUtils.h` | only `HS::PokePopup` is used |

**Discipline, same as `shim/oc_shim.h`:** this is the surface `PresetBusUI.cpp`
touches and nothing more. It is not the start of a host o_C emulator. If that
file grows a dependency, shim that one thing here or stop routing it through the
simulator — do not grow this into general Teensyduino/o_C emulation.
