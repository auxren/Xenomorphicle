# Audio effect applets as standalone full-screen apps

**Scope.** Lift four audio applets out of Quadrants' applet host and give each
its own full-screen entry in the app switcher's AUDIO folder:

| Applet source | Class in that file | Domain | Channels available |
| --- | --- | --- | --- |
| `software/src/audio_applets/FreeverbApplet.h:12` | `ReverbApplet` | F32-native | MONO only (not templated) |
| `software/src/audio_applets/SamverbApplet.h:13` | `BungverbApplet` | F32-native | MONO only (not templated) |
| `software/src/audio_applets/AbyssApplet.h:17-18` | `AbyssApplet<Channels>` | **int16** | MONO or STEREO |
| `software/src/audio_applets/DelayApplet.h:13-14` | `DelayApplet<Channels>` | F32-native | MONO or STEREO |

Note the filename/classname mismatch on the first two — `FreeverbApplet.h`
declares `ReverbApplet`, `SamverbApplet.h` declares `BungverbApplet`
(`software/src/audio_applets/_config.h:82-83`). Anything grepping for
`FreeverbApplet` as a type will find nothing.

**Written read-only.** Nothing in this document was built or flashed. Every
claim carries a `file:line`; where a number is arithmetic on source constants
rather than a measurement it says **(computed)**, and where it is a guess it
says **(inferred, unverified)**.

**Load-bearing discovery up front:** `AbyssApplet` is **commented out of both
registries** — `software/src/audio_applets/_config.h:79-80` (mono) and
`:108-109` (stereo). It is dead code in every current build. Promoting it to a
standalone app is not "lifting it out of Quadrants"; it is the *only* way it
becomes reachable at all. That also means it has never run on this hardware in
this tree, so it carries strictly more risk than the other three.

---

## 1. How an audio applet is hosted today

### 1.1 The host object

`AudioAppletSubapp` (`software/src/AudioAppletSubapp.h:20-26`) is a class
template over five parameters — `Slots`, `NumMonoProcessors`,
`NumStereoProcessors`, and the two registry types. Exactly one instance exists,
constructed as a file-scope global at
`software/src/audio_applets/_config.h:162-164` with `NUM_SLOTS = 5`
(`_config.h:57`). Quadrants does not own it; it merely calls into it. Every
call site in Quadrants:

| Quadrants | Calls |
| --- | --- |
| `apps/Quadrants.h:40` (`Start()`) | `audio_app.Init()` |
| `apps/Quadrants.h:436` (`Controller()`) | `audio_app.Controller()` |
| `apps/Quadrants.h:1605` (`Loop()`) | `audio_app.mainloop()` |
| `apps/Quadrants.h:1663` (`View()`) | `audio_app.View()` |
| `apps/Quadrants.h:704`, `:1749`, `:1756` | encoder / encoder-button / button events |
| `apps/Quadrants.h:1987`, `:2107` | `LoadPreset(id)` / `SavePreset(id)` |
| `apps/Quadrants.h:953` | `ReInit()` from the randomizer |

Everything else — the whole applet graph — is inside the host.

### 1.2 Services the host provides

**(a) Graph construction and connection.** `Init()`
(`AudioAppletSubapp.h:54-68`) walks all five slots, calls `BaseStart()` on each
selected applet, and then chains slot *n*'s `OutputStream()` into slot *n+1*'s
`InputStream()` via `ConnectMonoToNext` / `ConnectStereoToNext`
(`:341-375`). The tail of the chain is hard-wired to the global output:
`:350-353` and `:368-372` both fall through to
`&OC::AudioIO::OutputStream()` with `dest_index == side`, i.e. 0 and 1. Because
`output_route` is source-major (`software/src/AudioIO.h:22-25`), index 0/1 is
**source slot 0**, which is Quadrants' permanent claim.

Connections live in the host, not the applet: `array<array<AudioConnection,
Slots + 1>, 2> conns` (`AudioAppletSubapp.h:541`). The applet owns only its
*internal* cables, allocated lazily from its own pool
(`HemisphereAudioApplet.h:79-89`, `MAX_CABLES = 32` at `:30`; the F32 twin is
`HemisphereAudioAppletF32.h:81-88`, `MAX_CABLES_F32 = 32` at `:64`).

**(b) `BaseStart` and the started-once guard.**
`HemisphereApplet::BaseStart()` (`software/src/HemisphereApplet.cpp:18-31`)
sets the display side, resets the cursor, cancels edit mode, and calls
`Start()` **only if `applet_started` is false**. `Unload()` on the applets calls
`AllowRestart()` (e.g. `FreeverbApplet.h:28-32`) to clear that flag. A host
that forgets to pair `Disconnect()`/`Unload()` with `BaseStart()` gets an applet
that never re-patches its internal cables.

**(c) The input applet in slot 0.** The constructor
(`AudioAppletSubapp.h:37-52`) forces index 1 — `InputApplet` — into slot 0 on
both sides and both mono/stereo tracks, and sets `stereo = 1` so slot 0 is
stereo. `ReInit()` (`:70-84`) re-asserts `stereo |= 1`. Slot 0 is therefore
never a processor; it is where the codec/USB input enters the chain. **A
standalone host has to supply that itself** — the four applets have no input of
their own, they are pure processors.

**(d) Stereo vs mono.** A 32-bit bitset `stereo` (`:535`) says, per slot,
whether one stereo applet or two mono applets are live. `SwapMonoStereo()`
(`:205-223`) tears one down (`Disconnect()` + `Unload()`) and starts the other,
then re-links the previous slot. Note this is **not** bracketed by
`AudioNoInterrupts()`; only `Controller()` is (see (f)).

**(e) Buffer / instance ownership.** Applet instances are *not* members of the
host. They come from `Registry::get()`
(`software/src/AppletRegistry.h:91-110`), which lazily `calloc`s (RAM2) or
falls back to `extmem_calloc` (PSRAM) at `:49-51` and placement-news the
instance the first time a slot selects it. Instances are cached per
`(slot, applet)` pair for the rest of the session — `instances[Slots][Size]`
at `:60`. Big DSP objects go through a second layer, `Factory<T, N>`
(`software/src/OC_core.h:53-86`), a fixed pool with the same RAM2-then-PSRAM
policy (`:64-65`), guarded by `RAM2_HEADROOM = 10240` (`OC_core.h:24`). The four
static pools are defined at `audio_applets/_config.h:59-62`.

**(f) Control rate.** `Controller()` (`AudioAppletSubapp.h:86-110`) is called
from Quadrants' `Controller()` at 16.666 kHz (`OC_config.h:19-24`, 60 us
budget). It brackets every applet's `Controller()` in
`AudioNoInterrupts()` / `AudioInterrupts()` (`:87`, `:96-97`) — with a safety
valve: it does **not** re-enable if `cpu_percent > 100`. Every 250 ms it
recomputes `mem_percent` / `cpu_percent` from `AudioMemoryUsageMax()` and
`AudioProcessorUsageMax()` (`:99-109`).

**(g) Storage.** Not EEPROM. The host writes into PhzConfig with a packed
16-bit key: 5-bit preset id in the top bits, a 3-bit section
(`AudioConfigSections`, `:378-384`), and a slot/index byte (`key()` at
`:388-390`). Per-applet params are the applet's own
`std::array<uint64_t, CONFIG_SIZE>` with `CONFIG_SIZE = 4`
(`HemisphereAudioApplet.h:29`), moved by `OnDataRequest`/`OnDataReceive`
(`:63-71`) and written key-by-key with zero-suppression
(`AudioAppletSubapp.h:514-525`). **This is the piece a standalone app can reuse
almost verbatim**: 4 x uint64 = 32 bytes is the entire persistent state of any
of these applets.

**(h) UI dispatch.** Two independent cursors, one per side
(`state[2]`, `cursor[2]`, `candidate[2]`, `:571-574`), each in one of
`MOVE_CURSOR` / `SWITCH_APPLET` / `EDIT_APPLET` (`:560-564`). In `EDIT_APPLET`
the host calls `SetDisplaySide(side + AUDIO_SLOT_L)` then
`applet.BaseView(full, full)` (`:139-142`). Encoder turns route to
`OnEncoderMove()` (`:294-298`), encoder press to `OnButtonPress()` (`:262-264`),
X/Y to `AuxButton()` (`:190-197`), and a push-and-turn multiplies the delta by
10 (`:300-311`). `HandleButtonEvent` returns `true` when the user wants out
(`:181`, `:186`), which Quadrants turns into `view_state = APPLETS`
(`Quadrants.h:1756-1757`).

**(i) Metering.** `AudioAnalyzePeak` per side per slot boundary
(`:542-543`), tapped off the input at `:66-67` and off each applet output at
`:355-356`. Rendered as 1px inverted bars (`:683-696`).

### 1.3 What an applet actually depends on

Stripping the host away, an applet needs exactly this:

1. `BaseStart(HEM_SIDE)` once, with a side whose `gfx_offset` it can draw into.
2. Someone to feed `InputStream()` and drain `OutputStream()` — both are plain
   **int16** `AudioStream*` for all four applets (`HemisphereAudioApplet.h:48-49`;
   the F32 applets satisfy them with edge adapters at
   `HemisphereAudioAppletF32.h:66-71`).
3. `Controller()` at control rate.
4. `View()` / `OnEncoderMove()` / `OnButtonPress()` / `AuxButton()`.
5. `OnDataRequest`/`OnDataReceive` for persistence.
6. `Disconnect()` + `Unload()` before it is torn down.

It does **not** need slots, stereo bitsets, registries, peak meters, or the
two-cursor state machine. That is the whole argument for a small wrapper.

**Trap: `io_offset`.** `HemisphereApplet::In(ch)` reads
`cvmap[ch + io_offset]` (`HemisphereApplet.cpp:99`) where
`io_offset = hemisphere * 2` (`HSUtils.h:35`). With `hemisphere = AUDIO_SLOT_L`
(= 6, `HSUtils.h:51`) that is `cvmap[12..]`, past the end of
`cvmap[ADC_CHANNEL_COUNT]` = 8 (`CVInputMap.h:522`, `OC_config.h:12`). This
out-of-bounds is **pre-existing** and harmless only because audio applets never
call `In()`/`Out()`/`OutputLabel()`/`Quantize()` — they read their own
`CVInputMap` members instead (`FreeverbApplet.h:185-188`,
`DelayApplet.h:438,444,446`), and `CVInputMap::RawIn()` indexes `frame` by its
own stored source (`CVInputMap.h:72-90`). **A standalone host must not
introduce any `In()`/`Out()` calls into these applets.**

---

## 2. The precedent that already works, and the summing route

### 2.1 Tweighty

`AppTweighty` (`apps/TweightyApp.h:91-99`) is a plain `OC_APP_CLASS` +
`HSApplication` that owns an F32 engine as a direct member (`:206`) plus its own
edge adapters (`:207-208`) and eight heap-allocated `AudioConnection*` /
`AudioConnection_F32*` (`:209-216`).

Lifecycle:

- `WireAudio()` (`:349-381`) `new`s all eight connections **and immediately
  `disconnect()`s them** (`:371-378`), so the pooled cable objects exist but
  carry nothing.
- `ActivateOnce()` (`:402-422`) runs once per session on the first
  `APP_EVENT_RESUME` (`:424-430`): `AudioNoInterrupts()`, `engine_.Acquire()`,
  reconnect all eight, `AudioInterrupts()`. That bracket is the proven-safe
  live-rewiring pattern.
- `SUSPEND` is a deliberate no-op (`:431-435`). Tweighty stays connected and
  audible for the rest of the session.
- Because `Controller()` no longer runs when Tweighty is backgrounded, a
  separate `BackgroundPump()` (`:181-202`) is called from `loop()`
  unconditionally (`Main.cpp:1039`, dispatcher at `OC_apps.cpp:426-430`) and is
  **skipped while Tweighty is current** to avoid two writers racing the same
  non-atomic state (`OC_apps.cpp:415-425`).
- Its output lands on its own summing-route slot (`:365-370`), computed as
  `kOutputRouteTweightySlot * kOutputRouteChannels + ch`. The comment at
  `:358-364` records that failing to do this was the total-silence bug.

Sampler is the same shape (`apps/SamplerApp.h:196-212`), and Scope taps
`OutputStream()`'s *outputs* as a source (`apps/ScopeApp.h:136-142`) — proof
that the summing route's transmit side is a legitimate tap point.

### 2.2 The summing route: is it a fixed-arity template?

Yes. `AudioSummingRoute<NumChannels, NumSources>`
(`software/src/Audio/AudioMixer.h:75-112`) is a compile-time-sized
`AudioStream` — `AudioStream(NumChannels * NumSources, inputQueueArray)` at
`:78-79`, backing store `audio_block_t* inputQueueArray[NumChannels *
NumSources]` at `:111`. The instance is
`AudioSummingRoute<kOutputRouteChannels, kOutputRouteSources> output_route`
(`AudioIO.cpp:60`) with the constants at `AudioIO.h:26-29`:

```
constexpr uint8_t kOutputRouteChannels = 2;
constexpr uint8_t kOutputRouteSources  = 3;   // 0=Quadrants 1=Tweighty 2=Sampler
```

**What growing it costs — the crux.**

- *Source change*: one integer, `AudioIO.h:27`, plus one new
  `kOutputRoute<Name>Slot` constant per member. No code in `AudioMixer.h`
  changes.
- *Static RAM*: `inputQueueArray` grows by `NumChannels * 4` bytes per source =
  **8 bytes per added source** (computed). 3 -> 7 sources: 24 -> 56 bytes. Free.
- *Per-block CPU*: `update()` (`:81-108`) does one `receiveReadOnly()` per
  (source, channel) — a null check for empty slots — and for each *live* source
  one `arm_q15_to_float(128)` + `arm_add_f32(128)`. Empty slots are essentially
  free. Each additional **live** source costs roughly 1.3 us per channel, i.e.
  ~2.6 us per block against a 2667 us block period at 48 kHz / 128 samples —
  about **0.1 % CPU per live source** *(inferred, unverified; the arm_math
  timings are estimates, and the honest way to get this number is
  `AudioProcessorUsageMax()` before/after, which the applet host already
  displays — `AudioAppletSubapp.h:106`)*.
- *The real cost is gain staging, not arithmetic.* Every source sums at unity
  and nothing normalises the sum — stated explicitly at `AudioIO.cpp:52-59`.
  Going from 3 to 7 unity-summed sources moves worst-case headroom from
  +9.5 dBFS of overshoot to +16.9 dBFS (computed: `20*log10(N)`), all of it
  absorbed by `arm_float_to_q15`'s saturation at `AudioMixer.h:102`. That is a
  clean clip rather than a wrap, but it is still a clip.
- *Ordering*: nothing. Sources connect at runtime; `output_route` is a plain
  global constructed with the rest of `AudioIO`'s statics.

**Recommendation: do not grow it to 7.** Grow it to **4** and give the four
audio apps one shared sub-bus:

```
constexpr uint8_t kOutputRouteSources        = 4;
constexpr uint8_t kOutputRouteAudioAppsSlot  = 3;
```

with a second `AudioSummingRoute<2, 4>` living inside the wrapper's shared
support object, wired once into slot 3. Reasons:

1. Gain staging for the four effect apps is then solvable in **one** place
   (that sub-bus can grow a per-source gain later without touching the three
   existing members' unity contract).
2. `AudioIO.h`'s constants stay small and the existing three slot indices are
   untouched, so Quadrants/Tweighty/Sampler need no edits at all.
3. Adding a fifth audio app later costs one constant in the wrapper, not a
   change to the module's master output arity.

An alternative that also works and is simpler to *read* — one slot per app,
`kOutputRouteSources = 7` — is acceptable; it just spreads the headroom problem
across `AudioIO.cpp` instead of concentrating it.

---

## 3. The recommended design

### 3.1 New file: `software/src/AudioAppletApp.h`

A header-only host for exactly one applet, plus the shared sub-bus. Sketch:

```cpp
#pragma once
#ifdef ARDUINO_TEENSY41

#include "AudioIO.h"
#include "HemisphereAudioApplet.h"
#include "Audio/AudioMixer.h"

// The shared tap into OC::AudioIO::OutputStream(). One instance, lazily
// constructed on first use so it is created AFTER OutputStream() has run its
// own lazy construction (AudioIO.cpp:116-162) -- see 3.3.
namespace AudioApps {
  static constexpr uint8_t kBusSources = 4;
  AudioSummingRoute<2, kBusSources>& Bus();     // wires itself into
                                                // kOutputRouteAudioAppsSlot
  enum BusSlot : uint8_t { SLOT_REVERB, SLOT_SAMVERB, SLOT_ABYSS, SLOT_DELAY };
}

// Channels: MONO applets get 1 in / 1 out and are fanned to both bus channels.
template <class Applet, AudioChannels Channels, uint8_t BusSlot>
class AudioAppletHost {
public:
  // ---- lifecycle, called from the owning OC_APP_CLASS ----
  void Init();                      // reset applet to defaults (see 3.5)
  void Activate();                  // BaseStart + build/connect cables
  void Deactivate();                // disconnect only (applet stays alive)
  void Release();                   // Disconnect() + Unload() + free buffers

  void Controller();                // AudioNoInterrupts-bracketed
  void mainloop();                  // forwards Applet::mainloop()

  // ---- UI ----
  void View() const;                // applet pane (x 0..63) + status pane
  void HandleButtonEvent(const UI::Event&);
  void HandleEncoderEvent(const UI::Event&);

  // ---- storage: 4 x uint64 == 32 bytes, exactly CONFIG_SIZE ----
  size_t Save(util::StreamBufferWriter&) const;
  size_t Restore(util::StreamBufferReader&);

  bool live() const { return live_; }        // keep running off-screen
  void set_live(bool b) { live_ = b; }
  bool bypass() const { return bypass_; }

private:
  Applet applet_;                            // owned, NOT from the Registry
  AudioConnection conn_in_[Channels];        // InputStream -> applet
  AudioConnection conn_out_[Channels];       // applet -> AudioApps::Bus()
  AudioConnection conn_dry_[Channels];       // bypass path (see 3.4)
  AudioAnalyzePeak peak_in_, peak_out_;
  AudioConnection peak_in_conn_, peak_out_conn_;
  bool wired_ = false, connected_ = false, live_ = false, bypass_ = false;
};

#endif
```

Key decisions embedded in that sketch, each with its reason:

- **`Applet applet_;` as a direct member, not `Registry::get()`.** Two reasons.
  (a) The registry allocates lazily at *runtime* (`AppletRegistry.h:103-108`),
  which would place the applet's `AudioStream` objects **after**
  `output_stream` in the audio update list and buy an extra block of latency —
  precisely the hazard `AudioIO.cpp:117-123` documents. A direct member is
  constructed during static init, before anything calls `OutputStream()`.
  (b) It removes the whole `Slots x Size` instance-cache dimension, which is
  meaningless for a one-applet host.
- **Own `AudioConnection`s, not the applet's `PatchCable` pool.** The applet's
  pool is for its internal graph; `Disconnect()`
  (`HemisphereAudioApplet.h:119-124`, F32 override at
  `HemisphereAudioAppletF32.h:90-96`) resets `cable_count` to 0 and would eat
  the host's edges too.
- **Peak meters kept.** Two of them, not `Slots + 1`, and only for the status
  pane. Cost: 2 `AudioAnalyzePeak` per app (tiny) and 2 more int16 blocks in
  flight per app *(inferred)*.
- **`Channels` is a template parameter**, so `AbyssApplet<STEREO>` and
  `DelayApplet<STEREO>` get true stereo while `ReverbApplet` /
  `BungverbApplet` run mono and the host fans channel 0 to both bus channels.

### 3.2 UI layout — the 64-pixel problem

`gfx_offset` is `(hemisphere & 1) * 64` (`HSUtils.h:34`). With
`hemisphere = AUDIO_SLOT_L` (6) the offset is 0, so an applet's `View()`
renders into **x 0..63 only**. All four applets are written to that budget:
`AbyssApplet.h:144` uses `const int rx = 63 - 8`, `DelayApplet.h:214` the same,
and `gfxDisplayInputMapEditor()` clears exactly `(0,0,63,11)`
(`HemisphereApplet.h:347`).

Three options:

| Option | Cost | Verdict |
| --- | --- | --- |
| (a) Draw the applet in the left 64 px and leave the right blank | zero | ugly, wastes the whole point |
| (b) Rewrite the four applets to be width-aware | touches 4 shared files, breaks Quadrants parity, re-tests everything | **no** |
| (c) Applet pane at x 0..63 + host status pane at x 64..127 | ~60 lines in the host, zero applet edits | **yes** |

**Take (c).** The right pane is exactly the information the applet cannot show
in 64 px and the standalone app should: app name, in/out peak bars, bypass
state, LIVE state, CPU %, F32 and int16 pool usage, and the two or three CV
sources actually mapped. This also satisfies the L-06 inversion rule
(`docs/ui-redesign/Established-Rules.md`): the *applet* pane keeps its existing
`gfxStartCursor`/`gfxEndCursor` inversion, which already means "encR changes
this"; the status pane must draw **no inversion at all**, using a leading `>`
or a drawn box for anything it needs to emphasise.

`hemisphere` seat: **reuse `AUDIO_SLOT_L`**. It gives `gfx_offset == 0`, it is
already in range of `cursor_countdown[APPLET_CURSOR_COUNT]` and
`enc_edit[APPLET_CURSOR_COUNT]` (`HemisphereApplet.h:94,98`, count = 8 at
`HSUtils.h:54`), and it needs zero enum churn. The caveat is that all five
hosts (four apps + Quadrants' audio editor) share `enc_edit[6]`, so a stale
`isEditing` can survive an app switch — fix by calling
`applet_.CancelEdit()` + `ResetCursor()` on `APP_EVENT_RESUME`. *(Adding a
ninth seat instead would also work and is cheap — appending `AUDIO_APP` before
`APPLET_CURSOR_COUNT` at `HSUtils.h:52` keeps `2 * APPLET_CURSOR_COUNT = 18`
inside `CVInputMap`'s 5-bit index at `CVInputMap.h:62-64`. It is not worth the
churn for v1.)*

Control map (A, B, X, Y, encL, encR are all free per-app; Z and the Z-chords
are global per `docs/UI-Redesign-Constraints.md:88-92`):

| Control | Action |
| --- | --- |
| encR turn | `applet_.OnEncoderMove(dir)` (x10 on push-and-turn, mirroring `AudioAppletSubapp.h:302-305`) |
| encR press | `applet_.OnButtonPress()` |
| encL turn | move the status pane's own cursor (LIVE / BYPASS rows) |
| encL press | `applet_.CancelEdit()` — "get me out of edit mode" |
| A | BYPASS toggle |
| B | LIVE toggle (keep running off-screen) |
| X | `applet_.AuxButton()` — this is what Abyss's and Delay's Send-mode toggle needs (`AbyssApplet.h:64-67`, `DelayApplet.h:249-252`) |
| Y | reserved |

### 3.3 Audio graph wiring, and whether live rewiring is needed

Wiring per app, all of it with plain int16 `AudioConnection` — **no F32
converters in the host**, because all four applets present int16 at their edges
(`HemisphereAudioApplet.h:48-49`; the F32 ones convert internally at
`HemisphereAudioAppletF32.h:99-100`):

```
OC::AudioIO::InputStream(0) ch0[,ch1]  ->  applet_.InputStream()  ch0[,ch1]
applet_.OutputStream() ch0[,ch1]       ->  AudioApps::Bus()  BusSlot*2 + ch
AudioApps::Bus() ch0,ch1               ->  OC::AudioIO::OutputStream(),
                                            kOutputRouteAudioAppsSlot*2 + ch
```

For a MONO applet, `conn_out_[0]` is duplicated to both bus channels (two
`AudioConnection`s from the same source output — legal, and correct here
because the destination is a summing route, not an `AudioPassthrough`; the
distinction is exactly what `AudioIO.cpp:40-50` was written to record).

**`OutputStream()` must actually be called.** It is lazily constructed
(`AudioIO.cpp:116-162`) and its side effects — building `conv_out[]`, the F32
codec output and the USB monitor mix — happen only on first reference. Today
Quadrants (`Quadrants.h:40` -> `AudioAppletSubapp.h:54`), Sampler and Scope all
force it at `Init()`. The wrapper's `AudioApps::Bus()` must call it too, and
must call it **before** creating its own `AudioSummingRoute`, so the sub-bus
lands after `output_stream` in construction order and therefore in update
order. *(Inference: Teensy's `update_all` walks the construction-order list, so
a stream created after `output_stream` is updated after it and costs one block
of latency. The sub-bus is a pure adder with no internal state, so one block of
latency on it would be audible only as ~2.7 ms of extra delay on these four
apps. If that matters, construct the sub-bus as a file-scope global in
`AudioIO.cpp` alongside `output_route` instead — same fix the existing code
used for `input_route`.)* **Recommend the file-scope-global form** — put the
sub-bus in `AudioIO.cpp` next to `output_route`, wired in `OutputStream()`'s
lazy block at `AudioIO.cpp:124-160`. It costs one more `new AudioConnection`
there and removes the ordering question entirely.

**Live rewiring is needed only for connect/disconnect**, never for a topology
change: the applet is fixed for the life of the app. Use Tweighty's exact
pattern — `new` everything once and immediately `disconnect()`
(`TweightyApp.h:371-378`), then `connect()` / `disconnect()` the pooled objects
inside `AudioNoInterrupts()` / `AudioInterrupts()`
(`TweightyApp.h:404-421`). The no-arg `AudioConnection::connect(void)` is
upstream API and safe to use for the int16 cables.

**Also bracket the applet's own `Start()`**, because `Start()` calls
`PatchCable` -> `AudioConnection::connect` (`HemisphereAudioApplet.h:79-89`).
`AudioAppletSubapp` does **not** do this today
(`AudioAppletSubapp.h:268-292` runs `Disconnect`/`Unload`/`BaseStart`/reconnect
unbracketed) — that is a latent race the new host should not inherit.

### 3.4 Bypass

Bypass must not be "disconnect the output", or a reverb tail becomes a click.
Give the host a dry path `conn_dry_[]` from `InputStream()` straight to the
bus slot, and drive wet/dry at the *applet* level where possible: Reverb and
Samverb already expose a Mix parameter driving `dry_wet_mixer`
(`FreeverbApplet.h:47-50`), Delay has wet/send (`DelayApplet.h:141-158`), and
Abyss has `EqualPowerFade` (`AbyssApplet.h:45-58`). Simplest correct v1:
BYPASS disconnects `conn_in_[]` only (input muted, tail rings out) and connects
`conn_dry_[]`; a second press restores. Everything inside
`AudioNoInterrupts()`.

### 3.5 Storage

**Use the applet's own 4 x uint64.** `SaveAppData()` writes
`applet_.OnDataRequest(data)`'s four words plus one status byte (bypass, live,
mono/stereo if ever exposed); `RestoreAppData()` reads them back and calls
`applet_.OnDataReceive(data)`.

Size per app: `4 * 8 + 1 = 33` bytes payload, so
`OC_APP_INTERFACE_DECLARE(AppReverb, 33)`.

EEPROM cost (computed from `OC_storage.h:66-71`):

```
chunk = (sizeof(AppChunkHeader) + IOSettings::storageSize() + payload + 1) & ~1
      = (4 + 20 + 33 + 1) & ~1 = 58 bytes per app
```

`IOSettings::storageSize() == 20` is computed from `OC_io_settings.h:108-112`
(per channel: U8 + U4 + NOP + U4 + U4 = 20 bits) x 8 channels
(`OC_config.h:11-12`) = 160 bits, + 4 = 164 bits >> 3 = 20 bytes.

**Four apps = 232 bytes** against `AppData::kAppDataSize` = 3900
(`OC_config.h:91`, and the comment at `OC_config.h:84-87` states the constant
is 3900). The gate is the `static_assert` at `apps/_config.h:180-181`; if it
trips, the fallback is Quadrants' approach — declare `0` and keep the four
words in PhzConfig bank globals, which are already swept into all 30 preset-bus
slots (`docs/UI-Redesign-Constraints.md:60-62`).

*I could not compute current total EEPROM usage without building* — Calibr8or's
and Captain MIDI's chunks are the large unknowns
(`apps/Calibr8or.h`, `apps/CaptainMIDI.h` both size from other classes'
`storageSize()`). The `static_assert` is the authoritative check.

### 3.6 Suspend / Resume

| Event | Behaviour |
| --- | --- |
| `APP_EVENT_RESUME` | `Activate()` if not yet active: `AudioNoInterrupts()`, `BaseStart(AUDIO_SLOT_L)`, allocate the DSP buffer if needed, `connect()` all cables, `AudioInterrupts()`. Also `CancelEdit()` + `ResetCursor()` for the shared `enc_edit[6]` seat. |
| `APP_EVENT_SUSPEND` | if `live_` -> no-op (Tweighty semantics, `TweightyApp.h:431-435`); else `Deactivate()` — `AudioNoInterrupts()`, `disconnect()` all cables, `AudioInterrupts()`. DSP objects stay allocated so re-entry is instant. |
| `APP_EVENT_SCREENSAVER_ON/OFF` | no-op. Audio is unaffected by the screensaver. |
| `APP_EVENT_FLUSH` | no-op (state lives in the EEPROM chunk, written by the normal save path). |

**Default `live_ = false`.** Rationale in §4.

An explicit "RELEASE" action (encL press on the status pane's RAM row) should
call `Release()` — `Disconnect()` + `Unload()`, which returns the Factory
instance (`FreeverbApplet.h:28-32`, `SamverbApplet.h:29-33`), frees the Abyss
arena (`AbyssApplet.h:26-28` -> `effect_abyss.h:42-47`) or the Delay's PSRAM
(`DelayApplet.h:58-64` -> `AudioDelayExtF32.h:46-53`). Without it, four MB of
PSRAM and ~380 KB of RAM2 are held for the session after a single visit.

### 3.7 Registration

Four new app headers, each ~150-250 lines, each `#include`ing
`AudioAppletApp.h` and one applet header:

- `software/src/apps/ReverbApp.h` — `OC_APP_CLASS(AppReverb, TWOCCS("RV"), "Reverb", "Freeverb")`
- `software/src/apps/SamverbApp.h` — `TWOCCS("SV")`, "Samverb", "Schroeder verb"
- `software/src/apps/AbyssApp.h` — `TWOCCS("AB")`, "Abyss", "Big reverb"
- `software/src/apps/DelayApp.h` — `TWOCCS("DL")`, "Delay", "Multitap delay"

None of `RV`/`SV`/`AB`/`DL` collide with any ID in `OC_app_folders.h:69-114`.

Edits to `software/src/apps/_config.h`:

```
#ifdef ENABLE_APP_AUDIOFX          // after line 62 (SAMPLER/USBDRIVE block)
#include "ReverbApp.h"
#include "SamverbApp.h"
#include "AbyssApp.h"
#include "DelayApp.h"
#endif
...
#ifdef ENABLE_APP_AUDIOFX          // in the container, after AppSampler (line 169)
  , AppReverb
  , AppSamverb
  , AppAbyss
  , AppDelay
#endif
```

**Container position matters** — the comment at `apps/_config.h:69-74` warns
that `Start()` runs in registration order, and `DEFAULT_APP_INDEX = 4` is
asserted to be Captain MIDI (`apps/_config.h:186-195`). Inserting after
`AppSampler` (index 11) is safely past index 4. `kMaxApps = 32`
(`OC_app_folders.h:122`) accommodates 15 -> 19 apps.

Edits to `software/src/OC_app_folders.h` — four rows in `kDefaults`
(after `:100`):

```
{ TWOCCS("RV"), FOLDER_AUDIO },
{ TWOCCS("SV"), FOLDER_AUDIO },
{ TWOCCS("AB"), FOLDER_AUDIO },
{ TWOCCS("DL"), FOLDER_AUDIO },
```

Edit to `software/platformio.ini` — one flag in `[env:T41_audio]`
(after `:155`): `-DENABLE_APP_AUDIOFX`. Gate it so `T41` (no USB audio) and the
non-T4.1 envs are unaffected; the applets are already `#ifdef ARDUINO_TEENSY41`
via `HemisphereAudioAppletF32.h:9`.

Edits to `software/src/AudioIO.h` / `AudioIO.cpp` — see §2.2 and §3.3:
`kOutputRouteSources` 3 -> 4 (`AudioIO.h:27`), one new slot constant, and the
sub-bus global + its two connections inside `OutputStream()`'s lazy block
(`AudioIO.cpp:124-160`).

### 3.8 Per-app specifics

**Reverb (`ReverbApplet`, MONO).** Needs `GetFreeverb()` from the shared
`verb_factory` (`HemisphereAudioApplet.h:98-100`, pool of 8 at
`audio_applets/_config.h:60`). Holding one permanently shrinks Quadrants' pool
from 8 to 7. `Start()` (`FreeverbApplet.h:17-27`) tolerates a null reverb and
falls back to dry-only (`:38-41`), and `View()` prints "Out Of RAM !!!"
(`:54-57`) — so the failure mode is already graceful. Mono only; fan to both
bus channels.

**Samverb (`BungverbApplet`, MONO).** Same shape, `bung_factory`
(`HemisphereAudioApplet.h:91-96`). Note `effect_reverb_schroeder_F32.h:127`
hard-codes `sr = 44100` while the build runs at 48 kHz
(Teensy `AudioStream.h:37-38`; nothing in `platformio.ini` overrides it), so its
delay lines are ~8.9 % short in time (computed). Pre-existing, cosmetic,
worth a one-line fix but **out of scope**.

**Abyss (`AbyssApplet<STEREO>`, int16).** The only one that is *not* F32 and
the only one currently unreachable (`audio_applets/_config.h:79-80,108-109`).
`Start()` calls `reverb.begin()` (`AbyssApplet.h:258`) which mallocs the arena
from RAM2, falling back to PSRAM (`effect_abyss.h:26-40`). The header warns
that the PSRAM fallback cost >50 % CPU in v1 (`effect_abyss.h:21-25`) — so on
this app, **where the arena landed is a first-class diagnostic** and belongs on
the status pane. Its `View()` already scrolls 8 rows in a 5-row window
(`AbyssApplet.h:239-254`), which fits the 64 px pane unchanged.

**Delay (`DelayApplet<STEREO>`, F32).** The heaviest. `Start()`
(`DelayApplet.h:40-56`) cross-patches the two channels' tap mixers for
ping-pong and calls `clock_source.SetClockSource(0)`, so **Digital-In-1 is
already meaningful** and should be declared in `GetIOConfig()`. Buffers are
PSRAM-only — `ExtAudioBuffer<float>` (`AudioDelayExtF32.h:151`) allocates
exclusively with `extmem_calloc` (`AudioBuffer.h:146-150`). **The comment at
`DelayApplet.h:399-402` claiming a RAM2 fallback is wrong**: with no PSRAM the
buffer is null, `IsReady()` is false, and `update()` returns immediately
(`AudioDelayExtF32.h:86`), i.e. silence, not a smaller delay. Worth fixing or
at least surfacing on the status pane.

---

## 4. Does "keeps running when off screen" work?

**Audio: yes, and it is free.** Evidence:

- The audio graph is driven by the I2S DMA interrupt and `update_all()`, wholly
  independent of `AppSwitcher`. Nothing in `AudioIO.cpp` or `AudioMixer.h`
  consults the current app.
- Tweighty and Sampler are already proof (`AudioIO.h:16-25`,
  `TweightyApp.h:431-435`).
- Quadrants' entire applet chain is connected at boot for **every** build,
  because `AppBase::InitDefaults()` -> `Init()` runs for every app in the
  container (`OC_apps.cpp:710-713`, `OC_app_base.cpp:466-470`) and Quadrants'
  `Start()` calls `audio_app.Init()` (`Quadrants.h:40`). Quadrants is audible
  while you are in Captain MIDI right now.

**Control rate: no, and this is the constraint.** Only the current app's
`Process()` runs — `AppSwitcher::Process()` dispatches to `current_app_` alone
(`OC_app_switcher.h:61-66`), and `HSApplication::BaseController()` is what
refreshes the single global `HS::frame` (`HSApplication.h:73-74`). Since every
applet parameter's CV comes from a `CVInputMap` reading `frame.In(index())`
(`CVInputMap.h:72-90`), **a backgrounded audio app's CV modulation freezes at
whatever value was live when you left the screen.** Knob values persist; CV does
not track.

This is not fixable by a Tweighty-style `BackgroundPump()`: Tweighty's pump
works precisely *because* it deliberately does not touch `In()`/`Clock()`/
`HS::frame` (`TweightyApp.h:165-173`). An applet `Controller()` cannot make that
promise — it *is* CV reads, top to bottom.

**What it costs to leave all four live:**

- Four parallel wet paths off the same codec input, all summing at unity into
  the bus, on top of Quadrants + Tweighty + Sampler. Worst-case sum headroom
  goes to +16.9 dBFS of overshoot into `arm_float_to_q15`'s saturation
  (computed; see §2.2).
- Four DSP engines' CPU permanently, whether or not you can hear them.
- ~382 KB of RAM2/heap and 4 MB of PSRAM held for the session (§5).
- A frozen-CV surprise: leave Delay with a CV-modulated time at an extreme and
  it stays there.

**Therefore: `live_` defaults to OFF and is a per-app, persisted opt-in.** The
default behaviour is that leaving the screen disconnects the app's input and
output (tail rings out into the disconnect; see §3.4 if a click appears), which
is also what makes "four of them coexist" safe.

---

## 5. Risks, quantified

### 5.1 Memory — the numbers

All computed from source constants; none measured.

| App | Buffer | Where | Bytes |
| --- | --- | --- | --- |
| Reverb | `combstore[11024] + allpassstore[1563]` floats (`effect_freeverb_F32.h:98-107,116-117`) | Factory: RAM2 heap, PSRAM fallback (`OC_core.h:62-68`) | **50,348** |
| Samverb | `combBuf[8][2153] + apBuf[4][556]` floats (`effect_reverb_schroeder_F32.h:156-160`) | same | **77,792** |
| Abyss (STEREO) | 115,005 int16 samples across 13 lines (`abyss_core.h:295-321`, constants `:35-38,262-264`, at 48 kHz) | `malloc` RAM2, PSRAM fallback (`effect_abyss.h:26-40`) | **230,028** |
| Delay (STEREO) | 2 x 524,288 floats (`DelayApplet.h:403`, `:480-481`) | **PSRAM only** (`AudioBuffer.h:146-150`) | **4,194,304** |
| Delay crossfade LUTs | 2 ch x 2 x 2048 floats (`AudioDelayExtF32.h:38-45`, `CrossfadeSamples = 2048` at `:20`) | regular heap (`new float[]`) | **32,768** |

**Totals if all four are activated in one session:**

- **RAM2 / heap: 390,936 bytes (381.8 KiB)** — 50,348 + 77,792 + 230,028 + 32,768.
- **PSRAM: 4,194,304 bytes (4.00 MiB).**
- **Grand total: 4,585,240 bytes.**

Plus, per app, the applet object itself as a static member of the DMAMEM
`AppContainer` (`apps/_config.h:83`): mixers, passthroughs, edge adapters and
four `CVInputMap`s each. Every one of these is small — `CVInputMap` is 2 bytes
of state plus a `SemitoneQuantizer` (`CVInputMap.h:27-45`), `AudioMixer4_F32` is
a handful of pointers and gains. *(Inferred: low hundreds of bytes each,
< 4 KB total for all four; the container comment at `apps/_config.h:82` says the
existing 15 apps total 5.9 KB, so this is a meaningful but not alarming
increase.)*

**The RAM2 number is the risk.** `Factory::get()` and `Registry`'s factory both
check `OC::CORE::FreeRam() > RAM2_HEADROOM` (10,240 bytes,
`OC_core.h:24,64`) and silently fall back to PSRAM. For Abyss that fallback is
documented to cost **>50 % CPU** (`effect_abyss.h:21-25`) — so the third or
fourth app opened in a session can be the one that lands in PSRAM and tanks the
engine, *nondeterministically depending on visit order*. Mitigations, in order
of preference:

1. Ship fewer than four (see §5.5).
2. Make `Release()` reachable and document it.
3. Have the status pane show which pool the buffer landed in
   (`AudioEffectAbyssReverb` already tracks `arena_in_psram`,
   `effect_abyss.h:118` — expose it).

### 5.2 The `AudioMemory` pools

- int16: `AUDIO_MEMORY = 252` blocks (`AudioIO.h:9`), 260 bytes each = 65,520
  bytes DMAMEM. Abyss and every host edge connection draw from this.
- float32: `F32_AUDIO_MEMORY = 80` blocks (`AudioIO.h:13`). `sizeof(audio_block_f32_t)`
  is **532 bytes** (computed from `AudioStream_F32.h:73-82`: 4 + 512 + 4 + 4 + 4 + 4),
  so the pool is **42,560 bytes on the RAM2 heap** — `allocate_f32_memory()`
  does `new audio_block_f32_t[num]` (`AudioStream_F32.cpp:12-19`). Hard ceiling
  is 192 blocks (`AudioStream_F32.cpp:38`), and `f32_memory_used` is a `uint8_t`
  (`AudioStream_F32.h:140`).

`AudioIO.h:13` already states "The Delay applet alone can hold ~10 blocks per
channel in flight (8 taps + I/O)". A standalone stereo Delay app therefore wants
**~20 F32 blocks**, and a Quadrants slot running a stereo Delay wants another
20. Reverb and Samverb are ~3-4 blocks each *(inferred from their graph:
input adapter, reverb, filter, mixer, output adapter)*. Four live audio apps
plus a Quadrants chain could plausibly want 50-60 of 80.

**Recommendation: raise `F32_AUDIO_MEMORY` from 80 to 128** (`AudioIO.h:13`)
before shipping the Delay app. Cost: **+25,536 bytes** of RAM2 heap (computed:
48 x 532). Verify with `AudioMemoryUsageMax_F32()` — the applet host already
displays the int16 equivalent (`AudioAppletSubapp.h:102-105`) and the new status
pane should display both. A pool exhaustion here is **not** a crash: `allocate_f32()`
returns null, the adapter silently drops the block (`HemisphereAudioAppletF32.h:26-31`),
and you get intermittent dropouts — i.e. exactly the class of fault the
continuity scan in §6 exists to catch.

### 5.3 F32 vs int16 edges

- Reverb, Samverb, Delay are `HemisphereAudioAppletF32` and carry their own
  `AudioConvertI16toF32Multi` / `AudioConvertF32toI16Multi`
  (`HemisphereAudioAppletF32.h:17-57,99-100`). The host wires int16 on both
  sides and touches no converters. Correct by construction.
- **Abyss is int16 end to end** (`AbyssApplet.h:18` derives from
  `HemisphereAudioApplet`, and `AudioEffectAbyssReverb` is a plain `AudioStream`
  at `effect_abyss.h:16`), and its internal delay lines are int16 with
  per-line fixed-point scaling (`abyss_core.h:323-340`). Its noise floor and
  THD+N will be measurably worse than the other three. That is a design
  property, not a bug — but it must be measured separately and not averaged
  into a single "audio apps" verdict.
- The bus itself re-quantises to q15 (`AudioMixer.h:99-105`) and so does
  `output_route`, so there are **two** q15 round trips between an F32 applet and
  the codec if the sub-bus design is used. That is ~1 extra bit of dither-free
  requantisation *(inferred)*. If a bench measurement shows it, the fix is to
  make the sub-bus F32 (`AudioMixer4_F32`) and convert once — but do not
  pre-optimise; measure first.

### 5.4 Everything else that could make four coexist badly

| Risk | Evidence | Mitigation |
| --- | --- | --- |
| Shared `enc_edit[AUDIO_SLOT_L]` across five hosts | `HemisphereApplet.h:161-169`, `HSUtils.h:51` | `CancelEdit()` on RESUME |
| Factory pool contention with Quadrants | pools of 8 at `audio_applets/_config.h:59-60` | fine at 4 apps; document |
| Unity-sum clipping at 7 sources | `AudioIO.cpp:52-59`, `AudioMixer.h:99-105` | sub-bus with future per-source gain |
| Quadrants' unbracketed applet swap | `AudioAppletSubapp.h:268-292` | do not copy it; bracket `Start()` |
| `OutputStream()` never referenced | `AudioIO.cpp:116-124` | the sub-bus forces it; keep Quadrants' call too |
| PSRAM absent -> Delay is silent, not shorter | `AudioBuffer.h:146-150` vs `DelayApplet.h:399-402` | surface on status pane |
| EEPROM overflow | `apps/_config.h:180-181` | 232 bytes for four (computed); assert is the gate |
| **Cannot be simulated** | see below | hardware QA is mandatory |

**Simulator coverage is zero for these apps.** `tools/xeno-sim/Makefile:41-43`
builds with `-DNO_HEMISPHERE`, and `HemisphereApplet.cpp` is **not** in
`FW_SRCS` (`Makefile:47-77`). Any class deriving from `HemisphereApplet` — which
is all four applets — will not link. Making them simulatable means adding
`HemisphereApplet.cpp` and its transitive dependencies to the sim, which is a
separate project. Until then these four are hardware-only, consistent with
`docs/UI-Redesign-Constraints.md:110-125`.

### 5.5 Is four too many, and which ships first?

**Four is too many for one change.** Not because the architecture cannot carry
them — it can — but because of the RAM2 fallback nondeterminism in §5.1 and
because Abyss has never executed in this tree.

Ship order:

1. **Delay first.** It is the most-used effect, it is already registered and
   proven in Quadrants (`audio_applets/_config.h:78,107`), it is the one that
   forces the two infrastructure changes worth doing once (the sub-bus and
   `F32_AUDIO_MEMORY`), and its buffer is PSRAM so it does not compete for RAM2
   at all.
2. **Reverb second.** Smallest RAM2 footprint (50 KB), simplest applet,
   already-graceful out-of-RAM path. It is the cheapest possible proof that the
   MONO fan-to-stereo path is right.
3. **Samverb third.** Identical shape to Reverb, 78 KB. Ships as a one-line
   template instantiation once Reverb is proven.
4. **Abyss last, and gated on a measurement.** 230 KB is more than Reverb and
   Samverb combined, its PSRAM fallback is documented to cost >50 % CPU, it is
   the only int16 member, and it is currently unbuilt code. Before it ships,
   uncomment it in the Quadrants registry (`audio_applets/_config.h:79,108`)
   and bench it there first — that is a two-line change that de-risks the whole
   app.

If only one ships: Delay.

---

## 6. Build sequence, in dependency order

Each step is independently buildable and independently benchable.

1. **`AudioIO.h` / `AudioIO.cpp`** — add `kOutputRouteAudioAppsSlot`, bump
   `kOutputRouteSources` 3 -> 4 (`AudioIO.h:27`), add the
   `AudioSummingRoute<2,4> audio_apps_bus` global next to `output_route`
   (`AudioIO.cpp:60`) and its two connections into the lazy block
   (`AudioIO.cpp:124-160`). Bump `F32_AUDIO_MEMORY` 80 -> 128 (`AudioIO.h:13`).
   *Bench gate: nothing regresses. Noise floor and THD+N unchanged for
   Quadrants, Tweighty, Sampler.*
2. **`software/src/AudioAppletApp.h`** — the wrapper, with no app using it yet.
   Compiles under `T41_audio` only.
3. **`software/src/apps/DelayApp.h`** + `apps/_config.h` + `OC_app_folders.h` +
   `platformio.ini` flag. *Bench gate: full §7 suite.*
4. **`software/src/apps/ReverbApp.h`** — proves the MONO fan-out path.
5. **`software/src/apps/SamverbApp.h`** — trivial once 4 is done.
6. **Uncomment `AbyssApplet<STEREO>` in Quadrants' stereo registry**
   (`audio_applets/_config.h:108`) and bench it there. *Gate: measure CPU with
   the arena in RAM2 and, forced, in PSRAM.*
7. **`software/src/apps/AbyssApp.h`** — only if step 6 passes.
8. **Docs**: `docs/App-and-Applet-Index.md`, `docs/_apps/`,
   `docs/Panel-Binding-Matrix.md` for the new A/B/X/encL bindings.

---

## 7. Hardware verification (there is no simulator path)

Per app, over `tools/hwqa/hwctl.py` for the panel and an SSH'd `arecord`/
`aplay` on the USB-audio host for the signal. **Confirm capture format with
`arecord --dump-hw-params` and capture `-t raw`** — a WAV-headered capture read
as headerless raw byte-shifts every channel and has already produced a false
regression in this project's own QA tooling.

1. **Noise floor**, outputs silent, effect bypassed and then engaged with Mix at
   0 %. Report dBFS. **Disconnect any physical output-to-input patch cable
   first** — a closed loop through an app passing signal at unity will
   self-oscillate, which is correct modular behaviour and a completely
   misleading noise measurement.
2. **THD+N** at -6 dBFS and -40 dBFS via FFT bin analysis: fundamental +
   harmonics against a **notched** broadband noise integral, excluding the
   fundamental and harmonic bins and any known resident tone (Quadrants'
   always-on chain is still summing into the same output — either silence it or
   exclude its bands, and say which).
3. **Low-level linearity**: a 34 dB source drop must produce a 34 +/- 1 dB drop
   in the measured fundamental. Abyss's int16 delay lines are the one place this
   is expected to degrade.
4. **Continuity scan**, 30-60 s: FFT-brickwall the test tone, then scan
   sample-to-sample deltas against the maximum physically possible step for that
   frequency and amplitude. Any excess is a dropped, popped or repeated sample —
   this is the check that catches F32 pool exhaustion (§5.2), which produces
   silent per-block dropouts rather than any error.
5. **Pool and CPU telemetry** read off the new status pane at each step:
   `AudioProcessorUsageMax()`, `AudioMemoryUsageMax()`,
   `AudioMemoryUsageMax_F32()`. Record the numbers with one app live, then two,
   then all four with `live_` on.
6. **App-switch survival**: open the app, verify audio, switch to Captain MIDI,
   confirm the expected behaviour for the `live_` setting (silence when off,
   continued audio with frozen CV when on), switch back, confirm no click and no
   parameter loss.
7. **The steady-drone check.** If an unexplained steady tone at a
   harmonic-related frequency survives app and config changes and only a cold
   restart clears it, suspect a missing `arm_dcache_flush_delete` in a
   DMA-adjacent silence/reset path before suspecting the DSP graph — that exact
   bug has been found here before.
8. **Dead-output triage.** If the panel jacks go silent entirely, check whether
   `OC::AudioIO::OutputStream()` was ever called this boot before assuming the
   graph is broken (`AudioIO.cpp:116-124`).

Console note: `hwctl.py` requires the literal `pew!` unlock and re-locks on
every reboot and every flash; silence from the port is the locked state, not a
dead module.

---

## 8. Summary of files

**Create**

- `software/src/AudioAppletApp.h`
- `software/src/apps/DelayApp.h`
- `software/src/apps/ReverbApp.h`
- `software/src/apps/SamverbApp.h`
- `software/src/apps/AbyssApp.h`

**Modify**

- `software/src/AudioIO.h` (`:13` pool, `:27` sources, new slot constant)
- `software/src/AudioIO.cpp` (`:60` sub-bus global, `:124-160` lazy wiring)
- `software/src/apps/_config.h` (includes after `:62`, container after `:169`)
- `software/src/OC_app_folders.h` (four rows after `:100`)
- `software/platformio.ini` (`-DENABLE_APP_AUDIOFX` in `[env:T41_audio]`, after `:155`)
- `software/src/audio_applets/_config.h` (`:108` — uncomment `AbyssApplet<STEREO>`, step 6 only)

**Unchanged**

- All four applet headers. Nothing in `audio_applets/*.h` needs to be edited for
  this design, which is the point of the 64/64 split pane in §3.2.
- `software/src/AudioAppletSubapp.h` and `software/src/apps/Quadrants.h`. The
  applets stay available in Quadrants; the standalone apps are additive.
