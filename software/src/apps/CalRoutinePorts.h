#ifndef CALROUTINEPORTS_H_
#define CALROUTINEPORTS_H_

#include <stdint.h>

#include "../Cal259eBus.h"
#include "../CalRoutineFlow.h"

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

// Console 259e read-only access, key '=' (every letter is taken in Main.cpp's
// switch): "=<aa>s" status, "=<aa>r" record, "=<aa>a<0|1>" 8 ADC words,
// aa = the module's bus address in two hex digits. Logs CALR,259E,... lines.
// No PUT and no COMMIT. Refused with CALR,259E,ERR,BUSY_CAL while the Cal
// Routine owns the meter. Consume is called with every console byte before the
// command switch and returns true for a byte that belonged to a pending
// argument; Command is the switch's '='; Task runs once per loop pass.
bool Cal259Consume(int cmd);
void Cal259Command();
void Cal259Task();

}  // namespace CalRoutinePorts

#endif  // CALROUTINEPORTS_H_
