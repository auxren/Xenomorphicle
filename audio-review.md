# Float32 audio engine — correctness review

Read-only review, 2026-09-25, branch `main` @ 63027138. No files edited, no hardware
touched. Everything below was checked against the source in the repo plus the
Teensy core actually linked from
`~/.platformio/packages/framework-arduinoteensy/cores/teensy4/`.

## STATUS 2026-09-26 — what has been fixed since this review was written

Findings **5, 6, 7 and 9 are closed by one change** and none of them by the
per-site fix each section proposes. They were all the same mechanism: the
Teensy Audio library walks one list of every `AudioStream` in construction
order and never reorders it, so any object constructed before something that
feeds it consumes that input a block late. `src/AudioGraphOrder` now sorts
that list into dependency order and re-sorts it automatically whenever the
wiring changes (PR #7, fc1b7097). Bench: 20 late cables -> 0, "every cable
runs forwards".

That includes **#9, the only audible finding here.** The wet leg of
`PhaserApplet` was late because `dry_wet_mixer` is a member and `phaser` is
heap-allocated in `Start()`; the sort now puts `phaser` first regardless. Its
dry/wet split and sum form a DAG, not a feedback loop, so it is ordered
correctly rather than left in the unorderable set. `FreeverbApplet`'s 2-block
wet path goes the same way. **Do not rewrite either applet's allocation
order** -- the per-applet fix this review suggests would have had to be
remembered by every applet written afterwards.

What the sort cannot fix, and should not: genuine feedback loops. Around 14
nodes sit in loops once the full app set is loaded, and the few cables inside
them stay one block late. That is what feedback is. Console `O` reports them
as such.

Also closed since: **11** (the 32678 scale, PR #5 -- 65,535 of 65,536 int16
values failed a codec round trip, fixed with the asymmetric clamp it needed),
**14** (`OutputStream()`'s `new`ed streams published into the ISR walk list
before `next_update` was cleared, PR #3), **15** (`cable_count`), and
earlier: **1, 2, 3, 4, 8, 10, 12, 13**.

Still open: nothing in this document that is both real and fixable. See
`docs/Timing-Budget.md` for the measured state and for one recorded dead end
(`init_priority` on AudioIO's streams fixes ordering and kills the codec).

## Ordering facts this review depends on

`AudioStream::AudioStream()` (core `AudioStream.h:150-158`) **appends** itself to
`first_update`, and `software_isr()` (`AudioStream.cpp:444`) walks that list head to
tail. So **update order == construction order**, for the whole graph. `AudioStream_F32`
derives from `AudioStream`, so F32 and int16 streams share one list.

Consequences used throughout:
- File-scope/static objects are constructed before `main()` → they update **first**.
- Class members are constructed in declaration order; **base-class members before
  derived-class members**.
- Heap objects (`new`, `Factory::get()`, `Registry::get()`) take a position at the
  moment they are allocated.
- A stage that updates before its upstream consumes the block its upstream produced on
  the *previous* pass → **+1 block of latency per inversion**.

One block on this build = `AUDIO_BLOCK_SAMPLES / AUDIO_SAMPLE_RATE_EXACT` =
**128 / 48000 = 2.667 ms** (the linked core header defines 48000.0f, not 44100).

`-fno-exceptions` is on (`platformio.ini:66`) and `-fcheck-new` is **not**. GCC emits
no null test for `new T(...)`, so a failed allocation runs the constructor with
`this == nullptr`.

---

# P1 — Correctness / crash

## 1. `OutputStream()` builds the I2S output *before* the monitor mixers that feed it

**`software/src/AudioIO.cpp:127` vs `:136-143`** (24-bit USB builds:
`AUDIO_INTERFACE && AUDIO_SUBSLOT_SIZE == 3`, i.e. `env:T41_audio`,
`platformio.ini:115`)

```cpp
output_stream = new AudioOutputI2S2_F32();        // line 127  <-- constructed FIRST
...
usbmix_f32[0] = new AudioMixer4_F32();            // line 136
usbmix_f32[1] = new AudioMixer4_F32();            // line 137
...
new AudioConnection_F32(*usbmix_f32[0], 0, *output_stream, 0);   // line 142
new AudioConnection_F32(*usbmix_f32[1], 0, *output_stream, 1);   // line 143
```

The signal flows `usbmix_f32 -> output_stream`, but `output_stream` is constructed on
line 127 and the mixers on lines 136-137, so `output_stream->update()` runs **before**
`usbmix_f32[n]->update()` on every pass. The codec output therefore always plays the
monitor mix from the previous block.

**Failure:** +1 block = **+2.667 ms** of output latency on the codec path, in exactly
the function whose entire purpose (and 8-line comment, `AudioIO.cpp:117-123`) is to
avoid that. It is invisible on the 16-bit path and on `env:T41_console` (the `#else`
branch at `:154-158` constructs `conv_out` then `output_stream`, which is correct).

**Fix shape:** move the `output_stream = new ...` line below the `usbmix_f32`
allocations in the 24-bit branch (the other two branches already order it correctly,
so the allocation has to move per-branch, not stay shared).

**Confidence: high.** Pure construction order, no timing assumption.

## 2. Five unchecked `new` sites inside `OutputStream()` itself

**`software/src/AudioIO.cpp:125, 126, 127, 136, 137`**

```cpp
conv_out[0] = new AudioConvert_I16toF32();   // 125 — unchecked
conv_out[1] = new AudioConvert_I16toF32();   // 126 — unchecked
output_stream = new AudioOutputI2S2_F32();   // 127 — unchecked
usbmix_f32[0] = new AudioMixer4_F32();       // 136 — unchecked
usbmix_f32[1] = new AudioMixer4_F32();       // 137 — unchecked
```

All five are dereferenced immediately afterwards (`*conv_out[0]` at 130/133/140,
`*output_stream` at 142/152/157, `*usbmix_f32[0]` at 138/140/142). With
`-fno-exceptions` the Teensy `operator new` returns NULL on failure and GCC emits no
check, so the failure mode is a write through a null `this` inside
`AudioOutputI2S2_F32`'s constructor (which also calls `begin()` and programs DMA) —
not a graceful "no audio".

The bare `new AudioConnection_F32(...)` statements on lines 133/134/138-143/152/153/
157/158 are the same class of hazard (the 4-arg ctor calls `connect()`), and their
results are deliberately discarded, so there is nothing to check even if you wanted to.

**Practical risk:** moderate-low. This runs at boot from `AppQuadrants::Init()` (see
finding 6), after the five default applets have already been `calloc`'d from the same
RAM2 heap — the heap is not pristine at that point, and `Registry`'s factory
(`AppletRegistry.h:49`) deliberately drains RAM2 down to `RAM2_HEADROOM` (10240 bytes,
`OC_core.h:20`) before falling back to PSRAM. `new` here has no such guard.

**Confidence: high** that the check is missing; **medium** that it is reachable in
practice.

## 3. `Registry::get()` dereferences the factory result before returning it

**`software/src/AppletRegistry.h:106-107`**

```cpp
instances[slot][idx] = factories[idx]();
Serial.println(instances[slot][idx]->applet_name());   // <-- line 107
```

The factory (`AppletRegistry.h:48-53`) explicitly returns `nullptr` when both
`calloc` and `extmem_calloc` fail. Every caller handles that correctly —
`AudioAppletSubapp::get_mono_applet()` / `get_stereo_applet()` (`AudioAppletSubapp.h:
583-608`) test for null and fall back to `dummy_mono`/`dummy_stereo` — but line 107
dereferences it one line earlier, inside `get()`.

**Failure:** hard fault on `nullptr->applet_name()` the first time an applet is
selected with both RAM2 and PSRAM exhausted. The null-safety the callers were given is
unreachable. Exactly the scenario the bench note in `AudioDelayExtF32::Acquire()`
(`Audio/AudioDelayExtF32.h:40-49`) describes for Sampler-then-Delay.

**Confidence: high.**

## 4. Three unchecked `AudioStream::allocate()` sites (int16 pool)

The F32 side is uniformly clean; the int16 side has three survivors.

| file:line | object | what happens |
|---|---|---|
| `software/src/Audio/AudioMixer.h:44-45` | `AudioMixer<N>::update()` | `allocate()` then `arm_float_to_q15(out, out_block->data, ...)` — null write |
| `software/src/Audio/AudioVCA.h:48-49` | `AudioVCA::update()` | `allocate()` then `arm_float_to_q15(signal_f32, out->data, ...)` — null write |
| `software/src/Audio/InterpolatingStream.h:75` | `InterpolatingStream<>::update()` | `allocate()` passed straight into `UpdateZOH/Linear/Hermite`, each of which writes `out->data[i]` (lines 95, 105, 117) |

`AudioSummingRoute::update()` immediately below `AudioMixer` in the same file
(`AudioMixer.h:100-105`) **does** check — so the omission at line 44 is visibly an
oversight, not a policy.

Reachability: `AudioMixer<2>` is live in `UpsampledApplet` (`UpsampledApplet.h:140`),
`AdvKrpsStrngApplet` (`:285`), `AnimorfApplet` (`:339`), `AbyssApplet` (`:235`),
`ThreeBandz` (`AudioMixer<BANDZ>`, `:267`) and in `AudioIO.cpp:84` on 16-bit USB
builds. `AudioVCA`/`InterpolatingStream` are the int16 VCA/CV path.

**Failure:** hard fault in the audio ISR when the 252-block int16 pool is exhausted —
i.e. precisely under the overload condition the pool guard exists to survive.

**Confidence: high.**

---

# P2 — Latency / DSP correctness

## 5. `output_route` is a static object, so the applet chain can never update before it

**`software/src/AudioIO.cpp:60`**

```cpp
AudioSummingRoute<kOutputRouteChannels, kOutputRouteSources> output_route;
```

`OutputStream()` returns `output_route`, and every audio source in the instrument
connects *to* it. But `output_route` is a namespace-scope object: it is constructed
before `main()`, while every audio applet is constructed at runtime by
`Registry::get()` (`AppletRegistry.h:51`, placement-new into `calloc`'d memory, on
first selection). So `output_route.update()` runs **before every applet's update()**,
in every pass, permanently.

**Failure:** a fixed **+1 block (2.667 ms)** between the applet chain tail and the
codec, which the lazy-construction hack cannot remove because it only defers
`conv_out`/`output_stream`/`usbmix_f32`, never `output_route` itself.

To actually satisfy the documented invariant, `output_route` would have to be lazily
constructed too — and after the applets, which is impossible while applets are created
on demand. The honest options are (a) accept and document the one block, or (b) move
`output_route` to the heap and build it inside `OutputStream()` alongside the rest of
the tail, which recovers the block at boot but loses it again the first time a
runtime-selected applet is instantiated.

Same class, unspecified rather than definite: `AppSampler::slot_mix_`
(`apps/SamplerApp.h:177`) and `AppTweighty`'s adapters are static objects in
`app_container` (`apps/_config.h:88`), in a **different translation unit** from
`output_route`. Their relative construction order is link-order dependent, so whether
Sampler/Tweighty pay an extra block is currently a property of the link map, not of the
source. Worth pinning down deliberately.

**Confidence: high** for the applet-chain case, **high** for the TU-order fragility.

## 6. The boot-time ordering requirement *is* met for the default applets — but only there

Traced end to end, because the task asked specifically:

- `AppQuadrants` is second in `app_container` (`apps/_config.h:88-90`), ahead of
  `AppScope`/`AppSampler`.
- `AppSwitcher::Init()` runs `app.InitDefaults(app.instance)` for every app
  (`OC_apps.cpp:770-773`) → `AppBase::InitDefaults()` → `Init()`
  (`OC_app_base.cpp:457-460`).
- `AppQuadrants::Init()` (`apps/Quadrants.h:1536`) → `BaseStart()`
  (`HSApplication.h:82-86`) → `Start()` → `audio_app.Init()` (`apps/Quadrants.h:40`).
- `AudioAppletSubapp::Init()` (`AudioAppletSubapp.h:54-67`) walks slots **0..4 in
  ascending order**, constructing each slot's default applet via `Registry::get()` as
  it goes, and at slot 4 `ConnectMonoToNext`/`ConnectStereoToNext`
  (`AudioAppletSubapp.h:356` / `:374`) calls `&OC::AudioIO::OutputStream()` — which is
  where the output tail is actually built.

So at a **factory-default boot** the five default applets exist before the tail, in
chain order, and the tail is correct apart from finding 1. `AppScope::WireAudio()`
(`apps/ScopeApp.h:141`), `AppSampler::WireAudio()` (`apps/SamplerApp.h:225`) and
`Main.cpp:731` all hit the already-built `output_stream` and are no-ops for
construction. **The `AppBase::Init()`-for-every-app concern in the task brief does not
in fact cost latency at boot.**

What *does* break it:

- **Preset/EEPROM restore.** `RestoreAppData()` runs at `OC_apps.cpp:914`, i.e. **after**
  the `InitDefaults` pass at `:770`. Any applet the stored preset selects is
  instantiated by `ChangeMonoApplet`/`ChangeStereoApplet`
  (`AudioAppletSubapp.h:272-296`) at that point — after `conv_out` and `output_stream`
  already exist. `LoadPreset()` (`AudioAppletSubapp.h:404-457`) is the same story at
  runtime.
- **Any user applet change**, in any order. Slots can be filled in arbitrary order, and
  each upstream-after-downstream inversion costs another block.
- Finding 5 makes the applet-tail → `output_route` hop wrong unconditionally.

**Net answer to the brief's question:** the requirement is met for the default chain
and violated for every non-default one; cost is 2.667 ms per inversion, minimum one
inversion (finding 5), realistically 1-3 blocks (2.7-8.0 ms) on a loaded preset.

**Confidence: high.**

## 7. Every F32-native applet pays a structural extra block

**`software/src/HemisphereAudioAppletF32.h:105-106`**

```cpp
AudioConvertI16toF32Multi<Channels> input_adapter;
AudioConvertF32toI16Multi<Channels> output_adapter;
```

These are **base-class** members, so they are constructed before every derived-class
DSP member. `output_adapter` therefore updates before the applet's internal chain that
feeds it, on every pass, for every F32 applet (`VcaApplet`, `InputApplet`,
`DelayApplet`, `PhazerApplet`, `FreeverbApplet`, `LadderApplet`, ... — all of
`HemisphereAudioAppletF32`'s subclasses).

**Failure:** +1 block (2.667 ms) per F32 applet in the chain. Five F32 applets in the
five slots = 13.3 ms of avoidable latency on top of findings 1 and 5.

**Fix shape:** the adapters cannot be base members if ordering matters — either give
the base a two-phase construct (input adapter in the base, output adapter constructed
by the derived class last), or accept it and document it.

**Confidence: high.**

## 8. `InputApplet` declares its VCAs before the source mixer that feeds them

**`software/src/audio_applets/InputApplet.h:236-243`**

```cpp
AudioConvertI16toF32Multi<2> i2s_conv;      // 236
...
std::array<InterpolatingStreamF32<>, 2> attenuations;  // 239
std::array<AudioVCA_F32, 2> vcas;           // 240   <-- declared BEFORE srcmix
AudioMixerF32<2> srcmix[2];                 // 241
AudioMixerF32<3> mixer[Channels];           // 242
```

The patch built in `Start()` is `i2s_conv -> srcmix -> vcas -> mixer -> OutputF32()`
(`InputApplet.h:60`, `:68`, `:76`, `:79`), but `vcas` is declared — and therefore
constructed and updated — before `srcmix`.

**Failure:** `vcas` consumes `srcmix`'s previous-pass block. Combined with finding 7
(`output_adapter` before `mixer`), the **Input applet alone adds 2 blocks = 5.33 ms**
over the ideal. Since `InputApplet` is the default slot-0 applet, this is on the
signal path of essentially every patch.

**Fix:** swap the declaration order of `vcas` and `srcmix`. One-line, zero behaviour
change otherwise.

`UpsampledApplet` (`UpsampledApplet.h:138-141`) and `DelayApplet::DelayChannel`
(`DelayApplet.h:591-595`) declare theirs in correct flow order — so this is a local
slip, not a house style.

**Confidence: high.**

## 9. `PhazerApplet`: the wet leg is one block late relative to the dry leg

**`software/src/audio_applets/PhaserApplet.h:21` and `:26-27`**

```cpp
phaser = new AudioEffectPhazerF32();          // line 21, heap, at Start()
...
PatchCableF32(InputF32(), 0, dry_wet_mixer, 1);   // dry
PatchCableF32(InputF32(), 0, *phaser, 0);         // wet
PatchCableF32(*phaser, 0, dry_wet_mixer, 0);
```

with (`PhaserApplet.h:26-27`):
```cpp
AudioEffectPhazerF32* phaser = nullptr;   // heap, allocated in Start()
AudioMixer4_F32 dry_wet_mixer;            // member, constructed with the applet
```

`dry_wet_mixer` exists from the moment the applet is constructed; `phaser` only
appears when `Start()` runs. So `dry_wet_mixer` updates **before** `phaser`, and the
wet input to the summing mixer is 128 samples older than the dry input.

**This one is not merely latency.** A phaser is defined by the interference between dry
and all-passed wet. A 128-sample misalignment superimposes a fixed comb with ~375 Hz
notch spacing (1/2.667 ms) on top of the intended sweeping notches. The applet does not
sound like a phaser at the notch depths it is configured for.

Same mechanism, benign consequence, in `FreeverbApplet` (`FreeverbApplet.h:216-219`:
`reverb` is heap via `Factory::get()`, `filter` and `dry_wet_mixer` are members, so the
wet path is **2** blocks / 5.33 ms late) — for a reverb that is just pre-delay.

**Fix shape:** allocate the effect object before anything that consumes it, or make
`dry_wet_mixer` heap-allocated after it in `Start()`.

**Confidence: high** for the ordering; **medium-high** that it is audible as a phaser
character change rather than just latency (that part is a DSP argument, and worth a
bench A/B against a correctly-ordered build before anyone rewrites the applet).

## 10. `Factory<T, N>` hands the same instance to multiple owners when N > 16

**`software/src/OC_core.h:55-79`**

```cpp
template <typename T, size_t max_instances>
struct Factory {
  std::array<T*, max_instances> pool;
  uint16_t mask = 0;                              // <-- 16 bits
  ...
      if (mask & (1 << i)) continue;              // i up to max_instances-1
  ...
      mask |= (1 << i);
```

`mask` is `uint16_t`, but `HemisphereAudioApplet::compressor_factory` is declared
`Factory<AudioEffectDynamics, 20>` (`HemisphereAudioApplet.h:27`,
`audio_applets/_config.h:60`). For `i >= 16`, `mask & (1 << i)` is always 0 and
`mask |= (1 << i)` truncates away, so slots 16-19 are **never marked in use**.

**Failure:** once the first 16 compressors are taken, `get()` returns `pool[16]` to
*every subsequent caller*. Two or more applets then connect to the same
`AudioEffectDynamics` object: the core's `AudioConnection::connect()` refuses the
second connection to an already-used destination input (`AudioStream.cpp:262-272`,
`result = 4`), so the second applet silently gets no signal, and `Controller()` from
both applets fights over one compressor's parameters.

Reachable: `ThreeBandzApplet<STEREO>` takes `BANDZ * 2 = 6` compressors; three stereo
ThreeBandz slots is 18. `DynamicsApplet` uses the separate
`compressor_f32_factory` (`Factory<..., 10>`), which is under the limit.

**Fix:** `uint32_t mask` (and a `static_assert(max_instances <= 32)`).

**Confidence: high** on the arithmetic; **medium** on how often a user reaches 17+
concurrent instances.

---

# P3 — Lower severity / latent

## 11. `AudioConvert_F32toI16` scales by 32678, not 32768

**`software/src/extern/f32/AudioConvert_F32.h:144`** (and the same typo at `:199` in
`convertAudio_F32toI16x2`)

```cpp
const float MAX_INT = 32678.0;                 // should be 32768.0
out->data[i] = (int16_t)(max(min((in->data[i] * MAX_INT), MAX_INT), -MAX_INT));
```

The opposite direction, `convertAudio_I16toF32` (`:39`), correctly uses `32768.0`.

**Failure:** the F32→int16 boundary is **0.99725× (-0.0239 dB)** instead of unity, and
full-scale float clips at 32678 instead of 32767 (0.024 dB of lost headroom). A codec
round trip (`conv_in` F32→I16, applet bus, `conv_out` I16→F32) is therefore
-0.0239 dB rather than 0 dB. This is inherited from upstream OpenAudio/Tympan, not
introduced here.

It is far too small to hear and far too small to explain any real gain problem, but it
will show up as a systematic ~0.024 dB offset in any bench linearity measurement of the
int16 applet path, so it is worth knowing about before someone chases it.

**Confidence: high** (arithmetic).

## 12. `Acquire()` is not idempotent in two places — 16-32 KB leak on a double call

**`software/src/Audio/AudioDelayExtF32.h:50-51`** and
**`software/src/Audio/AudioTweightyF32.h:108-109`**

```cpp
xfade_in_scalars  = new (std::nothrow) float[CrossfadeSamples];
xfade_out_scalars = new (std::nothrow) float[CrossfadeSamples];
```

The underlying `ExtAudioBuffer::Acquire()` (`Audio/AudioBuffer.h:146-150`) **is**
guarded (`if (buffer == nullptr)`), but these two `new[]`s are not. A second
`Acquire()` without an intervening `Release()` overwrites both pointers and leaks
`2 * CrossfadeSamples * 4` bytes = **16 KB** (32 KB for a stereo `DelayApplet`, which
Acquires per channel).

Today both are guarded by their callers — `AppTweighty::ActivateOnce()`
(`apps/TweightyApp.h:405`) runs once per session, and `DelayApplet::Unload()`
(`DelayApplet.h:73-78`) calls `Release()` before any re-`Start()`. So this is latent,
not live. Given the comments at both sites already say the RAM2 heap is measurably
tight after visiting the Sampler, a `if (!xfade_in_scalars)` guard is cheap insurance.

**Confidence: high** that it is non-idempotent; **high** that it is currently
unreachable.

## 13. Unchecked `new AudioConnection(...)` across the app wiring

`apps/TunerApp.h:127-128`, `apps/ScopeApp.h:137-143`, `apps/SamplerApp.h:219-227`,
`apps/TweightyApp.h:352-369`, `AudioAppletHost.h:158, 171`,
`audio_applets/PhaserApplet.h:21`.

`TweightyApp` and `AudioAppletHost` null-check the *stored pointer* afterwards, which
is good hygiene for later use but does not help: the 4-arg `AudioConnection` ctor runs
`connect()` first, so with `this == nullptr` the fault has already happened.
`PhaserApplet:20` pre-checks `OC::CORE::FreeRam()` which narrows but does not close the
window. `TunerApp`, `ScopeApp` and `SamplerApp` do not check at all.

The cheap blanket fix for the whole family is `-fcheck-new` in `platformio.ini` (GCC
then emits the null test and skips the constructor), which costs a compare-and-branch
per `new` and turns every one of these into the intended "no cable, no crash".

**Confidence: high** on the mechanism; **low** on reachability at boot.

## 14. Dynamic `AudioStream` construction outside `AudioNoInterrupts()`

`AudioInputI2S2_F32`'s constructor calls `begin()` (`extern/f32/input_i2s2_F32.h:50`),
which enables the RX DMA and `update_setup()` at **static-init time** — so the audio
software ISR is live for the whole of `setup()` and all subsequent runtime wiring.

The core's `AudioStream` ctor links itself into `first_update` (`AudioStream.h:151-156`)
and only then assigns `next_update = NULL` (`:158`). For an object from `calloc`
(applets, `Factory`) that gap is harmless because the memory is already zero. For an
object from `new`/`malloc` it is not: an ISR traversal that reaches the new node inside
that window reads an uninitialised `next_update` and follows a wild pointer.

`OC::AudioIO::OutputStream()` (`AudioIO.cpp:124-160`) creates five such objects with no
interrupt bracket, and it is called from `Main.cpp:731`, `AudioAppletSubapp.h:356/374`,
`apps/ScopeApp.h:141`, `apps/SamplerApp.h:225`, `apps/TweightyApp.h:366`.
`AudioAppletSubapp::ChangeMonoApplet`/`ChangeStereoApplet` (`AudioAppletSubapp.h:
272-296`) construct applets from a button press with no bracket either (those use
`calloc`, so only the `new`ed cable arrays inside `PatchCable` are exposed).

`AudioAppletHost::StartOnce()`/`BuildCables()` (`AudioAppletHost.h:134-176`) already do
bracket with `AudioNoInterrupts()`, and the reasoning is written out there — so the
pattern is understood in this codebase and just not applied to `OutputStream()`.

**Confidence: high** on the mechanism; **low** on the probability of hitting the window.

## 15. `HemisphereAudioApplet::cable_count` has no in-class initialiser

**`software/src/HemisphereAudioApplet.h:37-38`**

```cpp
AudioConnection* cables = nullptr;   // initialised
size_t cable_count;                  // NOT initialised
```

Currently safe by accident: every applet is either placement-new'd into `calloc`'d
memory (`AppletRegistry.h:49-51`) or a static (`dummy_mono`/`dummy_stereo`,
`AudioAppletSubapp.h:580-581`), both zero-filled. Any future stack or plain-`new`
applet gets an indeterminate `cable_count`, which `PatchCable` (`:89`) and
`Disconnect()` (`:128`) both use as a bound → out-of-bounds indexing of `cables`.

**Confidence: high** that it is uninitialised; **high** that it is currently benign.

---

# Areas examined with nothing found

Stated explicitly rather than padded.

**`extern/f32/output_i2s2_F32.cpp` — block handling is correct.** I specifically chased
the things the brief named:

- The one-sided branches (`:180-192` `blockL` only, `:193-205` `blockR` only) write the
  correct interleave slots — even int32 slots for L, odd for R (`:189`, `:202`) — and
  the `+ i + 1` on the right branch is right, not an off-by-one. Both `memset(dest, 0,
  audio_block_samples * 4)` calls zero exactly the half-buffer (512 bytes of 1024).
- **No leak and no double release.** The release block at `:223-244` can only call
  `release(blockR)` when `offsetR >= audio_block_samples`, and `block_right_offset` is
  only ever written as `< audio_block_samples` (`:236`) or `0` (`:240`, and `:316`,
  `:329` in `update()`), so it can never be stale-high while `block_right_1st` is NULL.
  Symmetric for L.
- `update()` (`:251-341`) releases both scratch blocks on the alloc-failure path
  (`:260-261`) and on each "input never arrived" path (`:304`, `:339`).
- The cache maintenance is present and correctly sized on **all three** exits:
  `:217` (silence path), `:221` (normal path), plus `begin()`'s `:106` and
  `silence_now()`'s `:119`. `sizeof(i2s2_tx_buffer)/2` == 512 == the half `dest`
  covers. This is the frozen-drone bug from the project history, and it is fixed.

**`extern/f32/input_i2s2_F32.cpp` — clean.** The alloc-failure path releases both
blocks (`:188-189`) and NULLs them together, so the ISR's `left && right` test
(`:124`) can never see a half-pair. The `else if (new_left != NULL)` at `:214` uses
`new_right` unchecked, which is safe *only* because the failure path at `:185` nulls
both — correct, but load-bearing and undocumented. `I32_TO_F32_NORM_FACTOR` scaling
(`:171`) is right for left-justified 32-bit words.
One note, not a bug: `i2s2_rx_buffer` (`:46`) is plain static (DTCM, uncached) with the
`DMAMEM` attribute commented out, and there is **no** `arm_dcache_delete` anywhere in
the RX path. That is correct as written, but re-enabling that `DMAMEM` would silently
reintroduce stale-cache reads with nothing to catch it. Worth a comment at line 45.

**`Audio/AudioPassthrough.h`** — correct; receive/transmit/release balanced per channel.

**`Audio/AudioMixer.h:75-112` (`AudioSummingRoute`)** — correct, including the null
check on `allocate()` its sibling is missing.

**`Audio/AudioMixerF32.h`, `Audio/AudioVCA_F32.h`, `extern/f32/AudioMixer_F32.cpp`
(`AudioMixer4_F32`/`AudioMixer8_F32`)** — all allocations checked, all receives
released on every path.

**`Audio/AudioDelayExtF32.h`** — the `Taps`-sized `outs[]` array, the partial-alloc
release loop (`:127-134`), the `!buffer.IsReady()` guard (`:110`) and the
`Acquire()`-failure→`Release()` path are all correct. `set_taps()`
(`DelayApplet.h:404`) constrains to 1..8 against a `AudioDelayExtF32<9>`, so
`outs[taps_]` cannot overrun. The unused 9th tap is allocated and released each block
without a destination — wasteful (one pool block of transient pressure) but not a leak.

**`Audio/AudioTweightyF32.h`** — `update()` (`:206-386`) is correct on every early
return: `in_r` released if `in_l` alloc fails (`:215`), `in_l` released if `in_r` fails
(`:223`), both plus any partial out released at `:232-235`.

**`audio_applets/` sample (4 applets read in full: `InputApplet`, `VCAApplet`,
`UpsampledApplet`, `DelayApplet`, plus `PassthruApplet`/`PhaserApplet`/`FreeverbApplet`
partially)** — no missing `release()` and no int16/F32 scaling error found beyond
finding 11. The only applet-level defects are the declaration-order ones (findings 8
and 9). Domain bridging is correct everywhere I checked: every F32 applet reaches the
int16 chain through `HemisphereAudioAppletF32`'s edge adapters, and `InputApplet`
correctly puts its own `AudioConvertI16toF32Multi` on the hardware and USB taps
(`InputApplet.h:59-63`) rather than assuming a converter exists upstream.

**F32 allocation checks generally** — I audited all 26 `allocate_f32()` call sites in
`Audio/`, `audio_applets/`, `apps/` and `extern/f32/`. Every one is checked. The
`-fno-exceptions` cleanup done on `AudioDelayExtF32`/`AudioDelayExt`/`AudioTweightyF32`/
`PatchCable`/`DrLoFi`/`CVRecV2` was thorough on the F32 side; what it missed is
`AudioIO.cpp` itself (finding 2), `AppletRegistry.h` (finding 3) and the three int16
`allocate()` sites (finding 4).

---

# Suggested order of work

1. Finding 1 — one-line move, recovers 2.667 ms, and the function currently contradicts
   its own comment.
2. Findings 3 and 4 — four null checks, all in the ISR crash path.
3. Finding 8 — one-line declaration swap, recovers 2.667 ms on the default patch.
4. Finding 10 — `uint16_t` → `uint32_t`, one word.
5. Finding 2 / 13 — either individual checks or `-fcheck-new` globally.
6. Findings 5, 7, 9 — these are design decisions, not typos, and 9 in particular should
   get a bench A/B before anyone rearranges the applet.

## Bench verification note

Findings 1, 5, 7, 8 are all round-trip latency, and are measurable directly: drive a
known impulse or a step into the codec input, capture the codec output over a loopback
cable, and cross-correlate — each finding is a discrete 128-sample (2.667 ms) step, so
they are individually resolvable rather than needing to be inferred. Capture `-t raw`
after confirming the format with `arecord --dump-hw-params`, per the house rule.
Finding 9 needs a swept-sine transfer-function measurement of the Phazer applet
(notch depth and spacing) against a build with `phaser` allocated before
`dry_wet_mixer`; the predicted signature is a static ~375 Hz-spaced comb superimposed
on the sweeping notches.

**Nothing in this review was measured on hardware** — it is all static analysis against
the source and the linked Teensy core. Every latency number is derived from
construction order plus `128 / 48000 = 2.667 ms`, not from a bench capture.
