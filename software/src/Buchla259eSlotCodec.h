#ifndef BUCHLA259ESLOTCODEC_H_
#define BUCHLA259ESLOTCODEC_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// Buchla 259e (Dual Waveshaper) 33-byte preset record.
//
// The authoritative field map is Buchla_FW/docs/259e-PRESET-FORMAT.md, built
// from an 8051 disassembly of 259E_312.hex and confirmed on live hardware for
// two of its fields (morph, warp). Read that before changing anything here.
// BSP-free and host-testable, same split as Buchla251eSlotCodec.{h,cpp} -- no
// hardware access, no Arduino includes.
//
// NOT THE 251e. That module's bank is 30 x 2104; this one is 30 x 33. The two
// codecs share nothing but their shape on purpose.
//
// BYTE-EXACT PRESERVATION IS THE CORE INVARIANT, same as the 251e codec: a
// RESTORE re-sends the whole bank, so Encode(Decode(x)) must equal x for any
// 33 bytes, including values this codec assigns no meaning to. That is why
// the twelve continuous parameters are stored as the raw 16-bit words rather
// than as decoded 0..4095 values -- see param[] below.
//
// ONE PRINCIPAL OSCILLATOR PLUS ONE MODULATOR, not two symmetric channels.
// The "dual" in the name is about waveshaping, not architecture; an earlier
// reading of this record as 6+6 per channel was wrong.
// ---------------------------------------------------------------------------

static constexpr int kBuchla259eRecordBytes = 33;
static constexpr int kBuchla259eSlotsPerBank = 30;
static constexpr int kBuchla259eBankBytes =
    kBuchla259eRecordBytes * kBuchla259eSlotsPerBank;   // 990
static constexpr int kBuchla259eParamCount = 12;        // offsets 0..23

// Pitch is exponential: 512 counts/octave over 8 octaves. 512/12 counts per
// semitone; the helpers below do this in integer math so no caller needs a
// float or a printf("%f") the OLED cannot render.
static constexpr int kBuchla259eCountsPerOctave = 512;

struct Buchla259eSlot {
  // Offsets 0..23. RAW 16-bit words, big-endian on the wire: param[p] is
  // bytes p*2 (high) and p*2+1 (low). Stored raw so encode is lossless.
  //
  // The 0..4095 knob value is param[p] >> 4. The discarded low nibble is
  // normally zero, but offsets 2..3 (pitch) are the documented exception --
  // a bus note message writes raw note-table values whose low nibble is
  // meaningful. Never validate a record by checking that nibble, and never
  // reconstruct the word from a 12-bit value you decoded earlier.
  uint16_t param[kBuchla259eParamCount] = {0};

  // Offsets 24..25, always 7f ff. Initialiser residue: the firmware seeds
  // thirteen parameter slots but only twelve have controls, and nothing ever
  // reads this one. Preserved raw; not a parameter, do not present it.
  uint8_t residue[2] = {0, 0};

  uint8_t engine_mode = 0;        // 26: 0..3, modulation-oscillator sync only
  uint8_t mod_dest_mask = 0;      // 27: bit0 freq, bit1 warp, bit2 morph
  uint8_t mod_waveform = 0;       // 28: 0=triangle 1=square 2=sawtooth
  uint8_t mod_freq_mode = 0;      // 29: 0=slow 1=normal 2=track principal
  uint8_t wave_button_target = 0; // 30: 0=green 1=red
  uint8_t red_timbre = 0;         // 31: 0..7
  uint8_t green_timbre = 0;       // 32: 0..7
};

// Decode/encode exactly kBuchla259eRecordBytes (33). Callers pass a pointer
// into a larger resident card image; offsetting to the right slot
// (slot * 33) is the caller's job.
void Buchla259eDecodeSlot(const uint8_t *bytes, Buchla259eSlot &out);
void Buchla259eEncodeSlot(const Buchla259eSlot &slot, uint8_t *out);

// ---- parameter semantics --------------------------------------------------
// Named so no UI does this arithmetic inline and gets the encoding subtly
// wrong. All integer: the OLED's printf has no float support.

// Raw word -> the 0..4095 knob value.
inline uint16_t Buchla259eParam12(uint16_t w) { return (uint16_t)(w >> 4); }

// Exactly five parameters are bipolar attenuverters, in offset binary with
// centre 0x800 (word 0x8000). Everything else in 0..23 is unipolar --
// including params 4 and 5, which an earlier revision wrongly listed here.
inline bool Buchla259eParamIsBipolar(int p) {
  return p == 0 || p == 2 || p == 6 || p == 7 || p == 11;
}

// Bipolar attenuverter as a signed percent, -100..+99. Negative genuinely
// means the CV is inverted, not merely attenuated. Centre (word 0x8000,
// bytes 80 00) reads 0.
inline int Buchla259eBipolarPercent(uint16_t w) {
  return (((int)Buchla259eParam12(w)) - 2048) * 100 / 2048;
}

// Unipolar parameter as 0..100 percent of its stored range.
inline int Buchla259eUnipolarPercent(uint16_t w) {
  return (int)Buchla259eParam12(w) * 100 / 4095;
}

// Pitch, in tenths of a semitone above the bottom of the 8-octave span.
// semitones = v * 12 / 512, so tenths = v * 15 / 64 exactly.
inline int Buchla259eSemitoneTenths(uint16_t w) {
  return ((int)Buchla259eParam12(w) * 15) / 64;
}

// The modulator's frequency when it is tracking the principal (mod_freq_mode
// == 2) is an INTERVAL, centred at 2048 = unison, spanning +/-4 octaves.
inline int Buchla259eIntervalTenths(uint16_t w) {
  return (((int)Buchla259eParam12(w) - 2048) * 15) / 64;
}

// Warp's stored value is NOT the effect amount. The firmware's final scaler
// is 0.8*acc + 0x1999 over a Q15 range, so the knob spans 20%..60% of scan
// width and never reaches either 0 or 100 -- full scan is reachable only via
// the warp CV input. Returns the real scan width percent, 20..60.
inline int Buchla259eWarpScanPercent(uint16_t w) {
  const uint32_t v = Buchla259eParam12(w);
  return (int)((320u * v + 655300u + 16384u) / 32768u);   // +half for rounding
}

// Params 4 and 5 change meaning with the corresponding timbre index: below 5
// they are unipolar attenuators, at 5 and above they are a "memory skew"
// base that points the waveshaper's table fetch into ordinary program memory.
// Param 4 pairs with the RED timbre, param 5 with the GREEN one. A static
// label for these would be wrong half the time.
inline bool Buchla259eParam4IsSkew(const Buchla259eSlot &s) { return s.red_timbre >= 5; }
inline bool Buchla259eParam5IsSkew(const Buchla259eSlot &s) { return s.green_timbre >= 5; }

// The skew base is still an ordinary 0..4095 knob value; the firmware scales
// it by 16 to reach a CODE address. Returned for display only.
inline uint32_t Buchla259eSkewBase(uint16_t w) {
  return (uint32_t)Buchla259eParam12(w) * 16u;
}

// Modulation destination bits (offset 27).
static constexpr uint8_t kBuchla259eModDestFreq = 0x01;
static constexpr uint8_t kBuchla259eModDestWarp = 0x02;
static constexpr uint8_t kBuchla259eModDestMorph = 0x04;

// Engine mode 1 (reverse/mirror sync) is INERT when the modulator is in slow
// mode: that path bypasses the direction test entirely. A UI must say so
// rather than showing a setting that does nothing.
inline bool Buchla259eEngineModeIsInert(const Buchla259eSlot &s) {
  return s.engine_mode == 1 && s.mod_freq_mode == 0;
}

#endif  // BUCHLA259ESLOTCODEC_H_
