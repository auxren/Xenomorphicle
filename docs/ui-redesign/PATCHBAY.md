# PATCHBAY — one port table, one modifier, one inversion

Design proposal for the module-wide UI language, from the **UI/UX lead** seat of
the three-person UI/UXR team.

> **Status: awaiting Oren's greenlight. Nothing here is implemented. No code in
> this tree has been changed by this document.**

> **This is a re-run of a lost document.** The original PATCHBAY proposal was
> delivered once and its transcript became unrecoverable after a session
> compaction. Only its own addendum survived, preserved verbatim in
> [LOST-SEATS-addenda-and-briefs.md](LOST-SEATS-addenda-and-briefs.md). This
> reconstruction is written to be **consistent with that addendum** — the
> port-is-the-row model, the Z-only modifier rule, the outmap-at-gather-stage
> design for outputs, the three-tier concurrency honesty, and the P0-first
> ladder are all carried forward as the addendum describes them, and the
> addendum's five corrections are folded into the body rather than appended.
> Where this re-run *revises* the original, it says so in the text.
>
> The re-run also has one input the original never had: the 26-finding **Panel
> Audit**, distilled in [Established-Rules.md](Established-Rules.md), and the
> **[Panel-Binding-Matrix](../Panel-Binding-Matrix.md)**. Both changed the
> proposal. In particular, L-06 (the inversion grammar) is now load-bearing
> throughout, and several claims the original made about "a single consistent
> meaning for A/B" are withdrawn as unsound against the matrix.

Every code claim below was re-verified against `preset-bus` at the time of
writing. Line numbers are given so they can be checked. Where a cited line
disagrees with an earlier document, that is called out rather than smoothed
over.

---

## 0. Baseline: what this proposal assumes as already settled

Assumed as shipped and not re-litigated:

- **The chord card is Z-only.** `A` raises nothing; `A` is a free ordinary app
  button (`OC_app_base.cpp:449-461`). The card's rows are allocated in priority
  order into a hard `rows[5]` budget (`OC_app_base.cpp:395, 405-425`).
- **The release-first guard** (`IgnoreUntilRelease`, contract at
  `OC_ui.h:152-207`) is the reference implementation for any hold-to-commit
  gesture. New chords extend its call sites; they do not get a new mechanism.
- **Confirm screens mask all four face buttons**; the 200e app's arm(A) /
  confirm(encR) split is a hardened carve-out and is not re-bound here.
- **CAPS = active, lowercase = inactive.** Footer legend grammar is
  `A:toggle  B:env  R:edit`. Refusals state fact + remedy in one line.
- **Screen budget 21 columns x 8 rows** at the 6x8 font; the chord card is
  9px-pitched and caps at `rows[5]` (`OC_app_base.cpp:124, 143-146`).
- **`CVInputMap` is type-safe by construction.** `ChangeSourceType`
  (`CVInputMap.h:113-124`) and `ChangeSource` (`CVInputMap.h:126-137`) step only
  through valid `(type, index)` pairs. No routing UI needs a validator.

Two corrections to inherited documents, offered as corrections and not as
gotchas:

1. **L-06's own citation has moved.** `Established-Rules.md` and the brief both
   place "one focus grammar: invert exactly what the right encoder will change"
   at `PresetBusUI.cpp:355`. In the current tree it is at
   **`PresetBusUI.cpp:433`**. The audit was written against `79bda444`; the file
   has grown since. The rule is unchanged, the line is not.
2. **The self-patch type code is `#`, not `D`.** The sonnet senior's addendum
   describes Hemisphere's input grid as showing `D A` for a self-patch. The
   actual glyph table is `{' ', '-', 'C', '#', 'M', 'G', '?', '?'}`
   (`CVInputMap.h:161`), so `TYPE_DAC` renders as `#A`. Any proposal reusing the
   3-char code "verbatim" — as that addendum rightly urges — needs the right
   letters. The full set is:

   | code | meaning | source |
   |---|---|---|
   | ` - ` | unrouted | `CVInputMap.h:159` |
   | `C n` | physical CV input n | `CVInputMap.h:161, 168-175` |
   | `#X`  | another slot's output, slot letter X | `CVInputMap.h:163-166` |
   | `M n` | MIDI map slot n | `CVInputMap.h:161, 168-175` |
   | `* `  | internal (noise, later LFOs) | `CVInputMap.h:177-179` |
   | `T n` | physical trigger input n (digital map only) | `CVInputMap.h:410, 427-435` |
   | `CL1` `CL4` `RUN` `RST` | internal clock (digital map only) | `CVInputMap.h:411, 418-419` |

---

## 1. The idea, in one paragraph

**The module's ports are the stable objects; apps are what temporarily gives
them meaning.** There are exactly eight CV ports, eight gate ports and eight
outputs, and every route in the instrument is owned by exactly one of them. So
routing is not a graph to be drawn — it is a **function from destinations to
sources**, and the complete representation of a function on 24 destinations is a
24-row table you can walk with one encoder. PATCHBAY is that table, opened by a
gesture that already exists, drawn by a renderer that already exists, obeying an
inversion rule that already exists in the half of the UI that gets it right.
Everything else in this proposal is the discipline needed to keep the table the
only place routing lives.

The three pillars, restated from the surviving addendum:

- **The port is the row.** Never "assign source X to parameter Y". Always "this
  port; here is what feeds it; here is what this app calls it".
- **Z is the only modifier.** Every other control belongs to the app.
- **Outputs get routable at the gather stage**, not by touching `io_offset`.

---

## 2. The button language

### 2.1 The rule that makes it a language

> **Two layers, and the boundary between them is the Z button.**
>
> **Layer 1 (SYSTEM)** is every gesture that contains Z, plus the both-encoder
> chord. It is identical in all 15 apps, it is documented on exactly one screen
> (Z's card), and an app may not rebind any of it.
>
> **Layer 2 (APP)** is A, B, X, Y, encL, encR and every chord among them. It
> belongs entirely to the app. The system never reaches into it.

This is close to ratification rather than invention: `OC_app_base.cpp:44-51`
already writes the two-layer rule down as prose ("A (or Z) plus a push of the
RIGHT encoder opens the app switcher ... A+B means a different thing in each app
that binds it"). The single change PATCHBAY makes to it is deleting the "(or A)".

### 2.2 What each control means by default

These are **defaults and expectations**, not enforcement. An app that needs
something else takes it — but the chord card must then carry it, which is what
keeps the deviation honest.

| Control | Default meaning | Never |
|---|---|---|
| **A** | The app's *primary act* on the current selection: transport, preview, freeze, select. | Destructive; global; a modifier. |
| **B** | The app's *secondary act*, or the alternate half of A. | A page change (that is X/Y). |
| **X**, **Y** | *View* changes only: page, channel, scene, zoom, prev/next. Naturally paired. | Value changes. |
| **encL** | Go back / go up one level. Long press = the app's one "big" action, and it must appear on Z's card. | Silently destructive without a confirm. |
| **encR** | Pick / commit / toggle edit. **Its turn changes exactly what is inverted.** | Two different jobs on one screen. |
| **Z** (tap) | The app's run/stop-shaped toggle, where one exists. Acts on release. | Anything that isn't reversible by repeating it. |
| **Z** (hold, alone) | The chord card. | — |

**There is deliberately no global meaning for A/B as "octave" or "coarse
adjust".** The original PATCHBAY draft proposed one; the
[Panel-Binding-Matrix](../Panel-Binding-Matrix.md) kills it and this re-run
withdraws it. `A`/`B` currently mean octave in QQ/DQ/H1200/References, ±32 raw
in Piqued/Lorenz/Viznutcracker/BBGEN, scene select in Scenery, setup ± in
Captain MIDI, and applet select in Quadrants/Hemisphere. Two apps break any
symmetric reading outright:

- **ASR** binds A to octave but **B to freeze the sample-and-hold**
  (`ASR.h:883` dispatches `CONTROL_BUTTON_DOWN` to `HandleLowerButton()`, which
  is `asr_.toggle_manual_freeze()` at `ASR.h:952-954`). Under the table above,
  ASR is *already compliant*: A is the primary value act, B is the secondary
  act. It only looked like a violation under the rule I am not making.
- **Chords** overloads B with menu-page navigation on top of octave
  (`Chords.h:1162-1341`). That one **is** a violation of the X/Y column above,
  and the remedy is to move the page toggle from B to X. Cost on the shipping
  module: **zero** — `ENABLE_APP_CHORDS` is commented out in the `T41` env
  (`platformio.ini:129`), so Chords is not in the 15-app build at all. It is a
  cleanup for whoever re-enables it, not a migration step.

### 2.3 The alias trap, stated once so nobody falls in it

`CONTROL_BUTTON_A` is a preprocessor-level alias of `CONTROL_BUTTON_UP`, and `B`
of `DOWN` (`OC_ui.h:52-56`), inherited from 2-button-shield hardware this panel
does not have. Three apps bind `CONTROL_BUTTON_A` to a handler *named*
`OnDownButtonPress()` — `NeuralNetwork.h:657-678`, `WaveformEditor.h:414-436`,
`ScaleEditor.h:380-402`. Their behaviour is internally consistent; the **names
are inverted relative to the bindings**.

**Implementation rule for anyone building this grammar: read the `case
CONTROL_BUTTON_*` label, never the handler name.** New code writes
`CONTROL_BUTTON_A/B/X/Y` only. Renaming those three handlers is a one-line-each
janitorial item, parked at P4 because it changes no behaviour and touches two
apps the simulator cannot build.

### 2.4 KEPT / CHANGED / RETIRED

| Gesture | Verdict | Cost |
|---|---|---|
| `Z+encR` app switcher (`OC_ui.cpp:236-240`) | **KEPT** | — |
| `Z+encL` I/O settings (`OC_ui.cpp:242-246`) | **CHANGED**: opens PATCHBAY. The old I/O menu becomes PATCHBAY's JACK page (page 1 of 4). | Muscle memory is preserved — same chord, same neighbourhood of function. Chord-card row changes from `Z+encL: I/O Cfg` to `Z+encL: Patchbay` (16 chars, fits). Nothing else. |
| `Z+A` screensaver (`OC_ui.cpp:248-252`) | **KEPT** | — |
| `encL+encR` preset overlay (`OC_ui.cpp:211-221`) | **KEPT**, untouched. | — |
| Hold `Z` 700ms = card (`OC_app_base.cpp:429-455`) | **KEPT**, one row relabelled. | — |
| **`A` as an alias for `Z`** in the two encoder chords (`OC_ui.cpp:237, 243`, the `(z_hold \|\| a_hold)` tests) | **RETIRED — but gated.** See below. | Real, and it is the single riskiest item in this document. |
| Quadrants `B+Y` -> Input Mapping (`Quadrants.h:637-642`) | **KEPT**, re-pointed: it becomes a shortcut *into* PATCHBAY's IN page rather than a parallel screen. | One `config_page` assignment becomes a call into the shared view. |
| Quadrants' other seven chords (`Quadrants.h:613-641`) | **KEPT**, untouched. | — |
| App-switcher `B` long-press = encoder acceleration (`OC_apps.cpp:988-1003`) | **KEPT.** Already fixed by the audit (deliberate hold + worded feedback). Not re-opened. | — |
| Chords' `B` page toggle | **CHANGED** to `X`, whenever Chords is next compiled. | Zero today. |
| `Z+X`, `Z+Y` | **RESERVED, unspent.** | — |

**The card, after this proposal.** One row's text changes; nothing else does.
The row budget is unchanged and still fits inside the hard `rows[5]` cap
(`OC_app_base.cpp:143-146, 395`). Shown here in Quadrants, which binds both
`Z` and `A+B`, so — exactly as the shipped allocator does at
`OC_app_base.cpp:421` — the `Z+A: Screensaver` row yields to app content:

```
+------HOLD Z-------+
|Z+encR: Switch App |
|Z+encL: Patchbay   |
|Z: Clock Run       |
|A+B: Clock Setup   |
|encL+encR: Presets |
+-------------------+
```

The frame is the card's own `drawFrame` (`OC_app_base.cpp:363`), the title row
is inverted by `invertRect` (`:368`) — a banner, which is one of the two
things §2.6 says must *not* be inverted. **That is a real, pre-existing L-06
conflict on the card itself, and P1 must resolve it**: the card is already
inside a drawn frame, so the title can lose its inversion and keep its
separator rule with no loss of legibility. Flagged rather than inherited.

**On retiring the A alias.** The argument is the project's own: the splash-screen
factory-erase chord was already moved off `A+encR` onto `A+B` *specifically*
because it collided with the app-switcher gesture, and the comment at
`OC_ui.cpp:296-305` says so in as many words. A-as-modifier has caused one real
collision. It is also why the 200e app's arm(A)/confirm(encR) split has to be a
hardened carve-out — a fumbled `A+encR` is the app-switcher chord. Retiring the
alias makes that carve-out *unnecessary* rather than merely defended.

**The gate:** `UI-Redesign-Constraints.md` §7 records that **Z's reachability on
the real panel is unconfirmed** — the firmware maps it, two attempts to observe
the grey button firing it were inconclusive. If Z is not reachable, then
retiring A leaves the module with **no route between apps at all**. So:

> **This change must not ship before the Z bench check passes.** It is cheap to
> settle (`--keys` against the console, or a scope on `but_mid`). Until then, A
> stays an alias and this row of the table is a plan, not a change.

That is the honest version. The original draft asserted the retirement without
the gate; this re-run adds it.

### 2.5 How an app extends the language without breaking it

Three obligations, all cheap:

1. **Take only from Layer 2.** A new chord may use any subset of A/B/X/Y/encL/encR.
   A new chord containing Z is a system gesture and needs a card row.
2. **Register a card row.** Adding a binding means adding or editing the app's
   `ChordGloss` entry (`OC_app_base.cpp:137-142`, table at `:159-307`). The card
   is generated from that table, which is precisely why it cannot go stale
   (`OC_app_base.cpp:105-135` argues this at length). An app with no chord still
   gets a real row via `tip`.
3. **Claim the whole chord on entry** with `IgnoreUntilRelease(...)` if it opens
   a screen. Extend the existing call sites; do not add a second ignore
   mechanism — that machinery exists because a fumbled chord once fired a
   bus-wide preset recall across every 200e module in the case.

**Chords containing Z are safe inside the largest app by construction.**
Quadrants' `CheckButtonCombo()` is `mask == combo && mask != last_mask`
(`Quadrants.h:822-824`) — an **exact mask match**, not a subset test. Holding Z
and pressing X yields `mask == Z|X`, which matches none of Quadrants' eight
combos. No defensive work is required. (Carried forward from the addendum,
re-verified.)

### 2.6 The inversion law — adopting L-06, not extending it

PATCHBAY **adopts L-06 as stated** and adds nothing to it:

> **Exactly one region of a screen is inverted at a time: the thing the right
> encoder's turn will change right now. Nothing else is ever inverted.**
>
> - A **cursor** that merely selects which row a later press will act on gets a
>   leading `>` and a blinking underline — not inversion.
> - **State** (running, muted, frozen, global-vs-app scope) gets a **suffix or
>   bracket** — never inversion.
> - A **banner** gets a drawn box (`drawFrame`) — never inversion.
> - A **list where encR's turn IS the choice** (the app switcher; the curation
>   list) may invert the chosen row, because the chosen row *is* the value encR
>   changes. This is the one boundary case and it is called out explicitly
>   below rather than left to be discovered.

**The good news, and it is very good: the fix already exists as a shipped
primitive.** `HemisphereApplet::gfxCursor()` (`HemisphereApplet.cpp:153-173`)
and `HSApplication::gfxCursor()` (`HSApplication.h:202-210`) both do exactly the
right thing already — blinking underline when not editing, `gfxInvert` only in
`EditMode()`. The largest part of the UI is already L-06-compliant. Unifying the
grammar is therefore **not** inventing a convention and applying it to 26
screens; it is making the remaining screens call the shape that already ships.

Four concrete, verified violations, which is what P1 in §6 targets:

| Site | What inversion currently means there | Fix |
|---|---|---|
| `OC_menus.h:325-328, 341-344, 363-366, 385-388, 400-403, 412-415, 422-425, 436-437` — `SettingsListItem::Draw*` | **Cursor.** The whole row inverts when merely *selected*; editing is signalled by a small `DrawEditIcon`. This is the classic-menu grammar and it is the exact opposite of the Hemisphere grammar. | One primitive, eight call sites in one file: `selected && !editing` -> `>` + underline; `editing` -> invert the value field. Every classic-menu screen — including the I/O settings menu — converts at once. |
| `HemisphereApplet.cpp:64, 74, 83` — `DrawConfigHelp()` | **Column badge.** Three inverted 19px cells on the applet help screen marking the trig / CV / out code columns. encR changes none of them; it is a read-only screen. | Drawn frame, or drop the box and lead with the icon that is already there. |
| `Quadrants.h:1680-1681` | **Identity.** "Applets 3 and 4 get inverted titles" — inversion as *which slot this is*, on the main screen, at the same time as applet cursors that invert in EditMode. This is L-06's "two of them on the same screen at once", concretely. | Case (`CAPS`/lowercase already means active/inactive) or a drawn rule. |
| `OC_apps.cpp:922` — app-switcher selection bar | **Boundary case.** encR's turn *is* what changes the selection, so under the law above this is legal and should stay. | **Keep** — but say so in the commit, because it will look like a miss. Listed as open question 6. |

The preset overlay (`PresetBusUI.cpp:433-445`) already obeys the law and is the
reference. `HSUtils.cpp:717-735` (`DrawAppletList`) already uses the `>` cursor
the law recommends, and signals hidden/shown by the *presence of an icon* rather
than by inversion — which is why §5 reuses it verbatim.

---

## 3. The routing model

### 3.1 Where it lives, and what opens it

**PATCHBAY replaces the I/O settings menu on `Z+encL`.** It is four pages,
turned with encL:

```
  1 JACK   physical conditioning: gain, filter, out scaling, tuning
  2 IN     the 8 CV ports: what feeds each one
  3 TR     the 8 gate ports: what feeds each one
  4 OUT    the 8 output jacks: which virtual channel each one emits
```

Spending an existing gesture rather than a new one is deliberate. `Z+encL`
already means "the screen about this module's I/O", it is already on the chord
card, and the card row shortens rather than grows. **Gesture budget spent by
this entire proposal: zero.** `Z+X` and `Z+Y` remain unspent.

Quadrants' `B+Y` (`Quadrants.h:637-642`) becomes a shortcut straight to page 2.

### 3.2 The renderer P0 promotes — not invents

This was the single most important correction in the surviving addendum and it
stands: **P0 must promote the existing input-mapping page, not build a parallel
one.** The two implementations are:

- `AppQuadrants::DrawInputMappings()` — `Quadrants.h:1337-1367`. A 4x4 grid of
  3-char codes, TR/CV icons, a dotted vertical divider at x=63 and horizontal at
  y=38, cursor via `gfxCursor(4 + 32*x, 23 + 13*y, 19)`, and
  `gfxDisplayInputMapEditor()` for the attenuverter.
- `AppHemisphere::DrawInputMappings()` — `Hemisphere.h:1425-1457`. The same
  screen for 4 channels instead of 8.

Both already sit on the shared edit machinery in `HSApplication.h:230-297` /
`HemisphereApplet.h:303-386`: `EditInputMap`, `EditSelectedInputMap`,
`CheckEditInputMapPress`, `gfxDisplayInputMapEditor`, `IndexedInput`
(`HSUtils.h:316`). That machinery is app-agnostic already. P0 lifts the *drawing*
into the same place and points both apps at it.

**PATCHBAY changes the layout from a grid to a list**, and owes an argument for
that. The grid packs 16 codes onto one screen, which is excellent density and
terrible legibility: a code with no name beside it tells you where a signal comes
from but never what it does. The list trades density for the one column that
makes the screen answer a question a performer actually asks. Both renderings
can share the same row-model and the same edit machinery; the grid survives as
Quadrants' at-a-glance view if Oren wants it (open question 4).

### 3.3 The column that makes this work, and it is free

`AppBase::GetIOConfig(IOConfig&)` is **pure virtual** (`OC_apps.h:139`) — every
one of the 15 apps implements it. Each app already declares a human label for
every CV input, every digital input and every output:

- `AppScenery::GetIOConfig` (`Scenery.h:652-672`): TRs are `Scene 1..4`, CVs are
  `Smooth Select`, `Bias`, `Slew`, `RndScn4`.
- `AppCalibr8or::GetIOConfig` (`Calibr8or.h:889-906`): `Ch1 Clk`, `Ch1 CV`, `Ch1`.
- `AppASR::GetIOConfig` (`ASR.h:835-865`): `Clock`, `Freeze`, `Oct+`, `Oct-`,
  and a *computed* CV label like `*index`, `*scale`.

The I/O settings menu already consumes exactly this (`OC_io_settings_menu.cpp:56`
calls `app->GetIOConfig(io_config_)`, then formats labels at `:156-186`) and even
has a marquee for labels too long to fit (`vfx::Marquee<11>`,
`OC_io_settings_menu.h:63`, driven at `:117-119`).

So the hardest-looking column in PATCHBAY — "what does this app call this
port?" — needs **no new declarations, no new per-app work, and no new storage**.
It is a join between data two subsystems already publish. An app that declares
nothing for a port (`label[0] == 0` — `AppScope::GetIOConfig` is empty,
`ScopeApp.h:358-361`) renders `--`, which is exactly the "this app doesn't use
this port" signal the screen needs.

Two honest gaps this creates, both small:

- `IOConfig.digital_inputs[]` is `DIGITAL_INPUT_COUNT` (4) long
  (`OC_io.h:66`), while `HS::trigmap[]` is `ADC_CHANNEL_COUNT` (8)
  (`CVInputMap.h:521`). TR rows 5-8 have no label source. Either widen the
  array to 8 or render `--`. P0 renders `--`; widening is a P2 nicety.
- Labels are up to 32 chars (`OC_io.h:37`) and the column is 7. The marquee
  already handles the selected row; unselected rows truncate.

### 3.4 Mockups

Annotation convention: **text between `«` and `»` is drawn inverted.** The
markers are annotation, not glyphs — column counts below exclude them. Every
mockup is 21 columns or fewer and 7 rows (header + 5 + legend), inside the
21x8 budget.

**IN page, cursor resting (nothing inverted).** Scenery is the running app.

```
PATCHBAY  IN    2/4
>CV1 C 1 100% Smooth
 CV2 C 2 100% Bias
 CV3 M 1  62% Slew
 CV4  -    --  --
 CV5 #A  200% --
L:page R:edit X:src
```

Row 1 is the header: screen name, page name, page index. Rows 2-6 are ports,
scrolled by encR. Row 7 is the footer legend in the established grammar.
`>` is the cursor (`RIGHT_ICON` in practice, as `HSUtils.cpp:731` already does).
`CV4` is unrouted and unused; `CV5` is routed from slot A's output but Scenery
declares nothing for port 5, so it reads `--`.

**What is inverted: nothing.** encR's turn moves the cursor here, and a cursor
gets `>`, not inversion. Legal by the law in §2.6.

**IN page, encR pressed once — now editing the source.**

```
PATCHBAY  IN    2/4
 CV1 C 1 100% Smooth
 CV2 C 2 100% Bias
>CV3 «M 1»  62% Slew
 CV4  -    --  --
 CV5 #A  200% --
L:page R:next X:src
```

**What is inverted: the 3-char source code on the cursor row, and only that.**
encR's turn now steps `ChangeSource()` (`CVInputMap.h:126-137`), which walks
only valid `(type, index)` pairs. Legal.

**IN page, encR pressed again — now editing the attenuverter.**

```
PATCHBAY  IN    2/4
 CV1 C 1 100% Smooth
 CV2 C 2 100% Bias
>CV3 M 1 «62%» Slew
 CV4  -    --  --
 CV5 #A  200% --
R:done  turn:atten
```

**What is inverted: the percentage field, and only that.** encR's turn now runs
`EditSelectedInputMap()` (`HSApplication.h:243-263`). Legal.

This is a deliberate change from today's behaviour: `gfxDisplayInputMapEditor()`
(`HSApplication.h:265-289`) draws the attenuverter as an **inverted box in the
top-left corner**, away from the row it belongs to. On PATCHBAY's list layout the
value has a home in its own row, so inverting it in place is both simpler and
more literally L-06 ("invert *what* encR changes", not "invert somewhere"). The
shared function stays — Quadrants' zoom view still uses it — PATCHBAY just
doesn't call it. Cost: one extra draw branch.

**TR page.** Note `CL4` (internal clock x4) and the `/2` division, both from the
digital map's own vocabulary (`CVInputMap.h:411, 418-419`; div/mult edited by the
same `EditSelectedInputMap`).

```
PATCHBAY  TR    3/4
>TR1 T 1  x1 Scene1
 TR2 T 2  x1 Scene2
 TR3 CL4  /2 Scene3
 TR4  -   -- Scene4
 TR5 M 3  x1 --
L:page R:edit X:src
```

**What is inverted: nothing** (cursor state).

**OUT page.**

```
PATCHBAY  OUT   4/4
>A  <1A 100% Out A
 B  <1B 100% Out B
 C  <3A  50% Out C
 D  <-    -- Out D
 E  <1A 100% Out E
L:page R:edit X:src
```

The left column is the **physical jack** (A-H, `OC::Strings::capital_letters`).
The `<1A` column is the **virtual channel** it emits — slot 1's A side. `E` also
emits `1A`: fan-out on outputs is legal and needs no special case, because the
map is keyed by jack. `D` emits nothing. The `100%` column is
`HS::frame.output_atten[]`, which already exists (`HSIOFrame.h:881`,
edited at `:945`).

**What is inverted: nothing** (cursor state). Editing inverts `1A`, then `100%`,
exactly as on the IN page.

**JACK page — the absorbed I/O settings menu.**

```
PATCHBAY  JACK  1/4
 jack   CV3  (all)
>gain      1.00x
 filter      ON
 out C scale  1V/8
 tuning     Auto
L:page R:edit A:jack
```

Same five settings as today (`OC_io_settings.h:44-55`), same encL-picks-channel
behaviour — except the channel picker moves to `A` because encL is now the page
turn. `(all)` is the scope suffix: **state gets a suffix, never inversion**
(§2.6). If Oren keeps per-app overrides (§4), an overridden row reads `(app)`.

**What is inverted: nothing.** Under P1 this page inherits the new
`SettingsListItem` grammar automatically, since it is drawn by
`menu::SettingsList` (`OC_io_settings_menu.cpp:89-146`).

### 3.5 How type safety is communicated: it isn't, and that is the point

An invalid route is **not refused — it is unreachable.** `ChangeSourceType` and
`ChangeSource` enumerate only legal `(type, index)` pairs, clamping the index to
`channel_count(type)` on every step (`CVInputMap.h:113-137`). You cannot turn
the encoder to an illegal source, so there is no error to word and no validator
to explain. PATCHBAY inherits that shape and adds no UI for it.

This is a genuine agreement with CORDLESS's framing — "un-expressible rather
than rejected" is the right description — reached from the code rather than from
the metaphor.

There is exactly **one** genuinely refusable act, and it belongs to the OUT page:
pointing a jack at a virtual channel no app in this build drives. The refusal
takes over the footer row, fact + remedy, one line:

```
PATCHBAY  OUT   4/4
>A  <«1A» --  Out A
 B  <1B 100% Out B
 C  <3A  50% Out C
 D  <-    -- Out D
 E  <1A 100% Out E
1A idle - try 3A/3B
```

(19 columns.) Note the value stays inverted while the refusal shows — encR is
still the control that will change it, so the law is unbroken.

### 3.6 "How does a user see what is already patched" — the hardest part

The addendum called this the hardest problem on this screen size. PATCHBAY gives
three answers at three altitudes, and claims the *first* one is the real one.

**Answer 1 — the table is exhaustive, and exhaustive is the answer.** A routing
where every destination holds exactly one source is a **function**, not a graph.
There are 24 destinations (8 CV ports, 8 gate ports, 8 output jacks). The
complete state of the instrument's patch is 24 rows, five at a time, four pages.
There is nothing to summarise, nothing to lay out, and no hop the user cannot
see, because **every route appears exactly once and no route can hide.** The
reason a many-to-many graph is unwinnable on 6 rows is that it is many-to-many;
the fix is to notice that this one isn't.

The hard constraint that makes this true is: **PATCHBAY must never grow a row
that is not a port.** The moment routing acquires per-parameter destinations,
the table stops being 24 rows and the whole argument collapses. That is the
architectural discipline this proposal is asking for, and it is the reason the
port-is-the-row rule is a *rule* and not a layout preference.

**Answer 2 — the reverse question, on `X`.** The one thing a destination-keyed
table cannot show is fan-out: *where does CV IN 3 go?* A source may feed several
destinations. `X` re-keys the same table by source, which needs no new state
because it is a transposition of data already on screen:

```
PATCHBAY  BY SOURCE
 C 1 > CV1
 C 2 > CV2
 M 1 > CV3 TR5
 1A  > out A out E
 T 1 > TR1
X:by port  R:jump
```

`R` on a row jumps back to that destination's row in the by-port view. Sources
with no destination simply do not appear — the list is short precisely because
most sources are unused.

**What is inverted: nothing.** This view is read-only; `R` navigates. Legal.

**Answer 3 — the running screen already shows it, per applet.** This is easy to
forget: `HemisphereApplet::DrawConfigHelp()` already prints each applet's trig
source, CV source and output letter beside that applet's own help text
(`HemisphereApplet.cpp:62-86`). A performer inside Quadrants does not have to
open PATCHBAY to see where an applet's two inputs come from. PATCHBAY's job is
to be the *module-wide* answer, not the only answer. (Those three cells are also
the L-06 violation from §2.6 — fixing them makes this view better, not worse.)

### 3.7 Outputs: the outmap, at the gather stage

The constraint doc names non-routable outputs as "the single biggest genuinely
missing piece", because an applet's output channel is fixed by `io_offset`, a
macro defined as `(hemisphere * 2)` at **`HSUtils.h:35`** and used at ~20 sites
in `HemisphereApplet.cpp`/`.h`. Rewriting `io_offset` would be invasive and
would change what every applet believes about itself.

**Do not touch it.** There is a single place where virtual channels become
physical jacks:

```
// HSIOFrame.cpp:865-870, in HS::IOFrame::Send()
      outputs[i].push(output_slew[i]);
      if (i < DAC_CHANNEL_COUNT)
        ioframe->outputs.set_pitch_value(chan[i], outputs[i].get(output_atten[i]));
```

That loop walks 32 virtual channels and copies the first 8 to the 8 DACs by
position. **Invert the loop and index it through a map**, and outputs become
routable in about four lines:

```
    for (int p = 0; p < DAC_CHANNEL_COUNT; ++p) {
      const int v = HS::outmap[p];          // default: v == p  (identity)
      ioframe->outputs.set_pitch_value(chan[p], outputs[v].get(output_atten[v]));
    }
```

State cost: `int8_t outmap[DAC_CHANNEL_COUNT]` — **8 bytes**, identity by
default, so an unmigrated module behaves exactly as today. `io_offset` keeps its
current meaning ("slot N writes virtual channels 2N and 2N+1"); the map decides
which *jack* shows a given virtual channel. No applet changes. This is the
"outmap-at-gather-stage" design the addendum records, restated with the call site
verified.

**Coverage, stated honestly.** `HS::frame.Send()` is called from
`HSApplication.h:84`, so this covers every `HSApplication`-derived app. In the
real **T41_audio** build that is *every app that produces CV output*: of the 15
apps compiled (`apps/_config.h:83-176` under `platformio.ini:105-155`), the four
that are not `HSApplication`-derived — Scope, Sampler, USB Drive, Back It Up! —
contain **zero** calls to `frame.Out`, `set_pitch_value` or `DAC::set` (verified
by grep). So on this fork the single-site map is genuinely universal.

The caveat: re-enabling any of the commented-out classic apps (ASR, QQ, DQ,
Piqued, H1200, …, all disabled at `platformio.ini:126-136`) reintroduces a bypass
— they write `ioframe->outputs` directly. The second site, if that day comes, is
`IOFrameToChannel()` at `OC_io.cpp:79-95`, the last stop before `DAC::set`.

**One free win, carried from the addendum and re-verified:** `IO_SETTING_TRn` is
already `settings::STORAGE_TYPE_NOP` (`OC_io_settings.h:110, 116, 122, …`) — a
display-only row that occupies no storage. Anything PATCHBAY wants to show per
trigger jack can land there with **zero storage migration**.

---

## 4. Per-app or global? — the question the owner deferred

**Position: GLOBAL, with the app's names on it. Remove the split; do not label
it.** The addendum leaned this way; this re-run keeps the lean and now argues it.

### Leg 1 — the code already says global, for the half that matters

`HSApplication::In(ch)` *is* `cvmap[ch].In()` (`HSApplication.h:138`) and
`Gate(ch)` *is* `trigmap[ch].Gate()` (`HSApplication.h:168`). `HS::cvmap` and
`HS::trigmap` are plain namespace globals — **one array for the whole module**
(`HSUtils.cpp:135-136`, declared `extern` at `CVInputMap.h:521-522`).

At runtime the patch is **already global**. What is per-app is only the *saved
copy*, and only because two apps happen to serialize it into their own EEPROM
chunks: `Quadrants.h:2007-2013` / `:1935-1957` and `Hemisphere.h:387-393` /
`:514-529`. The consequence is not "per-app routing" — it is **"global routing
with a nondeterministic writer"**: whichever of Quadrants or Hemisphere saved
most recently wins, and every other app silently inherits it.

Making routing global therefore **fixes a latent bug**; it does not impose a
policy. That reframing is the strongest single argument here and it took reading
the save paths to find.

### Leg 2 — the two halves are different LAYERS, not different scopes

This is the part the constraint doc's framing obscures. Trace one CV sample:

```
  ADC
   |
   v  IO::Read()             OC_io.cpp:61-77
      per-channel FILTER and GAIN, from the app's IOSettings
   |
   v  ioframe->cv.pitch_values[ch]
   |
   v  HS::frame.In(ch)       HSIOFrame.h:918-922
   |
   v  CVInputMap::RawIn()    CVInputMap.h:72-89
      picks WHICH conditioned signal (ADC n / slot out / MIDI / internal)
   |
   v  * attenuversion        CVInputMap.h:91-94
   |
   v  HSApplication::In(ch)  HSApplication.h:137-139
```

Gain and filter run **before** anything is routed. They describe **the jack** —
what voltage arrives on CV3 from a 251e, and how much it is smoothed. Routing
runs **after**. It describes **the patch** — which conditioned signal an app port
reads.

A jack's conditioning is a property of *the case you are plugged into*, not of
the app on screen. That is not my opinion; it is already this tree's stated
principle for exactly this class of setting, written for the clock routing key:

> "GLOBALS.CFG, module-level: like device MIDI bindings, this is rig
> infrastructure and must survive both reboots and preset recalls, not just the
> app that happens to be on screen when it's edited"
> — `HSUtils.cpp:69-76`

Input gain on the jack a 251e is patched into is rig infrastructure by that
definition. So both halves of PATCHBAY want to be global — for two *different*
and independently sound reasons, which is a much better position than "make them
match".

### Leg 3 — the alternatives fail on their own terms

- **Everything per-app.** Triples routing storage, and breaks the mental model a
  banana case demands: the physical cords do not move when you change apps, so
  neither should the map that describes them. You would re-patch the
  Xenomorpher every time you switched screens.
- **Keep the split and label it.** Fails the 21-column test. A `[app]`/`[mod]`
  suffix on every row costs 5-6 of 21 columns — the entire label column — and
  asks a performer to hold two scoping models in their head *while patching*.
  Labelling is what you do when you cannot fix the inconsistency; here it can be
  fixed.

### The mechanism, and its cost — stated plainly

Do **not** change the EEPROM layout. Keep the per-app `io_settings_` chunk
exactly where it is (`AppBase::Save`/`Restore`, `OC_app_base.cpp:472-484`). Add
a bank-global JACK record on a new PhzConfig key — the neighbourhood is already
established, `FILTERMASK1_KEY = 100` / `FILTERMASK2_KEY = 101` at
`Hemisphere.h:357-358`, and the clock routing key is `10 << 8`
(`HSUtils.cpp:77`). Make the global the **master**: copy it into
`app->mutable_io_settings()` on `APP_EVENT_RESUME`.

- **Zero format change, zero migration, zero risk to existing presets' layout.**
- **Reversible** by deleting one copy.
- **Cost, stated:** a preset saved by an older build with a deliberately
  different per-app gain gets overwritten by the global, once, on first load
  under the new firmware. If Oren has such a preset, this is a real loss and it
  is open question 3.
- If per-app overrides are wanted later, the storage is still there to turn back
  on — and the override shows as a bracketed `(app)` suffix, per L-06.

Routing gets the same treatment in reverse: cvmap/trigmap move to a bank-global
key, and Quadrants/Hemisphere keep writing their app chunks for backward
compatibility, with the global winning on load. Persistence rides the ~250-byte
existing preset key already swept into all 30 preset-bus slots — no new file
format, no container-section change.

---

## 5. Concurrency, told honestly

There are three tiers and they are nothing alike. PATCHBAY's contribution is to
make the screen *tell the truth* about which tier a route is in, using
vocabulary that already exists.

**Tier 1 — audio: free, real, already shipped.** The audio graph is DMA-driven
on ~2.9 ms / 128-sample blocks and is decoupled from app switching. Quadrants,
Tweighty and Sampler are permanently wired into an
`AudioSummingRoute<kOutputRouteChannels, kOutputRouteSources>`
(`AudioIO.cpp:48-60`; the class is at `Audio/AudioMixer.h:56-78`) and keep
sounding while another app is on screen. Sampler nests its own `<2, 8>` route
inside its single slot (`SamplerApp.h:161`). That template already *is* the
"mix several sources onto one output" primitive. **Audio destinations accept
stacks; PATCHBAY needs to invent nothing for them.**

**Tier 2 — CV rate: one seat, and it is small.** All app logic runs in a
16.666 kHz core ISR — `OC_CORE_ISR_FREQ = 16666`, i.e. a **60 microsecond**
budget (`OC_config.h:19-24`) shared with display flush, DAC write, ADC scan and
digital-input scan. Exactly one app gets it: `AppSwitcher::current_app_` is a
single `RuntimeSlot`. No scheduler, no preemption, no per-app budget. Quadrants
is the only proof concurrency works there at all, and only because Hemisphere
applets are deliberately tiny.

**PATCHBAY does not promise more, and does not build a deck.** See §7.

**Tier 3 — the honest middle, and the reason it matters to this screen.** A
route whose source is `TYPE_DAC` (`#A`) reads `frame.ViewOut(index)`
(`CVInputMap.h:76-77`) — a virtual channel produced by an applet. When that
applet's app is not the focused app, the value **persists in RAM but stops
updating**. The route is not broken and it is not live: it is *frozen at its
last value*.

That is exactly what `CAPS = active / lowercase = inactive` was invented for.
PATCHBAY case-folds the app-label column: **CAPS when the row's source is
producing right now, lowercase when it is frozen or unused.**

```
PATCHBAY  IN    2/4
>CV1 C 1 100% SMOOTH
 CV2 C 2 100% BIAS
 CV3 M 1  62% SLEW
 CV4  -    --  --
 CV5 #A  200% --
L:page R:edit X:src
```

`C`, `M` and internal sources are always live, so their rows are always CAPS.
A `#` source is CAPS only while its producer is running. No new glyph, no new
indicator, no new state machine — one `toupper` pass over a 7-char column, and
the instrument stops lying about the 60 µs seat. Cost: `GetIOConfig` labels are
mixed case (`"Smooth Select"`, `"Out A"`), so the fold is a real change to how
they read; that is a deliberate use of an established rule and is listed as
part of P2, not P0.

There is one documented real bug in this area — the Tweighty background pump,
two unsynchronized writers racing app state. Nothing in this proposal adds a
second writer to any app's state: PATCHBAY writes only `cvmap`, `trigmap`,
`outmap` and the global jack record, all from the UI thread.

---

## 6. Menu curation

The applet version is already shipped and is already the right shape:
`HS::hidden_applets[2]` is a 128-bit mask (`applets/_config.h:257-264`) with a
toggle screen where **encR toggles one and encL inverts all**
(`Quadrants.h:1320-1327`, `Hemisphere.h:1407-1415`), persisted to bank globals
under `FILTERMASK1_KEY`/`FILTERMASK2_KEY` (`Quadrants.h:2054-2055`,
`Hemisphere.h:431-432, 583-584`).

Generalising it to *apps* is **one `uint32_t`** plus a skip in the switcher's
cursor loop. The addendum's correction stands and is re-verified: the ~31 figure
is the declared roster; the **T41_audio build compiles exactly 15 apps** —
Settings, Quadrants, Calibr8or, Scenery, Captain MIDI, Pong, 200e Modules,
Tuner, Tweighty, Scope, Sampler, USB Drive, Scale Editor, Waveform Editor,
Back It Up! (`apps/_config.h:83-176` under `platformio.ini:105-155`). One
`uint32_t` covers the full declared roster with room to spare.

**Reuse `DrawAppletList` verbatim** (`HSUtils.cpp:717-735`): `>` cursor, icon
present = shown, icon absent = hidden. It is already L-06-shaped — it does not
use inversion to mean "visible" — which is why it is the right thing to copy.

```
APPS        9 on/15
>* Quadrants
 * Captain MIDI
   Pong
 * Tweighty
R:toggle  L:invert
```

**What is inverted: nothing.** The cursor is `>`; visibility is the `*` (or the
app's icon, exactly as the applet list does it). Legal.

Two guards, both one-liners, both from hazards the audit already names:

- **Never hide the currently running app** — you would be unable to navigate
  back to it, and the switcher's `cursor.Init(0, num_apps-1)` /
  `IndexOfAppByID` seeding (`OC_apps.cpp:879-880`) assumes it is present.
- **Never hide Setup/About.** It is the only route to calibration and factory
  reset, it has no `#ifdef` in the container (`apps/_config.h:85`), and it is
  the recovery path if curation itself goes wrong.

Refusal line, fact + remedy, 19 columns: `last app: keep 1 on`.

The switcher itself does not change otherwise. Its legend
(`encR:pick  encL:back`, `OC_apps.cpp:934`) stays; its 10px line pitch and
five-visible-rows layout stay (`OC_apps.cpp:886-923`); and the selection bar
stays inverted per §2.6's boundary case. Curation removes rows from it, nothing
more:

```
«Quadrants          »
 Captain MIDI
 Tweighty
 200e Modules
 Setup / About
encR:pick  encL:back
```

**What is inverted: the selected row** — because encR's turn is what changes the
selection, so the selected row *is* the value. This is the boundary case named
in §2.6, and it is the one place in the instrument where an inverted row is
legal without an accompanying edit mode. In practice the bar hugs the name
rather than running to x=127 (`OC_apps.cpp:914-923`); the mockup shows it full
width only because ASCII cannot show a partial cell.

---

## 7. Where I disagree with CORDLESS

CORDLESS is a strong document and its central instinct — that this module's
patchfield is invisible and that the fix must be *physical*, not
administrative — is correct and I adopt it. Three specific disagreements, each
technical rather than stylistic.

**1. "Hold any control 700 ms, and press controls listed on the card to fire
them."** I think this is the most dangerous idea in the proposal, for three
reasons. (a) It makes the card a **live menu**, which means during a hold every
other button means something different from what it means the rest of the time —
the same shape as the audit's named hazard, a two-step confirm whose arm and
commit are the same class of control, measured committing in **6-51 ms** of
contact. (b) The 700 ms delay's floor is `Ui::kLongPressTicks` (500) *precisely
so that releasing the modifier is inert*, and `OC_app_base.cpp:64-84` spends
twenty lines arguing that point; a card you can press from re-arms every
long-press binding in every app underneath it. (c) It multiplies the discovery
surface from one card to seven, each needing its own accurate table, while the
shipped card's entire justification (`OC_app_base.cpp:105-142`) is that it is
generated from **one** table and therefore cannot go stale. PATCHBAY keeps one
card, read-only.

**2. Live meters on every picker row, with the list re-sorting by activity
("wiggle-to-patch").** The *idea* is excellent and I would keep it; the
*ambient* form is not selectable. A list that reorders itself while you look at
it cannot be walked with an encoder — the row under the cursor moves out from
under it — and the audit separately records that encoder acceleration's ~150
detents/sec threshold is unreachable by hand, so "scroll, and it moved" is
unrecoverable. There is also an L-06 cost CORDLESS's author was not briefed on:
filled/hollow/blinking jack glyphs, a carried-cord indicator and per-row meters
add three more visual states to a screen whose one visual state already means
six things.

My counter-proposal, which keeps the good half: bind wiggle-detection to an
explicit act — **press `A` on a source field to "learn from motion"**, freezing
the candidate list for the duration and picking the input that moves. The module
already has exactly this shape: `CVInputMap::AutoLearn()` (`CVInputMap.h:36-42`)
is MIDI auto-learn, and it is a **command**, not an ambient behaviour. Extending
a command is a smaller step than inventing a mode.

**3. "Carrying the cord across apps."** This requires a modal state that
survives the app switcher — a global mode with no visible owner, entered from any
screen and exited from any other. In an instrument whose worst recorded incident
was a gesture leaking across a single screen boundary (`OC_ui.h:161-166` — the
both-encoder chord's held encR firing the overlay's 250 ms RECALL, bus-wide,
across every 200e module in the case), deliberately building a mode that crosses
*every* screen boundary is the wrong direction. It is also unnecessary:
`TYPE_DAC` sources are addressed by **virtual channel index**, not by "the app
currently running" (`CVInputMap.h:63-65`, `channel_count` returns
`2 * APPLET_CURSOR_COUNT`). A destination-keyed table can name any producer from
anywhere without carrying anything. Cross-app routing is a *fact about the map*,
not a journey.

**What I take from CORDLESS.** Its occupied-destination rendering is better than
anything I had: a full destination shows its current owner *before* you take it.
PATCHBAY's OUT-page refusal line is that idea, reduced to one line and one
remedy. And its framing of type-safety as un-expressibility rather than
validation is exactly right, and I have adopted the wording.

---

## 8. Migration and sequencing

The ladder is reconstructed as the addendum describes it — P0-first, curation at
P3, the const-correctness fix at P4 — with one recommended revision stated
openly at the end.

### P0 — Promote the input-mapping renderer. *This is the smallest increment that delivers real value.*

- Extract the row-drawing out of `AppQuadrants::DrawInputMappings()`
  (`Quadrants.h:1337-1367`) and `AppHemisphere::DrawInputMappings()`
  (`Hemisphere.h:1425-1457`) into one shared view alongside the edit machinery
  that is already shared (`HSApplication.h:230-297`).
- Point `Z+encL` at it as PATCHBAY page 2 (IN) and page 3 (TR). The old I/O menu
  becomes page 1 (JACK), unchanged in behaviour.
- Join the `GetIOConfig` label column in (§3.3). No new declarations needed.
- Quadrants' `B+Y` becomes a jump to page 2.
- **New storage: none. New gestures: none. New inversion meanings: none.**
- **Value delivered:** every app in the 15-app build gets the routing screen
  that two apps had — including Captain MIDI, the default boot app.
- **Risk:** touches Quadrants, which the simulator cannot build. Bench-verified
  only. This is the argument for pulling P4 forward; see the caveat.

### P1 — L-06 unification, in one primitive

- Change `menu::SettingsListItem`'s selection rendering in `OC_menus.h` (eight
  call sites, one file): `selected && !editing` -> leading `>` + underline;
  `editing` -> invert the value field only. **Every classic-menu screen converts
  at once**, including PATCHBAY's own JACK page.
- Three hand fixes: `HemisphereApplet.cpp:64, 74, 83` (badge -> frame);
  `Quadrants.h:1680-1681` (identity -> case or rule); and an explicit *decision
  to keep* `OC_apps.cpp:922`, documented in the commit so it does not read as a
  miss.
- Closes the first of the two explicitly-deferred audit findings.
- **Risk:** high blast radius by design, but the six apps the simulator *can*
  build (Setup/About, 200e Modules, Scenery, Pong, Tweighty, Back It Up!) all
  use these primitives, so `--dump-fb` + `fbtext.py` + `edgecheck.py` give real
  coverage for the first time on a grammar change.

### P2 — Storage unification, and outputs

- The global-master JACK record and the bank-global routing key (§4). No EEPROM
  format change.
- `HS::outmap[8]` at `HSIOFrame.cpp:865-870`; PATCHBAY page 4 (OUT). 8 bytes,
  identity default.
- The CAPS/lowercase live-vs-frozen fold (§5).
- The `IO_SETTING_TRn` `STORAGE_TYPE_NOP` row (`OC_io_settings.h:110`) carries
  the trigger-source display with zero migration.

### P3 — Menu curation

- One `uint32_t` in bank globals, `DrawAppletList`'s shape reused verbatim, two
  guards (§6).

### P4 — Fix the const-correctness bug; finish the absorption; retire the A alias

- Fix the const-correctness bug (`CaptainMIDI.h:393-394`, `Calibr8or.h:755, 757`,
  `ScaleEditor.h:192-213`): a non-const member called from a const draw path,
  which `arm-none-eabi-g++` accepts and Apple clang rejects. Unblocks Captain
  MIDI, Calibr8or and Scale Editor in the simulator.
- Decide whether Quadrants' `B+Y` grid is absorbed or kept as a fast path
  (open question 4).
- Rename the three inverted `OnDownButtonPress` handlers (§2.3).
- **Retire the `A`-as-`Z` alias — only if the Z bench check has passed** (§2.4).

### Sequencing caveat, stated rather than hidden

The addendum's ladder puts the const fix at P4. **I now think that is wrong and
would move it to P0a**, before anything else. P0 and P1 both change code the
simulator cannot see: P0 touches Quadrants (unbuildable), P1 touches a shared
menu primitive used by Captain MIDI and Calibr8or (blocked by the const bug).
Both the FW and QA audits independently name the same fix as the cheapest way to
widen coverage before a module-wide gesture change. I am keeping the numbering
for fidelity to the surviving addendum and flagging the disagreement rather than
silently renumbering. **Recommendation: do P4's first bullet first.**

---

## 9. Where this deviates from the owner's stated vision

Five places, each with the reason.

1. **No concurrent full apps, and no "deck".** The vision asks for several
   things running at once. Audio already does; CV rate cannot, on a 60 µs
   single-seat ISR with no scheduler. Rather than approximate it with a UI that
   implies it, PATCHBAY tells the truth in the one column where it matters
   (§5). I would rather ship a screen that is honest than one that is
   aspirational.
2. **No new discovery gesture.** The vision's instinct — make the panel
   self-explaining — is right, and CORDLESS follows it further. PATCHBAY keeps
   exactly one discovery surface, Z's card, because a second one is a second
   thing that can go stale.
3. **The routing UI takes an existing gesture rather than a new one.** `Z+encL`
   already means "I/O". This spends no gesture budget but it does mean the old
   I/O menu loses its top-level identity, which is a real change for anyone who
   knows where it lives.
4. **Gain/filter stop being per-app** (§4). This changes existing behaviour and
   can overwrite a deliberately-different per-app gain, once. It is the only
   place in this proposal where an existing user could lose something.
5. **A/B get no global meaning.** The original draft proposed one; the binding
   matrix shows it would silently delete ASR's freeze and Chords' navigation.
   Withdrawn.

---

## OPEN QUESTIONS FOR THE OWNER

1. **Is Z reachable on the real panel?** Unconfirmed
   (`UI-Redesign-Constraints.md` §7). This gates the `A`-alias retirement — if Z
   is unreachable and A is retired, there is no route between apps at all. It
   also already gates two shipped changes (the Z-only card, the Z+A
   screensaver). Cheapest thing in this document to settle; please settle it
   first.
2. **Does PATCHBAY replace the I/O settings menu on `Z+encL`, or sit beside it
   on `Z+X`?** Replacing spends no gesture budget and keeps the card at five
   rows; sitting beside it preserves the old screen exactly for anyone who
   knows it.
3. **Global-master jack settings: accept the one-time overwrite?** Do you have a
   preset with a deliberately different per-app input gain? If yes, P2 needs a
   one-shot migration that seeds the global from the currently-loaded app
   instead of the other way round.
4. **Quadrants' `B+Y` grid: absorbed into PATCHBAY, or kept as a fast path into
   it?** Absorbing is cleaner and guarantees one renderer; keeping it preserves
   a genuinely good at-a-glance 16-code density that the list layout loses.
   Absorbing reaches into the app with no automated test net until the const bug
   is fixed.
5. **Does the OUT page address 32 virtual channels or 8?** 32 lets any applet
   slot reach any jack; 8 keeps the page's vocabulary to letters people already
   know. The map costs 8 bytes either way.
6. **Should the app switcher's inverted selection bar stay inverted?** I argue
   yes — encR's turn *is* what changes it, so it is the boundary case, not a
   violation. But it will look like a missed conversion to anyone auditing P1,
   so I want the decision on record.
7. **Curation defaults: all 15 apps on at first boot, or a curated default set?**
   Starting all-on is safest; starting curated is a better first impression.
8. **Chords' `B` page-toggle -> `X`:** worth writing now, or leave it for
   whoever re-enables `ENABLE_APP_CHORDS`? Nothing in the shipping build
   compiles Chords today.
9. **Case-folding the label column** (CAPS = live, lowercase = frozen) changes
   how `GetIOConfig` strings read on every screen that shows them. Is that an
   acceptable use of the established rule, or should frozen state get a suffix
   instead?
