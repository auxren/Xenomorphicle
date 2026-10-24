// The plant behind --sim-259e: an ideal output A, a USB meter reading it, a
// backup that completes, and a 259e on the bus (software/test/sim259e.h, the
// model the host tests use). The Cal Routine's own ports (apps/
// CalRoutinePorts.cpp, XENO_SIM branches) report what output A holds and talk
// to this through CalRoutinePorts::SimPlant.
#include "apps/CalRoutinePorts.h"
#include "CalRoutineMarker.h"
#include "../../software/test/sim259e.h"

#include <stdio.h>
#include <stdlib.h>

namespace {

struct Plant : CalRoutinePorts::SimPlant {
  Sim259e module;
  double mv = 0.0;
  BusPort *Bus259e() override { return &module; }
  void OutputMv(double v) override { mv = v; }
};

Plant plant;

}  // namespace

// One line at exit for the self-check: what the plant ended up holding.
static void Report() {
  fprintf(stderr, "SIM259E,commits=%d,puts=%d,reads=%d,bad_frames=%d,out_mv=%.1f,seq=%u,slots=%u\n",
          plant.module.count[CalRec::kCmdCommit], plant.module.count[CalRec::kCmdPut],
          plant.module.count[CalRec::kCmdRead], plant.module.bad_frames, plant.mv,
          unsigned(plant.module.seq), unsigned(plant.module.slots_used));
}

// Called after the firmware has booted. The 259e target needs Output A
// calibrated by Cal Routine, which the marker records.
void SimEnable259e() {
  plant.module.clock = &CalRoutinePorts::Clock();
  plant.module.addr = 0x28;
  plant.module.adc_noise = 1.0;
  plant.module.input_mv = [](int) { return plant.mv; };
  CalRoutinePorts::SimSetPlant(&plant);
  atexit(Report);
  CalRoutinePorts::MarkerWrite(kCalRoutineMarkerStored);
}

