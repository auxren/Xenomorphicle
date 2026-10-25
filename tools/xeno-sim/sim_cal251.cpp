// The plant behind --sim-251e: a 251e on the bus (software/test/sim251e.h, the model the host
// tests use, with its analog error model), the Xeno's outputs E-H as ideal DACs, and the Xeno's
// inputs 1-4 as an ideal input stage reading what the cables carry: in the loopback stage the
// held outputs E-H, afterwards the 251e's CV outputs 1-4. The Cal Routine's own ports (apps/
// CalRoutinePorts.cpp, XENO_SIM branches) tell this what each output holds and ask it to set
// the inputs' raw counts through CalRoutinePorts::SimPlant. No meter is involved.
#include "apps/CalRoutinePorts.h"
#include "CalRoutineMarker.h"
#include "sim_host.h"
#include "../../software/test/sim251e.h"

#include <stdio.h>
#include <stdlib.h>

namespace {

struct Plant : CalRoutinePorts::SimPlant {
  Sim251e module;
  double xeno_mv[8] = {};
  BusPort *Bus259e() override { return &module; }
  void OutputMv(double) override {}
  void OutputMvAt(int out, double mv) override { xeno_mv[out & 7] = mv; }
  // The input stage the 251e flow's mV port assumes: 10600 mV at raw 0, 2.918 mV per count.
  // The ADC is 12 bits, so the raw count dithers between its two neighbours (error feedback):
  // the smoothed reading then averages to the exact value, as noise does on the hardware.
  double carry[4] = {};
  void UpdateInputs() override {
    const bool loop = CalRoutinePorts::Flow251e().stage() == Cal251eFlow::Stage::Loopback;
    for (int k = 0; k < 4; ++k) {
      const double v = loop ? xeno_mv[4 + k] : module.OutMv(k);
      double raw = (10600.0 - v) / 2.918;
      raw = raw < 0.0 ? 0.0 : raw > 4095.0 ? 4095.0 : raw;
      double r = raw + carry[k];
      r = r < 0.0 ? 0.0 : r > 4095.0 ? 4095.0 : r;
      const double q = double(long(r + 0.5));
      carry[k] = r - q;
      SimCvRaw()[CalRoutinePorts::PanelInPhys(k)] = uint16_t(q * 16.0);
    }
  }
};

Plant plant;

}  // namespace

// One line at exit for the self-check: what the plant ended up holding.
static void Report() {
  fprintf(stderr, "SIM251E,commits=%d,puts=%d,reads=%d,overrides=%d,releases=%d,bad_frames=%d,"
                  "ovr_active=%d,seq=%u,has_record=%d,run_active=%d,verdict=%d\n",
          plant.module.count[CalRec::kCmdCommit], plant.module.count[CalRec::kCmdPut],
          plant.module.count[CalRec::kCmdRead], plant.module.overrides, plant.module.releases,
          plant.module.bad_frames, int(plant.module.OverrideActive()), unsigned(plant.module.seq),
          int(plant.module.has_active), int(CalRoutinePorts::CalRunActive()),
          CalRoutinePorts::Cancelled251Verdict());
}

// Called after the firmware has booted. The 251e target needs the outputs calibrated by Cal
// Routine, which the marker records.
void SimEnable251e(bool warn) {
  plant.module.clock = &CalRoutinePorts::Clock();
  plant.module.addr = 0x5C;
  if (warn) plant.module.inl_mv[1] = 9.0;   // over the write limit (non-linearity 3 mV), not gross
  CalRoutinePorts::SimSetPlant(&plant);
  atexit(Report);
  CalRoutinePorts::MarkerWrite(kCalRoutineMarkerStored);
}
