#ifndef XENOSIM_OC_SHIM_H_
#define XENOSIM_OC_SHIM_H_
// ---------------------------------------------------------------------------
// Host shim for the embedded environment src/apps/Bus200eApp.h expects.
//
// DISCIPLINE (same as software/test/host_stubs/Arduino.h): this covers exactly
// the surface Bus200eApp.h touches and nothing more. It is NOT the start of a
// host o_C emulator. If the app grows a dependency, either shim that one thing
// here or stop routing it through the simulator -- do not grow this into a
// general Teensyduino/o_C emulation layer.
//
// WHAT IS REAL AND WHAT IS FAKE:
//   real, compiled/linked from src/  : weegfx::Graphics + the 6x8 font,
//                                      util_macros.h, util_stream_buffer.h,
//                                      UI::Event, Bus200eMaster, PresetBus200e,
//                                      every Buchla*.{h,cpp}
//   faked here                       : millis/elapsedMicros, the MIDI ports,
//                                      OC::UiControl values, the app base
//                                      classes, gfxHeader
//   faked in sim_bus.cpp             : OC::PresetBus (the whole namespace)
//
// The one thing this header does by force is neutralise HSMIDI.h. Bus200eApp.h
// reaches it as `#include "../HSMIDI.h"` -- a path relative to the *including
// file*, so no -I can shadow it -- and it pulls in <MIDI.h> and <USBHost_t36.h>,
// neither of which exists on the host. Pre-defining its include guard makes the
// real file expand to nothing; the two things the app actually uses from it
// (HEM_MIDI_NOTE_ON, and the usbMIDI/usbHostMIDI/MIDI1 globals it implies) are
// declared below.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

// --- neutralised headers ---------------------------------------------------
#define HSMIDI_H   // see the note above; do NOT remove without a replacement

// --- Teensy attributes -----------------------------------------------------
// Placement attributes are meaningless on the host and must be empty here, not
// approximated: FLASHMEM/DMAMEM are exactly the things that do NOT transfer.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef FLASHMEM
#define FLASHMEM
#endif
#ifndef DMAMEM
#define DMAMEM
#endif
#ifndef FASTRUN
#define FASTRUN
#endif

#include "src/UI/ui_events.h"      // real UI::Event / UI::EventType
#include "src/drivers/weegfx.h"    // real weegfx::Graphics (impl in weegfx_host.cpp)
#include "util/util_macros.h"      // real CONSTRAIN / MACRO_CONCAT
#include "util/util_stream_buffer.h"  // real util::StreamBuffer{Reader,Writer}

// --- TWOCCS ----------------------------------------------------------------
// Copied rather than included: the real one lives in util/util_misc.h, which
// pulls in OC_options.h and the whole build-configuration tree.
constexpr uint16_t TWOCCS(const char s[3]) {
  return (uint16_t)(((s[0] & 0xff) << 8) | (s[1] & 0xff));
}

// --- clock -----------------------------------------------------------------
// The simulator's virtual millisecond clock. Everything that asks the time --
// the app's millis(), the real Bus200eMaster FSM's ops.now_ms, the terminal
// status line -- reads this one source, so "fast" and "real" pacing modes are
// a property of how the clock is advanced and nothing else.
uint32_t SimNowMs();
void SimAdvanceMs(uint32_t dt);

static inline uint32_t millis() { return SimNowMs(); }

// Only ever used to bound the ISR drain (`rec_timeout_ < 60`), in microseconds.
class elapsedMicros {
public:
  elapsedMicros() : base_(0) {}
  elapsedMicros &operator=(uint32_t v) { base_ = v; return *this; }
  operator uint32_t() const { return base_; }
private:
  uint32_t base_;
};

// --- MIDI ------------------------------------------------------------------
// Fake stand-ins for the four interfaces Controller() drains. Each is an
// independent FIFO so the simulator can inject a note on one port and prove
// which port the app is actually polling -- the exact question the Rec screen
// exists to answer on real hardware.
#define HEM_MIDI_NOTE_ON 0x90   // == midi::NoteOn, the value HSMIDI.h defines

class SimMidiPort {
public:
  bool read();
  uint8_t getType() const { return type_; }
  uint8_t getData1() const { return d1_; }
  uint8_t getData2() const { return d2_; }
  void Push(uint8_t type, uint8_t d1, uint8_t d2);
  const char *name() const { return name_; }
  void set_name(const char *n) { name_ = n; }
private:
  static constexpr int kCap = 32;
  uint8_t q_[kCap][3] = {};
  int head_ = 0, tail_ = 0;
  uint8_t type_ = 0, d1_ = 0, d2_ = 0;
  const char *name_ = "?";
};

extern SimMidiPort usbMIDI;
extern SimMidiPort usbHostMIDI[2];
extern SimMidiPort MIDI1;

// --- graphics --------------------------------------------------------------
extern weegfx::Graphics graphics;

// The 128x64 vertically-packed framebuffer `graphics` renders into -- the same
// layout the SH1106 driver is handed on target: bit (y & 7) of
// buf[(y >> 3) * 128 + x].
uint8_t *SimFrameBuffer();

// Reproduces HSUtils.cpp's gfxHeader() call for call: the app draws its title
// through this and the 10px rule under it sets every y coordinate below.
void gfxHeader(const char *str, const uint8_t *icon = nullptr);

// --- OC namespace ----------------------------------------------------------
// The panel's control bitmasks, OC::Ui and OC::AppEvent live in shim/fw/,
// because the real PresetBusUI.cpp -- which the simulator compiles and links
// unmodified -- includes "OC_ui.h" and "OC_apps.h" by those names and has to
// see the same definitions the app does. See shim/fw/README.md.
#include "fw/OC_apps.h"
#include "fw/OC_ui.h"

namespace OC {

enum OutputMode { OUTPUT_MODE_OFF, OUTPUT_MODE_UNI, OUTPUT_MODE_BIPOLAR };

struct IOFrame;   // opaque: the app only forwards a pointer

struct IOConfigOutput {
  const char *label = "";
  int mode = OUTPUT_MODE_OFF;
  void set(const char *l, int m) { label = l; mode = m; }
};

struct IOConfig {
  IOConfigOutput outputs[4];
};

}  // namespace OC

// --- app base classes ------------------------------------------------------
// Stand-ins for OC::AppBaseImpl<> and HSApplication. Only the virtuals the app
// marks `final` need to exist; everything else about the real bases (app
// switching, storage, the IOFrame pipeline) is deliberately absent.
class SimAppBase {
public:
  virtual ~SimAppBase() {}
  virtual void Init() = 0;
  virtual size_t appdata_storage_size() const = 0;
  virtual size_t SaveAppData(util::StreamBufferWriter &) const = 0;
  virtual size_t RestoreAppData(util::StreamBufferReader &) = 0;
  virtual void HandleAppEvent(OC::AppEvent) = 0;
  virtual void Loop() = 0;
  virtual void DrawMenu() const = 0;
  virtual void DrawScreensaver() const = 0;
  virtual void HandleButtonEvent(const UI::Event &) = 0;
  virtual void HandleEncoderEvent(const UI::Event &) = 0;
  virtual void GetIOConfig(OC::IOConfig &) const = 0;
  virtual void DrawDebugInfo() const = 0;
};

class HSApplication {
public:
  virtual ~HSApplication() {}
  virtual void Start() {}
  virtual void Resume() {}
  virtual void Controller() {}
  virtual void View() const {}
protected:
  void BaseController(OC::IOFrame *) {}   // no IO pipeline on the host
};

#define OC_APP_CLASS(clazz, app_id, name, boring_name)                 \
  struct MACRO_CONCAT(clazz, Traits) {                                 \
    static constexpr uint16_t id = app_id;                             \
    static constexpr const char *const app_name = name;                \
    static constexpr const char *const boring_app_name = boring_name;  \
  };                                                                   \
  class clazz : public SimAppBase

#define OC_APP_INTERFACE_DECLARE(clazz, s)                                  \
public:                                                                     \
  clazz() {}                                                                \
  virtual void Init() final;                                                \
  virtual size_t appdata_storage_size() const final {                       \
    return kAppDataStorageSize;                                             \
  }                                                                         \
  virtual size_t SaveAppData(util::StreamBufferWriter &) const final;       \
  virtual size_t RestoreAppData(util::StreamBufferReader &) final;          \
  virtual void HandleAppEvent(OC::AppEvent) final;                          \
  virtual void Loop() final;                                                \
  virtual void DrawMenu() const final;                                      \
  virtual void DrawScreensaver() const final;                               \
  virtual void HandleButtonEvent(const UI::Event &) final;                  \
  virtual void HandleEncoderEvent(const UI::Event &) final;                 \
  void Process(OC::IOFrame *ioframe);                                       \
  virtual void GetIOConfig(OC::IOConfig &) const final;                     \
  virtual void DrawDebugInfo() const final;                                 \
  static constexpr size_t kAppDataStorageSize = s

#endif  // XENOSIM_OC_SHIM_H_
