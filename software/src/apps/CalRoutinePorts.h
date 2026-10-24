#ifndef CALROUTINEPORTS_H_
#define CALROUTINEPORTS_H_

#include <stdint.h>

#include "../Cal259eBus.h"
#include "../Cal259eFlow.h"
#include "../CalRoutineFlow.h"
#include "CalRunActive.h"

// ---------------------------------------------------------------------------
// The Cal Routine app's hardware (CalRoutinePorts.cpp): the real ports the
// flow engine (CalRoutineFlow.h) measures and saves through.
//
//   Meter    usbMeter (src/drivers/USBTMCDevice.h). Connected() only once
//            the meter has answered *IDN?, which Service() asks itself.
//   Outputs  DAC::Hold / DAC::Release on the physical channel behind each
//            panel label; ReleaseAll lets go of this port's holds only.
//            Tables from calibration_data.dac.
//   Inputs   ADC::smoothed_raw_q8 of all 8 inputs, averaged by Service()
//            while a window is open.
//   Clock    millis().
//   Save     calibration_data.dac -> calibration_save(); the curve record ->
//            InputCurveSaveToFlash; ICA1 -> input_calibration_save(); the
//            marker (CalRoutineMarker.h); autotune reset; InputCal::Reload().
//
// Panel labels -> physical channels through PanelInPhys / PanelOutPhys (the
// console's mapping; CvMeter.cpp uses these too). Results arrive indexed by
// panel label and are stored physically, as every calibration table is.
//
// Loop context only. All code in flash: nothing here is ISR code, and ITCM
// grows in 32 KB steps taken out of DTCM.
// ---------------------------------------------------------------------------

namespace CalRoutinePorts {

// The one flow engine, constructed on first use in RAM2 (placement new from
// flash code, so neither its constructor nor its ~1.7 KB sit in ITCM or in
// the app container's static initialiser).
CalRoutineFlow &Flow();

int PanelInPhys(int label);   // 0..7 -> ADC_CHANNEL_n
int PanelOutPhys(int label);  // 0..7 -> DAC_CHANNEL_A..H

CalRoutine::MeterPort &Meter();
CalRoutine::OutputPort &Outputs();
CalRoutine::InputPort &Inputs();
CalRoutine::Clock &Clock();
CalRoutine::SavePort &Save();

// The app owns the meter between Open() and Close(): the console's 'e'
// refuses (METER,ERR,BUSY_CAL) and so do 'd' and 'Q' (a Q in progress is
// aborted at Open), the identity is asked for, and Service()
// samples the open input window. Close() also releases every hold this
// port took.
void Open();
void Close();
bool MeterOwned();
// Once per loop pass while open.
void Service();

enum MeterStatus : uint8_t { METER_ABSENT, METER_ASKING, METER_READY };
MeterStatus Status();
// The model field of the *IDN? reply ("SDM3065X"), "" until it is read.
const char *MeterModel();

// The step SavePort last failed at: "CURVES" (the curve file write), or ""
// (nothing failed; the DAC and ICA1 steps cannot report a failure).
const char *SaveFailedAt();

// The marker at EEPROM [4140,4144) (CalRoutineMarker.h).
bool MarkerSet();
void MarkerWrite(uint8_t flags);
// FNV-1a over every DAC table entry in calibration_data: Setup takes it when
// its wizard starts and again at its save, and clears the marker only if
// the tables changed in between.
uint32_t DacTableSum();

// The flow's CALR lines to the USB console (PRINT_DEBUG builds); a line the
// port has no room for is dropped and counted, never waited on.
void ConsoleLog(const char *line);

// True on hardware the routine's model fits: 1.2 V per octave, table entry k
// at k x 1.2 V from 0 V (Northern Light / the Xenomorpher).
bool HardwareSupported();

// The 200e bus for the 259e calibration client (Cal259eBus.h): Send is
// OC::PresetBus::SendRawFrame, PopReply is PresetBus::PopCalReply. Where the
// build has no preset bus, Send answers -2 and PopReply false.
BusPort &Bus();

// The 259e target's flow engine (Cal259eFlow.h), constructed on first use in
// DMAMEM like Flow(). The app drives it from Loop() between Open() and Close().
Cal259eFlow &Flow259e();

// The two ports the 259e flow needs besides the meter, output and bus:
//   Backup   the master BACKUP of one 259e (Bus200eMasterBackup through
//            OC::PresetBus::MasterBackup, as the console 'm' starts it); on
//            success the bank is stored with PresetEngine::SaveCalBackup
//            (CAL259BK.BIN, its own file: PBSNAP.BIN is the 200e app's undo
//            copy and is never touched). The
//            flow may abandon a job (Cancel, or its give-up): the master FSM
//            then finishes on its own, nothing is stored, and the next Begin()
//            retires the leftover DONE/FAILED or is refused while it still runs.
//   Entropy  the 8 unit-ID bytes and the record date. Not the TRNG: its
//            registers have never been brought up in this firmware, and the
//            unit ID needs uniqueness, not secrecy. Fill mixes the chip's
//            OCOTP serial (CFG0/CFG1), the cycle counter, micros(), the RTC and
//            a call counter. UnixTime is rtc_get(), or 0 when the RTC was never
//            set (before 2020).
BackupPort &Backup();
EntropyPort &Entropy();

// True from Open() to Close(): a Cal Routine run (any target, the 259e one
// included) is open, or a cancelled 259e run is still waiting for its COMMIT
// verdict (CalTask finishes it). Declared in CalRunActive.h. While it is true
// everything that can write to a 259e or restore presets refuses: the console
// '=' (no writes anyway), 'W' (PUT 0x12 and COMMIT 0x0B frames), 'm' and 'x';
// the USB SysEx bridge's GET_DUMP and PUT_DUMP (and the restore at DUMP_END);
// and the 200e app's reads and writes. (There is no SysEx PUT/COMMIT path for a
// 259e in this firmware; one added later must check this and acknowledge a COMMIT
// at once, not at the verdict, since a host's ack timeout is shorter than the
// ~4.5 s verdict window.)

// The COMMIT verdict (Cal259eBus result: 1 ok, < 0 Error) of the last 259e run that
// was cancelled after its COMMIT went out, 0 if none: the menu's banner. Does not
// construct the flow engine.
int Cancelled259Verdict();

// Once per loop pass, every build (not only PRINT_DEBUG ones, which have the
// console): finishes a cancelled 259e run's COMMIT verdict so CalRunActive()
// drops back to false. Bounded work per call.
void CalTask();

// Console 259e read-only access, key '=' (every letter is taken in Main.cpp's
// switch): "=<aa>s" status, "=<aa>r" record, "=<aa>a<0|1>" 8 ADC words,
// aa = the module's bus address in two hex digits. Logs CALR,259E,... lines.
// The `=` command has no PUT and no COMMIT (the console's raw-frame sender 'W'
// can send any frame, which main gates while CalRunActive()). Refused with CALR,259E,ERR,BUSY_CAL while the Cal
// Routine owns the meter. Consume is called with every console byte before the
// command switch and returns true for a byte that belonged to a pending
// argument; Command is the switch's '='; Task runs once per loop pass.
bool Cal259Consume(int cmd);
void Cal259Command();
void Cal259Task();

#ifdef XENO_SIM
// The simulator's plant (tools/xeno-sim/sim_cal259.cpp, flag --sim-259e): an
// ideal output A (the meter and the 259e inputs see the mV of the code held),
// a USB meter that answers READ? with it, a 259e on the bus, and a backup that
// completes. The ports tell the plant what output A holds.
struct SimPlant {
  virtual BusPort *Bus259e() = 0;
  virtual void OutputMv(double mv) = 0;
};
void SimSetPlant(SimPlant *plant);
#endif

}  // namespace CalRoutinePorts

#endif  // CALROUTINEPORTS_H_
