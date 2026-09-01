# xeno-sim -- host simulator for the Xenomorpher's "200e Modules" app.
#
#   make            build build/xeno-sim
#   make run        build and run it interactively, in this terminal
#   make gui        build and serve the browser front end on 127.0.0.1:8731
#   make clean
#
# Nothing here is part of the firmware build: no platformio.ini env references
# this directory, and no file under software/src is modified.

SRC   = ../../software/src
STUBS = ../../software/test/host_stubs
BUILD = build

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Werror -O2
INCLUDES  = -I. -I$(SRC) -I$(STUBS)

# ARDUINO_TEENSY41 + PRESET_BUS are what the real T41 firmware builds with, and
# what makes Bus200eApp.h compile its MIDI-record ISR path and reach the real
# OC::PresetBus declarations. They apply only to the TUs that include the app or
# PresetBus.h -- the firmware sources below are built exactly as the existing
# host test suites build them.
APPFLAGS = -DARDUINO_TEENSY41 -DPRESET_BUS

# Linked for real, unmodified, straight out of the firmware tree.
# The 200e preset-bus overlay -- the screen both encoder pushes open -- is
# compiled from the firmware source too, but through a symlink in shim/fw/ so
# its quoted includes land on host stand-ins instead of the display driver and
# util_math.h's ARM assembly. See shim/fw/README.md; the file itself is
# unmodified.
FW_SRCS = \
  shim/fw/PresetBusUI.cpp \
  $(SRC)/Buchla200eModuleTable.cpp \
  $(SRC)/Buchla200eUiGate.cpp \
  $(SRC)/Buchla200eWriteGuard.cpp \
  $(SRC)/Buchla251eGenerator.cpp \
  $(SRC)/Buchla251eRecorder.cpp \
  $(SRC)/Buchla251eSlotCodec.cpp \
  $(SRC)/Buchla259eSlotCodec.cpp \
  $(SRC)/Bus200eMaster.cpp \
  $(SRC)/PresetBus200e.cpp \
  $(SRC)/src/extern/bjorklund.cpp

SIM_SRCS = main.cpp sim_bus.cpp sim_capture.cpp sim_preset.cpp sim_term.cpp \
           sim_ui.cpp shim/oc_shim.cpp shim/weegfx_host.cpp

FW_OBJS  = $(patsubst %,$(BUILD)/fw_%.o,$(notdir $(basename $(FW_SRCS))))
SIM_OBJS = $(patsubst %,$(BUILD)/%.o,$(basename $(notdir $(SIM_SRCS))))

EXE = $(BUILD)/xeno-sim

.PHONY: all run gui clean
all: $(EXE)

$(EXE): $(SIM_OBJS) $(FW_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD):
	@mkdir -p $(BUILD)

# --- simulator sources (see APPFLAGS) ---
$(BUILD)/main.o: main.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(APPFLAGS) -c $< -o $@
$(BUILD)/sim_bus.o: sim_bus.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(APPFLAGS) -c $< -o $@
$(BUILD)/sim_preset.o: sim_preset.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(APPFLAGS) -c $< -o $@
$(BUILD)/sim_ui.o: sim_ui.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(APPFLAGS) -c $< -o $@
$(BUILD)/sim_capture.o: sim_capture.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(BUILD)/sim_term.o: sim_term.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(BUILD)/oc_shim.o: shim/oc_shim.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
# The real renderer. See the header comment in weegfx_host.cpp for why this one
# translation unit needs its own macro adjustments.
$(BUILD)/weegfx_host.o: shim/weegfx_host.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# --- firmware sources, built bare as the host test suites build them ---
# shim/fw first on the include path, and ONLY for this one file: that is what
# makes <Arduino.h> and the quoted OC_* includes inside PresetBusUI.cpp resolve
# to the host stand-ins. See shim/fw/README.md.
$(BUILD)/fw_PresetBusUI.o: shim/fw/PresetBusUI.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Ishim/fw $(INCLUDES) $(APPFLAGS) -c $< -o $@

$(BUILD)/fw_%.o: $(SRC)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(BUILD)/fw_%.o: $(SRC)/src/extern/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

run: $(EXE)
	@$(EXE)

# The browser front end. python3 stdlib only, and it drives this same binary
# over --stdio, so `make` on its own still produces a headless simulator.
gui: $(EXE)
	@python3 xeno_gui.py

clean:
	@rm -rf $(BUILD)
