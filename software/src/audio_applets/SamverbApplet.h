#pragma once

// F32-native: the Schroeder reverb's comb/allpass state was already float
// internally; now the wet path, post-filter, and dry/wet mix stay float32
// end to end inside the applet (see effect_reverb_schroeder_F32.h). The
// chain still sees int16 via the HemisphereAudioAppletF32 edge adapters.

#include "../HemisphereAudioAppletF32.h"
#include "../extern/f32/AudioMixer_F32.h"
#include "../Audio/effect_reverb_schroeder_F32.h"
#include "../Audio/filter_variable2_F32.h"

class BungverbApplet : public HemisphereAudioAppletF32<MONO> {
    public:
        const char* applet_name() {
            return "Bungverb";
        }
        void Start() override {
            if (!reverb) {
              reverb = GetBungverb();
            }
            PatchCableF32(InputF32(), 0, dry_wet_mixer, 1);
            PatchCableF32(dry_wet_mixer, 0, OutputF32(), 0);
            if (!reverb) return;
            PatchCableF32(InputF32(), 0, *reverb, 0);
            PatchCableF32(*reverb, 0, filter, 0);
            PatchCableF32(filter, 0, dry_wet_mixer, 0);
        }
        void Unload() override {
          if (reverb) ReleaseBungverb(reverb);
          reverb = nullptr;
          AllowRestart();
        }

        void Controller() override {
            if (reverb) {
              reverb->setDecayTime(decay_time + decay_time_cv.InF());
              reverb->setDamping(1.0f - ((damp * 0.01f) + damp_cv.InF()));
            } else {
              dry_wet_mixer.gain(1, 1.0f);
              return;
            }

            float freq = constrain(static_cast<float>(cutoff)
              + (cutoff_cv.InF() * abs(cutoff_cv.InF()) * 18000.0), 10.0f, 20000.0);
            filter.frequency(freq);

            float m = constrain(static_cast<float>(mix) * 0.01f + mix_cv.InF(), 0.0f, 1.0f);

            dry_wet_mixer.gain(0, m);
            dry_wet_mixer.gain(1, 1.0f - m);
        }

        FLASHMEM void View() override {
            if (!reverb) {
              gfxPrint(1, 15, "Out Of RAM !!!");
              return;
            };
            gfxPrint(1, 15, "T: ");
            gfxStartCursor();
            graphics.printf("%d.%1ds", SPLIT_INT_DEC(decay_time, 10));
            gfxEndCursor(cursor == DECAY_TIME);

            gfxStartCursor();
            gfxPrint(decay_time_cv);
            gfxEndCursor(cursor == DECAY_TIME_CV, false, decay_time_cv.InputName());

            gfxPrint(1, 25, "Damp:");
            gfxStartCursor();
            graphics.printf("%3d%%", damp);
            gfxEndCursor(cursor == DAMP);

            gfxStartCursor();
            gfxPrint(damp_cv);
            gfxEndCursor(cursor == DAMP_CV, false, damp_cv.InputName());

            gfxPrint(1, 35, "C:");
            gfxStartCursor();
            graphics.printf("%5dHz", cutoff);
            gfxEndCursor(cursor == CUTOFF);

            gfxStartCursor();
            gfxPrint(cutoff_cv);
            gfxEndCursor(cursor == CUTOFF_CV, false, cutoff_cv.InputName());

            gfxPrint(1, 45, "Mix:");
            gfxStartCursor();
            graphics.printf("%3d%%", mix);
            gfxEndCursor(cursor == MIX);

            gfxStartCursor();
            gfxPrint(mix_cv);
            gfxEndCursor(cursor == MIX_CV, false, mix_cv.InputName());

            gfxDisplayInputMapEditor();
        }

        FLASHMEM void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
            data[0] = PackPackables(mix, damp);
            data[1] = PackPackables(decay_time_cv, damp_cv, mix_cv);
            data[2] = PackPackables(cutoff, cutoff_cv);
            data[3] = PackPackables(decay_time);
        }

        FLASHMEM void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
            UnpackPackables(data[0], mix, damp);
            UnpackPackables(data[1], decay_time_cv, damp_cv, mix_cv);
            UnpackPackables(data[2], cutoff, cutoff_cv);
            UnpackPackables(data[3], decay_time);
        }

        FLASHMEM void OnButtonPress() override {
            if (CheckEditInputMapPress(cursor,
                IndexedInput(MIX_CV, mix_cv),
                IndexedInput(DECAY_TIME_CV, decay_time_cv),
                IndexedInput(DAMP_CV, damp_cv),
                IndexedInput(CUTOFF_CV, cutoff_cv)
            ))
            return;
          CursorToggle();
        }

        FLASHMEM void OnEncoderMove(int direction) override {
            if (!EditMode()) {
                MoveCursor(cursor, direction, MIX_CV);
                return;
            }

            if (EditSelectedInputMap(direction)) return;

            switch (cursor) {
                case DECAY_TIME:
                    // Was `decay_time + (direction * 0.1)` accumulated into a
                    // float: binary-inexact, so it walked off the tenths grid
                    // the display assumes (SPLIT_INT_DEC(decay_time, 10), :61),
                    // and floored at 0.0s while the engine clamps to 0.1s
                    // internally (effect_reverb_schroeder_F32.h:28) -- so the
                    // screen could read 0.0s for a reverb running at 0.1s.
                    // NudgeDecayTenths() works in integer tenths and writes back
                    // an exact multiple. Finding M-4.
                    NudgeDecayTenths(direction);
                    break;
                case DECAY_TIME_CV:
                    decay_time_cv.ChangeSource(direction);
                    break;
                case DAMP:
                    // Through the accessor so the 99 ceiling has ONE definition
                    // and one explanation -- see kMaxDamp below for why it is
                    // 99 and not Freeverb's 100.
                    NudgeDamp(direction);
                    break;
                case DAMP_CV:
                    damp_cv.ChangeSource(direction);
                    break;
                case CUTOFF:
                    cutoff = constrain(cutoff + direction * 50, 0, 17500);
                    break;
                case CUTOFF_CV:
                    cutoff_cv.ChangeSource(direction);
                    break;
                case MIX:
                    mix = constrain(mix + direction, 0, 100);
                    break;
                case MIX_CV:
                    mix_cv.ChangeSource(direction);
                    break;
                default:
                    break;
            }
        }

        // --- Standalone full-screen host interface (apps/BungverbApp.h) ----
        //
        // Narrow, semantic accessors so a host app can render and edit these
        // without holding a copy: this applet stays the single owner of every
        // parameter. Same shape and reasoning as DelayApplet's and
        // ReverbApplet's blocks -- accessors rather than a friend declaration
        // or a shared struct, because either makes the APPLET know about the
        // HOST and one host serves all three. Every setter clamps.
        static constexpr int kMaxCutoff = 17500;
        static constexpr int kMinDecayTenths = 1;    // 0.1s, the engine's floor
        static constexpr int kMaxDecayTenths = 200;  // 20.0s

        // decay_time is stored as a float because that is what the save format
        // already packs (:97-107, data[3]); these two keep every WRITE on the
        // exact tenths grid rather than changing the stored type.
        int GetDecayTenths() const {
            return (int)lroundf(decay_time * 10.0f);
        }
        void NudgeDecayTenths(int d) {
            const int t = constrain(GetDecayTenths() + d,
                                    kMinDecayTenths, kMaxDecayTenths);
            decay_time = (float)t * 0.1f;
        }

        // DAMP POINTS THE SAME WAY HERE AS IN FREEVERB: larger = darker. It
        // looks inverted (:38 passes 1.0f - damp*0.01f) and it is not -- the two
        // engines assign their coefficient to opposite operands.
        // AudioEffectReverbSchroederF32 multiplies the INPUT by damp1
        // (effect_reverb_schroeder_F32.h:92), so LARGER means brighter there and
        // this applet inverts to compensate; AudioEffectFreeverbF32 multiplies
        // the STATE (effect_freeverb_F32.h:71), so larger already means darker
        // and FreeverbApplet passes it straight through. Both applets are
        // correct. Audio-Apps-Screens.md finding M-1 says otherwise and is
        // wrong; making the two "agree" would invert this one.
        //
        // The 99 ceiling is LOAD-BEARING, not a typo against Freeverb's 100:
        // damp=100 gives d=0, and at d=0 the comb damping state freezes
        // permanently and injects a constant into the feedback path.
        static constexpr int kMaxDamp = 99;

        int GetDamp() const { return damp; }
        void NudgeDamp(int d) { damp = constrain(damp + d, 1, kMaxDamp); }

        int GetCutoff() const { return cutoff; }
        void NudgeCutoff(int d) { cutoff = constrain(cutoff + d, 0, kMaxCutoff); }

        int GetMix() const { return mix; }
        void NudgeMix(int d) { mix = constrain(mix + d, 0, 100); }

        CVInputMap& DecayCV() { return decay_time_cv; }
        CVInputMap& DampCV() { return damp_cv; }
        CVInputMap& CutoffCV() { return cutoff_cv; }
        CVInputMap& MixCV() { return mix_cv; }

        // False when Factory::get() could not find ~85KB for the reverb arena in
        // RAM2 or its PSRAM fallback (OC_core.h:57-73). Controller() already
        // fails safe -- dry forced to unity, wet silent (:39-42) -- so the
        // failure is "no effect", not "no signal".
        bool ArenaReady() const { return reverb != nullptr; }

    protected:
        void SetHelp() override {}

    private:
        enum Cursor: int8_t {
            DECAY_TIME,
            DECAY_TIME_CV,
            DAMP,
            DAMP_CV,
            CUTOFF,
            CUTOFF_CV,
            MIX,
            MIX_CV
        };

        int8_t cursor = DECAY_TIME;

        AudioEffectReverbSchroederF32* reverb = nullptr;
        AudioFilterStateVariable2F32 filter;

        AudioMixer4_F32 dry_wet_mixer;

        int8_t mix = 50;
        float decay_time = 1.0f;
        int8_t damp = 50;
        int16_t cutoff = 15000;

        CVInputMap mix_cv;
        CVInputMap decay_time_cv;
        CVInputMap damp_cv;
        CVInputMap cutoff_cv;
    };
