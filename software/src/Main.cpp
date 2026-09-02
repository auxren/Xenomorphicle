// Copyright (c) 2015, 2016 Max Stadler, Patrick Dowling
//
// Original Author : Max Stadler
// Heavily modified: Patrick Dowling (pld@gurkenkiste.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Main startup/loop for O&C firmware

#include <Arduino.h>
#include <EEPROM.h>

#include "OC_core.h"
#include "OC_app_switcher.h"
#include "OC_apps.h"
#include "OC_DAC.h"
#include "OC_debug.h"
#include "OC_gpio.h"
#include "OC_global_settings.h"
#include "OC_ADC.h"
#include "OC_calibration.h"
#include "OC_digital_inputs.h"
#include "OC_menus.h"
#include "OC_strings.h"
#include "OC_ui.h"
#include "OC_options.h"
#include "src/drivers/display.h"
#include "src/drivers/ADC/OC_util_ADC.h"
#include "util/util_debugpins.h"
#include "VBiasManager.h"
#include "HSMIDI.h"

#include "PhzConfig.h"
#include "PresetEngine.h"
#include "PresetBus.h"
#include "PresetBusUI.h"
#include "Bus200eBridgeUsb.h"
#include "HSUtils.h"

void CaptainDumpProfiles();  // CaptainMIDI.h (global scope)
void CaptainMidiHealth();    // CaptainMIDI.h (global scope)

#if defined(ARDUINO_TEENSY41)
USBHost thisUSB;
USBHub hub1(thisUSB);
// These MUST stay in DTCM: their member arrays are the EHCI DMA buffers,
// and USBHost_t36's midi/ehci paths do NO cache maintenance - DTCM is
// non-cacheable (EHCI reaches it via the CM7 AHBS backdoor), while RAM2 is
// write-back cached and silently corrupts host MIDI both directions.
// (Stack headroom is bought by app_container living in RAM2 instead.)
MIDIDevice_BigBuffer usbHostMIDI[2] {
  MIDIDevice_BigBuffer(thisUSB),
  MIDIDevice_BigBuffer(thisUSB)
};
MIDI_CREATE_INSTANCE(HardwareSerial, Serial8, MIDI1);
#include "AudioIO.h"
#include "usb_desc.h"
#include "Wire.h"
#ifdef MULTIBOOT
#include "util/cachedisable.h"
#endif

FLASHMEM
void ScanI2C() {
  noInterrupts();

  Serial.println("...Scanning i2c addresses...");
  uint8_t error;
  for (uint8_t address = 1; address < 127; address++) {
    // The i2c_scanner uses the return value of
    // the Write.endTransmisstion to see if
    // a device did acknowledge to the address.
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    } else if (error == 4) {
      Serial.print("Unknown error at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    } //else { Serial.print("Nothing happened at address 0x"); }
  }

  interrupts();
}
#endif // ARDUINO_TEENSY41

uint_fast8_t MENU_REDRAW = true;
volatile uint32_t loop_counter = 0;   // main-loop rate, read by DebugDump
static OC::UiMode ui_mode = OC::UI_MODE_MENU;
static OC::IOFrame io_frame;

/*  ------------------------ UI timer ISR ---------------------------   */

IntervalTimer UI_timer;

void FASTRUN UI_timer_ISR() {
  OC_DEBUG_PROFILE_SCOPE(OC::DEBUG::UI_cycles);
  OC::ui.Poll();
  OC_DEBUG_RESET_CYCLES(OC::ui.ticks(), 2048, OC::DEBUG::UI_cycles);
}

/*  ------------------------ core timer ISR ---------------------------   */
IntervalTimer CORE_timer;
volatile bool OC::CORE::app_isr_enabled = false;
volatile bool OC::CORE::display_update_enabled = false;
volatile bool OC::CORE::app_loop_enabled = false;
volatile uint32_t OC::CORE::ticks = 0;

void FASTRUN CORE_timer_ISR() {
  DEBUG_PIN_SCOPE(OC_GPIO_DEBUG_PIN2);
  OC_DEBUG_PROFILE_SCOPE(OC::DEBUG::ISR_cycles);

  using namespace OC;

  // DAC and display share SPI. By first updating the DAC values, then starting
  // a DMA transfer to the display things are fairly nicely interleaved. In the
  // next ISR, the display transfer is finalized (CS update).

  display::Flush();
  DAC::Update();
  display::Update();

  // see OC_ADC.h for details; empirically (with current parameters), Scan_DMA() picks up new samples @ 5.55kHz
  OC::ADC::Scan_DMA();

  // Pin changes are tracked in separate ISRs, so depending on prio it might
  // need extra precautions. Note: This call is required to clear flags
  DigitalInputs::Scan();

  ++CORE::ticks;
  if (CORE::app_isr_enabled) {
    OC::app_switcher.Process(&io_frame);
  }

  OC_DEBUG_RESET_CYCLES(OC::CORE::ticks, 16384, OC::DEBUG::ISR_cycles);
}

/*       ---------------------------------------------------------         */

#ifdef MULTIBOOT
extern "C" {
  static void jump_to_alt(uint32_t choice) {
    const uint32_t JUMP_ADDR = 0x60000000 + (choice * 0x100000);
    uint32_t instptr = JUMP_ADDR + 0x1000 + sizeof(uint32_t);
    uint32_t instaddr = *(uint32_t*)instptr;
    ((void (*)(void))instaddr)();
  }
}

// boot-time only; noinline so FLASHMEM sticks (free-function LTO rule)
FLASHMEM __attribute__((noinline)) void BootMenu() {
  bool save = false;
  int choice = -1;

  while (true) {
    const bool z_held = OC::ui.read_immediate(OC::CONTROL_BUTTON_Z);
    const bool a_held = OC::ui.read_immediate(OC::CONTROL_BUTTON_A);
    const bool b_held = OC::ui.read_immediate(OC::CONTROL_BUTTON_B);
    const bool x_held = OC::ui.read_immediate(OC::CONTROL_BUTTON_X);
    const bool y_held = OC::ui.read_immediate(OC::CONTROL_BUTTON_Y);
    const bool any_held = (a_held || b_held || z_held || x_held || y_held);

    if (a_held) choice = 0;
    if (b_held) choice = 1;
    if (x_held) choice = 2;
    if (y_held) choice = 3;
    if (choice > -1) {
      if (OC::calibration_data.bootchoice() != choice) {
        OC::calibration_data.set_bootchoice(choice);
        if (z_held) {
          save = true;
        }
      }
      if (!any_held)
        break;
    }

    GRAPHICS_BEGIN_FRAME(true);
    graphics.setPrintPos(1, 5);
    graphics.print("USB Device Mode:");

    graphics.setPrintPos(1, 15);
    graphics.print("A: MIDI + Audio");
    if (any_held && 0 == OC::calibration_data.bootchoice()) {
      graphics.invertRect(1, 15, 127, 9);
    }
    graphics.setPrintPos(1, 25);
    graphics.print("B: MIDI");
    if (any_held && 1 == OC::calibration_data.bootchoice()) {
      graphics.invertRect(1, 25, 127, 9);
    }
    graphics.setPrintPos(1, 35);
    graphics.print("X: MTP + O_C Stock");
    if (any_held && 2 == OC::calibration_data.bootchoice()) {
      graphics.invertRect(1, 35, 127, 9);
    }
    graphics.setPrintPos(1, 45);
    graphics.print("Y: (HW Debug)");
    if (any_held && 3 == OC::calibration_data.bootchoice()) {
      graphics.invertRect(1, 45, 127, 9);
    }

    graphics.setPrintPos(1, 55);
    graphics.print("(hold Z to set)");
    GRAPHICS_END_FRAME();

    delay(10);
  }

  if (save) {
    OC::calibration_save();
  }
}
#endif

// ---------------------------------------------------------------------------
// Crash forensics + hardware watchdog (T4.x)
// ---------------------------------------------------------------------------
#if defined(__IMXRT1062__)
// The core never zeroes .bss.dma (DMAMEM is documented as uninitialized), so
// C++ objects placed there boot with garbage in any member their constructor
// doesn't touch - USBHost_t36 state machines rely on bss-zero and lock up.
// Zero the section here: ResetHandler calls this hook after the DTCM bss
// clear and BEFORE __libc_init_array (C++ ctors). .bss.dma is the first
// section in RAM2 (origin 0x20200000) and ends at _heap_start per the .ld.
extern unsigned long _heap_start;
extern "C" FLASHMEM void startup_middle_hook(void) {
  memset((void *)0x20200000, 0, (uint32_t)&_heap_start - 0x20200000u);
}

// Capture CrashReport text at boot so it can be appended to CRASH.LOG once
// LittleFS is mounted (the Serial print is gone if nobody was watching).
// buffer lives in RAM2 (DMAMEM) - DTCM stack headroom is precious,
// especially in the dbg env (a 1KB DTCM buffer here caused a real stack
// overflow into the MPU guard: DACCVIOL at the exact end of variables)
static DMAMEM char crash_buf[1024];
class BufferPrint : public Print {
public:
  char *buf = crash_buf;
  size_t len = 0;
  size_t write(uint8_t c) override {
    if (len < sizeof(crash_buf) - 1) { buf[len++] = c; buf[len] = 0; return 1; }
    return 0;
  }
};
static BufferPrint crash_capture;

// Reset cause, captured then cleared at boot: SRSR bits are sticky w1c and
// would otherwise show every cause since the last power-on, forever.
//
// SRC_SRSR bit 1 is named lockup_sysresetreq for a reason: per the RT1060
// RM it is set by a CPU lockup *OR* by any software write of SYSRESETREQ to
// SCB_AIRCR -- and the Teensy core's own default fault handler
// (unused_interrupt_vector in startup.c) ends with exactly that write after
// parking for ~8s. So 0x02 alone does NOT mean "CPU lockup"; it means
// "lockup or somebody asked for a reset". SRC_GPR5 is the discriminator the
// core itself uses (CrashReport.cpp): the fault handler stamps 0x0BAD00F1
// there on its way out. Capture it here, BEFORE anything reads/clears it --
// CrashReportClass::clear() (called at the END of printTo!) zeroes both
// SRC_GPR5 and SRC_SRSR.
static uint32_t boot_srsr = 0;
static uint32_t boot_gpr5 = 0;
static constexpr uint32_t kFaultRebootMarker = 0x0BAD00F1;

// ---- stack low-water instrumentation ---------------------------------------
// DTCM stack on this board is whatever is left of RAM1 after ITCM banks and
// variables -- currently ~8.6KB in the dbg env, and the ld/MPU put a 32-byte
// NOACCESS guard at _ebss to trap an overflow. A stack that dips into that
// guard while an interrupt is being taken faults *during exception stacking*,
// which escalates and genuinely does lock the core up -- indistinguishable
// from a software reset by SRSR alone, which is exactly why this needs
// measuring rather than arguing about. Paint the unused region at the end of
// setup(), then report the deepest word ever touched.
extern unsigned long _ebss;
static constexpr uint32_t kStackPaint = 0xA5C3A5C3u;
static uint32_t *stack_paint_lo = nullptr;
static uint32_t *stack_paint_hi = nullptr;
FLASHMEM static void stack_paint() {
  uint32_t sp;
  asm volatile("mov %0, sp" : "=r"(sp));
  // start just above the MPU guard region that sits at _ebss
  uint32_t *lo = (uint32_t *)((((uintptr_t)&_ebss) + 32u + 31u) & ~31u);
  uint32_t *hi = (uint32_t *)((sp - 512u) & ~3u);   // leave our own frame be
  if (hi <= lo) return;
  // IRQs off: a nested ISR's frame can reach below sp-512, and painting over
  // a live exception frame would be a spectacular own goal. ~8KB of stores.
  __disable_irq();
  for (uint32_t *p = lo; p < hi; ++p) *p = kStackPaint;
  stack_paint_lo = lo;
  stack_paint_hi = hi;
  __enable_irq();
}
// Bytes of stack never touched since boot. 0 means the paint is gone: the
// stack has been at least this deep and the guard is in play.
uint32_t stack_low_water() {
  if (!stack_paint_lo) return 0xFFFFFFFFu;  // not painted yet
  const uint32_t *p = stack_paint_lo;
  while (p < stack_paint_hi && *p == kStackPaint) ++p;
  return (uint32_t)((const uint8_t *)p - (const uint8_t *)stack_paint_lo);
}

// WDOG1: 128s timeout, fed only from loop(). Long enough that the slowest
// legitimate blocking op (a full 4MB LittleFS format, ~45s) never trips it;
// a hard loop() hang (the K-Board-lockup class of bug) reboots instead of
// bricking the module until power-cycle, and boot recall restores state.
// Armed at the END of setup() so interactive boot flows (BootMenu,
// ConfirmReset) can block indefinitely.
static bool watchdog_armed = false;
FLASHMEM static void watchdog_arm() {
  // the core never ungates WDOG1's clock ("WDOG1 requires CCM_CCGR3_WDOG1"
  // per imxrt.h) - touching its registers without this bus-faults
  CCM_CCGR3 |= CCM_CCGR3_WDOG1(CCM_CCGR_ON);
  asm volatile("dsb");
  // PDE (set at reset) is a one-shot 16s power-down counter that asserts
  // reset unless cleared - with the clock just ungated it would fire ~16s
  // after arming and masquerade as a watchdog timeout
  WDOG1_WMCR = 0;
  WDOG1_WCR = (uint16_t)(255u << 8)  // WT: (255+1)*0.5s = 128s
            | WDOG_WCR_WDE | WDOG_WCR_SRS | WDOG_WCR_WDA
            | WDOG_WCR_WDBG | WDOG_WCR_WDZST;
  watchdog_armed = true;
}
// Not static: PresetEngine.cpp (SaveSlot/RecallSlot) feeds it directly
// between their own sequential LittleFS writes -- see the extern there.
void watchdog_feed() {
  WDOG1_WSR = 0x5555;
  WDOG1_WSR = 0xAAAA;
}

// A full 4MB LittleFS format can exceed 128s worst-case (flash sector
// erase is 400ms MAX, ~1024 sectors) - the one legitimate loop-blocking
// op longer than the watchdog. Feed from a timer for its duration only.
static IntervalTimer wdog_format_feeder;
static void watchdog_feed_isr() { watchdog_feed(); }  // ISR: stays in ITCM
FLASHMEM static void watchdog_feed_during(void (*op)()) {
  wdog_format_feeder.begin(watchdog_feed_isr, 1000000);  // 1s
  op();
  wdog_format_feeder.end();
}
#endif

FLASHMEM void setup() {
  delay(50);
#if defined(__IMXRT1062__)
  boot_srsr = SRC_SRSR;
  boot_gpr5 = SRC_GPR5;  // 0x0BAD00F1 = the core's fault handler rebooted us
  SRC_SRSR = boot_srsr;  // w1c: next boot reports only its own cause
#endif
  Serial.begin(9600);

  if (CrashReport) {
    while (!Serial && millis() < 3000) ; // wait
#if defined(__IMXRT1062__)
    // CAPTURE FIRST, then echo the buffer. CrashReportClass::printTo() calls
    // clear() on its way out (Teensy core), so the SECOND print of
    // CrashReport only ever yields "No Crash Data To Report" -- which is
    // exactly what CRASH.LOG has been recording, and why two rounds of
    // debugging had no fault data to work from. Also stamp the decoded reset
    // cause in: printTo()'s own "Reboot was caused by..." lines read
    // SRC_SRSR, which we cleared three lines above, so they never fire.
    crash_capture.printf("reset_cause SRC_SRSR=%08lX SRC_GPR5=%08lX (%s)\n",
                         boot_srsr, boot_gpr5,
                         (boot_srsr & (1u << 4)) ? "WDOG timeout"
                         : !(boot_srsr & (1u << 1)) ? "power-on / external"
                         : (boot_gpr5 == kFaultRebootMarker)
                               ? "fault handler rebooted (report below)"
                               : "CPU LOCKUP or software SYSRESETREQ");
    crash_capture.print(CrashReport);
    Serial.write((const uint8_t *)crash_capture.buf, crash_capture.len);
    Serial.println();
#else
    Serial.println(CrashReport);
#endif
    delay(1500);
  }
#if defined(__IMXRT1062__)
  else if (boot_srsr & (1u << 1)) {
    // Bit 1 with no fault report at all: either a genuine CPU lockup (no
    // handler ever ran, so nothing was recorded) or a deliberate software
    // reset. Record it -- this is the case the preset-bus Store crash has
    // been landing in, and it deserves a line in CRASH.LOG either way.
    crash_capture.printf("reset_cause SRC_SRSR=%08lX SRC_GPR5=%08lX "
                         "(%s, no fault report captured)\n",
                         boot_srsr, boot_gpr5,
                         (boot_gpr5 == kFaultRebootMarker)
                             ? "fault handler rebooted"
                             : "CPU LOCKUP or software SYSRESETREQ");
  }
#endif

  #if defined(ARDUINO_TEENSY41)
  OC::Pinout_Detect();
  #endif
#if defined(__MK20DX256__)
  NVIC_SET_PRIORITY(IRQ_PORTB, 0); // TR1 = 0 = PTB16
#endif
  SPI_init();
  SERIAL_PRINTLN("* O&C BOOTING...");
  SERIAL_PRINTLN("* %s", OC::Strings::VERSION);

  OC::DEBUG::Init();
  OC::DigitalInputs::Init();

#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
  if (DAC8568_Uses_SPI) {
    // DAC8568 Vref does not turn on by default like DAC8565
    // best to turn on Vref as early as possible for analog
    // circuitry to settle
    OC::DAC::DAC8568_Vref_enable();
  }
  if (ADC33131D_Uses_FlexIO) {
    // ADC33131D wants calibration for Vref, takes ~1150 ms
    OC::ADC::ADC33131D_Vref_calibrate();
  } else {
#endif
    delay(400);
#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
  }
#endif

  OC::calibration_load();
  OC::SetFlipMode(OC::calibration_data.flipcontrols());

#if defined(ARDUINO_TEENSY41)
  Wire.begin();
  Wire.setClock(100000);
#endif

  OC::ADC::Init(&OC::calibration_data.adc, OC::calibration_data.flipcontrols());
  OC::ADC::Init_DMA();
  OC::DAC::Init(&OC::calibration_data.dac, &OC::global_settings.autotune_calibration_data, OC::calibration_data.flipcontrols());

  display::AdjustOffset(OC::calibration_data.display_offset);
  display::SetFlipMode( OC::calibration_data.flipscreen() );
  display::Init();

  GRAPHICS_BEGIN_FRAME(true);
  GRAPHICS_END_FRAME();

  OC::ui.Init();
  OC::ui.configure_encoders(OC::calibration_data.encoder_config());

  SERIAL_PRINTLN("* CORE ISR @%luus", OC_CORE_TIMER_RATE);
  io_frame.Reset();
  CORE_timer.begin(CORE_timer_ISR, OC_CORE_TIMER_RATE);
  CORE_timer.priority(OC_CORE_TIMER_PRIO);

  // Wait until there's at least some ADC values read
  delay(4);
  uint32_t random_seed =
      OC::ADC::raw_value(ADC_CHANNEL_1) * OC::ADC::raw_value(ADC_CHANNEL_2) +
      OC::ADC::raw_value(ADC_CHANNEL_3) + OC::ADC::raw_value(ADC_CHANNEL_4);
  randomSeed(random_seed);

  SERIAL_PRINTLN("* UI ISR @%luus", OC_UI_TIMER_RATE);
  UI_timer.begin(UI_timer_ISR, OC_UI_TIMER_RATE);
  UI_timer.priority(OC_UI_TIMER_PRIO);

  // first sign of life
  GRAPHICS_BEGIN_FRAME(true);
  graphics.setPrintPos(1, 28);
  graphics.print("*Main Screen Turn On*");
  GRAPHICS_END_FRAME();

#if defined(ARDUINO_TEENSY41)
  // Standard MIDI I/O on Serial8, only for Teensy 4.1
  if (MIDI_Uses_Serial8) {
    Serial8.begin(31250);
    MIDI1.begin(MIDI_CHANNEL_OMNI);
  }
  // USB Host support for 4.1 only
  thisUSB.begin();
#endif

#ifdef MULTIBOOT
  delay(100);
  if (OC::ui.read_immediate(OC::CONTROL_BUTTON_Z)) {
    BootMenu();
  }

  if (OC::calibration_data.bootchoice() == 3) {
    for (int i = 0; i < DAC_CHANNEL_COUNT; ++i) {
      // -3V to +4V
      OC::DAC::set_octave(DAC_CHANNEL(i), i-3);
    }
    OC::ui.DebugStats();
  } else if (OC::calibration_data.bootchoice()) {
    GRAPHICS_BEGIN_FRAME(true);
    graphics.setPrintPos(1, 28);
    graphics.print("Switching to alt mode!");
    GRAPHICS_END_FRAME();
    AudioNoInterrupts();
    delay(10);
    disableCache();
    jump_to_alt(OC::calibration_data.bootchoice());
  }
#endif

  // --- more hardware init
#if defined(ARDUINO_TEENSY41)
  // this takes a couple seconds to timeout if no card
  SDcard_Ready = SD.begin(BUILTIN_SDCARD);

  if (I2S2_Audio_ADC && I2S2_Audio_DAC) {
    OC::AudioIO::Init();
  }
#endif

  // initialize LittleFS for config files
  PhzConfig::Init();

  // Display loading splash screen and optional calibration
  bool reset_settings = false;
  ui_mode = OC::ui.Splashscreen(reset_settings, 0);

  bool start_cal = false;
  if (ui_mode == OC::UI_MODE_CALIBRATE) {
    start_cal = true;
    ui_mode = OC::UI_MODE_MENU;
  }
  OC::ui.set_screensaver_timeout(OC::calibration_data.screensaver_timeout);

#ifdef VOR
  VBiasManager *vbias_m = vbias_m->get();
  vbias_m->SetState(VBiasManager::BI);
#endif

  bool firstrun = false;
#ifdef __IMXRT1062__
  // use default global config file in LFS
  firstrun = !PhzConfig::load_config();
  if (firstrun) {
    // GLOBALS.CFG missing/corrupt: try the boot-time backup before the
    // ConfirmReset flow gets a chance to threaten a factory wipe.
    if (PhzConfig::load_config(PhzConfig::BACKUP_FILENAME)) {
      Serial.println("CONFIG: GLOBALS.CFG bad; restored from GLOBALS.BAK");
      PhzConfig::save_config();  // re-materialize the primary from memory
      firstrun = false;
    }
  } else {
    PhzConfig::backup_config();  // known-good primary: refresh the backup
  }

  // append any captured crash report to CRASH.LOG (rotate at 8KB)
  if (crash_capture.len) {
    File cl = PhzConfig::myfs.open("CRASH.LOG", FILE_READ);
    const bool rotate = cl && cl.size() > 8192;
    if (cl) cl.close();
    if (rotate) {  // keep one generation of history instead of deleting
      PhzConfig::myfs.remove("CRASH.OLD");
      PhzConfig::myfs.rename("CRASH.LOG", "CRASH.OLD");
    }
    cl = PhzConfig::myfs.open("CRASH.LOG", FILE_WRITE);  // append
    if (cl) {
      cl.printf("--- boot @ %lu ms ---\n", millis());
      cl.write((const uint8_t *)crash_capture.buf, crash_capture.len);
      cl.close();
      Serial.println("CrashReport appended to CRASH.LOG");
    }
  }
#endif

  // initialize apps (on T3.x firstrun is detected by the EEPROM load inside)
  firstrun |= !OC::app_switcher.Init(reset_settings || firstrun);
#if defined(ARDUINO_TEENSY41) && defined(AUDIO_INTERFACE)
  // Force the audio output path (I2S codec out + host-playback monitor mix)
  // into existence. It is lazily built and was only ever constructed when an
  // audio applet wired up the chain - an appletless boot had DEAD panel outs
  // and no USB monitoring. Called here so it is created after every other
  // stream (its documented ordering requirement).
  OC::AudioIO::OutputStream();
#endif

  OC::PresetEngine::Init();
  OC::PresetBus::Init();
  OC::PresetBusUI::Init();
  // browser <-> 200e preset bridge: registers a usbMIDI SysEx handler that
  // rides along with whatever already polls the port (see
  // Bus200eBridgeUsb.h). No bus traffic until a host asks for some.
  OC::Bus200eBridgeUsb::Init();
  HS::LoadClockRouting();  // GLOBALS.CFG is still the loaded map here
  // restores the last bus preset on any T4.1 (bench units included);
  // no bus traffic is emitted, so non-bus hardware is unaffected
  OC::PresetEngine::BootRecall();

  // Welcome splash
  OC::ui.Splashscreen(firstrun, 1);

  if (start_cal)
    OC::start_calibration();

  OC::app_switcher.current_app()->DispatchAppEvent(OC::APP_EVENT_RESUME);

#if defined(__IMXRT1062__)
  // paint the unused stack before loop() takes over, so 't' (and the
  // post-save report in PresetEngine) can say how close DTCM ever came to
  // the MPU guard instead of anyone having to guess
  stack_paint();

  // last: everything interactive that can legitimately block forever is
  // behind us, and loop() takes over feeding from here
  watchdog_arm();
  SERIAL_PRINTLN("* WDOG1 armed (128s, fed from loop)");
#endif

  SERIAL_PRINTLN("[End of setup()]");
}

/*  ---------    main loop  --------  */

// loop() is the slow path (drawing, UI events, deferred work) — all the
// real-time work happens in the CORE/UI timer ISRs. Run it from cached
// flash instead of burning ~4KB of ITCM (it gets inlined into main()).
#if defined(__IMXRT1062__)
// console 't': one-shot system health report
#ifdef AUDIO_INTERFACE
#include "extern/f32/AudioStream_F32.h"
#ifdef ARDUINO_TEENSY41
#include "Audio/USB_F32.h"
#endif
#endif
// TEMPORARY bench diagnostic: name the physical buttons. The pin tables in
// OC_gpio.cpp branch on the hardware ID voltage and the variants disagree
// about which pin is X vs Z, so read the panel rather than the source.
// Watches for ~20s and prints every press/release with its control name.
FLASHMEM __attribute__((noinline)) static void ButtonWatch() {
  using namespace OC;
  static const struct { UiControl c; const char *name; } kBtns[] = {
    { CONTROL_BUTTON_UP,    "UP / A"      },
    { CONTROL_BUTTON_DOWN,  "DOWN / B"    },
    { CONTROL_BUTTON_L,     "encL (left encoder push)"  },
    { CONTROL_BUTTON_R,     "encR (right encoder push)" },
    { CONTROL_BUTTON_M,     "M / Z"       },
    { CONTROL_BUTTON_UP2,   "UP2 / X"     },
    { CONTROL_BUTTON_DOWN2, "DOWN2 / Y"   },
  };
  const int n = (int)(sizeof(kBtns) / sizeof(kBtns[0]));
  bool prev[7] = { false, false, false, false, false, false, false };
  Serial.println("=== button watch: press each button (20s) ===");
  const uint32_t t0 = millis();
  while (millis() - t0 < 20000) {
    for (int i = 0; i < n; ++i) {
      const bool now = ui.read_immediate(kBtns[i].c);
      if (now != prev[i]) {
        Serial.printf("  %-28s %s\n", kBtns[i].name, now ? "PRESSED" : "released");
        prev[i] = now;
      }
    }
    watchdog_feed();
    delay(5);
  }
  Serial.println("=== button watch done ===");
}

extern char _heap_end[], *__brkval;
FLASHMEM __attribute__((noinline)) static void SelfTest() {
  Serial.println("=== selftest ===");
  // SRC_SRSR (i.MX RT1060 RM 21.6.3): bit 1 = lockup_sysresetreq, bit 4 =
  // wdog_rst_b, bit 5 = JTAG. Bit 1 is AMBIGUOUS by design -- CPU lockup OR
  // a software write of SYSRESETREQ, which is how the core's own fault
  // handler reboots. SRC_GPR5 == 0x0BAD00F1 is the core's discriminator
  // (see CrashReport.cpp): stamped by that handler, so bit 1 WITHOUT it is
  // the only reading that actually means lockup. Both captured at boot.
  Serial.printf("uptime=%lus  reset_cause(SRC_SRSR@boot)=%08lX gpr5=%08lX%s%s\n",
                millis() / 1000, boot_srsr, boot_gpr5,
                !(boot_srsr & (1 << 1)) ? ""
                : (boot_gpr5 == kFaultRebootMarker) ? " [FAULT-REBOOT]"
                                                    : " [LOCKUP or SW-RESET]",
                (boot_srsr & (1 << 4)) ? " [WDOG]" : "");
  Serial.printf("watchdog: %s (128s, fed from loop)\n",
                watchdog_armed ? "armed" : "OFF");
  Serial.printf("stack: %lu bytes never touched (of ~%lu free; 0 = guard hit)\n",
                (unsigned long)stack_low_water(),
                (unsigned long)((uintptr_t)stack_paint_hi
                                - (uintptr_t)stack_paint_lo));
  {
    static uint32_t last_lc = 0, last_ms = 0;
    const uint32_t lc = loop_counter, ms = millis();
    if (last_ms && ms != last_ms)
      Serial.printf("loop rate ~%lu Hz\n", (lc - last_lc) * 1000 / (ms - last_ms));
    last_lc = lc; last_ms = ms;
    const uint32_t t0 = OC::CORE::ticks;
    delay(5);
    Serial.printf("core delta5ms=%lu (expect ~83)\n", OC::CORE::ticks - t0);
  }
  Serial.printf("heap free: %lu bytes (RAM2)\n",
                (unsigned long)(_heap_end - __brkval));
#ifdef AUDIO_INTERFACE
  // integer tenths: %f would drag the float-printf tables into DTCM
  Serial.printf("audio i16 pool: %u now / %u max   cpu: %lu.%lu%% now / %lu.%lu%% max\n",
                AudioMemoryUsage(), AudioMemoryUsageMax(),
                (unsigned long)(AudioProcessorUsage() * 10) / 10,
                (unsigned long)(AudioProcessorUsage() * 10) % 10,
                (unsigned long)(AudioProcessorUsageMax() * 10) / 10,
                (unsigned long)(AudioProcessorUsageMax() * 10) % 10);
  Serial.printf("audio f32 pool: %u now / %u max\n",
                AudioStream_F32::f32_memory_used, AudioStream_F32::f32_memory_used_max);
#ifdef ARDUINO_TEENSY41
  // All four must stay 0. Non-zero = the USB audio transport handed an ISR
  // callback a ring index or block pointer it had already invalidated, which
  // is what used to hard-fault inside copy_to_buffers after a preset Store.
  // See the note at the top of src/Audio/USB_F32.cpp.
  Serial.printf("usb f32 guards: rx idx=%lu null=%lu  tx idx=%lu null=%lu\n",
                (unsigned long)usb_audio_f32_guards.rx_bad_index,
                (unsigned long)usb_audio_f32_guards.rx_null_block,
                (unsigned long)usb_audio_f32_guards.tx_bad_index,
                (unsigned long)usb_audio_f32_guards.tx_null_block);
#endif
#endif
  {
    // %llu is unsupported by Print::printf (prints literal "lu")
    Serial.printf("littlefs: %luKB/%luKB used\n",
                  (unsigned long)(PhzConfig::myfs.usedSize() >> 10),
                  (unsigned long)(PhzConfig::myfs.totalSize() >> 10));
    // write-verify: the failure mode where writes "succeed" as 0-byte files
    const char *tf = "SELFTST.TMP";
    uint8_t pat[64], chk[64];
    for (unsigned i = 0; i < sizeof(pat); ++i) pat[i] = (uint8_t)(i * 37 + 5);
    PhzConfig::myfs.remove(tf);
    File f = PhzConfig::myfs.open(tf, FILE_WRITE_BEGIN);
    bool ok = f && f.write(pat, sizeof(pat)) == sizeof(pat);
    if (f) f.close();
    if (ok) {
      f = PhzConfig::myfs.open(tf, FILE_READ);
      ok = f && f.read(chk, sizeof(chk)) == sizeof(chk)
             && memcmp(pat, chk, sizeof(pat)) == 0;
      if (f) f.close();
    }
    PhzConfig::myfs.remove(tf);
    Serial.printf("fs write-verify: %s\n", ok ? "PASS" : "FAIL");
    f = PhzConfig::myfs.open("CRASH.LOG", FILE_READ);
    if (f) {
      Serial.printf("CRASH.LOG present: %lu bytes (crashes recorded)\n",
                    (unsigned long)f.size());
      f.close();
    } else {
      Serial.println("CRASH.LOG: none (no crashes recorded)");
    }
  }
#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)
  {
    const OC::PresetBus::Stats &s = OC::PresetBus::GetStats();
    Serial.printf("bus rings hw: ev=%lu midi_rx=%lu midi_tx=%lu  stuck=%lu/%lu\n",
                  s.ring_hw, s.midi_rx_hw, s.midi_tx_hw,
                  s.bus_recovered, s.bus_stuck);
  }
#endif
#if defined(ARDUINO_TEENSY41)
  CaptainMidiHealth();
#endif
  Serial.println("=== selftest done ===");
}
#endif

FLASHMEM __attribute__((noinline)) void loop() {
  using namespace OC;
  CORE::app_isr_enabled = true;
  CORE::display_update_enabled = true;
  CORE::app_loop_enabled = true;
  uint32_t menu_draw_count = 0;
  uint32_t last_redraw_time = 0;

  while (true) {
    ++loop_counter;
#if defined(__IMXRT1062__)
    watchdog_feed();  // a wedged loop() now reboots instead of bricking
#endif
#if defined(ARDUINO_TEENSY41)
    thisUSB.Task();
#endif

    // Refresh display
    if (MENU_REDRAW && CORE::display_update_enabled) {
      GRAPHICS_BEGIN_FRAME(false); // Don't busy wait

      if (UI_MODE_APP_SETTINGS == ui_mode) {
        // Only draw the App menu here...
        // Handle events and process state changes elsewhere.
        ui.AppSettings(true);

      } else if (OC::PresetBusUI::Active()) {
        OC::PresetBusUI::Draw();
      } else { // if (UI_MODE_MENU == ui_mode) {
        OC_DEBUG_RESET_CYCLES(menu_draw_count, 512, DEBUG::MENU_draw_cycles);
        OC_DEBUG_PROFILE_SCOPE(DEBUG::MENU_draw_cycles);
        app_switcher.current_app()->Draw(ui_mode);
        ++menu_draw_count;
#ifdef VOR
        // TODO: move this into AppBase
        // only if not screensaver
        VBiasManager *vbias_m = vbias_m->get();
        vbias_m->DrawPopupPerhaps();
#endif
      }

      MENU_REDRAW = 0;
      last_redraw_time = ui.ticks();
      GRAPHICS_END_FRAME();
    }

    // Run current app
    if (CORE::app_loop_enabled)
      app_switcher.current_app()->DispatchLoop();

    // Take care of queued tasks
    OC::CORE::FlushTasks();
    OC::PresetEngine::Process();
    OC::PresetBus::Task();
    OC::PresetBusUI::Task();
    OC::Bus200eBridgeUsb::Task();
    HS::ClockRoutingPump();

    // UI events
    if (UI_MODE_APP_SETTINGS == ui_mode) {
      if (!ui.AppSettings(false)) {
        // exit menu, resume app
        ui_mode = UI_MODE_MENU;
      }
    } else {
      UiMode mode = ui.DispatchEvents(app_switcher.current_slot());

      // State transition for app
      if (mode != ui_mode) {
        if (UI_MODE_SCREENSAVER == mode)
          app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SCREENSAVER_ON);
        else if (UI_MODE_SCREENSAVER == ui_mode)
          app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SCREENSAVER_OFF);
        else if (UI_MODE_APP_SETTINGS == mode)
          app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SUSPEND);

        ui_mode = mode;
      }
    }

    if (ui.ticks() - last_redraw_time > REDRAW_TIMEOUT_MS)
      MENU_REDRAW = 1;

#ifdef MTP_INTERFACE
    // handle MTP Disk requests
    MTP.loop();
#endif

    static size_t cap_idx = 0;
    static elapsedMicros cap_send_time = 0;
    // check for request from PC to capture the screen
    if (Serial && Serial.available() > 0) {
      bool capreq = false;
      // Console lock: hosts like the Jetson's ModemManager AT/MBIM-probe every
      // new CDC port, and that byte soup has hit real commands ('D' froze the
      // display, '(' fired preset saves, 'i'/'C'/'F' are worse). Ignore all
      // input until the literal sequence "pew!" arrives.
      static bool console_unlocked = false;
      static uint32_t unlock_shift = 0;
      static uint32_t destructive_arm_ms = 0;  // C/F double-press confirm
      static char destructive_arm_key = 0;
      // 'm' bench trigger: master a BACKUP against a foreign module. The
      // address isn't known ahead of time (this is the empirical-discovery
      // step), so it's typed as 2 hex digits right after the key -- no
      // blocking Serial read, just two more passes through this same loop
      // with the next bytes captured as digits instead of dispatched.
      static bool master_addr_pending = false;
      static uint8_t master_addr_digits = 0;
      static uint8_t master_addr_value = 0;
      // 'q' bench trigger: master a QUERY at a foreign module and print the
      // version string it answers with -- the direct way to find out WHICH
      // module is at an address (a BACKUP only proves something is there).
      // Same 2-hex-digit, no-blocking-read convention as 'm' above.
      static bool query_addr_pending = false;
      static uint8_t query_addr_digits = 0;
      static uint8_t query_addr_value = 0;
      // 'S' bench trigger: bus-wide broadcast SAVE to a specific slot
      // (0-29), typed as 2 DECIMAL digits right after the key -- same
      // no-blocking-read pattern as master_addr_pending above.
      static bool save_slot_pending = false;
      static uint8_t save_slot_digits = 0;
      static uint8_t save_slot_value = 0;
      // 'R' bench trigger: bus-wide broadcast RECALL from a specific slot
      // (0-29), same 2-DECIMAL-digit convention as 'S'.
      // LOCAL slot save/recall by number. The bench had only '(' and ')' for
      // slot 0 and '{' '}' for slot 1, which is useless on a module whose
      // low slots hold real presets: there was no way to exercise the slot
      // writer without overwriting somebody's work. These touch this module's
      // own storage only -- no bus traffic, nothing broadcast.
      static bool lsave_pending = false;
      static uint8_t lsave_digits = 0;
      static uint8_t lsave_value = 0;
      static bool lrecall_pending = false;
      static uint8_t lrecall_digits = 0;
      static uint8_t lrecall_value = 0;
      static bool recall_slot_pending = false;
      static uint8_t recall_slot_digits = 0;
      static uint8_t recall_slot_value = 0;
      // 'w' bench trigger: patch ONE byte in the resident card image (the
      // last MasterBackup capture) -- 4 hex digits for the offset (0-0xFFFF,
      // covers the whole card), then 2 hex digits for the new byte value.
      // Read-modify-verify only: never touches the bus by itself. Pairs
      // with 'x' below to actually push the patched image back out.
      static bool patch_pending = false;
      static uint8_t patch_digits = 0;
      static uint16_t patch_offset = 0;
      static uint8_t patch_value = 0;
      // 'x' bench trigger: MasterRestore() the resident (possibly just-'w'-
      // patched) card image out to a foreign module -- the first time this
      // codebase has ever pushed bytes TO a real module rather than only
      // reading them. Same double-press-within-3s guard as C/F/Z, on
      // purpose: this is the one bench command that can actually change
      // what a real, physical Buchla module has stored. 2 hex digits for
      // the target address, same convention as 'm'/'q'.
      static bool restore_addr_pending = false;
      static uint8_t restore_addr_digits = 0;
      static uint8_t restore_addr_value = 0;
      do {
        int cmd = Serial.read();
        if (!console_unlocked) {
          unlock_shift = (unlock_shift << 8) | (uint8_t)cmd;
          if (unlock_shift == 0x70657721) {  // "pew!"
            console_unlocked = true;
            Serial.println("-=[ console unlocked ]=-");
          }
          continue;
        }
        if (master_addr_pending) {
          int v = -1;
          if (cmd >= '0' && cmd <= '9') v = cmd - '0';
          else if (cmd >= 'a' && cmd <= 'f') v = cmd - 'a' + 10;
          else if (cmd >= 'A' && cmd <= 'F') v = cmd - 'A' + 10;
          if (v < 0) {
            Serial.println("master backup: cancelled (not a hex digit)");
            master_addr_pending = false;
            continue;
          }
          master_addr_value = (uint8_t)((master_addr_value << 4) | v);
          if (++master_addr_digits < 2) continue;  // wait for the 2nd digit
          master_addr_pending = false;
#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
          Serial.printf("master backup: targeting module %02X\n",
                        master_addr_value);
          // Same discipline as StartRead(): retire whatever the last job
          // left behind, or the master FSM refuses this one as busy.
          OC::PresetBus::MasterReset();
          const int rc = OC::PresetBus::MasterBackup(master_addr_value);
          Serial.printf("  MasterBackup() returned %d (0=accepted; "
                        "watch 'b' for progress)\n", rc);
#endif
          continue;
        }
        if (query_addr_pending) {
          int v = -1;
          if (cmd >= '0' && cmd <= '9') v = cmd - '0';
          else if (cmd >= 'a' && cmd <= 'f') v = cmd - 'a' + 10;
          else if (cmd >= 'A' && cmd <= 'F') v = cmd - 'A' + 10;
          if (v < 0) {
            Serial.println("query: cancelled (not a hex digit)");
            query_addr_pending = false;
            continue;
          }
          query_addr_value = (uint8_t)((query_addr_value << 4) | v);
          if (++query_addr_digits < 2) continue;  // wait for the 2nd digit
          query_addr_pending = false;
#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
          Serial.printf("query: asking module %02X who it is\n",
                        query_addr_value);
          // Clear a terminal state left by the PREVIOUS query before starting
          // this one -- exactly what AppBus200e::StartProbe does, and what
          // this path was missing. A query to an address nobody answers ends
          // FAILED/NO_RESPONSE and stays there, so the next MasterQuery is
          // refused and so is every one after it. Found scanning the bus: the
          // first address in the sweep was empty, and all sixty that followed
          // reported nothing while the modules were sitting right there.
          OC::PresetBus::MasterQueryReset();
          const int qrc = OC::PresetBus::MasterQuery(query_addr_value);
          // The answer is a separate bus frame arriving milliseconds later,
          // so it can't be printed here: PresetBus::Task() prints it (or the
          // timeout) as soon as it lands. 'b' shows the same result on demand.
          Serial.printf("  MasterQuery() returned %d (0=accepted; the reply "
                        "prints itself when it arrives)\n", qrc);
#endif
          continue;
        }
        if (lsave_pending || lrecall_pending) {
          const bool saving = lsave_pending;
          uint8_t &digits = saving ? lsave_digits : lrecall_digits;
          uint8_t &value  = saving ? lsave_value  : lrecall_value;
          if (cmd < '0' || cmd > '9') {
            Serial.printf("local %s: cancelled (not a decimal digit)\n",
                          saving ? "save" : "recall");
            lsave_pending = lrecall_pending = false;
            continue;
          }
          value = (uint8_t)(value * 10 + (cmd - '0'));
          if (++digits < 2) continue;
          lsave_pending = lrecall_pending = false;
          if (value > 29) {
            Serial.printf("local %s: slot %d out of range (0-29)\n",
                          saving ? "save" : "recall", value);
            continue;
          }
          Serial.printf("local %s slot %d\n", saving ? "save" : "recall", value);
          if (saving) OC::PresetEngine::RequestSave(value);
          else        OC::PresetEngine::RequestRecall(value);
          continue;
        }
        if (save_slot_pending) {
          if (cmd < '0' || cmd > '9') {
            Serial.println("broadcast save: cancelled (not a decimal digit)");
            save_slot_pending = false;
            continue;
          }
          save_slot_value = (uint8_t)(save_slot_value * 10 + (cmd - '0'));
          if (++save_slot_digits < 2) continue;  // wait for the 2nd digit
          save_slot_pending = false;
          if (save_slot_value > 29) {
            Serial.printf("broadcast save: slot %d out of range (0-29), "
                          "cancelled\n", save_slot_value);
            continue;
          }
#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)
          Serial.printf("broadcast SAVE: slot %d -- every remote-enabled "
                        "module on the bus stores its current state here "
                        "now\n", save_slot_value);
          OC::PresetBus::BroadcastSave(save_slot_value);
#endif
          continue;
        }
        if (recall_slot_pending) {
          if (cmd < '0' || cmd > '9') {
            Serial.println("broadcast recall: cancelled (not a decimal digit)");
            recall_slot_pending = false;
            continue;
          }
          recall_slot_value = (uint8_t)(recall_slot_value * 10 + (cmd - '0'));
          if (++recall_slot_digits < 2) continue;  // wait for the 2nd digit
          recall_slot_pending = false;
          if (recall_slot_value > 29) {
            Serial.printf("broadcast recall: slot %d out of range (0-29), "
                          "cancelled\n", recall_slot_value);
            continue;
          }
#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)
          Serial.printf("broadcast RECALL: slot %d\n", recall_slot_value);
          OC::PresetBus::BroadcastRecall(recall_slot_value);
#endif
          continue;
        }
        if (patch_pending) {
          int v = -1;
          if (cmd >= '0' && cmd <= '9') v = cmd - '0';
          else if (cmd >= 'a' && cmd <= 'f') v = cmd - 'a' + 10;
          else if (cmd >= 'A' && cmd <= 'F') v = cmd - 'A' + 10;
          if (v < 0) {
            Serial.println("patch: cancelled (not a hex digit)");
            patch_pending = false;
            continue;
          }
          if (patch_digits < 4) {
            patch_offset = (uint16_t)((patch_offset << 4) | v);
          } else {
            patch_value = (uint8_t)((patch_value << 4) | v);
          }
          if (++patch_digits < 6) continue;  // 4 offset digits + 2 value digits
          patch_pending = false;
#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
          uint8_t *img = OC::PresetBus::MasterCardImage();
          if (!img) {
            Serial.println("patch: no card image resident (not CardServing())");
          } else if (patch_offset >= 65536) {
            Serial.printf("patch: offset %04X out of range\n", patch_offset);
          } else {
            const uint8_t old = img[patch_offset];
            img[patch_offset] = patch_value;
            Serial.printf("patch: offset %04X (%u): %02X -> %02X\n",
                          patch_offset, patch_offset, old, patch_value);
          }
#endif
          continue;
        }
        if (restore_addr_pending) {
          int v = -1;
          if (cmd >= '0' && cmd <= '9') v = cmd - '0';
          else if (cmd >= 'a' && cmd <= 'f') v = cmd - 'a' + 10;
          else if (cmd >= 'A' && cmd <= 'F') v = cmd - 'A' + 10;
          if (v < 0) {
            Serial.println("master restore: cancelled (not a hex digit)");
            restore_addr_pending = false;
            continue;
          }
          restore_addr_value = (uint8_t)((restore_addr_value << 4) | v);
          if (++restore_addr_digits < 2) continue;  // wait for the 2nd digit
          restore_addr_pending = false;
          if (millis() - destructive_arm_ms >= 3000 || destructive_arm_key != 'x') {
            destructive_arm_ms = millis();
            destructive_arm_key = 'x';
            Serial.printf("master restore: target %02X armed -- press 'x' "
                          "then the SAME 2 hex digits again within 3s to "
                          "actually push the card image to the module\n",
                          restore_addr_value);
            continue;
          }
          destructive_arm_ms = 0;
#if defined(__IMXRT1062__) && defined(ARDUINO_TEENSY41)
          Serial.printf("master restore: pushing card image to module %02X "
                        "NOW\n", restore_addr_value);
          const int rrc = OC::PresetBus::MasterRestore(restore_addr_value);
          Serial.printf("  MasterRestore() returned %d (0=accepted; "
                        "watch 'b' for progress)\n", rrc);
#endif
          continue;
        }
        switch (cmd) {
#ifdef PRINT_DEBUG
          case 'z':
            Serial.println("-=[ PEW PEW NERDS! ]=-");
            Serial.println("-- system --");
#if defined(__IMXRT1062__)
            Serial.println("t selftest   a activate Captain MIDI");
#endif
            Serial.printf("I app ISR [%s]   D display [%s]   L app loop [%s]\n",
                          OC::CORE::app_isr_enabled ? "on" : "OFF",
                          OC::CORE::display_update_enabled ? "on" : "OFF",
                          OC::CORE::app_loop_enabled ? "on" : "OFF");
#if defined(__IMXRT1062__)
#if defined(ARDUINO_TEENSY41)
            Serial.println("i i2c scan   u host devices + MIDI profiles   g save globals+app data");
            Serial.println("-- preset bus --");
            Serial.println("local:  ( save0  ) recall0  { save1  } recall1");
#if defined(PRESET_BUS)
            Serial.println("bus:    > save0  < recall0  . save1  , recall1  (broadcast!)");
#endif
            Serial.printf("p overlay UI [%s]   b bus dump   B verbose [%s]   k card serve [%s]\n",
                          OC::PresetBusUI::Active() ? "open" : "closed",
                          OC::PresetBus::Verbose() ? "on" : "off",
                          OC::PresetBus::CardServing() ? "on" : "off");
            Serial.println("m master BACKUP <2 hex digit addr>   c dump last capture (hex)");
            Serial.println("q QUERY module identity <2 hex digit addr>");
            Serial.printf("y USB bridge status + fallback usbMIDI poll [%s]\n",
                          OC::Bus200eBridgeUsb::Polling() ? "on" : "off");
#endif
            Serial.println("-- files --");
            Serial.println("l list LittleFS   s list SD");
            Serial.println("-- DANGER --");
            Serial.println("C RESET config file   F ERASE ALL LittleFS files");
#endif
            Serial.println("(any other key = screen capture)");
            break;

          case 'I':
            OC::CORE::app_isr_enabled = !OC::CORE::app_isr_enabled;
            Serial.printf("App ISR = %s\n", OC::CORE::app_isr_enabled ? "ON" : "OFF");
            break;
          case 'D':
            OC::CORE::display_update_enabled = !OC::CORE::display_update_enabled;
            Serial.printf("Display Redraw = %s\n", OC::CORE::display_update_enabled ? "ON" : "OFF");
            break;
          case 'L':
            OC::CORE::app_loop_enabled = !OC::CORE::app_loop_enabled;
            Serial.printf("App Loop = %s\n", OC::CORE::app_loop_enabled ? "ON" : "OFF");
            break;

#if defined(__IMXRT1062__)
#if defined(ARDUINO_TEENSY41)
          case 'i':
            ScanI2C();
            break;
          // preset-engine bench triggers: [ = save, ] = recall (slot 0);
          // { and } use slot 1
          case '(': OC::PresetEngine::RequestSave(0); break;
          case ')': OC::PresetEngine::RequestRecall(0); break;
          case '{': OC::PresetEngine::RequestSave(1); break;
          case '}': OC::PresetEngine::RequestRecall(1); break;
          // Preset export/import to the SD card. Presets themselves always
          // live on internal flash; the card is how one moves between modules.
          // Console-only for now -- there is deliberately no panel gesture
          // yet, because a half-built one would be worse than none.
          case 'E': {   // export every used slot
            int n = 0, fail = 0, legacy = 0, nocard = 0;
            for (uint8_t i = 0; i < OC::PresetEngine::kNumSlots; ++i) {
              switch (OC::PresetEngine::ExportSlot(i)) {
                case OC::PresetEngine::EXPORT_OK:     ++n; break;
                case OC::PresetEngine::EXPORT_EMPTY:  break;
                case OC::PresetEngine::EXPORT_LEGACY: ++legacy; break;
                case OC::PresetEngine::EXPORT_NO_CARD: ++nocard; break;
                default: ++fail; break;
              }
            }
            // Report each cause separately. "N failed" alone sent someone
            // looking at the card when the real answer was that every slot
            // predated the container format.
            if (nocard) { Serial.println("no card"); break; }
            Serial.printf("exported %d slot(s)\n", n);
            if (legacy)
              Serial.printf("%d slot(s) are pre-container: save each one once "
                            "to convert it, then export again\n", legacy);
            if (fail) Serial.printf("%d slot(s) FAILED to copy\n", fail);
            break;
          }
          // 'J', not 'I': 'I' is the app-ISR toggle thirty lines up, and this
          // clashed with it. It compiled everywhere the four-target build gate
          // looks because none of those define PRINT_DEBUG -- which is also
          // why nobody noticed the whole export/import feature was compiled
          // OUT of every shipping target. The gate must include T41_console.
          case 'J': {   // import every slot the card holds
            const int have = OC::PresetEngine::CardSlotCount();
            if (have < 0) { Serial.println("no card"); break; }
            int n = 0, fail = 0;
            for (uint8_t i = 0; i < OC::PresetEngine::kNumSlots; ++i) {
              const OC::PresetEngine::ExportResult r =
                  OC::PresetEngine::ImportSlot(i);
              if (r == OC::PresetEngine::EXPORT_OK) ++n;
              else if (r != OC::PresetEngine::EXPORT_EMPTY) ++fail;
            }
            Serial.printf("card holds %d, imported %d, %d failed\n",
                          have, n, fail);
            break;
          }
          case 'g':
            Serial.println("Saving global settings + app data...");
            OC::SaveAppData();
            break;
          case 'u':  // USB host port device identities + profile table
            for (int hp = 0; hp < 2; ++hp) {
              const char *pn = (const char *)usbHostMIDI[hp].product();
              Serial.printf("host%d: vid=%04X pid=%04X product=%s\n", hp + 1,
                            usbHostMIDI[hp].idVendor(), usbHostMIDI[hp].idProduct(),
                            (usbHostMIDI[hp].idVendor() && pn) ? pn : "-");
            }
            CaptainDumpProfiles();
            break;
          case 'p':  // toggle the preset-bus overlay (remote UI inspection)
            if (OC::PresetBusUI::Active()) OC::PresetBusUI::Exit();
            else OC::PresetBusUI::Enter();
            Serial.printf("PresetBusUI %s\n",
                          OC::PresetBusUI::Active() ? "open" : "closed");
            break;
          case 'b': OC::PresetBus::DebugDump(); break;
          case 'k':  // toggle 0x50 card serving (WPM-less bus only; hard-gated)
            OC::PresetBus::CardServeEnable(!OC::PresetBus::CardServing());
            break;
          case 'B':
            OC::PresetBus::SetVerbose(!OC::PresetBus::Verbose());
            Serial.printf("PresetBus verbose = %d\n", OC::PresetBus::Verbose());
            break;
          case 'm':  // master BACKUP from a foreign module (address: see above)
            Serial.println("master backup: type 2 hex digits for the target "
                           "module address");
            master_addr_pending = true;
            master_addr_digits = 0;
            master_addr_value = 0;
            break;
          case 'q':  // master QUERY at a foreign module: "who are you?"
            Serial.println("query: type 2 hex digits for the module address "
                           "to identify");
            query_addr_pending = true;
            query_addr_digits = 0;
            query_addr_value = 0;
            break;
          case 'c': OC::PresetBus::DumpCard(); break;  // hex-dump the last capture
          case 'w':  // patch one byte in the resident card image: 4 hex
                     // digits offset + 2 hex digits value, no bus traffic
            Serial.println("patch: type 4 hex digits for the offset, then "
                           "2 hex digits for the new byte value");
            patch_pending = true;
            patch_digits = 0;
            patch_offset = 0;
            patch_value = 0;
            break;
          case 'x':  // MasterRestore() the resident card image to a module.
                     // 2 hex digits for the address; press 'x' + the SAME
                     // 2 digits again within 3s to actually fire.
            Serial.println("master restore: type 2 hex digits for the "
                           "target module address");
            restore_addr_pending = true;
            restore_addr_digits = 0;
            restore_addr_value = 0;
            break;
          case 'y':  // browser bridge status; toggles the fallback usbMIDI poll
            OC::Bus200eBridgeUsb::SetPolling(!OC::Bus200eBridgeUsb::Polling());
            Serial.printf("bridge fallback poll = %s (leave OFF under any app "
                          "that reads USB MIDI itself)\n",
                          OC::Bus200eBridgeUsb::Polling() ? "ON" : "off");
            OC::Bus200eBridgeUsb::DebugDump();
            break;
          case 'r': {  // print LittleFS CRASH.LOG (bench diagnostic, one-off)
            File f = PhzConfig::myfs.open("CRASH.LOG", FILE_READ);
            if (!f) {
              Serial.println("CRASH.LOG: not present");
              break;
            }
            Serial.printf("--- CRASH.LOG (%lu bytes) ---\n",
                          (unsigned long)f.size());
            while (f.available()) Serial.write(f.read());
            Serial.println("\n--- end CRASH.LOG ---");
            f.close();
            break;
          }
          case 'V':  // LOCAL save to slot NN -- this module only, no bus
            Serial.println("local save: type 2 decimal digits (00-29)");
            lsave_pending = true; lsave_digits = 0; lsave_value = 0;
            break;
          case 'N':  // LOCAL recall of slot NN -- this module only, no bus
            Serial.println("local recall: type 2 decimal digits (00-29)");
            lrecall_pending = true; lrecall_digits = 0; lrecall_value = 0;
            break;
          case 'S':  // broadcast SAVE to slot NN (2 decimal digits, 00-29)
            Serial.println("broadcast save: type 2 decimal digits for the "
                           "target slot (00-29)");
            save_slot_pending = true;
            save_slot_digits = 0;
            save_slot_value = 0;
            break;
          case 'R':  // broadcast RECALL from slot NN (2 decimal digits, 00-29)
            Serial.println("broadcast recall: type 2 decimal digits for the "
                           "target slot (00-29)");
            recall_slot_pending = true;
            recall_slot_digits = 0;
            recall_slot_value = 0;
            break;
#endif
          // destructive keys need a second press within 3s ('pew!' stops
          // robots typing garbage; this stops human typos)
          case 'C':
            if (millis() - destructive_arm_ms < 3000 && destructive_arm_key == 'C') {
              destructive_arm_ms = 0;
              Serial.println("Resetting Config File!!");
              PhzConfig::clear_config();
              PhzConfig::save_config();
            } else {
              destructive_arm_ms = millis();
              destructive_arm_key = 'C';
              Serial.println("'C' RESETS the config - press 'C' again within 3s");
            }
            break;
          case 't': SelfTest(); break;  // one-shot system health report
          case 'K': ButtonWatch(); break;  // name the physical buttons
          case 'a': OC::SwitchToDefaultApp(); break;  // remote: activate Captain
          case 'l':
            Serial.println(" -=- LittleFS -=- ");
            PhzConfig::listFiles();
            break;
          case 's':
            Serial.println(" -=- SD Card -=- ");
            PhzConfig::listFiles(SD);
            break;
          case 'F':
            if (millis() - destructive_arm_ms >= 3000 || destructive_arm_key != 'F') {
              destructive_arm_ms = millis();
              destructive_arm_key = 'F';
              Serial.println("'F' ERASES ALL LittleFS files - press 'F' again within 3s");
              break;
            }
            destructive_arm_ms = 0;
            Serial.println("!! ERASING ALL FILES on LittleFS !!");
            // worst-case format outlives the 128s watchdog: timer-fed
            watchdog_feed_during([] { PhzConfig::eraseFiles(); });
            break;
          case 'Z':
            // Drop into HalfKay so the host can flash us, WITHOUT anybody
            // touching the module.
            //
            // This board is mounted with the Teensy's PROGRAM button
            // unreachable, so the only way into the bootloader was the
            // front panel (SETTINGS -> Reflash). That is fine at a bench
            // and impossible from the Orin, which is where every build
            // actually comes from -- a deploy that needs a human to walk
            // over is a deploy that does not happen. docs/bench-console.md
            // and FAQ.md both still say "press the PROGRAM button"; they
            // are inherited from upstream hardware that exposes one.
            //
            // Same double-press guard as C and F: this stops the
            // instrument dead until something re-flashes it, which mid-set
            // would be the worst possible surprise.
            if (millis() - destructive_arm_ms >= 3000 || destructive_arm_key != 'Z') {
              destructive_arm_ms = millis();
              destructive_arm_key = 'Z';
              Serial.println("'Z' REBOOTS to the bootloader (stops playing) "
                             "- press 'Z' again within 3s");
              break;
            }
            destructive_arm_ms = 0;
            Serial.println("rebooting into HalfKay -- run teensy_loader_cli now");
            Serial.flush();
            delay(50);          // let the line reach the host before USB drops
            _reboot_Teensyduino_();
            break;
#endif

            // TODO:
          case '+':
          case '-':
            // simulate UP and DOWN buttons
            break;
          case '[':
          case ']':
            // simulate Encoder button press
            break;
#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)
          // commander mode: bus-wide preset ops (every module + local engine)
          case '<': OC::PresetBus::BroadcastRecall(0); break;
          case '>': OC::PresetBus::BroadcastSave(0); break;
          case ',': OC::PresetBus::BroadcastRecall(1); break;
          case '.': OC::PresetBus::BroadcastSave(1); break;
#else
          case ',':
          case '.':
            // simulate Left Encoder turn
            break;
          case '<':
          case '>':
            // simulate Right Encoder turn
#endif
            break;
#endif
          default:
            capreq = true;
            break;
        }
      } while (Serial.available() > 0);
      if (capreq) {
        display::frame_buffer.capture_request();
        cap_idx = 0;
      }
    }

    // check for frame buffer to have capture data ready
    const uint8_t *capture_data = display::frame_buffer.captured();
    if (capture_data && cap_send_time > 950) {
      cap_send_time = 0;
      capture_data += cap_idx; // start where we left off

      // limit to n bytes every 950 micros
      const size_t chunk_size = 32;
      for (size_t i=0; i < chunk_size; i++) {
        uint8_t n = *capture_data++;
        if (n < 16) Serial.print("0");
        Serial.print(n, HEX);

        if (++cap_idx >= display::frame_buffer.kFrameSize) {
          // we're done sending this one
          Serial.println();
          Serial.flush();
          cap_idx = 0;
          display::frame_buffer.capture_retire();
          break;
        }
      }
    }

  }
}


