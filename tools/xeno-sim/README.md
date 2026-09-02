# xeno-sim — host simulator for the Xenomorpher's UI

Runs the module's firmware on a Mac (or any host with a C++17 compiler) so the
whole interface can be navigated, exercised and critiqued without hardware.

It runs the **real firmware**, not a mock-up of it. The app switcher, the
preset-bus overlay, the Setup/About app, the 200e app, the UI event loop with
its debounce and encoder acceleration, the display pipeline and the 6x8 font
are all compiled from `software/src/`, unmodified. Only hardware is replaced.

```sh
cd tools/xeno-sim
make                       # -> build/xeno-sim
make gui                   # ...the panel in a browser (recommended)
make check                 # the self-checks: determinism, gestures, the write
                           # path, storage across a power cycle, chord guards
./build/xeno-sim           # interactive, in your own terminal
```

---

## What you can reach, and how

The simulator boots the way the module does: hardware detect, calibration load,
splash screen, app restore. Every screen below is drawn by firmware.

| screen | gesture | scripted |
|---|---|---|
| the current app | — | (boot) |
| **app switcher** | hold **A** (or **Z**), press the **right encoder** | `--keys "a-down,step60,r-down,step60,r-up,step60,a-up,step200"` |
| **preset-bus overlay** | press **both encoder buttons** | `--keys "l-down,step20,r-down,step80,l-up,r-up,step200"` |
| **screensaver** | hold **Z**, press **A** | `--keys "z-down,step60,a-down,step60,a-up,step60,z-up,step300"` |
| **IO settings** | hold **A** (or **Z**), press the **left encoder** | `--app Scenery --keys "a-down,step60,l-down,step60,l-up,step60,a-up,step300"` |
| **EEPROM reset prompt** | hold **A** and **B** through the splash | `--reset-settings` |
| **Setup/About**, and its calibration wizard | app switcher → *Setup/About* | `--app "Setup/About"` |

The IO settings screen is offered per app: the 200e app declines it
(`io_settings_allowed()`), so reach it from Scenery or Pong.

Inside the app switcher the **right** encoder scrolls, a click switches app, and
a long click switches and saves. The **left** encoder cancels on a press and
opens the debug-stats page on a **hold** — that page is a blocking firmware
loop whose only exit is the right encoder, so scripting it needs a press
already in flight before it is entered: see `<button>-inN` under *Keys*.
`--app NAME` boots straight into one; `--app x` lists them.

Inside the preset overlay: the left encoder moves the cursor, the right one
changes what is highlighted, **hold the left encoder 500 ms to STORE**, **hold
the right encoder 250 ms to RECALL**, and both progress bars fill for real —
they are timed by `PresetBusUI::Task()` against `ui.read_immediate()`, and the
simulator drives that from actual held pin state. `1` `2` `3` `4` pulse TR1-TR4,
which is what the 225e last/next jacks are wired to; `~1`..`~4` are the same
jacks with a zero-width spike — high and low again before any time passes —
that only the GPIO block's edge latch can see. The simulator sets the port's
ISR flag on the active-going edge the way the silicon does, so
`DigitalInputs::Scan()` sees real edges here and the preset overlay's
NEXT/LAST stepping runs on the latched-edge path it uses on hardware.

The boot-time gestures — hold the left encoder through the splash for
Calibrate, the right encoder for the app menu — run too, but there is no flag
for them yet; they need a button held before `SimRuntimeBoot` starts.

---

## Reproducing what someone saw

This is the part to reach for when a screen looks wrong.

**In the browser**, the *Session* card has **Copy session to clipboard**,
**Download session file** and **Copy this frame as hex**. A session is every
button, encoder detent and injected event since boot, with the **simulated
milliseconds between them** — which is what makes a 500 ms hold or a 250 ms
hold reproducible at all. It is short enough to paste into a message:

```
xeno-sim-session 1
0 btn l down
25 btn r down
87 btn l up
5 btn r up
612 btn r down
301 btn r up
209 end
```

**On the other end**, save that to a file and:

```sh
./build/xeno-sim --replay session.txt            # end state, as a screen
./build/xeno-sim --replay session.txt --frames   # every step
./build/xeno-sim --replay session.txt --at 4     # stop after event 4
./build/xeno-sim --replay session.txt --dump-fb  # the framebuffer, as hex
```

Running the same session twice produces byte-identical frames. `make check`
verifies that, because a replay that silently diverges would be worse than no
replay at all. What guarantees it:

* the simulated clock is the **only** time source any firmware code can
  observe — `millis()`, `micros()`, `delay()`, `delayMicroseconds()`,
  `elapsedMillis` and `elapsedMicros` all read it, and it moves only when the
  simulator moves it;
* `delay()` and `delayMicroseconds()` **advance the clock and run the
  background** — the core ISR and the 1 kHz UI poll on their own schedules,
  exactly as `display::SimPump()` does for a blocked frame. On hardware a delay
  is not a pause in the machine, and firmware relies on that: every
  `while (!done) { …; delay(x); }` is waiting for something only the ISR can
  deliver. `delayMicroseconds()` was a no-op, which froze the clock inside
  precisely those loops — `Ui::DebugStats()` is a `while (!exit_loop)` paced
  only by `delayMicroseconds(10)`, so the UI poll never ran, no button event
  was ever queued, the frame buffer was never drained, and the simulator spun
  forever with a blank screen and had to be killed;
* `random()` is seeded to a fixed value at start, so even the splash screen's
  icon roulette repeats;
* every simulator-owned static is reset before boot.

Wall-clock time is read in exactly one place — the interactive terminal loop's
pacing — and only ever to decide *how much simulated time to advance*, which is
the number the session records.

`--record FILE` writes a session from a scripted or interactive run.

### Comparing against the real module

The module's console dumps its framebuffer as 2048 hex digits on one line
(press an unmapped key on the console; see `Main.cpp`'s capture path). The
simulator prints the same 1024 bytes in the same page-packed order:

```sh
./build/xeno-sim --keys "..." --dump-fb > sim.hex
python3 fbdiff.py device.hex sim.hex
```

`fbdiff.py` reports differing bytes and differing pixels, and draws an ASCII
delta: `#` lit in both, `.` dark in both, `a`/`b` lit in only one. It skips the
console chatter around a capture and does not care about case or whitespace.
`--quiet` gives the counts only and exits non-zero on any difference.

`fbtext.py` reads the same capture the other way: it tells you what the screen
SAYS.

```sh
./build/xeno-sim --keys "..." --dump-fb | python3 fbtext.py -
y=46  x=0    WROTE + VERIFIED
y=56  x=100  [inv] Save
```

It is not guesswork — it renders every glyph of the firmware's own 6x8 font
(parsed from `gfx_font_6x8.h`, never copied, so it cannot drift) and demands an
exact pixel match, reporting inverted text as `[inv]`. That makes a regression
check assert what a person would actually read, instead of a frame hash that
says only "something moved". `selfcheck.sh` uses it for the whole 200e write
path.

`corrupt_slot.py IN OUT SLOT SECTION [--fix-sum] [--sd]` damages one preset
container inside a `--state` file: one bit flipped in the named section's
payload, and with `--fix-sum` the container's section checksum recomputed so
the damage gets past the engine and reaches PhzConfig's own reader. `--sd`
damages the copy on the card volume instead — an exported container — for the
import path. `selfcheck.sh` recalls both and expects `BAD PRESET` from each
layer, and imports the damaged card copy expecting `bad file` with the local
slot untouched.

`edgecheck.py` reads the same capture for the one thing `fbtext.py` structurally
cannot see: **text clipped at the right edge**.

```sh
./build/xeno-sim --keys "..." --dump-fb | python3 edgecheck.py -
y=46  CLIPPED: a glyph is cut off at x=127 (column bits 01100000)
```

The 6x8 font gives a row 21 columns, x=0..125. A 22nd character starts at x=126
and only its first two pixel columns fit, so it is drawn and then cut off — and
it does not *look* cut off, it looks like a shorter string. `fbtext.py` demands
an exact glyph match, and two thirds of a glyph matches nothing, so the clipped
character is simply absent from the decode: the truncation is invisible to any
text assertion. This was a real defect. `LIVE %lus ago  wire %d` had 17 columns
of fixed overhead, so a 3-digit age plus a 2-digit slot came to 22 and rendered
`wire 29` as `wire 2` — a perfectly well-formed slot number, on the screen that
precedes a 30-slot rewrite.

So `edgecheck.py` looks at the pixels. In columns 126-127 an honest row is
uniform: all dark, or all lit under an `invertRect` (which is how this UI
shouts). Two thirds of a glyph is neither. It exits non-zero on any offending
row, and `selfcheck.sh` sweeps it over a dozen screens.

One case is uniform in neither sense and is still honest: text right-aligned
with `print_right(127)` legitimately ends at x=126. The debug-stats page's
`R:exit` hint does exactly that. So the rule is not "is this column uniform"
but **"is every lit pixel here accounted for by a glyph that decoded"** — a
whole glyph decodes, two thirds of one matches nothing, and it is that same
property which makes a truncation invisible to `fbtext.py` in the first place.
The original defect is still caught: its 22nd character starts at x=126, where
no cell can even be read, so those pixels belong to no decoded run.

**No device captures have been compared yet.** The mechanism is here and tested
against itself; the numbers that would turn "looks right" into "is right" need
someone with console access to capture the reference frames. Worth capturing
first, in this order: the 200e app's home screen, the app switcher, the preset
overlay at slot 1, Setup/About's first page.

## The write path

A 200e write rewrites all 30 presets and has no undo, so the interesting
question is not whether it works — it is whether it tells the truth when it
does not. `--write-fault` makes the simulated 251e mishandle the RESTORE:

| value | the module... |
|---|---|
| `none` | stores the bank faithfully (the default) |
| `ignore` | reports done and stores nothing |
| `drop-tail` | stores all but the last record |
| `flip-first` | stores one byte wrong in the FIRST slot |
| `flip-last` | stores one byte wrong in the LAST slot — collateral damage |
| `short-readback` | stores the bank perfectly, then serves one slot instead of thirty on the read-back — and reports DONE |

The firmware reads the whole bank straight back after every write and compares
it against what it built, so each of these should end on a `BAD:` line rather
than `WROTE + VERIFIED`. `make check` asserts all six.

**The two truncations are opposites, and neither is a spelling of the other.**

`drop-tail` is a bad STORE seen through a **good** read-back. The record it
drops is one this edit never changed, so all 63,120 bytes come back and they
genuinely match the intent: it verifies clean, and that is the correct answer.
There is nothing wrong with the module's contents to report, and inventing an
alarm there would teach the user to ignore the real ones.

`short-readback` is a good STORE seen through a **bad** read-back, and it is
the one shape that can fake a verify. The read-back lands in the *same* 64 KB
card image the RESTORE was sourced from, so every byte the module does not
send back still holds the value we intended — and a whole-bank hash of that
image agrees with itself, happily, on 3% of the evidence. The firmware
therefore requires `Bus200eMasterBytesTransferred() >= bank_len` before it will
compute the verify hash at all; requiring only that the read-back covered the
*edited slot's* 2,104-byte window (which a 2,104-byte read-back does exactly)
earned `WROTE + VERIFIED` on a bank nobody had seen, and then stamped
`read_hash_` with it, poisoning every later diff. A short DONE is real: one
bench capture during bring-up came back exactly one record shy while reporting
success.

`make check` asserts both, side by side, for exactly this reason.

---

## Apps

`app_container` is the real one, and the app switcher lists exactly what is in
it. This build carries five apps:

| app | what the shims cost it |
|---|---|
| **Setup/About** | The calibration wizard runs and its pages navigate, but every measurement it takes reads a **fixed, silent ADC**, so no step converges on anything and the numbers are meaningless. The bus statistics page is all zeros (see *the bus*, below). The invert-display toggle on `A` **does** work — the panel shim honours the inversion command. |
| **200e Modules** | The most faithful app here: the bus master FSM, the codecs, the write guard and the bank data are real (see *the simulated bus*). Writes go nowhere. |
| **Scenery** | Scene state and the UI are real; the CV that would drive a scene is a fixed value, so nothing modulates. |
| **Pong 2.0** | Plays. Its pace comes from the loop rather than an audio-rate ISR, so speed is not representative. |
| **Back It Up!** | Runs against the RAM-backed file system, so a "backup" is written to a file that dies with the process. |

### Apps that are **not** simulated, and why

* **Quadrants / Hemisphere** — needs the whole applet registry
  (`applets/_config.h`, ~60 applets and the audio applet chain). This is the
  big one, and it is the largest single gap in coverage: much of the module's
  UI lives in Hemisphere. It is a project, not a shim.
* **Captain MIDI**, **Calibr8or**, **Scale Editor** — blocked by a **firmware
  portability issue, not by the simulator**. Each calls a non-`const` member
  from a `const` draw path:
  * `apps/CaptainMIDI.h:393-394` — `MainView() const` calls `DrawCopyScreen()`
    and `DrawLogScreen()`
  * `apps/Calibr8or.h:755,757` and `apps/ScaleEditor.h:192-213` — a `const`
    draw method calls `SegmentDisplay::PrintWhole()` / `PrintDecimal()`

  `arm-none-eabi-g++` accepts all of these; Apple clang rejects them. Marking
  those methods `const` (and `SegmentDisplay`'s cursor members `mutable`) would
  fix it and cost the firmware nothing — no simulator change is needed. Captain
  MIDI is the module's default app on hardware, so this is worth doing.
* Everything else in `apps/` — simply not in this build's manifest. Adding one
  is a line in `shim/src/apps/_config.h` plus its `-DENABLE_APP_*`.

---

## How the firmware gets compiled

The firmware reaches its own headers with quoted includes — `OC_apps.cpp` says
`#include "OC_core.h"` — and a quoted include resolves against the directory of
the *including file* before it looks at any `-I`. So no include path can shadow
a firmware header for a firmware `.cpp`.

`build/shadow/` is a mirror of `software/src/` made entirely of **symlinks**,
with `tools/xeno-sim/shim/src/` laid over the top (`mkshadow.sh`). Compiling
through the mirror moves "the directory of the including file" somewhere we
control, and the overlay becomes possible. Everything in the mirror is the real
file unless `shim/src/` names it.

**Nothing under `software/src/` is copied or modified.** `make` rebuilds the
mirror every time; it is only symlinks, and `make` stats through them to the
real sources, so it costs nothing in recompilation.

### What `shim/src/` replaces — the entire list

| file | why |
|---|---|
| `apps/_config.h` | this build's app manifest (see above) |
| `OC_ADC.cpp` | a FlexIO/ADC_ETC/DMA driver for a converter that is not here. The *header* is real, so the smoothing and calibration arithmetic are the firmware's; only the raw counts are fake. |
| `OC_core.cpp` | owns the two `IntervalTimer`s. `sim_runtime.cpp` calls the same two functions at the same rates from one loop. |
| `src/drivers/SH1106_128x64_driver.cpp` | the OLED panel's SPI transfer and command set. Everything above it — `GRAPHICS_BEGIN_FRAME`, the double-buffered `FrameBuffer`, the paged driver, `display.cpp` — is real. |
| `src/drivers/FreqMeasure/OC_FreqMeasure.cpp` | a FlexPWM/QuadTimer edge-capture peripheral |
| `src/drivers/display.h` | **one macro**, `GRAPHICS_BEGIN_FRAME`. Its busy-wait needs the ISR to keep running underneath it, which the simulator has to arrange by hand; everything else comes from the real header, included from it. Without this the splash screen, the EEPROM-reset prompt and the calibration wizard would all hang. |
| `util/util_math.h` | two ARM `ssat`/`usat` inline-asm helpers, replaced with portable C++ of the same behaviour. The rest of the real header is included. |
| `src/extern/dspinst.h` | Cortex-M4 DSP intrinsics; takes the header's own Cortex-M0 fallbacks where they exist. |
| `avr/pgmspace.h` | a one-line forward to the Arduino stand-in |

`shim/arduino/` is the Teensyduino side: the clock, the pins, `Serial`, `SPI`,
`Wire`, a RAM-backed `EEPROM`, a RAM-backed `FS`/`SD`/`LittleFS`, the MIDI
ports, `Audio.h`, and stand-ins for the handful of i.MXRT registers the
firmware writes from headers that are otherwise real.

`sim_stubs.cpp` holds the last few symbols that live in files the simulator
cannot compile (`Main.cpp`, `HemisphereApplet.cpp`, `applets/_config.h`).
**Check there first** when something behaves as though a global were missing.

`sim_runtime.cpp` is the one piece of the running system that is *not* the
firmware's own: `Main.cpp` cannot be compiled here (USB descriptors, the audio
graph, MTP, the watchdog, the crash handler, a 400-line serial console), so it
transcribes `setup()`'s init order and `loop()`'s redraw/dispatch order, naming
the `Main.cpp` line ranges it follows. If `loop()` changes, that file must too.

---

## The panel

Click a button to press it; right-click or shift-click to **latch** it down,
which is how chords are made with a mouse. Keyboard `keydown`/`keyup` hold too.
Click an encoder to push it, wheel over it or drag around it to turn it, or use
the `−` / `+` beside it (Shift for ×10).

Input is **pins, not events**. The simulator drives the seven button pins and
the four encoder quadrature pins, and the firmware's own `OC_ui.cpp` reads
them, so:

* a press must survive `UI::Button`'s 8-bit debounce (~7 UI ticks) before it is
  a press;
* a long press is `kLongPressTicks` (500) **UI ticks**, counted by the 1 kHz UI
  poll, not by a wall clock;
* **encoder acceleration is real** — `UI::Encoder`'s acceleration accumulator
  is being fed real detents at a real rate, and
  `global_settings.encoders_enable_acceleration` still turns it off from the
  app menu's `B` button.

A detent is four pin phases at one per UI tick — 4 ms, which is a brisk but
possible hand speed. Queue several (a drag, a ×10 key) and they arrive back to
back and acceleration builds; a single detent never accelerates. **The
simulator cannot express a turn slower than 4 ms per detent through the
multi-detent path**, so a leisurely fast turn is under-represented.

### `Z`, the grey `clock` button

It is wired here as a working control. The firmware initialises `but_mid` on
pin 20 for this hardware (`OC_gpio.cpp`'s `Pinout_Detect`, ID voltage ≥ 0.05 V)
and `OC::Ui::Poll` reads it with the other six as `CONTROL_BUTTON_M`, so the
simulator drives its pin like any other button. Hold **Z** and press **A** for
the screensaver, or hold **Z** and press the right encoder for the app menu.

**What is unconfirmed is the hardware binding**: two bench captures failed to
observe the physical grey button firing, and both were inconclusive. The
project owner states it is a real control. The simulator treats it as one; the
panel's caption says the binding is unconfirmed.

### What is deliberately not drawn

MIDI OUT, the audio IN L / IN R / OUT jacks, and the CV, trigger and attenuator
sections — nothing here models them. There is no power LED because the firmware
drives none. The simulator-only affordances (inject MIDI, advance a second,
toggle pacing) and the session controls live in visually distinct cards marked
*not on the panel*. Nothing on the drawn instrument is fictional.

---

## Command line

| flag | effect |
|---|---|
| `--keys "a-down,r,a-up"` | apply a key sequence, print the resulting screen, exit |
| `--dump-frames` | with `--keys` or `--replay`: print the screen after every token |
| `--dump-fb` | print the framebuffer in the console's capture format, and nothing else |
| `--record FILE` | write the session to FILE on exit |
| `--replay FILE` | replay a recorded session |
| `--at N` | with `--replay`: stop after event N |
| `--app NAME` | boot into an app by name (`--app x` lists them) |
| `--reset-settings` | take the first-run / EEPROM-erase path, answering its `ConfirmReset` prompt **OK** |
| `--reset-cancel` | the same gesture, answered **CANCEL** — the only way to ask whether a refused reset left storage alone |
| `--sd-card` | seat an SD card, so `SDcard_Ready` is true |
| `--state FILE` | non-volatile memory: read FILE before boot, write it back at exit. Two runs sharing one FILE are a power cycle |
| `--state-in FILE` / `--state-out FILE` | the halves of `--state`, separately |
| `--dump-fs` | list stored files with sizes and CRCs, and nothing else |
| `--test-phzconfig` | no boot: run the PhzConfig codec checks (serialize/deserialize, save/load, `save_filtered`, the clean-map skip, and the same damaged images through both readers) against the RAM volume, and exit with the result |
| `--snap-at MS` | capture the first complete frame drawn at or after MS (absolute), and let `--dump-fb` print that |
| `--id-voltage V` | the hardware-ID divider the firmware reads at boot to choose its pin map (default 0.10) |
| `--real-timing` | interactive only: pin the virtual clock to wall-clock, so a scan takes the ~60 s it takes on the module |
| `--bus-off` | make `PresetBus::Enabled()` false |
| `--capture-251e PATH` / `--capture-259e PATH` | different bank hex dumps |
| `--write-fault WHAT` | make the simulated modules mishandle a RESTORE: `none` (default), `ignore`, `drop-tail`, `flip-first`, `flip-last`, `short-readback` — see *the write path* |
| `--write-fault-once` | apply that fault to the **first** restore only, so the recovery write after it goes out to a module that behaves |
| `--no-log` | omit the status/log lines under the frame |
| `--stdio` | line protocol on stdin/stdout, for the browser front end |

### Keys

| key | control |
|---|---|
| `a` `b` `x` `y` | face buttons A / B / X / Y |
| `z` | the grey `clock` button (`CONTROL_BUTTON_M`) |
| `l` `r` | left / right encoder **push** |
| `[` `]` | left encoder turn, −1 / +1 |
| `,` `.` | right encoder turn, −1 / +1 |
| `{` `}` `<` `>` | the same, ×10 |
| `1` `2` `3` `4` | pulse TR1-TR4 |
| `~1` `~2` `~3` `~4` | a zero-width spike on TR1-TR4, seen only by the edge latch |
| `export` `import` | the bench console's `E` and `J`: every slot to / from the card |
| `n` | note-on into `usbHostMIDI[0]` — the port the k-board lives on |
| `N` | note-on into the 200e bus MIDI RX ring |
| `w` | advance one simulated second |
| `t` | toggle fast / real scan pacing |
| `q` | quit |

In `--keys`, tokens are comma-separated, so the comma key needs its word alias.
Word aliases: `encl` `encr` (pushes), `encl-` `encl+` `encr-` `encr+` (turns),
`note` `busnote` `wait` `quit`. Two extra scripted-only tokens:

* `stepN` — advance exactly N simulated milliseconds (e.g. `step200`)
* `snapN` — arm a frame capture N ms from now and return immediately; `--dump-fb`
  then prints **that** frame instead of the one on screen at exit
* `qencl+N@T` / `qencr-N@T` — queue N encoder detents for delivery T ms from
  now, without advancing the clock
* `<button>-down` / `<button>-up` — hold and release, which is how a chord or a
  timed hold is scripted. A bare token is still a tap.
* `<button>-inN` — schedule a tap N simulated ms from now and return
  **immediately**, without advancing the clock.

`-inN` is the only way a script can answer a screen the firmware draws from
inside its *own* blocking loop — `Ui::DebugStats()`, `ConfirmReset()`, the
calibration wizard. Those never return to the simulator's driver, so the token
that would press the exit button is never reached; the press has to be in
flight before the loop is entered. Scheduled taps are consumed by
`SimInputTick()`, which runs on the UI-poll schedule inside the pump, so they
fire while the firmware is spinning:

```sh
# in and out of the app switcher's debug-stats page (encL held = enter)
./build/xeno-sim --keys "a-down,step60,r-down,step60,r-up,step60,a-up,step200,\
r-in900,l-down,step1400,l-up,step200"
```

Capturing a frame from **inside** such a loop needs the same idea pointed the
other way, which is what `snapN` is: it arms a capture N ms ahead and returns
at once, and the capture fires from the panel driver's last-page write — so
what it keeps is one whole frame drawn from inside the loop, never a torn blend
of two. `--snap-at MS` is the absolute-time form, for screens drawn during boot
itself.

Paging inside such a loop needs `qencl+N@T`. `Ui::DebugStats` pages on an encL
**turn** while an encL **press** is its exit, and detents queued for delivery
*now* are eaten by whatever is still on screen while the entry gesture is being
held — the app menu takes them as list scrolling. The `@T` delay puts them past
the long-press threshold, i.e. inside the loop.

Together those make the debug-stats page checkable, and `selfcheck.sh` now
asserts its layout — the exit hint is present, the title and the hint do not
collide, and no page of it clips at the right edge:

```sh
./build/xeno-sim --keys "a-down,step60,r-down,step60,r-up,step60,a-up,step200,\
r-in3000,qencl+3@700,l-down,snap1400,step3500,l-up,step200" --dump-fb
```

A terminal cannot report a key being *held*, so chords in interactive terminal
mode are not possible — use `--keys` or the browser.

---

## The browser front end

```sh
make gui                   # or: python3 xeno_gui.py
```

Then open **<http://127.0.0.1:8731/>**. `--port N` moves it, `--no-open` leaves
the browser alone, and `--real-timing` / `--bus-off` / `--capture-251e` /
`--capture-259e` are passed straight through.

`xeno_gui.py` is **python3 stdlib only** — no pip, no npm, no CDN, works
offline — and it binds **127.0.0.1 only**, refusing any request that does not
arrive with a loopback `Host`. It takes the child down on every exit path, and
the child carries a `getppid()` guard so it cannot outlive the front end.

It contains **no simulation and no UI logic**. It spawns `build/xeno-sim
--stdio` — the same binary the terminal mode runs — and shuttles input tokens
in and the 128x64 framebuffer out. The page unpacks those bits onto a canvas
and draws nothing of its own: every glyph, every cursor, every menu decision is
made by real firmware, in C++, on the other end of a pipe. There is
deliberately no JavaScript reimplementation of any screen.

---

## The simulated bus

The user's actual bus:

```
0x20 -> "210"     answers QUERY; no bank, so a Read times out as NO_RESPONSE
0x28 -> "259 A"   answers QUERY; 30 x 33-byte records
0x5C -> "251 A"   answers QUERY; 30 x 2104-byte slots
```

Every other address is silent and costs the firmware's real 1000 ms QUERY reply
timeout, so a full 61-address scan honestly costs ~57 **simulated** seconds. By
default the virtual clock runs as fast as the loop will carry it, so that
returns immediately in wall-clock; `--real-timing` (or `t`) makes it real,
since scan pacing is itself a UX question.

The banks are **real bench captures**, parsed out of the console `PresetBus:
card image dump` hex format. Defaults point at the scratchpad files this was
built against; if they are gone the simulator falls back to synthetic banks and
stamps `[SYNTHETIC BANK DATA]` into the frame border.

Preset STORE and RECALL run the **real `PresetEngine`** against the RAM-backed
file system, so the pending/banner path, the "EMPTY SLOT n" refusal and the
slot-name editor all behave. What is not real is the broadcast: on the module a
save or recall masters a general-call frame the whole case obeys, and here only
the local half happens.

---

## Where the simulation is NOT faithful

Read this before trusting a green screen here for anything.

* **Nothing is written to any module.** A confirmed Save runs the real guard,
  the real diff and the real `MasterRestore` call, and the simulated module
  then stores the bank so the firmware's own read-back has something honest to
  compare against — but no hardware is touched. The frame border says so on
  every frame. The simulator can tell you the write path's *decisions* are
  right. It cannot tell you a 251e will accept the bytes.
* **Persistence is opt-in, and it is not LittleFS.** The file system is a
  `std::map` and the EEPROM an array, both of which die with the process unless
  `--state FILE` is given: that reads an image before boot and writes it back at
  exit, so two runs sharing one file are a power cycle. That is what makes
  "does this come back afterwards" askable at all — but none of LittleFS's real
  behaviour is modelled. No wear, no erase timing, and not the 0-byte-file
  failure `PresetEngine` guards against. A clean round-trip here tells you which
  BYTES the firmware chose to keep, and on which volume; it tells you nothing
  about whether the flash would have kept them.
* **Every CV input is a fixed, silent value, and there is no audio at all.**
  Any screen whose display follows CV is *still*: scopes do not move,
  quantizers do not step, CV-driven cursors do not wander. That is the shim,
  not the app.
* **Outputs go nowhere.** The DAC code runs and forms its bits; they land in a
  variable.
* **Transfer and reply pacing on the bus is invented.** The firmware's timeouts
  are real and linked; the fake modules' behaviour inside them (200 ms to first
  card touch, ~30 bytes/ms, 20 ms QUERY turnaround) is a plausible guess, not a
  measurement. Do not tune real timing constants against this.
* **The bus is never busy, never noisy, never wedged.** `tx_gate_open()` always
  returns 1, sends never NAK, there is no arbitration loss, no WPM contending
  for 0x50, no stray QUERY replies, no I2C errors at all. Every failure mode
  that starts with "the bus was contended" is unreachable. The bus statistics
  page reads zero for the same reason — every counter on it counts something
  that happens on a real wire.
* **No preemption, no CPU load, no memory pressure.** The core ISR and the UI
  poll run at the right *rates* and in the right *order*, but from one loop:
  nothing can overrun, be late, or interrupt anything. Free-RAM and
  stack-low-water figures are fixed numbers. Nothing here says anything about
  FLASHMEM/ITCM placement, DTCM pressure, or whether the firmware fits.
* **The display cannot tear or fall behind.** A page transfer always succeeds
  immediately, so every timing question about the display is unanswerable.
* **The MIDI ports are FIFOs, not devices.** Injecting a note proves the app
  polls that port and the parser consumes it. It proves nothing about USB host
  enumeration — the actual open question about the k-board — and no device is
  ever attached, so every device-identity display reads "none".
* **Identification is the same lookup the firmware does**, so the simulator
  inherits its caveat: a clone squatting an address is indistinguishable here
  too, by construction.
* **A `ConfirmReset` prompt is answered for you.** With no valid stored
  settings — which is every boot unless `--state` supplies some —
  `AppSwitcher::Init()` opens a blocking prompt. The simulator schedules a real button press to answer it
  (CANCEL by default; `--reset-settings` holds A+B through the splash, which is
  the module's own gesture, and then answers OK) and logs that it did. Nothing
  reaches inside the firmware to skip the screen.
* **`NO_HEMISPHERE`.** This build defines it, which is a configuration the
  firmware supports (`T41_MTP` ships it) but is not what the module runs. The
  visible cost is Quadrants and the applet system.
* **The terminal font is not the module's font.** Glyph shapes come from the
  real 6x8 table, but a half-block terminal cell is not an OLED pixel. The
  browser canvas is closer — one square pixel per pixel, at 4x — but it is
  still a backlit LCD pretending to be an OLED, and the module's screen is
  small and physically about 2:1. Do not judge legibility from either; judge it
  with `fbdiff.py` against a device capture, or on the module.
* **The GUI's timing is polled, not driven.** The page asks the simulator to
  advance the clock about eleven times a second and repaints on the reply, so
  the animation is the simulator's pacing seen through a 90 ms shutter. No
  screen refresh here is evidence about the module's refresh rate.

---

## The `--stdio` protocol

Only `xeno_gui.py` speaks it, but it is small enough to drive by hand:

```
$ ./build/xeno-sim --stdio
frame <2048 hex chars>          # 1024 bytes, page-packed, as the SH1106 got them
caption ...                     # the terminal frame's caption
status ...                      # the bus status line
millis / busy / held / overlay / screen / app / timing / synthetic
logtotal N
log ...                         # the last 120 lines
session ...                     # only in reply to `session`
END
```

Requests are one line each: `key <token>`, `btn <a|b|x|y|z|l|r> <down|up>`,
`release-all`, `enc <l|r> <signed delta>`, `trig <1-4>`, `pump <ms>`, `state`,
`session`, `bye`. Every request answers with one block in the shape above.

---

## Not part of the firmware build

No `platformio.ini` env references this directory and no file under
`software/src/` was modified for it. `pio run -e T41_console -e T41_audio -e T40`
is unaffected, `make` on its own produces a headless `build/xeno-sim`, and the
server is a separate python3 script that is never built, imported or shipped.
