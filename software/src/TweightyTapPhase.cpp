// Pure 8-tap phase-ring math for the Tweighty app. See the header for
// why this is split out from the transport state machine.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define TW_TAPPHASE_CODE FLASHMEM
#else
#define TW_TAPPHASE_CODE
#endif

#include "TweightyTapPhase.h"

TW_TAPPHASE_CODE
void TweightyTapPhaseDefault(TweightyTapPhaseTable &table) {
  for (int i = 0; i < kTweightyTapCount; ++i) {
    table.phase[i] = 20.0f * static_cast<float>(i + 1);
  }
}

TW_TAPPHASE_CODE
bool TweightyTapPhaseValidate(TweightyTapPhaseTable &table) {
  bool clamped = false;
  for (int i = 0; i < kTweightyTapCount; ++i) {
    float &p = table.phase[i];
    if (p < 0.0f) {
      p = 0.0f;
      clamped = true;
    } else if (p > PHASE_FULLSCALE) {
      p = PHASE_FULLSCALE;
      clamped = true;
    }
  }
  return clamped;
}

// NOT FLASHMEM, unlike its siblings above: called from TweightyApp's
// Controller() once per active tap, every 16.666kHz pass -- confirmed via a
// linker veneer in the linked ELF that this was landing in flash and costing
// an ITCM->flash branch on the hot path. TweightyTapPhaseDefault/Validate
// above stay FLASHMEM: both are cold (Init()/RestoreAppData()-only).
float TweightyTapTargetSecs(float base_delay_secs, float phase_0_160,
                             float time_mult) {
  if (base_delay_secs < 0.0f) base_delay_secs = 0.0f;
  if (phase_0_160 < 0.0f) phase_0_160 = 0.0f;
  else if (phase_0_160 > PHASE_FULLSCALE) phase_0_160 = PHASE_FULLSCALE;
  return base_delay_secs * (phase_0_160 / PHASE_FULLSCALE) * time_mult;
}
