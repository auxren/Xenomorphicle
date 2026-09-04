# Audio Apps Screens — four standalone full-screen effects

Freeverb, Samverb, Abyss and Delay, lifted out of the audio-applet host and
given the whole 128x64 panel as apps in their own right, reachable from the app
switcher's AUDIO folder (`OC_app_folders.h:95-100`).

**Baseline assumed:** Established-Rules **L-06** (inversion means "the right
encoder changes this", and nothing else), the release-first / entry-gesture-leak
rule, the footer-legend grammar `A:toggle  B:env  R:edit`, CAPS = active /
lowercase = inactive, refusals state fact + remedy in one line, and the screen
budget of 21 characters by ~6 usable rows. The global chords (`Z+encR`,
`Z+encL`, `Z+A`, hold-Z, both-encoder-push) are untouched; **no Z chord is
claimed by any of these four apps.**

The precedent this whole document is built on is Tweighty
(`software/src/apps/TweightyApp.h`), which is already a standalone full-screen
audio app and is the closest sibling in the tree. Where these four differ from
Tweighty, the difference is named and argued. Where they can be identical, they
are identical to the pixel.

---

## 1. The shared layout

All four apps draw the same three-zone screen. **The parameter row is Tweighty's
`SCR_EDIT` row, pixel for pixel** (`software/src/apps/TweightyApp.h:616-647`):
label at x=4, bar at x=34 w=54 h=8, value right-aligned at x=127 via
`print_right`. Only the row pitch changes, 9px -> 10px.

```
y=1..8    gfxHeader("F R E E V E R B")  + presence box at (118,1,8,8)
y=10      header rule (drawn by gfxHeader)
y=12      row 0   label x=4..33 | bar x=34..87 | value right-aligned to x=127 (>= x91)
y=22      row 1
y=32      row 2
y=42      row 3          (cursor band = invertRect(0, y-1, 128, 10))
y=54      footer rule (drawn by gfxFooter)
y=56      footer legend, <= 21 chars
```

**Four rows, never five.** The body is y=12..53 = 42px; 4 x 10px = 40px, so row
3's cursor band ends at y=50 and clears the footer rule at y=54 with 3px to
spare. This is a real improvement on Tweighty, whose 9px pitch makes the last
band span y=47..55 and therefore forces `gfxFooter()` to be called *first* or
the band erases it (see the comment at `TweightyApp.h:598-601`). Here the draw
order is free.

`gfxHeader()` prints the title at (1,1) and rules at y=10
(`software/src/HSUtils.cpp:909-919`). `gfxFooter()` clears y=54..63, rules at
y=54 and prints at (1,56) (`software/src/HSUtils.cpp:920-929`), which is why the
footer is exactly 21 characters: 1 + 21*6 = 127.

ASCII, exactly 21 columns (cols 0-4 label, cols 5-14 bar, cols 15-20 value):

```
123456789012345678901
F R E E V E R B     #
---------------------
Size:[####    ]   50%
Damp:[####    ]   50%
Cut :[#######_]  17.5k
Mix :[####    ]   50%
---------------------
A:byp  X:fine  R:cv
```

`#` in the header row = filled 8x8 box = the effect is ACTIVE. `o` = hollow box
= BYPASSED.

### 1.1 What is inverted, and why it is L-06-legal

Exactly one thing, ever: `invertRect(0, y-1, 128, 10)` on the cursor row.

On these screens **encL turn moves the cursor and encR turn changes that row's
value, always, with no edit mode.** So "the row encL points at" and "the value
encR will change" are the same object. The band is therefore literally *invert
exactly what the right encoder will change* — the rule stated in
`PresetBusUI.cpp:433` and adopted as L-06. Nothing else on the screen inverts,
on any page, in any app.

L-06 also prescribes a leading `>` for cursors. **It is deliberately omitted
here, and this is the argument:** `>` exists to distinguish an encL cursor from
a *different* encR target on screens where the two are separate objects. On a
two-encoder full-screen app they collapse into one object. A second mark for the
same thing is redundant decoration, and the design grammar's cardinal rule is
one mark, one meaning. If a future screen in this family ever gives encL and
encR different targets, the `>` comes back on that screen and only that screen.

**This kills the applet's `CursorToggle()` / `EditMode()` outright.** The applet
needed a mode because a quarter-screen applet has one encoder to do both jobs; a
standalone app has two encoders. Deleting the mode removes the *second*
inversion meaning these four applets carry today — see
`HemisphereApplet.cpp:153-171`, where a blinking underline means "cursor" and an
inverted field means "edit mode". One of those two meanings goes away for free
the moment the app owns both encoders.

### 1.2 Presence, state and refusal — all non-inverting

- **Bypass presence.** `drawRect(118,1,8,8)` when active, `drawFrame(118,1,8,8)`
  when bypassed. Same x, same 8x8 geometry and the same filled-means-live
  semantics as Tweighty's transport box (`TweightyApp.h:556-557` and
  `:677-678`), and the same banana-jack presence idiom the rig's design system
  uses everywhere. It clears the longest header name
  (`F R E E V E R B` = 15 chars, ending x=90) by 28px.
- **Bypass wording.** While bypassed the footer legend is replaced *entirely* by
  `BYPASSED - A:active` (19 chars). Fact plus remedy in one line — the
  `busy: scan (L stops)` shape the audit calls the best example in the tree. A
  hollow box alone would be an unworded glyph for a state, which the grammar
  forbids.
- **Out of RAM.** A *drawn box*, never an inverted band: `drawFrame(10,22,108,20)`
  then `setPrintPos(19,26); print("NO RAM: DRY ONLY")` — 16 chars, x=19..114,
  comfortably inside the frame's interior (x=11..116). Footer:
  `no RAM - reboot frees` (21 chars).
- **Live gate** (Delay's MODE page clock row only). `drawRect(34,y,8,8)` filled
  while the gate is high, `drawFrame(34,y,8,8)` otherwise, occupying the bar
  gutter. Explicitly **not** the shipped `gfxPrint(DigitalInputMap&)` behaviour
  — see finding H-2.

### 1.3 Bars

Two variants, both built from primitives verified present in
`software/src/src/drivers/weegfx.h`:

- `fxDrawBar(x, y, w, h, frac)` — `drawFrame(x,y,w,h)` (weegfx.h:68) plus
  `drawRect(x+1, y+1, round((w-2)*frac), h-2)` (weegfx.h:65). This generalises
  Tweighty's `DrawBar` (`TweightyApp.h:528-534`), which hard-codes h=8.
- `fxDrawBipolarBar(x, y, w, h, frac)` for signed parameters — same frame, plus
  `drawVLine(x + w/2, y+1, h-2)` (weegfx.h:71) as the zero datum, with the fill
  growing left or right from the centre column. For the standard 54-wide bar at
  x=34 the centre column is x=61 and each half is 26px of fill.

Primitives confirmed present before being specified: `invertRect` (weegfx.h:67),
`drawFrame` (:68), `drawRect` (:65), `drawHLine` (:70), `drawVLine` (:71),
`drawCircle` (:81), `setPrintPos` (:83), `print_right` (:107), `printf` (:117).
Nothing in this document invents a primitive.

### 1.4 Pages

`encR push` cycles pages; `encL push` returns to MAIN. The page count is
per-app, and **a page that would be empty does not exist**:

| App | Pages |
|---|---|
| Freeverb | MAIN -> CV |
| Samverb | MAIN -> CV |
| Abyss | MAIN -> TONE -> CV |
| Delay | MAIN -> MODE -> CV |

**Pages, not Abyss's current scrolling.** Abyss scrolls with
`scroll = row - (kVisibleRows - 1)` (`AbyssApplet.h:246-248`), which pins the
cursor to the bottom visible row: every row's y position moves as you turn the
encoder, and you can never see what is below the cursor. With fixed pages a
parameter sits at the same y forever, which is the only thing muscle memory can
hold on to.

There is **no page-name field on screen.** The rows identify the page
unambiguously (MAIN shows Mix/Size/Damp, TONE shows Mod/Rate, CV shows CV rows),
and the footer's `R:tone` / `R:cv` / `R:main` names the *destination of the
press* — which is exactly what the "turns are unlabeled, presses are labeled"
rule asks for. See §5 cut 2 for the full argument.

### 1.5 The CV page row

This page replaces both the inline CV columns in the applets' `View()` methods
and the `gfxDisplayInputMapEditor()` overlay.

```
cols   x         content
0-4    4..33     param label, 5 chars
6-9    40..57    CVInputMap::InputName(), 3 chars (CVInputMap.h:157-188): "C 1", "M12", " - "
10-14  64..87    fxDrawBipolarBar(64, y, 24, 8, InRescaled(12)/12.0f) — live incoming CV
15-20  ..127     attenuversion, signed whole percent, "%d%%", <= 5 chars: "-448%"
```

Gaps: source name ends x=57, bar starts x=64 (6px); bar ends x=87, value starts
at x>=97 for a 5-char field (9px). Both comfortable.

```
123456789012345678901
F R E E V E R B     #
---------------------
Size  C 1  [ #  ]  100%
Damp   -   [    ]    0%
Cut   M12  [  ##]  -50%
Mix   C 3  [ ###]   75%
---------------------
A:byp  X:fine  R:main
```

encL turn = which row. encR turn = **attenuversion**. `Y` press = cycle that
row's source (`CVInputMap::ChangeSource`). Putting attenuversion on encR keeps
encR's meaning ("the number on this row") identical on every page of every app
in the family.

Note that the live bar is `InRescaled(12)` scaled into 24px of width, **not**
the shipped `gfxPrint(CVInputMap&)` (`HemisphereApplet.cpp:239-246`), which
draws a vertical line up to `InRescaled(24)` = 24 pixels tall upward from the
row baseline. At a 10px row pitch that line bleeds through the two rows above
it. See finding H-1.

---

## 2. Per-app screens

All parameter names, ranges and defaults below are cited from source. Bypass and
page-cycling are the only things on these screens that do not already exist in
the applets.

### 2.1 Freeverb — `software/src/audio_applets/FreeverbApplet.h`

MAIN (all four parameters fit; there is no TONE page):

| Row | Param | Range | Default | Cite |
|---|---|---|---|---|
| 0 | `Size:` | 0..100 % | 50 | `:129-131`, `:181` |
| 1 | `Damp:` | 1..100 % | 50 | `:138-140`, `:182` |
| 2 | `Cut :` | 0..17500 Hz, step 50 | 15000 | `:141-143`, `:183` |
| 3 | `Mix :` | 0..100 % | 50 | `:147-149`, `:180` |

Row order is the applet's own cursor order (`:162-171`: SIZE, DAMP, CUTOFF,
MIX), so nobody has to relearn anything. Bars: `size/100`, `damp/100`,
`cutoff/17500`, `mix/100`.

CV page: `size_cv`, `damp_cv`, `cutoff_cv`, `mix_cv` (`:185-188`), in the same
row order.

Cut renders `%d.%dk` above 1000 Hz (`17.5k`) and `%dHz` below (`250Hz`). The
applet's `%5dHz` (`:78`) is 7 characters and overruns the 6-character value
field at full-screen geometry — see finding M-6.

```
123456789012345678901
F R E E V E R B     #
---------------------
Size:[####    ]   50%
Damp:[####    ]   50%
Cut :[#######_]  17.5k
Mix :[####    ]   50%
---------------------
A:byp  X:fine  R:cv
```

### 2.2 Samverb — `software/src/audio_applets/SamverbApplet.h`

**Naming, unresolved.** The file is `SamverbApplet.h`, the class is
`BungverbApplet` (`:13`), `applet_name()` returns `"Bungverb"` (`:16`), and the
allocator is `GetBungverb()` (`:20`). Three names for one effect. The header
string here is spec'd as `S A M V E R B` (13 chars, x=1..78) per the name used
in the brief, but the implementer has to pick one and make the other two agree.
See finding M-5.

MAIN:

| Row | Param | Range | Default | Cite |
|---|---|---|---|---|
| 0 | `Time:` | 0..20 s, step 0.1 (`decay_time`) | 1.0 | `:132-134`, `:184` |
| 1 | `Damp:` | 1..99 % | 50 | `:138-140`, `:185` |
| 2 | `Cut :` | 0..17500 Hz, step 50 | 15000 | `:144-146`, `:186` |
| 3 | `Mix :` | 0..100 % | 50 | `:150-152`, `:183` |

Bars: `decay_time/20`, `damp/100`, `cutoff/17500`, `mix/100`. Time renders
`%d.%ds` from `SPLIT_INT_DEC(decay_time, 10)` (`:61`), max `20.0s` (5 chars).

CV page: `decay_time_cv`, `damp_cv`, `cutoff_cv`, `mix_cv` (`:188-191`).

```
123456789012345678901
S A M V E R B       #
---------------------
Time:[#       ]   1.0s
Damp:[####    ]   50%
Cut :[#######_]  17.5k
Mix :[####    ]   50%
---------------------
A:byp  X:fine  R:cv
```

### 2.3 Abyss — `software/src/audio_applets/AbyssApplet.h`

MAIN — the four the Blackhole puts on its own front panel:

| Row | Param | Range | Default | Cite |
|---|---|---|---|---|
| 0 | `Mix :` / `Snd :` | 0..100 % (`wet`) | 50 | `:284-286`, `:215`; label flip `:147` |
| 1 | `Grv :` | -100..+100, **bipolar bar** | 35 | `:290-292`, `:216` |
| 2 | `Size:` | 0..100 % | 70 | `:296-298`, `:217` |
| 3 | `Pre :` | 0..500 ms (`predelay_2ms` 0..250, x2) | 0 | `:302-304`, `:218`; render `:176` |

TONE:

| Row | Param | Range | Default | Cite |
|---|---|---|---|---|
| 0 | `Mod :` | 0..100 % (`mod_depth`) | 25 | `:305-307`, `:219` |
| 1 | `Rate:` | 0.05..3.00 Hz (`mod_rate` 1..60) | 10 | `:311-313`, `:220`; render `:190-192` |
| 2 | `LoCt:` | 0..10 (`locut`) | 2 | `:314-316`, `:221` |
| 3 | `Damp:` | 0..10 (`hidamp`) | 3 | `:317-319`, `:222` |

Bars: `wet/100`, bipolar `gravity/100`, `size/100`, `predelay_2ms/250`,
`mod_depth/100`, `(mod_rate-1)/59`, `locut/10`, `hidamp/10`.

CV page: `mix_cv`, `grav_cv`, `size_cv`, `mod_cv` (`:225-228`) — labelled Mix,
Grv, Size, Mod.

`send_mode` (`:222`) is on B and flips the row-0 label `Mix:` -> `Snd:`, which
is the already-shipped idiom at `:147`.

```
123456789012345678901
A B Y S S           #
---------------------
Mix :[####    ]   50%
Grv :[  #|##  ]    35
Size:[######  ]   70%
Pre :[        ]   0ms
---------------------
A:byp  B:snd  R:tone
```

TONE page:

```
123456789012345678901
A B Y S S           #
---------------------
Mod :[##      ]   25%
Rate:[###     ]0.50Hz
LoCt:[##      ]     2
Damp:[##      ]     3
---------------------
A:byp  B:snd  R:cv
```

### 2.4 Delay — `software/src/audio_applets/DelayApplet.h`

**MAIN is Tweighty's `SCR_EDIT`, field for field.** Tweighty's four edit fields
are Time / Taps / Fdbk / Mix (`TweightyApp.h:612-614`). Delay's MAIN is Time /
Taps / Fdbk / Wet, at the same four y positions, with the same bar geometry.
That is the single strongest family decision in this document: open Tweighty,
open Delay, and the same four things are in the same four places.

MAIN:

| Row | Param | Range | Default | Cite |
|---|---|---|---|---|
| 0 | `Time:` | SECS: `MIN_DELAY_SECS*1000` .. `MAX_DELAY_SECS*1000-1` ms (~12s with PSRAM, ~0.37s without) | 500 | `:300-307`, `:437`, `:399-402` |
| 0 | `Time:` | CLOCK: `ratio` -127..+127, shown `x N` / `/ N`, **bipolar bar** with the datum at ratio=0 | 0 | `:285-287`, `:439`; render `:196-202` |
| 0 | `Time:` | HZ: pitch-derived, `%d.%01d` | | `:288-299`; render `:178-180` |
| 1 | `Taps:` | 1..8 | 1 | `:334-336`, `:387-396`, `:447` |
| 2 | `Fdbk:` | 0..100 % mono, -100..100 % stereo (**bipolar bar** when stereo) | 0 | `:322-324`, `:443` |
| 3 | `Wet :` / `Snd :` | 0..100 % | 50 | `:328-330`, `:445`; label flip `:228` |

Taps bar is `(taps-1)/7`, the same formula Tweighty uses at
`TweightyApp.h:630`. Time renders `%d.%02ds` from milliseconds (`11.99s`, 6
chars), matching Tweighty's `%d.%02ds` at `TweightyApp.h:626`.

MODE (3 rows):

| Row | Param | Values | Cite |
|---|---|---|---|
| 0 | `Unit:` | `ms` / `clk` / `Hz` | `:313-315`; enum `:421-426` |
| 1 | `Clk :` | `DigitalInputMap` source + div/mult, e.g. `CL1 /4`; live gate presence box at x=34 | `:310-312`, `:440`, `:55`; `CVInputMap.h:408-411` |
| 2 | `Mod :` | `Xfade` / `Strch` | `:319-321`; enum `:428-431`; default `:448` |

The MODE page **deletes the hidden-row problem**. The clock source becomes an
always-visible, always-editable row rather than a cursor stop that appears and
disappears with `time_units` — which is what forces the double-`MoveCursor` plus
`++cursor` hack at `:269-274`, whose own comment reads "smh my head". A row
whose value is inert until you switch units is honest; a row that vanishes and
teleports the cursor is not. See finding L-2.

CV page: `delay_time_cv`, `feedback_cv`, `wet_cv` (`:438`, `:444`, `:446`) —
three rows, labelled Time, Fdbk, Wet.

```
123456789012345678901
D E L A Y           #
---------------------
Time:[###     ]  0.50s
Taps:[        ]     1
Fdbk:[        ]    0%
Wet :[####    ]   50%
---------------------
A:byp  B:snd  R:mode
```

MODE page (note the filled gate box in the bar gutter of the Clk row):

```
123456789012345678901
D E L A Y           #
---------------------
Unit:              ms
Clk :#          CL1 /4
Mod :           Xfade
---------------------
A:byp  B:snd  R:cv
```

---

## 3. The control map

| Control | Action | Same as Tweighty? |
|---|---|---|
| **encL turn** | move the parameter cursor | **Same** — `TweightyApp.h:497-500` |
| **encL push** | back to MAIN page | **Same** — `:487-488` (`screen_ = SCR_HOME`) |
| **encR turn** | adjust the cursor row's value, direct-commit, no confirm gate | **Same** — `:501-502` |
| **encR push** | next page, cycling | **Same** — `:483-485` (HOME -> EDIT -> MIXER -> HOME) |
| **A press** | BYPASS toggle (all four apps) | **Differs.** Tweighty's A is WRITE<->RECIRC. Same *kind*: the one two-state performance toggle, live from every page, footer-labeled, worded on screen. |
| **B press** | SEND mode toggle — Abyss and Delay only; unbound in Freeverb/Samverb | **Differs.** Tweighty's B is envelope-out. Same kind: the secondary routing state, with a visible label flip `Mix:` -> `Snd:`. |
| **X held** | fine adjust — encR steps at 1/10 the coarse rate (all four) | **Same gesture kind** — `:513` reads X live via `OC::ui.read_immediate(OC::CONTROL_BUTTON_X)`. Tweighty's X redirects encR to a different value; ours rescales its step. |
| **Y press** | CV page only: cycle that row's source (`ChangeSource`) | New. Y is free in-app per the binding matrix. |
| **Z, and every Z chord** | untouched | — |

### 3.1 Justification against the binding matrix

Checked against `docs/Panel-Binding-Matrix.md`:

- **No collision.** These are new apps; nothing in the matrix's "Distinct-shape
  apps" table is displaced or re-bound.
- **The Bus200e carve-out is not touched.** A=BYPASS is not the arm half of a
  two-step confirm and shares no chord with encR, so the hardened arm(A)/
  confirm(encR) split (matrix lines 138-140) is unaffected. There is no
  destructive action anywhere in these four apps, so no arm/commit split is
  needed at all — every edit is instantly reversible, which is the same
  reasoning Tweighty writes down at `TweightyApp.h:31-32`.
- **X and Y are the free reservoir** (matrix lines 133-135). X-as-a-held-
  modifier is precedented twice: Tweighty (`:513`) and SETTINGS (X held while
  turning encR, matrix line 97).
- **Quadrants cannot collide.** Its `CheckButtonCombo()` is an exact mask match,
  and in any case it is a different app.

### 3.2 Why bypass earns a button, and Mix=0 does not replace it

Mix is a value you would have to restore by hand and by eye. A gives an instant,
lossless A/B comparison and returns you to the exact number you left. It is the
one affordance a full-screen effect owes a player that a quarter-screen applet
sharing one encoder cannot give.

### 3.3 Why send mode moves from `AuxButton()` to a labeled B

Today `AuxButton()` flips `send_mode` **only if the cursor happens to be on
MIX/WET** (`AbyssApplet.h:64-67`, `DelayApplet.h:249-252`). On any other row the
press does nothing, silently. That is the "unlabeled B with 4 pixels of
feedback" hazard in miniature. Unconditional, footer-labeled, with a visible
label flip, fixes it. See finding M-3 — and note that `AuxButton()` is *shared*
code, so this is not a change confined to these four applets.

### 3.4 Why X-held-fine is on the chord card and not the footer

The four footers are already 18-20 characters. Adding `X:fine` overruns 21 in
both Abyss (`A:byp B:snd X:fine R:tone` = 25) and Delay. The hold-Z chord card's
`tip` slot (`OC_app_base.cpp:137-141`, filled at `:419-421`) exists precisely
for "one control, the least guessable one", and it fires for any app that binds
neither Z nor A+B — which is all four of these. The gloss row to add:

```
{ TWOCCS("??"), nullptr, nullptr, "Hold X: fine adjust" }
```

(19 chars). This also removes any need for the `knob_accel` acceleration hack —
see finding L-3, and note that the audit already found the ~150 detents/sec
acceleration threshold unreachable by hand.

### 3.5 The step sizes X modifies

Coarse steps are chosen so a full sweep is 100-200 detents; X-held divides by
10. This matters most for Delay's time (12 seconds at 1 ms/detent is 12000
detents today, `:301`) and for the reverbs' cutoff (50 Hz/detent over 17500 Hz,
`FreeverbApplet.h:142`, `SamverbApplet.h:145`).

---

## 4. Metering

**Nothing on MAIN or TONE moves.** That is a decision, not an omission: on a
still screen, anything that moves is something you changed.

**What moves:** the CV page's per-row live bars — four `InRescaled(12)` calls
(`CVInputMap.h:109-111`, one `Proportion()` each) plus four
`drawFrame`/`drawRect`/`drawVLine` per frame. Negligible against the 60us ISR
budget, and proportional to a constant 4, never to anything large.

**Delay's MODE page** adds one `map.Gate()` read and one 8x8
`drawRect`/`drawFrame` for the clock presence box.

**No output level meter, in any of the four.** Verified by inspection:
`AudioEffectFreeverbF32` (`software/src/Audio/effect_freeverb_F32.h`),
`AudioEffectReverbSchroederF32` (`.../effect_reverb_schroeder_F32.h`),
`AudioEffectAbyssReverb` (`.../effect_abyss.h`) and `AudioDelayExtF32`
(`.../AudioDelayExtF32.h`) expose **no** `meter`, `peak` or `rms` field of any
kind. Tweighty's meter exists only because `AudioTweightyF32` publishes
`meter_level_` as an audio-ISR-hot mirror
(`TweightyApp.h:144`, `:610`, `:684`). Adding four peak trackers to four DSP
classes so that 32 pixels can wobble is not worth it in an instrument whose
output is audible in the room.

---

## 5. What I would cut

A full screen tempts a designer to fill it. Each cut below carries its argument,
because **a cut without its argument gets silently reinstated by the next
person.**

1. **The output level meter.** Four new DSP-side peak trackers, in four separate
   engine classes, to animate 32 pixels of a thing you can already hear. None of
   the four engines exposes a meter field today (§4). Cut.
2. **A page-name field on screen.** The rows name the page: MAIN shows
   Mix/Size/Damp, TONE shows Mod/Rate, CV shows CV source rows. The footer names
   the *destination* of the encR press (`R:tone`, `R:cv`, `R:main`), which is
   what the press-legend rule requires. A page name would be a third statement
   of a fact already made twice, and the header's right edge — the only place it
   could go — is occupied by the bypass presence box. Cut.
3. **A dedicated bypass banner.** The header presence box plus the wholesale
   replacement of the footer legend with `BYPASSED - A:active` carry it at zero
   pixel cost and without covering the parameter rows, so you can still dial
   while bypassed. Cut.
4. **`CursorToggle()` / `EditMode()` entirely.** Dead weight given two encoders,
   and the source of the applets' second inversion meaning
   (`HemisphereApplet.cpp:153-171`). Cut.
5. **Abyss's scrolling list** (`AbyssApplet.h:246-252`). It pins the cursor to
   the bottom visible row, so every row's y moves as you turn and you can never
   see what is below you. Replaced by fixed pages. Cut.
6. **`gfxDisplayInputMapEditor()`** (`HemisphereApplet.h:344-370`). The CV page
   shows source and attenuversion inline, on the row they belong to. The
   overlay's `gfxInvert(0,0,63,11)` is an inverted banner and an L-06 violation
   in its own right (finding H-3). Cut from these four screens.
7. **A per-tap mixer for Delay.** Tweighty has one because its engine exposes
   per-tap gains (`SetTapMix`/`SetTapFeedback`, `TweightyApp.h:139-140`).
   Delay's taps share one normalised gain (`set_taps`, `DelayApplet.h:387-396`).
   Do not invent DSP to fill a page. Cut.
8. **Numeric CV voltage readouts.** The bar answers the only question ("is CV
   arriving, and how much"). The module's only voltmeter — Calibr8or's — has
   unverified sign and scale (UI-Redesign-Constraints §7), so a number here
   would be a precise-looking claim nobody has checked. Cut.
9. **Any tail, impulse-response or waveform visualisation.** Decorative; it
   occupies space a measurement wants, and on a 1-bit 128x64 panel it would
   convey less than the number beside it. Cut.
10. **A freeze control on Delay.** `frozen` is fully implemented in
    `Controller()` (`DelayApplet.h:114-125`) and completely unreachable: its
    only writer is commented out at `:71` and its cursor entry at `:413`.
    Reviving dead DSP is out of scope for a UI spec — and more importantly,
    **Tweighty *is* the freeze/recirculate app**; that is its entire thesis
    (`TweightyApp.h:1-21`). A second, weaker one dilutes both. Cut from v1, and
    flagged as finding L-1 so it is either wired up deliberately later or
    deleted.
11. **A fifth parameter row.** 42px of body at a 10px pitch gives four. An 8px
    pitch would give five with zero leading and a cursor band that touches its
    neighbours' ink on a 1-bit panel. Four rows also happens to be exactly
    Tweighty's edit-field count, which is the family argument. Cut.

---

## 6. Findings, ranked

Severity follows the design-QA convention used elsewhere in this tree:
spatial-mapping errors and inversion-grammar violations are High; wording and
consistency issues are Medium; polish is Low.

**Three of these are defects in SHARED code, not in the four applets.**
H-2, H-3 and M-3 live in `HemisphereApplet.cpp` / `HemisphereApplet.h` and are
inherited by **every Hemisphere and Quadrants applet in the build**, not just
these four. Their blast radius and their fix priority are therefore different in
kind from a bug inside one reverb: fixing them changes screens nobody in this
review looked at, and *not* fixing them means the four new apps either inherit
the violation or diverge from every other applet. Neither is free. They are
called out inline below and summarised again in §6.4.

### 6.1 High

**H-1 — `gfxPrint(CVInputMap&)` bleeds 24px into the rows above.**
*Scope: shared code, but only reached by applets that print a CV map inline —
which is all four of these.*
`HemisphereApplet.cpp:239-246`:

```cpp
FLASHMEM void HemisphereApplet::gfxPrint(CVInputMap &map) const {
  gfxPrintIcon(map.Icon());
  const int xpos = gfxGetPrintPosX() - 1;
  const int ypos = gfxGetPrintPosY() + 4;
  const int height = map.InRescaled(24);
  gfxLine(xpos, ypos, xpos, ypos - height);
}
```

`InRescaled(24)` returns up to 24 (and negative for negative CV), and the line
is drawn *upward* from the row baseline. That is tolerable in a sparse
quarter-screen applet and a three-row bleed at a 10px full-screen pitch. This
spec replaces it on the CV page with
`fxDrawBipolarBar(64, y, 24, 8, InRescaled(12)/12.0f)`, which is bounded to the
row.

**H-2 — inversion-grammar violation in SHARED code: `gfxPrint(DigitalInputMap&)`.**
*Scope: shared. Every applet that prints a digital input map inherits this.*
`HemisphereApplet.cpp:235-238`:

```cpp
FLASHMEM void HemisphereApplet::gfxPrint(DigitalInputMap &map) const {
  gfxPrintIcon(map.Icon());
  if (map.Gate()) gfxInvert(gfxGetPrintPosX()-8, gfxGetPrintPosY(), 8, 8);
}
```

Inversion here means "the gate is high" — a **seventh** meaning of inversion
across the instrument, on top of the six the Panel Audit already counted. Fix:
`drawRect(x, y, 8, 8)` when the gate is high, `drawFrame(x, y, 8, 8)` when it is
not — the filled/hollow presence idiom, identical in shape to Tweighty's
transport box and to this spec's bypass box. This is a two-line fix with a
build-wide blast radius; it should be scheduled as part of the L-06 unification
work, not as a side effect of shipping four reverbs.

**H-3 — inversion-grammar violation in SHARED code: `gfxDisplayInputMapEditor()`.**
*Scope: shared. Every applet with a CV or digital input map raises this overlay.*
`HemisphereApplet.h:344-370` ends with `gfxInvert(0, 0, 63, 11)` — an inverted
**banner**. L-06 is explicit that banners get a drawn box. Fix:
`drawFrame(0, 0, 63, 11)` and leave the polarity alone. These four screens
sidestep it by cutting the overlay entirely (§5 cut 6), but every other applet
in the build still raises it.

**H-4 — Abyss mutes the channel when it is out of RAM.**
*Scope: `AbyssApplet.h` only.*
`AbyssApplet.h:31-59` runs the full `Controller()` regardless of `alloc_ok`,
including:

```cpp
wetdry[ch].gain(WD_WET_CH, wet_gain);
wetdry[ch].gain(WD_DRY_CH, dry_gain);
```

At Mix=100% with no arena, the reverb outputs nothing and the dry path is faded
out — **silence**, with only a small "Out of RAM!!" on screen to explain it.
Freeverb (`FreeverbApplet.h:39-41`) and Samverb (`SamverbApplet.h:39-41`) both
explicitly force `dry_wet_mixer.gain(1, 1.0f)` and early-return in exactly this
case. Abyss should do the same, so the failure mode is "no effect" rather than
"no signal".

### 6.2 Medium

**M-1 — ~~Samverb's Damp label is inverted relative to Freeverb's.~~ WITHDRAWN.**

> **This finding is wrong, and acting on it would have broken Samverb.** It is
> left here rather than deleted so nobody rediscovers it. Both applets are
> correct; "Damp: 90%" already means dark in both.

The original finding read: *`SamverbApplet.h:38` passes `1.0f - ((damp * 0.01f)
+ damp_cv.InF())` to `setDamping()`, whose contract is documented as "0..1 where
1 = strong high-frequency damping"; `FreeverbApplet.h:37` passes `(damp * 0.01f)
+ damp_cv.InF()` to `damping()`, which has the same contract. So "Damp: 90%"
means dark in Freeverb and bright in Samverb. The ranges disagree too: 1..100
versus 1..99. Decide which way "Damp" points and make both agree.*

**Why it is wrong.** The two engines do NOT have the same contract. They use the
same variable name for opposite operands:

- `AudioEffectReverbSchroederF32` (`effect_reverb_schroeder_F32.h`, comb loop):
  `combStore = combStore*damp2 + y*damp1` with `damp1 = d`. The coefficient
  multiplies the **input**, so as a one-pole `y[n] = a·x[n] + (1-a)·y[n-1]` the
  new-sample weight **is** `d`. Higher `d` = less smoothing = **brighter**.
  `setDamping()` actually means *brightness*.
- `AudioEffectFreeverbF32` (`effect_freeverb_F32.h`, comb loop):
  `combfilter = bufout*damp2 + combfilter*damp1` with `damp1 = n·0.4`. The
  coefficient multiplies the **state**, so the new-sample weight is
  `damp2 = 1-0.4n`. Higher `n` = more smoothing = **darker**. `damping()` means
  *damping*.

So a row labelled "Damp" must pass its value **straight through to Freeverb and
inverted to Schroeder** — which is exactly what the two applets already do. Each
is compensating correctly for its own engine.

**What the finding actually caught** was a lying comment. The "documented
contract" it quoted (`0..1 where 1 = strong high-frequency damping`) described
the opposite of what the code did. That comment was the whole basis of the
finding, and it is fixed in `ff473435`, along with the Freeverb contrast written
down at the point of confusion.

**The range half is wrong too, and for a better reason.** 1..99 versus 1..100 is
not sloppiness: Samverb's 99 ceiling is **load-bearing**. `damp = 100` gives
`d = 0`, and at `d = 0` the comb loop degenerates to `combStore = combStore*1.0
+ y*0.0` — the damping state stops tracking its input and holds its last value
forever, injecting a constant into every comb's feedback path. Freeverb's 100 is
safe because its coefficient maxes at 0.4 and never approaches a degenerate
value. Two different numbers for two different engines, both correct.

That degenerate point *was* reachable, though not by the knob: `damp_cv` is
summed in after the knob and routes around the 99 cap. Guarded in `72429502` with
a symmetric low clamp in the engine.

**Method note, since this is the second time it has happened in this document's
subject area.** The finding trusted a prose comment over the arithmetic beneath
it. So did the claim that Delay falls back to the RAM2 heap without PSRAM
(`DelayApplet.h`), which `e789b9cf` corrected — `ExtAudioBuffer::Acquire()` only
ever calls `extmem_calloc`. **In this tree, when a comment and an implementation
disagree about DSP or allocation, assume the comment is wrong until the code says
otherwise.**

**M-2 — Delay's tap-time clamp is on the wrong variable and in the wrong place.**
`DelayApplet.h:94-105`:

```cpp
for (int tap = 0; tap < taps; tap++) {
  float t = d * static_cast<float>(taps - tap) / taps;
  CONSTRAIN(d, 0.0f, MAX_DELAY_SECS);
```

`d` is clamped *after* `t` has already been derived from it, so tap 0 always
receives an unclamped delay time; and the clamp is applied to `d` rather than to
the `t` that is actually passed to `cf_delay()` / `delay()`. Move it above the
loop, or clamp `t` after computing it.

**M-3 — `AuxButton()` is a silent no-op on most rows.**
*Scope: shared dispatch, applet-local behaviour.* The dispatch is shared
(`AudioAppletSubapp.h:188-196` routes X/Y to `AuxButton()`; the base is
`HemisphereApplet.h:118`), and the cursor-conditional guard is repeated in each
applet: `AbyssApplet.h:64-67` and `DelayApplet.h:249-252` flip `send_mode` only
when `cursor == MIX` / `cursor == WET`. On any other row the button does nothing
and says nothing. Because the pattern is in the shared base's contract, any
applet copying it inherits the same dead press. This spec's unconditional,
footer-labeled B fixes it for the four apps; the shared pattern is worth a
separate look.

**M-4 — Samverb accumulates a float `0.1` and allows a decay time of zero.**
`SamverbApplet.h:133`:

```cpp
decay_time = constrain(decay_time + (direction * 0.1), 0, 20);
```

Binary-inexact accumulation walks the value off the tenths grid the display
assumes (`SPLIT_INT_DEC(decay_time, 10)`, `:61`), and the floor of 0 is a
zero-length reverb. Store centiseconds in an integer, the way Tweighty stores
`time_centis_` (`TweightyApp.h:233`, `:445-450`).

**M-5 — Samverb has three names.** File `SamverbApplet.h`, class
`BungverbApplet` (`:13`), `applet_name()` returning `"Bungverb"` (`:16`),
allocator `GetBungverb()` (`:20`). Pick one before the header string is written,
or the app switcher, the header and the source will disagree.

**M-6 — Freeverb's and Samverb's cutoff field overruns a 6-char value slot.**
`%5dHz` (`FreeverbApplet.h:78`, `SamverbApplet.h:79`) is 7 characters. That fits
in a 64px-wide applet; at full-screen geometry the value field starts at x>=91
and 7 characters need x>=85, which collides with the bar's right edge at x=87.
Hence the `%d.%dk` / `%dHz` reformat specified in §2.1.

**M-7 — inconsistent, unhelpful refusal wording.** `"Out Of RAM !!!"`
(`FreeverbApplet.h:56`, `SamverbApplet.h:56`) versus `"Out of RAM!!"`
(`AbyssApplet.h:242`). Neither states a consequence or a remedy, and they
disagree on capitalisation and punctuation. Spec: `NO RAM: DRY ONLY` in a drawn
box, plus `no RAM - reboot frees` in the footer.

### 6.3 Low

**L-1 — Delay's `frozen` is unreachable dead code.** Fully implemented in
`Controller()` (`DelayApplet.h:114-125`); its only writer is commented out at
`:71` (`// if (Clock(1)) frozen = !frozen;`) and its cursor entry at `:413`.
Either wire it to Y in a later pass or delete it. Leaving it invites someone to
"fix" it into a second, weaker Tweighty (§5 cut 10).

**L-2 — Delay's hidden-clock-row cursor hack.** `DelayApplet.h:269-274` calls
`MoveCursor` twice and then does `++cursor` to skip a row that only exists in
CLOCK units, with the author's own comment "smh my head". Whatever it does, it
produces an unpredictable cursor jump. The MODE page removes the need for it
(§2.4).

**L-3 — Abyss's `knob_accel` is computed for every parameter and consumed by
one.** `AbyssApplet.h:279-281` maintains it on every turn; only `PREDELAY`
(`:303`) reads it. `DelayApplet.h:278-280` has the same structure. The X-held
fine modifier replaces the whole mechanism, and the audit has already found the
~150 detents/sec acceleration threshold unreachable by hand.

**L-4 — the cursor band inverts the bar it crosses.** `invertRect(0,y-1,128,10)`
turns the selected row's bar fill inside out, which can be misread as "the bar
emptied". This is Tweighty's shipped behaviour (`TweightyApp.h:646`) and this
spec keeps it for consistency. If it tests badly on hardware, the fix is
`invertRect(0, y-1, 34, 10)` — the label gutter only — **not** two rects with a
hole in the middle, which reads as a rendering bug at native pixel scale on a
1-bit panel.

### 6.4 Shared-code summary

| Finding | File | Blast radius | Fix |
|---|---|---|---|
| H-2 | `HemisphereApplet.cpp:235-238` | every applet printing a `DigitalInputMap` | filled/hollow 8x8 box instead of `gfxInvert` |
| H-3 | `HemisphereApplet.h:344-370` | every applet raising the input-map overlay | `drawFrame(0,0,63,11)` instead of `gfxInvert(0,0,63,11)` |
| M-3 | `HemisphereApplet.h:118`, `AudioAppletSubapp.h:188-196`, plus each applet's override | every applet overriding `AuxButton()` with a cursor guard | make the aux action unconditional, or give the base a documented "no-op is visible" contract |

H-1 is shared code too (`HemisphereApplet.cpp:239-246`) but its damage is
geometry-dependent: it is only a bleed where the row pitch is tight, which is
why it is scoped here to the full-screen apps rather than raised as a build-wide
regression.

These three should be scheduled with the deferred L-06 unification work, not
folded into shipping four effect apps. Shipping the apps does not require them:
the apps sidestep H-1 and H-3 by not using those code paths, and sidestep M-3 by
binding B directly.

---

## 7. Could not verify from source, and had to assume

1. **Delay's maximum HZ-mode value.** `MIN_DELAY_SECS` is read off the stream at
   runtime (`DelayApplet.h:53-54`) and never appears as a literal, so the
   largest number the HZ mode can print is unknown. This spec assumes it stays
   inside a 6-character field (`%d.%01d`, i.e. below 10000.0). If it can exceed
   that, the row needs a `k` suffix like the cutoff row.
2. **Delay's `MAX_DELAY_SECS`.** ~12s with PSRAM / ~0.37s without is taken from
   the comment at `DelayApplet.h:399-402`, not measured.
3. **`Xfade` / `Strch`** as the MODE-page wording for `CROSSFADE` / `STRETCH`
   (`DelayApplet.h:243`) is my abbreviation — the applet's own strings are 9
   characters and will not fit a 6-character value field. Worth a wording call
   before implementation; `Fade` / `Warp` is an alternative.
4. **The `k` suffix on cutoff** assumes "k" reads as kilohertz without a unit
   glyph on this panel. Unverified at arm's length.
5. **Header clearance.** Freeverb's letterspaced header ends at x=90 and the
   presence box starts at x=118, leaving 28px. Comfortable in arithmetic, **not
   confirmed against a real framebuffer.** Per this project's own history,
   code-review pixel math has been wrong more than once — dump the SSD1306
   framebuffer over the serial console (8 pages x 128 cols, LSB = top pixel per
   page), decode it to a PNG, and look at it before calling this spec landed.
6. **App IDs and folder assignment.** The four `TWOCCS` ids and their
   `OC_app_folders.h:95-100` `FOLDER_AUDIO` default entries are the
   implementer's to choose; this document does not invent ids.
7. **Whether `send_mode` should be added to Freeverb and Samverb.** They have no
   such bit today. This spec deliberately does **not** add it, leaving B unbound
   in those two rather than growing new persisted state purely for the symmetry
   of a footer legend. A footer that omits an unbound button is honest; a state
   bit added so a legend looks tidy is not.
8. **`gfxFooter()` clearing behaviour under the new pitch.** `gfxFooter()`
   clears y=54..63 (`HSUtils.cpp:921`). At the 10px pitch the last cursor band
   ends at y=50, so draw order should be free — but that is arithmetic, and
   item 5's caveat applies to it as well.
