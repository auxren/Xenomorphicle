#ifndef CVMETER_H_
#define CVMETER_H_

// Bench CV meter, DAC hold, per-input CV calibration and frequency counter,
// all driven from the USB serial console (T41 builds with PRINT_DEBUG; see
// docs/bench-console.md and docs/CV-Meter.md). Loop context only: nothing
// here runs in an ISR, and nothing here blocks on a host that is not reading.
//
//   v              toggle the 50 Hz CV stream (all 8 inputs)
//   d<o><mV>Enter  hold output o (1-8 or a-h) at a calibrated voltage
//   d<o>x / dx     release output o / every held output
//   Q<i><o>        calibrate input i (1-8) against output o
//   Qa<o>          calibrate inputs 1..8 in turn against output o
//   Qp / Qc Qc     print / clear (double press) the per-input calibration
//   n              toggle the frequency counter on TR1

#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41) && defined(PRINT_DEBUG)
#define CVMETER_ENABLED 1

namespace OC {
namespace CvMeter {

// Once per loop() pass: sampling, stream lines, calibration steps, counter.
void Task();

// Console bytes, before the command switch. True if the byte belonged to a
// pending argument or to the calibration dialog and must not be dispatched.
bool Consume(int cmd);

// The command switch's 'v', 'd', 'Q' and 'n'. 'd' and 'Q' refuse
// (DAC,ERR,BUSY_CAL / INCAL,ERR,BUSY_CAL) while the Cal Routine owns the
// outputs (CalRoutinePorts::MeterOwned()).
void Command(char key);

// The Cal Routine is starting a run: aborts a Q in progress (releasing its
// holds) and drops a half-typed 'd' or 'Q' argument.
void CalRoutineTakeover();

// Lines for the console's 'z' help.
void PrintHelp();

}  // namespace CvMeter
}  // namespace OC

#endif
#endif  // CVMETER_H_
