#ifndef XENOSIM_ARDUINO_H_
#define XENOSIM_ARDUINO_H_
// ---------------------------------------------------------------------------
// Teensyduino stand-in for the host simulator.
//
// DISCIPLINE: this covers the Arduino/Teensy surface the *UI* half of the
// firmware touches -- time, pin reads, Serial printing, the placement
// attributes -- and nothing else. It is a hardware shim, not an emulator:
// digitalReadFast() returns a pin state nobody drives, delay() advances the
// simulator's virtual clock instead of sleeping, and Serial goes to the
// simulator's event log. Anything that wants a real peripheral belongs in
// shim/src/ as a whole replaced header, not here.
// ---------------------------------------------------------------------------

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <math.h>

// --- placement attributes --------------------------------------------------
// Meaningless on the host and deliberately empty: FLASHMEM/DMAMEM/ITCM
// placement is exactly the thing that does NOT transfer to a simulator.
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
#ifndef EXTMEM
#define EXTMEM
#endif
#ifndef PROGMEM_ATTR
#define PROGMEM_ATTR
#endif

#define pgm_read_byte(addr)      (*(const uint8_t *)(addr))
#define pgm_read_word(addr)      (*(const uint16_t *)(addr))
#define pgm_read_dword(addr)     (*(const uint32_t *)(addr))
#define pgm_read_ptr(addr)       (*(void *const *)(addr))

// --- pin/level names -------------------------------------------------------
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define OUTPUT_OPENDRAIN 4
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2
#define LSBFIRST 0
#define MSBFIRST 1
#define CHANGE 4
#define FALLING 2
#define RISING 3

typedef bool boolean;
typedef uint8_t byte;

// --- clock -----------------------------------------------------------------
// One source of time for the whole simulator: see shim/oc_shim.h.
uint32_t SimNowMs();
uint32_t SimNowUs();
void SimAdvanceMs(uint32_t dt);

// Time passing WITH the background running: the core ISR and the 1 kHz UI poll
// on their own schedules, exactly as display::SimPump does for a blocked
// frame. See sim_runtime.cpp.
void SimBackgroundUs(uint32_t us);

static inline uint32_t millis() { return SimNowMs(); }
static inline uint32_t micros() { return SimNowUs(); }

// delay() and delayMicroseconds() must ADVANCE THE CLOCK AND RUN THE
// BACKGROUND, not merely return.
//
// On hardware a delay is not a pause in the machine: the core ISR keeps
// firing, the display drains page by page and the UI poll keeps debouncing
// buttons. Firmware relies on that -- any `while (!done) { ...; delay(x); }`
// is waiting for something only the ISR can deliver.
//
// delayMicroseconds() used to be a no-op, which made the clock stand still
// inside exactly those loops. Ui::DebugStats() (OC_debug.cpp) is a
// `while (!exit_loop)` whose only pacing is delayMicroseconds(10): with a
// frozen clock the UI poll never ran, no button event was ever queued, the
// frame buffer was never drained, and the simulator spun forever with no
// screen and no way out. A reviewer lost a process to it. delay() had the
// milder half of the same bug -- it moved the clock but ran nothing, so a
// firmware loop that delays while waiting on the ISR still made no progress.
// A second at a time, because ms * 1000 overflows a uint32_t past ~71 minutes
// and a wrapped delay would be a silent, undebuggable time machine.
static inline void delay(uint32_t ms) {
  while (ms) {
    const uint32_t chunk = ms > 1000u ? 1000u : ms;
    SimBackgroundUs(chunk * 1000u);
    ms -= chunk;
  }
}
static inline void delayMicroseconds(uint32_t us) { SimBackgroundUs(us); }
static inline void yield() {}

class elapsedMillis {
public:
  elapsedMillis() : ms_(SimNowMs()) {}
  elapsedMillis(uint32_t v) : ms_(SimNowMs() - v) {}
  operator uint32_t() const { return SimNowMs() - ms_; }
  elapsedMillis &operator=(uint32_t v) { ms_ = SimNowMs() - v; return *this; }
private:
  uint32_t ms_;
};

class elapsedMicros {
public:
  elapsedMicros() : us_(SimNowUs()) {}
  elapsedMicros(uint32_t v) : us_(SimNowUs() - v) {}
  operator uint32_t() const { return SimNowUs() - us_; }
  elapsedMicros &operator=(uint32_t v) { us_ = SimNowUs() - v; return *this; }
private:
  uint32_t us_;
};

// --- GPIO ------------------------------------------------------------------
// A flat array of pin levels the simulator drives (sim_input.cpp). The
// firmware's button matrix and its trigger inputs both come through here, so
// the panel's buttons and the TR jacks are the same mechanism they are on
// hardware: a pin that is high or low when the firmware looks at it. Every
// pin idles HIGH, which is "not pressed" for the active-low front panel.
uint8_t *SimPinLevels();          // 64 entries, index == Teensy pin number
static constexpr int kSimPinCount = 64;

static inline void pinMode(uint8_t, uint8_t) {}
static inline void digitalWrite(uint8_t p, uint8_t v) {
  if (p < kSimPinCount) SimPinLevels()[p] = v ? 1 : 0;
}
static inline void digitalWriteFast(uint8_t p, uint8_t v) { digitalWrite(p, v); }
static inline int digitalRead(uint8_t p) {
  return p < kSimPinCount ? SimPinLevels()[p] : 1;
}
static inline int digitalReadFast(uint8_t p) { return digitalRead(p); }

// GPIO port block. The digital-input driver reaches for the edge-detect
// registers directly; here every pin's "port" is one shared dummy block, so
// its ISR-driven edge path is inert and DigitalInputs::read_immediate (which
// uses digitalReadFast) is the path that actually works.
typedef struct {
  volatile uint32_t DR, GDIR, PSR, ICR1, ICR2, IMR, ISR, EDGE_SEL;
} IMXRT_GPIO_t;
IMXRT_GPIO_t *SimGpioPort();
static inline void *digitalPinToPortReg(uint8_t) { return SimGpioPort(); }

// Peripheral blocks named in headers the simulator still compiles for real
// (the FreqMeasure driver's descriptor table). Opaque and never dereferenced:
// the .cpp side of those drivers is replaced under shim/src/.
typedef struct { volatile uint32_t r[64]; } IMXRT_FLEXPWM_t;
typedef struct { volatile uint32_t r[64]; } IMXRT_TMR_t;
typedef int IRQ_NUMBER_t;
#define F_BUS_ACTUAL 150000000
static inline uint32_t digitalPinToBitMask(uint8_t p) { return 1u << (p & 31); }
static inline int analogRead(uint8_t) { return 0; }
static inline void analogWrite(uint8_t, int) {}
static inline void analogWriteResolution(uint8_t) {}
static inline void analogReadResolution(uint8_t) {}
static inline void analogReadAveraging(uint8_t) {}
static inline void attachInterrupt(uint8_t, void (*)(), int) {}
static inline void detachInterrupt(uint8_t) {}
static inline void noInterrupts() {}
static inline void interrupts() {}
static inline void __disable_irq() {}
static inline void __enable_irq() {}
static inline void NVIC_SET_PRIORITY(int, int) {}

// --- random ----------------------------------------------------------------
// Seeded once, deterministically, so a scripted --keys run is reproducible.
static inline long random(long howbig) { return howbig > 0 ? ::random() % howbig : 0; }
static inline long random(long howsmall, long howbig) {
  return howbig > howsmall ? howsmall + random(howbig - howsmall) : howsmall;
}
static inline void randomSeed(unsigned long s) { ::srandom(s); }

// --- math helpers ----------------------------------------------------------
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
// A function, not the Arduino macro: `map` as a macro collides with std::map
// the moment any header this stub reaches includes <map>.
static inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
#ifndef _BV
#define _BV(b) (1UL << (b))
#endif
using std::max;
using std::min;

// arm-none-eabi-gcc constant-folds cosf()/sinf() inside a constexpr
// initialiser -- OC_menus.cpp builds the Tonnetz note-circle lookup table that
// way -- and Apple clang folds neither those nor their __builtin_ spellings.
// These are constexpr Taylor series with quadrant reduction, computed in
// double and returned as float. The float return is what makes them safe: the
// series is accurate to ~1e-15, which is far below a float's resolution, so
// rounding to float lands on the same value libm's cosf would return and the
// table comes out identical. <math.h> and <cmath> are both included above, so
// nothing can re-declare these after the macros are in place.
namespace xenosim {
constexpr double kHalfPi = 1.57079632679489661923;
constexpr double sin_series(double x) {   // |x| <= pi/4
  double x2 = x * x, term = x, sum = x;
  for (int n = 1; n < 10; ++n) { term *= -x2 / ((2 * n) * (2 * n + 1)); sum += term; }
  return sum;
}
constexpr double cos_series(double x) {   // |x| <= pi/4
  double x2 = x * x, term = 1.0, sum = 1.0;
  for (int n = 1; n < 10; ++n) { term *= -x2 / ((2 * n - 1) * (2 * n)); sum += term; }
  return sum;
}
constexpr int quadrant(double x) {
  const double n = x / kHalfPi;
  const long long k = (long long)(n < 0 ? n - 0.5 : n + 0.5);
  return (int)(((k % 4) + 4) % 4);
}
constexpr double reduced(double x) {
  const double n = x / kHalfPi;
  const long long k = (long long)(n < 0 ? n - 0.5 : n + 0.5);
  return x - (double)k * kHalfPi;
}
constexpr float cosf_(float xf) {
  const double x = xf, r = reduced(x);
  switch (quadrant(x)) {
    case 0: return (float)cos_series(r);
    case 1: return (float)-sin_series(r);
    case 2: return (float)-cos_series(r);
    default: return (float)sin_series(r);
  }
}
constexpr float sinf_(float xf) {
  const double x = xf, r = reduced(x);
  switch (quadrant(x)) {
    case 0: return (float)sin_series(r);
    case 1: return (float)cos_series(r);
    case 2: return (float)-sin_series(r);
    default: return (float)-cos_series(r);
  }
}
}  // namespace xenosim
#define cosf(x) xenosim::cosf_(x)
#define sinf(x) xenosim::sinf_(x)

// arm-none-eabi-gcc folds ::pow in a constant expression; Apple clang does
// not, and OC_calibration.h initialises a constexpr from pow(2, kAdcResolution).
// This exact-match overload wins over std::pow's template for that call and
// leaves every floating-point pow() alone.
constexpr double pow(int base, uint8_t exp) {
  double r = 1.0;
  for (uint8_t i = 0; i < exp; ++i) r *= base;
  return r;
}

// --- heap ------------------------------------------------------------------
static inline void *extmem_malloc(size_t n) { return malloc(n); }
static inline void *extmem_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static inline void extmem_free(void *p) { free(p); }


// --- Cortex-M / i.MXRT register stand-ins ----------------------------------
// util_profiling.h reads the DWT cycle counter directly. A host has no such
// register; these are writable variables that never advance, so every profile
// the firmware takes here reads zero. That is the honest answer -- see the
// README: the simulator says nothing about timing.
#ifndef F_CPU
#define F_CPU 600000000
#endif
// The core's live clock rate (set_arm_clock can change it); the preset
// engine divides DWT cycles by it. A constant here, like F_CPU.
#ifndef F_CPU_ACTUAL
#define F_CPU_ACTUAL F_CPU
#endif
extern uint32_t SIM_ARM_DWT_CYCCNT;
extern uint32_t SIM_ARM_DWT_CTRL;
extern uint32_t SIM_ARM_DEMCR;
#define ARM_DWT_CYCCNT SIM_ARM_DWT_CYCCNT
#define ARM_DWT_CTRL SIM_ARM_DWT_CTRL
#define ARM_DEMCR SIM_ARM_DEMCR
#define ARM_DEMCR_TRCENA (1 << 24)
#define ARM_DWT_CTRL_CYCCNTENA (1 << 0)

// The handful of i.MXRT peripheral registers the firmware writes from an
// inline function in a header we still compile for real (OC_DAC.h's SPI
// push). They are plain variables: the DAC's bits are formed by the real
// code and then land nowhere, which is what "the simulator has no outputs"
// means in practice.
extern uint32_t SIM_LPSPI4_TDR, SIM_LPSPI4_TCR, SIM_LPSPI4_SR;
#define LPSPI4_TDR SIM_LPSPI4_TDR
#define LPSPI4_TCR SIM_LPSPI4_TCR
#define LPSPI4_SR SIM_LPSPI4_SR
#define LPSPI_TCR_FRAMESZ(n) ((uint32_t)(n) & 0xfff)
#define LPSPI_TCR_PCS(n) (((uint32_t)(n) & 3) << 24)
#define LPSPI_TCR_RXMSK ((uint32_t)1 << 19)
#define LPSPI_SR_TCF ((uint32_t)1 << 10)

// Pad mux registers the DAC driver writes at init. Variables, not registers:
// the pin multiplexer has nothing to multiplex here.
extern uint32_t SIM_IOMUX[8];
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_B0_00 SIM_IOMUX[0]
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_B0_01 SIM_IOMUX[1]
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_B0_01 SIM_IOMUX[2]

// --- Print / Stream --------------------------------------------------------
class __FlashStringHelper;
#define F(s) (s)

// Arduino's String, thinly. Only what the Scala scale reader in OC_scales.cpp
// asks of it.
#include <string>
class String {
public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  char charAt(unsigned i) const { return i < s_.size() ? s_[i] : 0; }
  const char *c_str() const { return s_.c_str(); }
  unsigned length() const { return (unsigned)s_.size(); }
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  float toFloat() const { return strtof(s_.c_str(), nullptr); }
  int indexOf(char c) const {
    const size_t p = s_.find(c);
    return p == std::string::npos ? -1 : (int)p;
  }
  String substring(int from) const {
    return String(from >= 0 && (size_t)from <= s_.size() ? s_.substr(from) : std::string());
  }
  String substring(int from, int to) const {
    if (from < 0 || (size_t)from > s_.size() || to < from) return String();
    return String(s_.substr(from, to - from));
  }
  void trim() {
    const size_t a = s_.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s_.clear(); return; }
    s_ = s_.substr(a, s_.find_last_not_of(" \t\r\n") - a + 1);
  }
  bool operator==(const char *o) const { return s_ == o; }
private:
  std::string s_;
};

class SimPrint {
public:
  virtual ~SimPrint() {}
  // Everything the firmware prints lands in the simulator's event log rather
  // than on stdout: stdout is the frame, or the --stdio protocol.
  virtual size_t write(const uint8_t *b, size_t n);
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t write(const char *b, size_t n) { return write((const uint8_t *)b, n); }
  size_t print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return printf("%d", v); }
  size_t print(unsigned int v) { return printf("%u", v); }
  size_t print(long v) { return printf("%ld", v); }
  size_t print(unsigned long v) { return printf("%lu", v); }
  size_t print(double v) { return printf("%f", v); }
  size_t println() { return print("\n"); }
  size_t println(const char *s) { size_t n = print(s); return n + print("\n"); }
  size_t println(int v) { return printf("%d\n", v); }
  size_t println(unsigned int v) { return printf("%u\n", v); }
  size_t println(long v) { return printf("%ld\n", v); }
  size_t println(unsigned long v) { return printf("%lu\n", v); }
  size_t println(unsigned long long v) { return printf("%llu\n", v); }
  size_t print(unsigned long long v) { return printf("%llu", v); }
  size_t println(double v) { return printf("%f\n", v); }
  size_t print(int v, int base) { return base == 16 ? printf("%X", v) : printf("%d", v); }
  size_t print(unsigned long v, int base) { return base == 16 ? printf("%lX", v) : printf("%lu", v); }
  size_t println(unsigned long v, int base) { return base == 16 ? printf("%lX\n", v) : printf("%lu\n", v); }
  size_t println(unsigned long long v, int base) { return base == 16 ? printf("%llX\n", v) : printf("%llu\n", v); }
  size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void flush() {}
};

class SimSerial : public SimPrint {
public:
  void begin(unsigned long = 0) {}
  void end() {}
  operator bool() const { return true; }
  int available() { return 0; }
  int read() { return -1; }
  int peek() { return -1; }
  size_t readBytes(char *, size_t) { return 0; }
  void setTimeout(unsigned long) {}
  void clear() {}
};

extern SimSerial Serial;

#endif  // XENOSIM_ARDUINO_H_
