#ifndef TWEIGHTYTAPPHASE_H_
#define TWEIGHTYTAPPHASE_H_

// ---------------------------------------------------------------------------
// TIME-mode 8-tap phase math for the Tweighty app.
//
// Each tap sits at a PHASE on a 0..PHASE_FULLSCALE ring (288r's own unit for
// "how far around the captured window"); a tap's actual delay time is that
// fraction of the current base delay / loop-window length, uniformly rescaled
// by whatever the app's TIME control is currently applying (CV + knob). Taps
// are independent of the transport state -- WRITE or RECIRC, they read the
// same ring, which is what makes the crossfade at a WRITE<->RECIRC edge
// (TweightyTransport.h) the only discontinuity the engine has to smooth,
// rather than needing per-mode phase tables.
//
// Pure math, no Arduino/Audio.h: host-tested by
// test/test_tweighty_tap_phase.cpp. AudioTweightyF32 does not own this
// table -- TweightyApp's Controller() computes each tap's target seconds
// from it every control-rate pass and pushes the result to the engine's
// per-tap setter, so the phase ring lives with the settings it is persisted
// alongside, not duplicated into the audio engine.
// ---------------------------------------------------------------------------

inline constexpr int kTweightyTapCount = 8;
inline constexpr float PHASE_FULLSCALE = 160.0f;

struct TweightyTapPhaseTable {
  float phase[kTweightyTapCount];
};

// 288r's own default spread: evenly spaced eighths of the ring, 20,40,...,160
// -- the last tap sits exactly at PHASE_FULLSCALE, i.e. the full loop length.
void TweightyTapPhaseDefault(TweightyTapPhaseTable &table);

// Clamps every entry into [0, PHASE_FULLSCALE] in place. Returns whether any
// entry needed clamping, so a restore path can tell a corrupted table from a
// clean one instead of silently repairing it.
bool TweightyTapPhaseValidate(TweightyTapPhaseTable &table);

// One tap's target delay time. phase_0_160 and base_delay_secs are each
// clamped internally (a caller passing a stored-but-unvalidated phase, or a
// TIME control momentarily above the loop max, must not read outside the
// ring or the buffer) -- time_mult is trusted as-is since it is the caller's
// live modulation value, not persisted data.
float TweightyTapTargetSecs(float base_delay_secs, float phase_0_160,
                               float time_mult);

#endif  // TWEIGHTYTAPPHASE_H_
