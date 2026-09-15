#pragma once

// Float32 port of the local Schroeder/Moorer reverb (effect_reverb_schroeder.h).
// The int16 version already ran its comb/allpass state in float internally;
// this version keeps float32 at the block boundaries too, so the wet path
// never quantizes or saturates to int16 on its way to the applet's filter
// and dry/wet mix. Same tunings, decay/damping mappings, and 0.6 wet gain.

#include <Arduino.h>
#include <arm_math.h>
#include <imxrt.h>
#include "../extern/f32/AudioStream_F32.h"

// ---- Schroeder/Moorer Reverb (wet only) ----
// Parallel comb filters -> series allpass filters
// Tunable decay time (RT60) in seconds

class AudioEffectReverbSchroederF32 : public AudioStream_F32 {
public:
  AudioEffectReverbSchroederF32() : AudioStream_F32(1, inputQueueArray) {
    setDecayTime(2.5f);   // default ~2.5s
    setDamping(0.5f);     // 0 = bright, 1 = dark
    reset();
  }

  // Set decay time in seconds (approximate RT60)
  void setDecayTime(float seconds) {
    if (seconds < 0.1f) seconds = 0.1f;
    if (seconds > 20.0f) seconds = 20.0f;

    float avgDelay = 0.0f;
    for (int i = 0; i < COMB_COUNT; i++) avgDelay += combLen[i];
    avgDelay /= COMB_COUNT;

    // combLen is in SAMPLES, so convert with the rate we actually run at.
    // This used to divide by a hard-coded 44100.0f while the core runs at
    // AUDIO_SAMPLE_RATE_EXACT = 48000.0f, which overstated the per-comb delay
    // by 8.9% and therefore made `g` too small: every requested decay time
    // came out ~8% SHORT. The default 2.5 s setting measured about 2.30 s.
    float delaySec = avgDelay / AUDIO_SAMPLE_RATE_EXACT;
    float g = expf((-3.0f * delaySec) / seconds); // feedback coefficient
    if (g > 0.9999f) g = 0.9999f;

    __disable_irq();
    decayFeedback = g;
    __enable_irq();
  }

  // 0..1, and READ THE NEXT PARAGRAPH BEFORE USING IT: higher d is BRIGHTER.
  //
  // This comment used to say "1 = strong high-frequency damping", which is the
  // opposite of what the code does, and that error propagated: it is cited as
  // the contract in Audio-Apps-Screens.md finding M-1, which concluded on the
  // strength of it that SamverbApplet's `1.0f - damp*0.01f` was an inversion
  // bug. It is not. The applet is compensating for this function correctly.
  //
  // The arithmetic, from update() below: `combStore = combStore*damp2 +
  // y*damp1` with damp1 = d and damp2 = 1-d. As a one-pole
  // y[n] = a*x[n] + (1-a)*y[n-1], the coefficient multiplies the INPUT, so the
  // new-sample weight IS d. Higher d means LESS smoothing, so more high
  // frequency survives: brighter.
  //
  // CONTRAST WITH FREEVERB, because the two live side by side in this build and
  // the trap is that they use the same variable name for opposite roles.
  // AudioEffectFreeverbF32::damping() (Audio/effect_freeverb_F32.h:39-47) sets
  // damp1 = n*0.4 and its loop is `combfilter = bufout*damp2 +
  // combfilter*damp1` -- the coefficient multiplies the STATE there, so its
  // new-sample weight is damp2 = 1-0.4n and higher n means DARKER. Freeverb's
  // parameter genuinely means "damping"; this one means "brightness".
  //
  // Hence: a UI row labelled "Damp" must pass damp straight through to
  // Freeverb and INVERTED to this. Both applets in this tree already do.
  void setDamping(float d) {
    // The low clamp is 0.01, not 0, and it is a guard rather than taste. At
    // d == 0 exactly the loop degenerates to `combStore = combStore*1.0 +
    // y*0.0`: the damping state stops tracking its input and holds its last
    // value FOREVER, injecting a constant into every comb's feedback path
    // until reset() is called. SamverbApplet's damp knob caps at 99 to stay
    // off that point, but damp_cv is summed in AFTER the knob
    // (SamverbApplet.h:38) and routes around the cap, so the engine has to
    // defend itself. Symmetric with the 0.99 already on the high end, which
    // exists for the mirror-image reason.
    if (d < 0.01f) d = 0.01f;
    if (d > 0.99f) d = 0.99f;
    __disable_irq();
    damp1 = d;
    damp2 = 1.0f - d;
    __enable_irq();
  }

  void reset() {
    __disable_irq();
    for (int i = 0; i < COMB_COUNT; ++i) {
      combIdx[i] = 0;
      combStore[i] = 0.0f;
      memset(combBuf[i], 0, sizeof(combBuf[i]));
    }
    for (int i = 0; i < ALLPASS_COUNT; ++i) {
      apIdx[i] = 0;
      memset(apBuf[i], 0, sizeof(apBuf[i]));
    }
    __enable_irq();
  }

  virtual void update(void) override {
    audio_block_f32_t *in = receiveReadOnly_f32(0);
    if (!in) return;

    audio_block_f32_t *out = AudioStream_F32::allocate_f32();
    if (!out) {
      AudioStream_F32::release(in);
      return;
    }

    float localDamp1 = damp1;
    float localDamp2 = damp2;
    float feedback   = decayFeedback;

    for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {
      float x = in->data[n];

      // ---- Parallel combs ----
      float combSum = 0.0f;
      for (int i = 0; i < COMB_COUNT; ++i) {
        if (combIdx[i] < 0) combIdx[i] += combLen[i];
        if (combIdx[i] >= combLen[i]) combIdx[i] -= combLen[i];

        float y = combBuf[i][combIdx[i]];
        combStore[i] = (combStore[i] * localDamp2) + (y * localDamp1);
        combBuf[i][combIdx[i]] = x + feedback * combStore[i];
        combIdx[i]++; if (combIdx[i] >= combLen[i]) combIdx[i] = 0;
        combSum += y;
      }

      combSum *= (1.0f / COMB_COUNT);

      // ---- Series allpasses ----
      float apOut = combSum;
      for (int i = 0; i < ALLPASS_COUNT; ++i) {
        float bufOut = apBuf[i][apIdx[i]];
        float z = apOut + (-AP_GAIN * bufOut);
        apBuf[i][apIdx[i]] = z;
        apIdx[i]++; if (apIdx[i] >= apLen[i]) apIdx[i] = 0;
        apOut = bufOut + z * AP_GAIN;
      }

      // wet only; no int16 saturation in the float path
      out->data[n] = apOut * 0.6f;
    }

    AudioStream_F32::transmit(out);
    AudioStream_F32::release(out);
    AudioStream_F32::release(in);
  }

private:
  audio_block_f32_t *inputQueueArray[1];

  // Tunables
  volatile float decayFeedback = 0.85f; // per-comb feedback
  volatile float damp1 = 0.5f, damp2 = 0.5f;

  // Delay line layout.
  //
  // kTuningSr IS NOT THIS INSTRUMENT'S SAMPLE RATE, and it is not meant to be.
  // It is the rate the classic Schroeder/Freeverb comb and allpass tunings were
  // specified at, and it survives here for one job only: reproducing those
  // exact sample counts. We run at AUDIO_SAMPLE_RATE_EXACT = 48000.0f
  // (framework-arduinoteensy/cores/teensy4/AudioStream.h:37-38; nothing in this
  // project overrides it), so each line below is 8.9% shorter in TIME than the
  // 1962 tuning intends. The millisecond figures in the comments are now the
  // times these lines actually produce AT 48 kHz. They used to be the 44.1 kHz
  // times, which is what made an accepted voicing look like a defect.
  //
  // Scaling the lengths up to restore the classic times was considered and
  // rejected on cost: it would take combBuf 68,896 -> 74,976 B and apBuf
  // 8,896 -> 9,680 B, i.e. +6,864 B of RAM2 heap per instance. RAM2 is the
  // binding constraint on the whole standalone-effects workstream -- it is the
  // reason Abyss is out -- and `Factory::get()` silently falls back to PSRAM
  // once free RAM2 drops under RAM2_HEADROOM (OC_core.h:24,64-65), which
  // effect_abyss.h:21-25 records as costing >50% CPU. A 9%-shorter reverb is a
  // voicing you cannot call wrong; a CPU cliff that depends on which app you
  // opened third is.
  //
  // The wrong rate DID produce one wrong number rather than a different
  // voicing, and that one is fixed: setDecayTime() above.
  static constexpr float kTuningSr = 44100.0f;
  static constexpr int COMB_COUNT = 8;
  static constexpr int combLenConst[COMB_COUNT] = {
    1319,  // 27.5 ms
    1493,  // 31.1 ms
    1559,  // 32.5 ms
    1613,  // 33.6 ms
    1747,  // 36.4 ms
    1873,  // 39.0 ms
    2017,  // 42.0 ms
    2153,  // 44.9 ms
  };

  static constexpr int ALLPASS_COUNT = 4;
  static constexpr int apLenConst[ALLPASS_COUNT] = {
    int(0.0050f * kTuningSr + 0.5f),  // 221 samples = 4.6 ms at 48 kHz
    int(0.0017f * kTuningSr + 0.5f),  //  75 samples = 1.6 ms
    int(0.0083f * kTuningSr + 0.5f),  // 366 samples = 7.6 ms
    int(0.0126f * kTuningSr + 0.5f)   // 556 samples = 11.6 ms
  };
  static constexpr float AP_GAIN = 0.5f;

  int combLen[COMB_COUNT] = {
    combLenConst[0], combLenConst[1], combLenConst[2], combLenConst[3],
    combLenConst[4], combLenConst[5], combLenConst[6], combLenConst[7],
  };
  int apLen[ALLPASS_COUNT] = { apLenConst[0], apLenConst[1], apLenConst[2], apLenConst[3] };

  // The maximum buffer size must fit the largest delay
  static constexpr int COMB_MAX = combLenConst[COMB_COUNT - 1];
  static constexpr int AP_MAX   = apLenConst[ALLPASS_COUNT - 1];

  float combBuf[COMB_COUNT][COMB_MAX] __attribute__((aligned(4)));
  float apBuf[ALLPASS_COUNT][AP_MAX]  __attribute__((aligned(4)));

  volatile int32_t combIdx[COMB_COUNT] = {0};
  volatile uint16_t apIdx[ALLPASS_COUNT] = {0};

  float combStore[COMB_COUNT] = {0};
};
