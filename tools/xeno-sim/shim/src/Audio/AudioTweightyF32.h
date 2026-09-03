#ifndef XENOSIM_AUDIO_TWEIGHTYF32_H_
#define XENOSIM_AUDIO_TWEIGHTYF32_H_
// ---------------------------------------------------------------------------
// The real Audio/AudioTweightyF32.h, with only its update() method's body
// kept off this host.
//
// update()'s denormal guard reads/writes the Cortex-M7 FPSCR with inline
// `vmrs`/`vmsr` asm -- real armv7e-m instructions. Two separate problems on
// this host:
//   1. Sema (-Wasm-operand-widths, -Werror'd into a hard stop) objects to
//      the constraint/operand-width pairing before it ever reaches codegen.
//   2. Even past that, arm64/x86_64 have no such mnemonic at all, so the
//      integrated assembler itself would fail with "unrecognized
//      instruction" the moment this function's body is actually emitted.
// update() is dead code on this host either way -- see the README's
// Tweighty section: there is no audio-rate callback here at all, so it was
// never going to run.
//
// util/util_math.h's shim handles the same class of problem (ARM-only
// ssat/usat inline asm) by renaming the offending free functions out of the
// way: never called, never emitted, so problem 2 never arises (problem 1
// still needs its pragma, same as here). update() can't take the rename
// alone as-is, because it isn't a free function -- it's the override that
// satisfies AudioStream_F32's `virtual void update(void) = 0`, so as long
// as it keeps that name on a class that gets instantiated, its address is
// always taken for a vtable, asm and all, regardless of whether anything
// ever calls it.
//
// So the rename goes one level up: for the extent of one include below, the
// real class is parsed under a name nothing else references, with its
// update() renamed too (so it no longer overrides anything and the real
// header's `override` on it -- stripped here for the same reason -- would
// otherwise be a hard error on its own). Never instantiated under that
// name, its renamed update() -- vmrs/vmsr included -- is unreferenced and
// never reaches codegen, so problem 2 never arises either; the pragma below
// covers problem 1 for the one Sema pass that does still parse it.
//
// Every header AudioTweightyF32.h itself pulls in is included for real,
// unrenamed, ABOVE the rename -- each has its own include guard, so when
// AudioTweightyF32.h's nested copies of the same #include lines run again
// below, they resolve to no-ops. Skipping this step was tried first and
// broke the whole rest of the build: AudioStream_F32.h's own
// `virtual void update(void) = 0` (and shim/arduino/AudioStream.h's
// int16 counterpart, reached the same way through <Audio.h>) got swept up
// in the same rename, since it is only first-included right here, so
// every OTHER real update() override in this compile -- HemisphereAudioApplet's
// reverb/freeverb/dynamics, the I16<->F32 converters -- silently stopped
// satisfying it for the rest of the translation unit.
//
// The concrete class the rest of the firmware actually uses keeps the real
// name below: same constructor, same layout, same Acquire()/Release()/
// IsReady() and every Set*/Get* accessor and field TweightyApp.h reads --
// all real, all inherited -- with update() itself replaced by a no-op,
// which is exactly what it already was here: this simulator's
// engine_.update() was never called by anything.
// ---------------------------------------------------------------------------
#include <real/Audio/AudioBuffer.h>
#include <real/Audio/AudioParam.h>
#include <real/dsputils.h>
#include <real/util/util_macros.h>
#include <real/extern/f32/AudioStream_F32.h>
#include <real/TweightyTransport.h>
#include <real/TweightyTapPhase.h>
#include <Audio.h>

#define AudioTweightyF32 AudioTweightyF32_arm_asm_unused_on_host
#define update update_arm_asm_unused_on_host
#define override
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wasm-operand-widths"
#endif
#include <real/Audio/AudioTweightyF32.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#undef override
#undef update
#undef AudioTweightyF32

class AudioTweightyF32 : public AudioTweightyF32_arm_asm_unused_on_host {
public:
  void update(void) override {}
};

#endif  // XENOSIM_AUDIO_TWEIGHTYF32_H_
