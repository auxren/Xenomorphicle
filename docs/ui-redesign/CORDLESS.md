# CORDLESS — the invisible patchfield, made touchable

Design proposal for the module-wide UI language, from the "inventive seat" of
the three-person UI/UXR team. **Status: awaiting Oren's greenlight. Nothing
here is implemented.**

Preserved verbatim (ASCII-transliterated from the original box-drawing mockups;
the shapes matter, the exact glyphs must come from `icons.h` and weegfx
primitives anyway). The other two seats' proposals were lost — see
[UI-Redesign-Constraints.md](../UI-Redesign-Constraints.md).

## The point of view

The Xenomorpher lives in a Buchla case, surrounded by banana jacks. Its problem
is that its own patchfield is **invisible**: routing lives inside a struct
(`CVInputMap`), not on the panel. Every framing the other seats will reach for —
"assign a source to a variable", "configure the routing table" — imports a
programmer's mental model into a patching culture. CORDLESS refuses that import.

Every app input, app output and physical jack is a **JACK**; every route is a
**CORD**. You grab a cord at one end, carry it, and plug it in at the other —
the motor sequence of physical patching, translated to one cursor. This does
three jobs the "assignment" model cannot:

1. **Type safety becomes jack geometry, not validation.** A banana only fits a
   banana jack. You can never *see* an incompatible destination, so an invalid
   route is un-expressible rather than rejected. (Already true in firmware —
   `CVInputMap` is type-safe by construction. The UI should *inherit* that
   shape, not re-explain it.)
2. **The mix/no-mix rule becomes physical intuition.** In banana culture you
   stack outputs into one input freely, but never stack into a voltage source.
   So: audio out jacks accept **stacks** (mixing is free — already true in the
   audio graph); a CV out jack holds one banana, and a full jack simply reads as
   full. No error message for CV mixing will ever exist, because the concept has
   no gesture that could request it. When CV mixing arrives, the jack just
   starts accepting stacks.
3. **"Connected" becomes visible the way a real cord is: it moves.** On a 1-bit
   128x64 screen you cannot draw the graph — but you can draw the *signals*,
   live, and let the user follow them.

> **The law:** if you can point at it, you can patch it; if it's moving, you can
> see it; if you hold it, it explains itself.

## Part 1 — Discoverability: "hold to ask", and cards you can press

The module has one existing discovery affordance: hold Z 700 ms -> the chord
card. That is not a feature, it is a **seed**. Generalize it into a universal
rule: **hold any control ~700 ms and the screen becomes that control's label.**

Every button and both encoder switches get a card, generated from the same
binding table the actions dispatch from (the `kChordGloss` pattern already in
`OC_app_base.cpp`) — so cards are accurate by construction, never stale
documentation.

**The inventive step: cards are live menus, not signs.** While a card is up
(modifier still held), pressing any control *listed on the card* executes it. A
chord is learned by doing it: hold, read, press.

```
+- A -----------------+
| tap    (app action) |
| +click> PATCH       |
| +turn > volume      |
| +Z     mute cord    |
| release: nothing    |
+---------------------+
```

This *strengthens* the release-first guard that exists because of the bus-wide
preset-recall accident: **new chords fire only from a visible card.** A fumbled
grab lasts under 700 ms, never sees a card, and therefore cannot fire anything.
Deliberation enforced by time, not hope. (An expert "quick chords" toggle can
later waive the card; ship without it.)

Gesture budget spent: hold-A (card) and A+encR-click (PATCH) — using the newly
freed A — plus context taps of A *inside* CORDLESS screens. Nothing else. Z's
chords, both-encoders and Quadrants' legacy chords are untouched.

## Part 2 — The visualization crux: never draw the graph, always show the signal

A many-to-many graph on 6 rows of 21 characters is unwinnable as a *diagram*.
CORDLESS wins it as **three views of one truth**, turned with encL inside the
PATCH overlay.

### View 1 — THE CASE (where is everything?)

A panel mirror. Each jack is one glyph: hollow = free, filled = patched,
**blinking = signal moving now**. Bottom row is the PROBE.

```
IN  @o@o oooo  T ^^~~
OUT ##<o oooo  A ##

     probe v
CV1 < Loops.outA
_-=-_,_-=#=-_,_-=-_,_
```

- Turn encR: the cursor walks jacks **in panel order** — the screen *is* the
  panel, so eight unlabeled inputs stop being anonymous.
- The probe shows exactly one hop. Click encR on a patched jack to **walk the
  cable**: the probe rewrites to the far end's provenance
  (`Loops.outA < pitch < MIDI.CC1`), one hop per click. You never see the whole
  graph; you *trace* it, like running a finger along a cord in a dense patch.
- The bottom meter is a live scope of the probed signal, from values already in
  `frame`.

### View 2 — THE APP (what does this app need?)

The jack strip. Per declared jack: name, far end, live 4-char meter. `>` input,
`<` output, `--` unpatched, `+` a stack.

```
 SCENERY    -- jacks
>pitch =CV1     _-=-
>gate  =TR2     __#_
>warp  =Vib.CC1 ~~~~
<outL  =AU L+   =_-_
<outR  =--
```

An unpatched jack is not an error — it is a hollow jack, waiting, like the
panel. But **visible**: the strip is the app's honest dependency list, and a
performer sees at a glance what is wired, what is silent, and what is actually
carrying signal right now.

### View 3 — THE CORDS (what have I patched?)

Flat cord list, scrollable, the tidy-up view. `=/=` marks a muted cord.

```
 CORDS 5        _-=-
 CV1<Loops.outA
 CV2<Loops.outB
=TR2>Scen.gate
 CC1>Scen.warp
 AUL<Loops+Scen (2)
```

## Part 3 — Making a patch: grab, carry, plug — and wiggle-to-patch

Click encR on any jack, in any view (or hold-A + click while the app cursor sits
on a patchable parameter). You are now **holding a cord.** Two things happen:

**1. The picker appears showing only jacks the cord fits** — type-safety as
un-expressibility; the list is literally `CVInputMap::ordered_types` rendered.

```
>warp  plug into:
 o --      (unplug)
!@ CV IN 3  _=#=
 o CV IN 1  ____
 o MIDI CC.  learn
 o Loops.outA ~~~
```

**2. Every candidate row carries a live meter — and the list sorts by recent
activity.** This is the gem: **you choose a source by seeing it move.** The
panel is unlabeled and the case has dozens of banana runs, so don't make the
user *know* that the 251e's pressure output landed on CV IN 3. Have them
**wiggle** it. The wiggling input leaps to the top with a `!`, meter dancing.
Click. Done.

This generalizes the MIDI auto-learn that already exists into a universal law:
**wiggle the thing you mean.** The most Buchla-native gesture here —
performative, physical, and it turns the module's worst liability (8 anonymous
inputs) into a two-second act.

**Occupied CV outs — repatching without a fight.** A full CV jack appears in the
picker rendered as occupied with its current owner, and takes a *second* click
to steal:

```
<outB  plug into:
 o CV 4    (free)
 # CV 3 <Scen.outL
   click again to
   repatch CV 3
```

The consequence is shown **before** it happens. No "conflict state" can ever
exist on screen, because the only way to create one resolves it in the same
gesture. Audio destinations instead show `AU L (2)` and simply accept the stack.

**Carrying the cord across apps** — the routing UI nobody else will propose.
While holding a cord, the top-right shows a dangling-plug glyph and the cord's
name, and **the cord survives navigation.** Grab `Loops.outA`, press Z+encR (the
existing app switcher), land in Scenery, cursor onto `warp`, click — plugged.
Cross-app routing is not a screen; it is a **walk**, with the cord visibly in
hand.

```
 SCENERY       ~outA
>pitch =CV1     _-=-
>gate  =TR2     __#_
>warp  =--   <plug?
<outL  =AU L+   =_-_
<outR  =--
```

Z (or hold-A) drops the cord anywhere, harmlessly. Feasibility:
app-output-to-app-input already exists as `TYPE_DAC` sources; the genuinely new
storage is the tiny 8-entry physical-CV-out map, and persistence rides the
existing ~250-byte preset key.

## Part 4 — Performability: pull the banana, keep the voltage

Setup is between-pieces work. Live, two one-tap moves on any cord row:

- **Tap A: mute the cord.** Banana half-pulled, glyph flips to `=/=`, and the
  input **holds its last value** — a superpower physical cords don't have, since
  pulling a real CV cord slams you to 0 V. Tap again: reconnect, live.
  Non-destructive, instant, reversible — the safe version of the classic Buchla
  cord-yank.
- **Turn encL on a cord row:** ride the attenuverter (already in `CVInputMap`) —
  a per-cord performance fader with a live meter under your eyes.

And because routing persists on preset keys, the both-encoders 200e preset
overlay already makes **whole patchfields** recallable from the 225e/226e bus —
scene-level repatching as a performance gesture, for free.

## Part 5 — Many things at once, honestly

Audio concurrency is free and stays invisible-but-audible; the CV lane is a solo
seat. **The UI must not lie about that.** The app switcher becomes THE DECK,
showing what is *actually* running, with live glyphs — exactly one app wears the
`cv` badge:

```
 RUNNING
>Loops   _-=-  au
 Scenery ~~~~  cv
 Captain       midi
 Pong
 + more.    (curate)
```

- What this seat *wants* — several full apps at CV rate — is ruled out by the
  60 us ISR budget, and it won't pretend otherwise. The affordable
  approximation: **applet-weight apps run headless** (the Hemisphere scheduler
  already proves 2-4 fit in budget). Apps declare their weight; light ones keep
  their cords warm off-screen, heavy ones **surrender the `cv` badge visibly**
  when focus moves — the deck shows the badge *hop*, so the performer always
  knows who owns the lane. No silent death of a modulation source, ever.
- Curation (`+ more.`) reuses the existing hidden-items bitmask verbatim.

## Interaction sequence, end to end (cold user, unlabeled panel)

1. Curious, holds A -> **card** appears: `+click> PATCH`.
2. Still holding A, clicks encR -> **THE CASE** view; encL pages to **THE APP**
   strip.
3. Cursor to `>pitch`, clicks encR -> **picker**, cord in hand.
4. **Wiggles** the 251e output they want -> `! CV IN 3 _=#=` jumps to top,
   dancing.
5. Click. Strip reads `>pitch =CV3 _=#=` — moving, therefore believed.
6. Mid-performance, cursor to that row, taps A -> `=/=`, pitch freezes at last
   value; taps A -> it breathes again.

Six steps, zero labels, zero manual, one accident-proof chord.

## What to prototype first

**Wiggle-to-patch:** the live-meter, activity-sorted picker. It is the riskiest
novel claim (that motion + geography beat names on an unlabeled panel) and the
cheapest to test — a standalone test screen fed by real `frame` ADC data, no
routing writes needed.

On the Orin bench with the 251e/259e patched in: give five subjects (or one
owner, five trials) tasks of the form "patch *that* module's output into this
input", with inputs deliberately un-memorized. Measure **time-to-correct-patch**
and **wrong-jack rate** against a control picker listing CV IN 1-8 as bare
names; simultaneously verify per-row meter redraws hold the display frame budget
with all rows animating.

**Success bar:** under 10 seconds, near-zero wrong jacks, no frame overruns. If
wiggle-to-patch wins there, the rest of CORDLESS — cards you can press, the
probe row, the carried cord — is low-risk craft on a proven foundation.
