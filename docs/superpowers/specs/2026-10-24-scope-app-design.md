# Scope app: oscilloscope + FFT — design

2026-10-24. Brainstormed with Oren; hardware facts from `hardware/xenomorpher/README.md`.

## Intent

Oren patched a ring modulator: a wavefolded 259e against a sine from a 258. He
wanted to see the resulting wave on the Xenomorpher. The existing **Scope** app
(`software/src/apps/ScopeApp.h`, AUDIO folder of the app switcher) cannot show it:

- **Audio:** it keeps one peak value per 128-sample block (~375 points/s), so it's a
  scrolling envelope, not a waveform.
- **CV:** it samples once per screen redraw.
- **Controls:** there's no trigger, no time or volt scale, no FFT, and one channel at a time.

The goal is a real scope that sits on the instrument and is used *while patching*.
Two moments define success:

1. **Still.** A complex audio wave (the ring-mod case) held steady enough to read its shape.
2. **Live.** The same wave *morphing* as the fold or modulation is turned, with the
   picture staying locked: no rolling, no jumping between trigger points.

Plus a third use: watching **several slow or fast CVs** from the banana jacks at once.

## Goals

- Upgrade the existing Scope app in place: same app ID `SP`, same name, same folder.
  Do not add a second app.
- Sources:
  - the **2 audio-in jacks**, for audio;
  - the **8 CV-in banana jacks**, for CVs (slow or fast);
  - the **4 TR jacks**, as gate traces and as external trigger;
  - keep the existing **CV out 1–8** and **audio out L/R** monitor channels the
    current app already offers.
- Views:
  - **scope**, with **overlaid** or **stacked** multi-channel display (user-selectable);
  - **FFT spectrum**.
- **Automatic** trigger, timebase and volts/div by default. The user can override any
  one of them, and that one stays where it was put.
- Voltages in **real jack volts**.

## Non-goals

- Recording or exporting captures. Measurement cursors. Math channels (A−B, XY) and
  persistence/afterglow modes. None of these came up; leave them out.
- Anything that needs a second app, a new USB endpoint, or a change to the audio
  graph's other users.

## Anti-goals (a failure even if it works)

- **Audio glitches.** USB MIDI + audio quality is this instrument's foundation
  requirement. The scope's capture runs in or beside the audio ISR and the core ISR:
  its hot-path cost must be small and bounded. No FLASHMEM on ISR paths. Nothing may
  run when the app is not in the foreground (or showing its screensaver trace).
- **A jittery or jumping picture** in the "live" case. Auto-ranging that hunts, or a
  trigger that hops between equally good edges every frame, defeats the purpose.
  Damping and hysteresis are required, not optional.
- **Unreadable text.** On the bench, mockups using a scaled browser font failed. Use
  only the firmware's 6×8 font (`gfx_font_6x8.h`, 21 columns). Oren approved
  the redrawn mockups (`.superpowers/brainstorm/…/scope-layouts-v2.html`, local).

## Hardware facts that shape the design

From the schematics (`hardware/xenomorpher/README.md`), bench-confirmed where noted:

- **Audio inputs are AC-coupled** (1 µF / 100 kΩ, ≈1.6 Hz high-pass) and full scale is
  ≈ ±9.3 V at the jack (PCM1808, 0.6·VCC). The audio channels must be labelled **AC**.
  DC and slow LFOs belong on a CV jack.
- **CV inputs are DC-coupled.** About −1.35 V … +10.65 V (bench), inverted at the ADC
  (0 V reads raw ≈ 3635). The firmware already corrects the inversion and applies
  calibration (`OC::ADC::calibrated_value`, per-input calibration in `InputCal.h`). The
  scope must reuse that conversion, never raw ADC codes.
- The MCP33131D is scanned through the 4051 mux by FlexIO DMA at **300 kHz / 8 =
  37.5 kHz per channel** (`OC_ADC.cpp`, `ADC33131_SAMPLE_RATE`). `Scan_DMA()` (core ISR,
  rate-limited to every 3rd call) averages the frames it finds down to one value per
  channel. The scope needs the un-averaged frames, so add a tap there. Slot order
  0..7 = CV5–8, CV1–4, already handled by `Scan_DMA()`.
- **TR jacks** are LM339 comparators, threshold ≈ 3 V at the jack, active-high at the
  Teensy (pins 0, 1, 23, 22). Today they are polled in the 60 µs core ISR
  (`OC_CORE_ISR_FREQ` = 16 666 Hz).
- Audio runs at 48 kHz, 128-sample blocks. The existing `AudioScopeCapture`
  (`Audio/AudioScopeCapture.h`) is permanently wired to in L/R and out L/R and gated by
  an active-channel flag.

## What it does (decided with Oren)

### Screen

- 128×64 OLED. **One line of text at the top** (21 chars, 6×8 font). Everything
  below it is plot.
- The top line is also the **parameter row**: `channel · time · volts · trigger · view`.
  The focused field is drawn **inverted**, and that is the only use of inversion. An
  auto-controlled field shows an **A** marker (mockup: `AUTO` at the right of the
  audio screen).
- **Scope, one channel:** dotted centre line plus dotted division grid. A small tick at
  the left edge marks the trigger level.
- **Overlaid:** every trace on one shared volt scale. Each trace has its own line style
  (solid, dotted, dashed, sparse), because the panel is monochrome, and its jack
  number is drawn at the trace's right end.
- **Stacked:** equal-height lanes. Each lane auto-scales on its own, with the jack number
  (inverted chip) at the left and the live value at the right. **TR jacks** in a stack
  draw as thin on/off lanes, like a logic analyser.
- **FFT:** log frequency axis 20 Hz–20 kHz, with dotted gridlines at 100 Hz, 1 kHz and
  10 kHz, and level in dB. The top line shows the strongest peak's frequency (`pk 550Hz`).

### Controls (the instrument's grammar: encL = where you are, encR = what it is)

| control | does |
|---|---|
| encL turn | move the cursor along the top-line fields |
| encR turn | change the focused field. Turning time, volts or trigger takes that field off auto |
| encR push | return the focused field to auto |
| encL push | cancel / back (as everywhere on the instrument) |
| A | freeze / run |
| B | auto-set: every field back to auto at once |
| X | scope ⇄ FFT |
| Y | overlaid ⇄ stacked (when more than one channel is shown) |

Remember the app-switcher chord (hold A, press encR) when wiring A and encR.
`Bus200eApp.h` documents why Z/`CONTROL_BUTTON_M` was historically unwired. Z *is*
reachable on hardware, but nothing is assigned to it here.

### Channel selection

The channel field steps through a fixed list of groupings, then **Custom…** opens a
picker where any set of jacks can be ticked. The quick list must include:

- IN L, IN R, IN L+R;
- each single CV in;
- CV1–4, CV5–8;
- CV1–4 + TR1–4.

The exact list order, and whether CV outs and audio outs get groupings of their own,
are the builder's call. The quick-list-plus-Custom split itself was **my call**:
Oren's answer to "groupings or picker?" was "sure".

### Triggering

- Trigger source choices:
  - **Auto**: lock to the displayed signal itself;
  - **CVn level**: a level on a CV channel;
  - **TR1–TR4 edge**: external sync, e.g. a clock, or the 258's sync output for the
    ring-mod case.
- External TR sync must be **sample-accurate** against audio. Catch the edge with a pin
  interrupt and timestamp it, so it does not inherit the 60 µs core-ISR polling jitter.
  This is my suggestion, which Oren accepted ("both").
- **Auto** trigger has to handle the ring-mod case: lock to the full repeat period of
  the composite wave, not to whichever sideband crossing is nearest, and *stay* locked
  as the wave morphs. That is the hard part of this project.

### Auto-ranging

- **Timebase:** choose so a few repeat periods fill the screen.
- **Volts/div:** fill the plot height. On a step change, react quickly; when the signal
  is steady, settle slowly. No hunting.
- The **trigger level** follows the signal's range.
- Overriding a field leaves the others on auto.

## Constraints

- Builds: the full scope (audio channels, FFT of audio) needs `XENO_CODEC_AUDIO` (gate
  on that macro, not `AUDIO_INTERFACE` or `ARDUINO_TEENSY41`; see
  `docs/Timing-Budget.md` and the CI gate). Without the codec the app keeps working
  with CV/TR sources, as it does today.
- Persistence: settings are saved through `SaveAppData`/`RestoreAppData` like today.
  The format grows, so old 3-byte saves must still restore (or fall back to defaults
  cleanly).
- The host-test runner (`software/test/run-host-tests.sh`) is how pure logic gets tested.
  Trigger detection, auto-ranging and channel/grouping maps belong in pure functions
  next to `ScopeMath.h`, with host tests.
- Hot-path code (audio `update()`, the `Scan_DMA` tap, the TR pin interrupt) is not
  FLASHMEM. It allocates nothing and does bounded work. When the app is suspended it
  is gated off (today: `SetActiveChannel(-1)` on `APP_EVENT_SUSPEND`).

## Left to the builder

- Capture buffer sizes and memory placement (DTCM vs DMAMEM).
- The FFT size, window, and implementation (CMSIS in the Teensy core, or the Audio
  library's FFT objects).
- The auto-trigger and auto-range algorithms, within the anti-goals above.
- The exact quick-list order, division values, lane heights and line-style patterns.
- How the old peak-envelope behaviour maps forward. A slow "roll" timebase is a natural
  home for it.
- Whether the encL-push screensaver-trace toggle survives. encL push is now "cancel/back".

## Calls settled during review (mine, not Oren's: check these)

1. **Reference channel.** The trigger field picks a **source channel plus a mode**. By
   default the source is the first channel in the shown set, mode Auto. The FFT analyses
   that same source channel. A CV source gives an FFT at its 37.5 kHz rate (useful for
   audio-rate CVs), with the axis capped at Nyquist. A TR source has no FFT (X is a no-op
   and the top line says so).
2. **Mixed audio + CV views are allowed** (Custom can tick anything). Every trace is drawn
   against one shared time axis: each column takes the sample nearest that column's time
   from its own capture. Alignment between channels need only be within one sample of the
   coarser rate. Nothing is resampled in the ISR.
3. **"Sample-accurate" TR sync** means the sweep starts within one sample *at the source
   channel's rate* of the true edge. A fixed, known offset between capture domains is fine
   if it is compensated. A varying one (the 60 µs polling jitter) is not.
4. **The CV tap captures only the selected CV channels**, but every DMA frame for those
   channels (37.5 kHz each). When no CV channel is selected, it does no work.
5. **Acceptance bar for "still" and "no jumping".** On a steady periodic input (the
   ring-mod case, or a test-vector equivalent in host tests), the trigger point varies by
   ≤ 1 sample frame to frame, and auto timebase/volts change at most once per second
   with no back-and-forth between two values. When the input morphs, the trigger stays on
   the same feature of the waveform (no jumps of a whole sub-period) unless the period
   itself changes by more than ~10 %.
6. **The old peak-envelope view is not required.** A roll timebase is optional. Old 3-byte
   saves restore their channel where it still exists in the new list, and defaults for
   everything else.
