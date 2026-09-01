# xeno-sim -- host simulator for the Xenomorpher's UI.
#
#   make            build build/xeno-sim
#   make run        build and run it interactively, in this terminal
#   make gui        build and serve the browser front end on 127.0.0.1:8731
#   make check      the self-checks (determinism, the four hold cases)
#   make clean
#
# Nothing here is part of the firmware build: no platformio.ini env references
# this directory, and no file under software/src is modified.
#
# HOW THE FIRMWARE GETS COMPILED. build/shadow/ is a mirror of software/src/
# made of symlinks, with shim/src/ laid over it (see mkshadow.sh). The firmware
# reaches its own headers with quoted includes, which resolve against the
# directory of the including file before any -I, so an include path cannot
# shadow a firmware header for a firmware .cpp -- compiling through the mirror
# can. Everything in build/shadow/ is the real file unless shim/src/ replaces
# it, and shim/src/ holds only hardware (the OLED panel, the ADC, the core
# timers, the frequency counter, one macro in the display header) plus this
# build's app manifest.

SRC    = ../../software/src
SHIM   = shim/src
BUILD  = build
SHADOW = $(BUILD)/shadow

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Werror -O1 -g
# The firmware is built with -Wall but not -Wextra, and cleaning up warnings it
# does not have on target is not this tool's job.
CXXFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
            -Wno-deprecated-declarations -Wno-missing-braces \
            -Wno-unused-but-set-variable -Wno-format \
            -Wno-unused-private-field
INCLUDES  = -I. -Ishim/arduino -I$(BUILD) -I$(SHADOW) -I$(SHADOW)/src/extern

# What T41_console builds with, minus the parts that only mean something on the
# chip. NO_HEMISPHERE is the one real difference and it is a configuration the
# firmware supports (T41_MTP builds with it): see README.md for what it costs.
FWFLAGS = -DARDUINO_TEENSY41 -D__IMXRT1062__ -DPRESET_BUS -DNO_HEMISPHERE \
          -DENABLE_APP_BUS200E \
          -DENABLE_APP_PONG -DENABLE_APP_SCENES -DXENO_SIM

# Compiled from the mirror, so each of these is the real file unless shim/src/
# replaces it (OC_ADC.cpp, OC_core.cpp, the SH1106 driver, OC_FreqMeasure.cpp).
FW_SRCS = \
  OC_ADC.cpp \
  OC_app_base.cpp \
  OC_apps.cpp \
  OC_bitmaps.cpp \
  OC_calibration.cpp \
  OC_chords.cpp \
  OC_core.cpp \
  OC_cv_utils.cpp \
  OC_DAC.cpp \
  OC_debug.cpp \
  OC_digital_inputs.cpp \
  OC_global_settings.cpp \
  OC_gpio.cpp \
  OC_input_map.cpp \
  OC_io.cpp \
  OC_io_settings_menu.cpp \
  OC_menus.cpp \
  OC_patterns.cpp \
  OC_scale_edit.cpp \
  OC_scales.cpp \
  OC_strings.cpp \
  OC_ui.cpp \
  HSIOFrame.cpp \
  HSUtils.cpp \
  PhzConfig.cpp \
  PresetBusUI.cpp \
  PresetEngine.cpp \
  Buchla200eModuleTable.cpp \
  Buchla200eUiGate.cpp \
  Buchla200eWriteGuard.cpp \
  Buchla251eGenerator.cpp \
  Buchla251eRecorder.cpp \
  Buchla251eSlotCodec.cpp \
  Buchla259eSlotCodec.cpp \
  Bus200eMaster.cpp \
  PresetBus200e.cpp \
  src/drivers/display.cpp \
  src/drivers/SH1106_128x64_driver.cpp \
  src/drivers/FreqMeasure/OC_FreqMeasure.cpp \
  src/util/util_misc.cpp \
  src/util/util_settings.cpp \
  src/extern/bjorklund.cpp \
  src/extern/braids_quantizer.cpp

# The mirror has to exist before make can match its pattern rules against it,
# so it is built while the Makefile is read rather than as a target. It is only
# symlinks: make stats through them to the real sources, so rebuilding it every
# time costs nothing in recompilation.
$(shell mkdir -p $(BUILD) && ./mkshadow.sh $(SRC) $(SHIM) $(SHADOW) && ln -sfn ../$(SRC) $(BUILD)/real)

# weegfx.cpp is the real renderer too, but it needs one host adjustment inside
# its own translation unit -- see the header comment in shim/weegfx_host.cpp.
SIM_SRCS = main.cpp sim_bus.cpp sim_capture.cpp sim_host.cpp sim_input.cpp \
           sim_runtime.cpp sim_session.cpp sim_stubs.cpp sim_term.cpp \
           shim/weegfx_host.cpp

FW_OBJS  = $(patsubst %,$(BUILD)/fw/%.o,$(basename $(FW_SRCS)))
SIM_OBJS = $(patsubst %,$(BUILD)/%.o,$(basename $(SIM_SRCS)))

EXE = $(BUILD)/xeno-sim

.PHONY: all run gui check clean
all: $(EXE)

$(EXE): $(SIM_OBJS) $(FW_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/fw/%.o: $(SHADOW)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(FWFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(FWFLAGS) -c $< -o $@

run: all
	@$(EXE)

# The browser front end. python3 stdlib only, and it drives this same binary
# over --stdio, so `make` on its own still produces a headless simulator.
gui: all
	@python3 xeno_gui.py

check: all
	@./selfcheck.sh

clean:
	@rm -rf $(BUILD)
