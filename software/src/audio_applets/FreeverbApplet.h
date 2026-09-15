#pragma once

// F32-native: Freeverb's comb/allpass recirculation, the post-filter, and the
// dry/wet mix stay float32 inside the applet (see effect_freeverb_F32.h); the
// chain still sees int16 via the HemisphereAudioAppletF32 edge adapters.

#include "../HemisphereAudioAppletF32.h"
#include "../extern/f32/AudioMixer_F32.h"
#include "../Audio/effect_freeverb_F32.h"
#include "../Audio/filter_variable2_F32.h"

class ReverbApplet : public HemisphereAudioAppletF32<MONO> {
    public:
        const char* applet_name() {
            return "Reverb";
        }
        void Start() override {
            if (!reverb) {
              reverb = GetFreeverb();
            }
            PatchCableF32(InputF32(), 0, dry_wet_mixer, 1);
            PatchCableF32(dry_wet_mixer, 0, OutputF32(), 0);
            if (!reverb) return;
            PatchCableF32(InputF32(), 0, *reverb, 0);
            PatchCableF32(*reverb, 0, filter, 0);
            PatchCableF32(filter, 0, dry_wet_mixer, 0);
        }
        void Unload() override {
          if (reverb) ReleaseFreeverb(reverb);
          reverb = nullptr;
          AllowRestart();
        }

        void Controller() override {
            if (reverb) {
              reverb->roomsize((size * 0.01f) + size_cv.InF());
              reverb->damping((damp * 0.01f) + damp_cv.InF());
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
            gfxPrint(1, 15, "Size:");
            gfxStartCursor();
            graphics.printf("%3d%%", size);
            gfxEndCursor(cursor == SIZE);

            gfxStartCursor();
            gfxPrint(size_cv);
            gfxEndCursor(cursor == SIZE_CV, false, size_cv.InputName());

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
            data[0] = PackPackables(mix, size, damp);
            data[1] = PackPackables(size_cv, damp_cv, mix_cv);
            data[2] = PackPackables(cutoff, cutoff_cv);
        }

        FLASHMEM void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
            UnpackPackables(data[0], mix, size, damp);
            UnpackPackables(data[1], size_cv, damp_cv, mix_cv);
            UnpackPackables(data[2], cutoff, cutoff_cv);
        }

        FLASHMEM void OnButtonPress() override {
            if (CheckEditInputMapPress(cursor,
                IndexedInput(MIX_CV, mix_cv),
                IndexedInput(SIZE_CV, size_cv),
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
                case SIZE:
                    size = constrain(size + direction, 0, 100);
                    break;
                case SIZE_CV:
                    size_cv.ChangeSource(direction);
                    break;
                case DAMP:
                    damp = constrain(damp + direction, 1, 100);
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

        // --- Standalone full-screen host interface (apps/ReverbApp.h) -------
        //
        // Narrow, semantic accessors so a host app can render and edit these
        // without holding a copy: this applet stays the single owner of every
        // parameter. Same shape and same reasoning as DelayApplet's block --
        // accessors rather than a friend declaration or a shared struct,
        // because either of those makes the APPLET know about the HOST and one
        // host serves all of these. Every setter clamps, so no caller has to
        // know a range.
        //
        // The ranges below are this applet's own, unchanged: OnEncoderMove()
        // clamps size/mix to 0..100, damp to 1..100 (not 0 -- see :136) and
        // cutoff to 0..17500 (:142).
        static constexpr int kMaxCutoff = 17500;

        int GetSize() const { return size; }
        void NudgeSize(int d) { size = constrain(size + d, 0, 100); }

        int GetDamp() const { return damp; }
        void NudgeDamp(int d) { damp = constrain(damp + d, 1, 100); }

        int GetCutoff() const { return cutoff; }
        void NudgeCutoff(int d) {
          cutoff = constrain(cutoff + d, 0, kMaxCutoff);
        }

        int GetMix() const { return mix; }
        void NudgeMix(int d) { mix = constrain(mix + d, 0, 100); }

        CVInputMap& SizeCV() { return size_cv; }
        CVInputMap& DampCV() { return damp_cv; }
        CVInputMap& CutoffCV() { return cutoff_cv; }
        CVInputMap& MixCV() { return mix_cv; }

        // False when Factory::get() could not find ~50KB for the reverb arena,
        // in RAM2 or in its PSRAM fallback (OC_core.h:57-73). Controller()
        // already fails safe in that case -- it forces dry to unity and returns
        // (:38-41) -- so the failure is "no effect", not "no signal". What was
        // missing was a way for a host to SAY so.
        bool ArenaReady() const { return reverb != nullptr; }

    protected:
        void SetHelp() override {}

    private:
        enum Cursor: int8_t {
            SIZE,
            SIZE_CV,
            DAMP,
            DAMP_CV,
            CUTOFF,
            CUTOFF_CV,
            MIX,
            MIX_CV
        };

        int8_t cursor = SIZE;

        AudioEffectFreeverbF32* reverb = nullptr;
        AudioFilterStateVariable2F32 filter;

        AudioMixer4_F32 dry_wet_mixer;

        int8_t mix = 50;
        int8_t size = 50;
        int8_t damp = 50;
        int16_t cutoff = 15000;

        CVInputMap mix_cv;
        CVInputMap size_cv;
        CVInputMap damp_cv;
        CVInputMap cutoff_cv;
    };
