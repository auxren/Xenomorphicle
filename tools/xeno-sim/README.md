# xeno-sim — host simulator for the "200e Modules" app

Runs `software/src/apps/Bus200eApp.h` on a Mac (or any host with a C++17
compiler) so its screens can be navigated and tested without the module.

It runs the **real app**, not a mock-up of it. `Bus200eApp.h` is compiled
unmodified; it draws through the **real** `weegfx::Graphics` renderer and the
real 6x8 font from the firmware tree; its bus work goes through the **real**
`Bus200eMaster` FSM with the firmware's own timeout constants; and the codecs,
generator, recorder, module table and write guard are the firmware's `.cpp`
files, linked. So is `PresetBusUI.cpp`, the 200e preset-bus overlay both
encoder pushes open. Only the hardware itself is faked.

## Build and run

```sh
cd tools/xeno-sim
make                       # -> build/xeno-sim
./build/xeno-sim           # interactive, in your own terminal
make gui                   # ...or the panel in a browser, see below
```

Scripted (no terminal needed — this is how screens get captured for review):

```sh
./build/xeno-sim --keys "r,r"                   # enter 0x5C, read its bank
./build/xeno-sim --keys "r,r,>,encr-" --no-log  # ...and browse to slot 10
./build/xeno-sim --keys "l" --dump-frames       # scan, printing every step
```

Options:

| flag | effect |
|---|---|
| `--keys "r,x,x,a"` | apply a key sequence, print the resulting screen, exit |
| `--dump-frames` | with `--keys`: print the screen after every token |
| `--real-timing` | interactive only: pin the virtual clock to wall-clock, so a scan takes the ~60s it takes on the module |
| `--bus-off` | make `PresetBus::Enabled()` false (the "preset bus disabled" screen) |
| `--capture-251e PATH` / `--capture-259e PATH` | point at different bank hex dumps |
| `--no-log` | omit the status/log lines under the frame |
| `--stdio` | line protocol on stdin/stdout, for the browser front end |

## The browser front end

```sh
cd tools/xeno-sim
make gui                   # or: python3 xeno_gui.py
```

Then open **<http://127.0.0.1:8731/>** — `make gui` opens it for you.
`--port N` moves it, `--no-open` leaves the browser alone, and
`--real-timing` / `--bus-off` / `--capture-251e` / `--capture-259e` are passed
straight through to the simulator.

`xeno_gui.py` is **python3 stdlib only** — no pip, no npm, no CDN, works
offline — and it binds **127.0.0.1 only**, refusing any request that does not
arrive with a loopback `Host`.

It contains **no simulation and no UI logic**. It spawns `build/xeno-sim
--stdio`, the same binary the terminal mode runs, and shuttles input tokens in
and the 128x64 framebuffer out. The page unpacks those bits onto a canvas and
draws nothing of its own: every glyph, every cursor, every menu decision is
made by the real `Bus200eApp.h` through the real `weegfx` renderer, in C++, on
the other end of a pipe. There is deliberately no JavaScript reimplementation
of any screen — one would drift from the firmware and test nothing.

The panel is drawn as it is: green **A** / **B** and blue **X** / **Y** in two
rows, the `device` (USB-C) and `host` (USB-A) jacks, the grey `clock` button
between them, the OLED in a bezel at 4x, and the two encoders flanking it. Click
a button to press it (it stays down while the mouse is down); click an encoder
to push it, wheel over it or drag around it to turn it, or use the `−` / `+`
beside it (Shift for ×10). The terminal build's keys all work in the page too,
and are listed in it.

### Chords

Several real gestures are **chords**, because the firmware dispatches on
`event.mask` — the set of buttons *held* when one of them changed — not on the
one that moved:

| chord | what it opens |
|---|---|
| both encoder pushes | the 200e preset-bus overlay (`OC_ui.cpp` → `PresetBusUI::Enter()`) |
| `A` (or `Z`) + right encoder push | the app menu |
| `A` + `B` | screen flip, inside the Setup app |

So the page models **held** state rather than firing one press per click, and
there are three ways to express a chord:

* **Keyboard** — `keydown` holds, `keyup` lets go, and auto-repeat is ignored.
  Physically hold `a` and tap `r`. This is the direct way.
* **Mouse** — a click cannot hold two things, so **right-click** (or
  shift-click) a button or a knob to **latch** it down; it stays visibly
  depressed and outlined until you click it again. Latch `encL`, then click
  `encR`.
* **Scripted** — `--keys` accepts `<button>-down` and `<button>-up`, which is
  also how a long press is scripted:

  ```sh
  ./build/xeno-sim --keys "l-down,r,l-up"           # -> the preset-bus overlay
  ./build/xeno-sim --keys "a-down,r,a-up"           # -> the app menu
  ./build/xeno-sim --keys "l-down,step600,l-up"     # a 500ms hold -> STORE
  ```

The panel shows a **held** line under the screen listing what is currently
down, so a forgotten latch is obvious rather than mysterious. What it lists is
the *simulator's* `ui.read_immediate()` view, not the page's guess, so a latch
out of step with the firmware would show up there. Tab-blur and page-unload
release everything.

The **terminal build cannot express a chord**: a TTY reports keystrokes, not
which keys are being held, and contorting the interactive UI to fake it (a
"sticky modifier" key, say) would model something the panel does not do. Use
`--keys` with `-down` / `-up`, or the browser, for those.

Two things the page is careful about:

* The **clock button is inert**, and says so on the panel. It fires no UI
  control on this hardware. It is drawn because it is there; it does nothing
  here because it does nothing there.
* The four simulator-only affordances — inject a MIDI note on the USB-host
  port, inject one on bus MIDI, advance a simulated second, toggle scan pacing
  — live in a **separate, visually distinct panel** marked *not on the panel*.
  Nothing on the instrument above is fictional.

The terminal frame's caption carries into the page as the orange border and
banner around the panel, live from the simulator, so `[SYNTHETIC BANK DATA]`
shows up there too. The bus status line and the event log sit beside the
screen, which is where most of the debugging value is: `BACKUP 5C: served 63120
bytes from capture` next to the pixels it produced.

Everything under **Where the simulation is NOT faithful** below applies to the
GUI exactly as it applies to the terminal. In particular **writes still go
nowhere**. The MIDI OUT jack, the audio jacks and the CV / trigger sections are
on the real panel and are deliberately *not* drawn, because nothing here models
them; the page says so. There is no power LED in the page because the firmware
drives none.

## Keys

The panel has **A, B, X, Y and both encoder pushes**. `Z` /
`CONTROL_BUTTON_M` is polled by the firmware on this hardware (`OC_gpio.cpp`
gives it a pin in the ORN8 profile) but is not one of the six positions this
panel draws, so it is reachable by key and by script only — which is enough to
exercise the `Z` chords through the same code path the module uses.

| key | control |
|---|---|
| `a` `b` `x` `y` | face buttons A / B / X / Y — **held** while the key is down |
| `z` | the Z button — off-panel, see above |
| `l` `r` | left / right encoder **push** (encL = back/cancel, encR = confirm/enter) |
| `[` `]` | left encoder turn, −1 / +1 |
| `,` `.` | right encoder turn, −1 / +1 |
| `{` `}` `<` `>` | the same, ×10 |
| `n` | note-on into `usbHostMIDI[0]` — the port the k-board lives on |
| `N` | note-on into the 200e bus MIDI RX ring |
| `w` | advance one simulated second |
| `t` | toggle fast / real scan pacing |
| `q` | quit |

In `--keys`, tokens are comma-separated, so the comma key needs its word
alias. Word aliases: `encl` `encr` (pushes), `encl-` `encl+` `encr-` `encr+`
(turns), `note` `busnote` `wait` `quit`. Two extra scripted-only tokens reach
states that only exist mid-job:

* `stepN` — advance exactly N simulated milliseconds (e.g. `step200`)
* `+key` — apply the key **without** settling the bus afterwards
* `<button>-down` / `<button>-up` — hold and release, for chords and long
  presses (see **Chords** above)

```sh
# press Read while a scan is walking -- the refusal path
./build/xeno-sim --keys "+l,step200,+r,step10,+r,step10"
```

## The simulated bus

The user's actual bus:

```
0x20 -> "210"     answers QUERY; no bank, so a Read times out as NO_RESPONSE
0x28 -> "259 A"   answers QUERY; 30 x 33-byte records
0x5C -> "251 A"   answers QUERY; 30 x 2104-byte slots
```

Every other address is silent and costs the firmware's real 1000 ms QUERY reply
timeout, so a full 61-address scan honestly costs ~57 **simulated** seconds. By
default the virtual clock is advanced as fast as the loop will carry it, so that
returns immediately in wall-clock; `--real-timing` (or `t`) makes it real, since
scan pacing is itself a UX question.

The banks are **real bench captures**, parsed out of the console
`PresetBus: card image dump` hex format (CRLF — the loader strips it; a parser
that does not silently returns nothing). Defaults point at the scratchpad files
this was built against. If they are gone, the simulator falls back to synthetic
banks and stamps `[SYNTHETIC BANK DATA]` into the frame border.

## What is real, what is faked

**Linked for real, unmodified, from `software/src/`:**

* `apps/Bus200eApp.h` — the whole app: every draw function, every event handler
* `src/drivers/weegfx.cpp` + `src/extern/gfx_font_6x8.h` — the renderer and font,
  so clipping, character cells and layout are pixel-identical to the module
* `Bus200eMaster.cpp`, `PresetBus200e.cpp` — the QUERY/BACKUP/RESTORE master FSM,
  its frame builders and all its timeout constants
* `Buchla251eSlotCodec`, `Buchla259eSlotCodec`, `Buchla251eGenerator`,
  `Buchla251eRecorder`, `Buchla200eModuleTable`, `Buchla200eWriteGuard`,
  `Buchla200eUiGate`, `bjorklund`
* `PresetBusUI.cpp` — the 200e preset-bus overlay: its layout, its 7-segment
  window, its STORE / RECALL hold timing, its rename grammar and its
  confirmation banners are the module's own code, not a drawing of them. It is
  compiled through a symlink in `shim/fw/` so that its quoted includes land on
  host stand-ins instead of the display driver and `util_math.h`'s ARM inline
  assembly — see `shim/fw/README.md` for exactly how, and for the discipline
  that keeps it from growing.
* `UI::Event`, `util_macros.h`, `util_stream_buffer.h`

**Shimmed (`shim/oc_shim.h`, `sim_bus.cpp`):**

* `OC::PresetBus` — the entire namespace. Bodies here, declarations from the
  real header.
* `millis()` / `elapsedMicros` — a virtual millisecond clock
* `usbMIDI`, `usbHostMIDI[2]`, `MIDI1` — independent fake FIFOs
* `OC::UiControl` values, `OC::AppEvent`, `OC::IOConfig`, `TWOCCS` — copied
  constants (the real headers drag in the whole build-configuration tree)
* the app base classes (`OC::AppBaseImpl`, `HSApplication`) and `gfxHeader()`
* `OC::Ui` (`sim_ui.cpp`) — a **deliberate mirror** of `OC_ui.cpp`'s `Poll()`
  and `DispatchEvents()`. The real ones cannot be linked (they reach the ADC,
  the app registry and ARM inline assembly), but the mask semantics *are* the
  thing the chords depend on, so the mirror follows them branch for branch and
  names the lines it is following. `Poll()` runs once per simulated
  millisecond, so `kLongPressTicks` (500) means 500 ms here too. **Keep the two
  in sync** — if `OC_ui.cpp`'s dispatch changes and this does not, the
  simulator will start lying about gestures.
* `OC::PresetEngine`, `OC::DigitalInputs`, `HS::PokePopup`, `OC::app_switcher`
  (`sim_preset.cpp`) — the environment the real `PresetBusUI.cpp` sits on. 30
  slots in RAM, four of them pre-stored with invented names; a save or recall
  completes after a plausible delay so the pending/banner path and the
  `EMPTY SLOT n` refusal both run for real.

`HSMIDI.h` is neutralised by pre-defining its include guard: `Bus200eApp.h`
reaches it by a path relative to itself, so no `-I` can shadow it, and it pulls
in `<MIDI.h>` and `<USBHost_t36.h>`.

## Where the simulation is NOT faithful

Read this before trusting a green screen here for anything.

* **Nothing is written to any module.** A confirmed Save runs the real guard,
  the real diff and the real `MasterRestore` call, and then the bytes are
  discarded. The frame border says so on every frame, and the log says
  `RESTORE 5C: SIMULATED ONLY -- 63120 bytes went nowhere`. The simulator can
  tell you the write path's *decisions* are right. It cannot tell you a 251e
  will accept the bytes.
* **Transfer and reply pacing is invented.** The firmware's timeouts are real
  and linked; the fake modules' behaviour inside them (200 ms to first card
  touch, ~30 bytes/ms, 20 ms QUERY turnaround) is a guess shaped to be
  plausible, not measured. Do not tune real timing constants against this.
* **The bus is never busy, never noisy, never wedged.** `tx_gate_open()` always
  returns 1, sends never NAK, there is no arbitration loss, no WPM contending
  for 0x50, no stray QUERY replies, no I2C errors at all. Every failure mode
  that starts with "the bus was contended" is unreachable here.
* **No ISR, no timing, no CPU load.** `Controller()` is called from the same
  loop as everything else, not at 16.6 kHz from a timer. The `rec_timeout_`
  bound on the MIDI drain is a stub that never expires, so the simulator cannot
  show you a drain that overruns its ISR budget. Nothing here says anything
  about FLASHMEM/ITCM placement, DTCM pressure, or whether the firmware fits.
* **The MIDI ports are FIFOs, not devices.** Injecting a note proves the app
  polls that port and the recorder consumes it. It proves nothing about
  USB host enumeration — the actual open question about the k-board.
* **Identification is the same lookup the firmware does**, so the simulator
  inherits its caveat: a clone squatting an address is indistinguishable here
  too, by construction.
* **No app switching, no storage, no screensaver, no IO frame.**
  `SaveAppData`/`RestoreAppData` are never exercised; outputs go nowhere. The
  `A`+`encR` and `A`+`encL` chords are dispatched by the real conditions, and
  the log says which branch fired, but there is no app registry to draw: the
  app menu, the IO settings screen and the screensaver get a **stand-in**
  screen that says `SIMULATOR STAND-IN` on the glass. That is the one place in
  this simulator where the pixels are not the firmware's, and it says so where
  you are looking.
* **The preset overlay's world is invented, even though the overlay is not.**
  Its 30 slots live in RAM and go nowhere; a STORE writes nothing; no WPM is
  present, so its `wpm` dot stays hollow; and the 225e last/next pulse jacks
  can be *assigned* but will never pulse, because the simulator models no
  trigger inputs. What the overlay itself decides — hold lengths, the banner
  wording, when a recall is refused — is the module's own code.
* **The terminal font is not the module's font.** Glyph shapes come from the
  real 6x8 table, but a half-block terminal cell is not an OLED pixel:
  contrast, persistence and the physical aspect ratio all differ. The browser
  canvas is closer — it is one square pixel per pixel, at 4x — but it is still
  a backlit LCD pretending to be an OLED, and the module's screen is small and
  physically about 2:1. Do not judge legibility from either.
* **The GUI's timing is polled, not driven.** The page asks the simulator to
  advance the clock about eleven times a second and repaints on the reply, so
  the animation is the simulator's pacing seen through a 90 ms shutter. The
  pacing rule itself is shared C++ with the terminal build, but no screen
  refresh here is evidence about the module's refresh rate.

## The `--stdio` protocol

Only `xeno_gui.py` speaks it, but it is small enough to drive by hand:

```
$ ./build/xeno-sim --stdio
frame <2048 hex chars>          # 1024 bytes, vertically packed, as the SH1106 gets them
caption ...                     # the terminal frame's caption
status ...                      # the terminal's bus status line
millis / busy / timing / synthetic
held a+r                        # what ui.read_immediate() reports as down
overlay 0                       # 1 while PresetBusUI owns the screen
logtotal N
log ...                         # the last 120 lines
END
```

Requests are one line each: `key <token>` (any interactive key or word alias —
a tap: down, one tick, up), `btn <a|b|x|y|z|l|r> <down|up>` (the half of a press
that makes a chord possible: the button stays held until an `up`),
`release-all`, `enc <l|r> <signed delta>` (arbitrary turns, which the key tokens
cannot express — this is what a drag or a wheel sends), `pump <ms>` (one refresh
of simulated time, using the interactive loop's pacing rule), `state`, `bye`.
Every request answers with one block in the shape above.

The child is taken down on every exit path the server has: `atexit`, a
`SIGTERM`/`SIGHUP` handler and an explicit `terminate` → `kill` in `stop()`. It
also guards itself — in `--stdio` it exits when `getppid()` changes, so it
cannot outlive the front end even if the front end dies badly. And an exception
in one request handler cannot take the server down: it becomes a 500 or a 503
with the traceback printed on the `xeno_gui.py` terminal, so the next crash
leaves evidence.

## Not part of the firmware build

No `platformio.ini` env references this directory and no file under
`software/src/` was modified for it — `shim/fw/PresetBusUI.cpp` is a **symlink**
to the firmware file, not a copy, so there is exactly one of it. `pio run -e T41_console -e T41_audio -e T40`
is unaffected, and the GUI adds nothing to it: `make` on its own still produces
a headless `build/xeno-sim`, and the server is a separate python3 script that
is never built, imported or shipped.
