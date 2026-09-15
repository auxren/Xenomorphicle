# SENIOR — a migration-first button language for the Xenomorpher

*Seat: UI/UXR senior — the pragmatist and continuity advocate.*
*Status: awaiting Oren's greenlight. Nothing in this document is implemented.*

> **This is a re-run.** I delivered this proposal once already. That document
> was lost — the subagent transcript holding it became unreachable
> ("No transcript found for agent ID") after the parent session compacted and
> its job directory moved. Only my own addendum survived, preserved in
> [LOST-SEATS-addenda-and-briefs.md](LOST-SEATS-addenda-and-briefs.md) under
> "Sonnet senior addendum." That addendum said the proposal needed **no
> corrections**, only citation upgrades — so this reconstruction targets
> consistency with it: same recommendations, same disagreements, now with the
> exact file:line citations the addendum promised. Where this document and
> that addendum could conflict, the addendum wins; I found no such place while
> verifying against the tree.
>
> Since that first run, two things changed under me, and this document
> accounts for both: (1) I was briefed on the **26-finding Panel Audit**
> ([Established-Rules.md](Established-Rules.md)) for the first time — it is
> now load-bearing input, not something I re-derive; (2) the chord card
> shipped as **Z-only**, with A confirmed as a free ordinary app button
> (148/148 sim checks passing). I build on that; I do not re-propose it.
>
> Every code citation below was re-verified against `preset-bus` in this tree
> today (2026-09-03), not carried over unchecked. Line numbers may drift a few
> lines from the audit's own citations where commits have landed since; I note
> the ones that moved.
>
> **Mid-task addition:** [`docs/Panel-Binding-Matrix.md`](../Panel-Binding-Matrix.md)
> landed after my first drafting pass and is now folded in throughout —
> it is the only written record of bindings for the five apps the simulator
> cannot build (Quadrants, Hemisphere, Captain MIDI, Calibr8or, Scale Editor),
> and it supplies exact line numbers for the ASR/Chords A/B collision, the
> encL-overload sites, and a trap I had not accounted for: `CONTROL_BUTTON_A`
> is a bare alias of `CONTROL_BUTTON_UP` and `B` of `CONTROL_BUTTON_DOWN`
> (`OC_ui.h:53-56`), inherited from 2-button-shield hardware this panel does
> not have — and three apps (`NeuralNetwork.h:657-678`,
> `WaveformEditor.h:414-436`, `ScaleEditor.h:380-402`) bind `CONTROL_BUTTON_A`
> to a function *named* `OnDownButtonPress()`. I verified this directly:
> `NeuralNetwork.h`'s `HandleButtonEvent` reads `if (event.control ==
> OC::CONTROL_BUTTON_A && ...) OnDownButtonPress();` — the binding is A, the
> name says Down. Anyone implementing this proposal's later steps by reading
> those three files as a template will get the mapping backwards. I call this
> out explicitly wherever it bears on the sequence below, and it changes one
> risk rating in the SHIP SEQUENCE.

## Concept: **RATIFY**

The instrument already has a button language. It just isn't written down, and
it isn't followed consistently. `OC_app_base.cpp:46-51` states it in a code
comment:

> "Every global gesture on this panel is an unlabelled chord. A (or Z) plus a
> push of the RIGHT encoder opens the app switcher — the only route from one
> app to any other — A/Z plus the LEFT encoder opens I/O settings, both
> encoder pushes open the preset-bus overlay, and A+B means a different thing
> in each app that binds it."

My proposal's spine is: **write that comment into the panel, don't replace
it.** Every step below either (a) makes an already-shipped rule visible, (b)
closes a gap the rule has (inversion, encL-overload) using the rule's own
grammar, or (c) extends the rule to one new surface (routing) using a screen
that already exists. Nothing invents a new verb. That is why I call it
RATIFY rather than a new name for a new grammar — the deviation-from-CORDLESS
is the point, and I say so explicitly in the section that compares them.

---

## 1. A migration-first button language, as a sequence of safe steps

Ground rules for the whole sequence, taken directly from the audit's hazard
list so I don't reintroduce them:

- **Release-first is the only pattern for a new hold-to-commit gesture.**
  `OC_ui.h:152-172` documents `IgnoreUntilRelease()` as the fix for exactly
  this class of bug (the both-encoder RECALL leak, "airtight" under
  adversarial testing). Every step that adds a chord uses it; none add a
  parallel timer-based guard.
- **Confirm screens mask all four face buttons** except the designated commit
  control (Established-Rules, "Other established rules"). Any step that adds
  a confirm screen follows this without exception.
- **No seventh inversion meaning.** Steps that touch a screen's visual
  grammar apply the L-06 fix (below) or leave the screen's existing,
  already-wrong inversion alone rather than inventing an eighth.

### Step 0 — Fix the const-correctness bug (prerequisite, not a UI step)

**What changes:** one or a few `const` member-function signatures in the apps
the audit names as blocked (Captain MIDI, Calibr8or, Scale Editor) so
`arm-none-eabi-g++`'s permissiveness stops masking a real const violation that
Apple clang (and therefore `xeno-sim`) rejects.
**What breaks:** nothing observable. This is a header-only signature fix; if
it changes behavior at all, that behavior was already latent UB.
**Who notices:** nobody, on hardware. The build target doesn't even change.
**Rollback:** revert the commit; zero user-facing risk means zero rollback
cost.
**xeno-sim:** this step's entire purpose is to make xeno-sim buildable
against Captain MIDI, Calibr8or and Scale Editor — three apps and, per the
Established-Rules coverage gap, the reason Hemisphere/Quadrants have never
been reviewed at all. I sequence this **first**, ahead of every visible UI
change, because every later step in this document is only as trustworthy as
xeno-sim's coverage of it, and right now xeno-sim cannot see the default boot
app. This is the single highest-leverage, lowest-risk thing in the whole
proposal.

### Step 1 — Print the chord grammar Oren already has

**What changes:** the chord card (Z-only, shipped) gains no new triggers, but
every app's card content is audited against the `OC_app_base.cpp:46-51`
sentence so the five global chords (app switcher, I/O settings, preset
overlay, screensaver, A+B-is-per-app) are each represented somewhere
reachable from a card, using the row-budget the card already has (adaptive:
Z's own action, then A+B, then the screensaver hint only if room remains).
**What breaks:** nothing — this is a content pass over an already-shipped
mechanism, not new code paths.
**Who notices:** a first-time or forgetful user who holds Z and, for the
first time, sees the *entire* reachable vocabulary rather than a subset.
**Rollback:** trivial — it's card text.
**xeno-sim:** yes, directly — the sim already regression-tests the chord-guard
property and can assert card row content per app (`--dump-fb` + `fbtext.py`).

### Step 2 — Unify L-06 inversion, migrated screen by screen

This is where I depart from "list a rule" and instead **sequence a
migration**, because that is the actual hard part: six existing meanings, all
currently readable by a player mid-performance, cannot flip atomically
without the instrument lying to Oren's hands for one release.

The target grammar (Established-Rules, quoting `PresetBusUI.cpp:433` —
current line; the audit's `355` reference has drifted a few commits since):

> "one focus grammar: invert exactly what the right encoder will change"

Global fix, deferred, now adopted here:
- Inversion means "encR changes this." Nothing else.
- Cursors get a leading `>`.
- State gets a suffix or bracket, e.g. `[LOCKED]`.
- Banners get a drawn box (`graphics.drawFrame`), not an invert.

**Sequencing — one screen at a time, cheapest and least-visible first:**

1. **200e overlay itself** — already correct (it's the reference
   implementation the rule was extracted from). Zero-cost: document it as
   "done," ship nothing.
2. **Loop-point / mode inversions in apps not covered by xeno-sim** — flag as
   "needs Step 0 first," do not touch blind.
3. **State-warning inversions** (the "two meanings on one screen at once"
   case Established-Rules calls out) — convert to bracket suffix. This is the
   riskiest single screen because two things move at once; ship it alone, not
   bundled with any cursor change, so a regression is attributable to exactly
   one commit.
4. **Cursor inversions** (elsewhere in the instrument, meaning "here is where
   turning the encoder would land you," conflated with encR-focus) — convert
   to leading `>`. Lower risk: adding a `>` glyph doesn't remove information,
   it disambiguates it, so even an incomplete migration leaves the screen
   more readable than before, never less.

**What breaks, each round:** the *visual meaning* of an invert on that one
screen. This is the real cost: Oren's eye has muscle memory that "the black
box on this screen == this thing is selected," and step 3/4 change what an
invert *means* without changing what triggers it. That is a genuine
retraining cost, not a free refactor — which is why it ships as one screen
per release, never as a single instrument-wide flip day.
**Who notices:** Oren, every time he reads a screen mid-patch. This is the
step in the whole proposal most likely to cause a "wait, what does that mean
now" moment, and I am not going to pretend otherwise.
**Rollback:** per-screen — revert that screen's commit; other migrated
screens are unaffected since each is an independent draw-path change.
**xeno-sim:** partial. Screens the sim can build (Setup/About, 200e, Scenery,
Pong, Tweighty, Back It Up!) get an exact-glyph regression test asserting the
new bracket/`>`/box convention per screen. Screens gated behind Step 0
(Captain MIDI, Calibr8or, Scale Editor, Hemisphere, Quadrants) cannot be
regression-tested until Step 0 lands — another reason Step 0 sits first in
the sequence.

### Step 3 — The encL-long-press overload

Six confirmed sites overload encL-long-press with unrelated destructive or
service actions:

| Site | Action | Confirmed at |
|---|---|---|
| Preset-bus STORE | commit write (500 ms hold), broadcasts bus-wide | `PresetBusUI.cpp:270-280` |
| App-switcher DebugStats | enter diagnostic loop, exit only via encR | `OC_apps.cpp:967-978` |
| SETTINGS.h bootloader arm | offer reflash | `SETTINGS.h:516-532` |
| H1200.h reset to defaults | wipe scale/root config | `H1200.h:1037-1039` |
| Automatonnetz.h clear grid | ClearGrid + Reset | `Automatonnetz.h:747-749` |
| QQ.h / DQ.h copy scale/root | copy to other channels | `QQ.h:1484-1493`, `DQ.h:1393` |

(Cross-checked against `docs/Panel-Binding-Matrix.md`'s own encL-long-press
inventory, which lists the same six meanings independently — good
corroboration this is the closed set.)

**What changes:** nothing about *what* fires — this is a discoverability fix,
not a rebinding. Each site's footer legend (already-established grammar,
`A:toggle  B:env  R:edit` pattern) gets the encL action named explicitly
where room allows, and the chord-card content pass in Step 1 includes encL's
own card so a long-hold-in-progress is legible before it commits.
**What breaks:** nothing functionally. This is intentionally the least risky
step in the whole document — I am not consolidating six different meanings
into one, because that *would* be inventing a seventh grammar for L-06's
cousin problem, and would touch six independently-owned draw paths for a
cosmetic win. **I am choosing not to unify encL-long-press semantics**, only
to make each site's meaning visible before commit, which is cheap and doesn't
touch the destructive-action logic at all.
**Who notices:** anyone who has ever hit an encL long-hold by accident and
gotten a silent wipe. This directly reduces D-01..D-06-class unguarded
destruction risk without a rewrite.
**Rollback:** per-site, trivial (footer text / card content).
**xeno-sim:** only for the two simulator-reachable sites (app-switcher
DebugStats, preset-bus STORE) until Step 0 lands. QQ.h/DQ.h/SETTINGS.h/H1200.h
/Automatonnetz.h are Hemisphere-family and live in apps the sim cannot build —
another dependency on Step 0.

### Step 4 — The ASR-and-Chords A/B collision

Established-Rules flags "A+B means a different thing in each app that binds
it" as already-documented, intentional per-app polymorphism, not a defect.
`docs/Panel-Binding-Matrix.md` now gives this the exact grounding it lacked
in my first run. I re-verified it myself against source:

- **ASR is asymmetric, not just "different."** `ASR.h:872-989`: A (press) is
  `HandleTopButton()` — octave, toggle-based ±1. B (press) routes through
  `CONTROL_BUTTON_DOWN -> HandleLowerButton()`, and `HandleLowerButton()`'s
  entire body is `asr_.toggle_manual_freeze()` — **freeze/sample-and-hold, not
  octave** (confirmed at `ASR.h:883` for the dispatch, `ASR.h:952-954` for the
  handler body). A naive "A/B always means octave" read of this app is simply
  wrong for B.
- **Chords overloads A/B with menu-page navigation on top of octave.**
  `Chords.h:1162-1341`: A is octave ±1 *only* while `MENU_PARAMETERS` is the
  active page, and jumps to that page otherwise; B toggles
  `MENU_PARAMETERS <-> MENU_CV_MAPPING`, with a long-press that clears the CV
  mapping — a second destructive long-press site this proposal's Step 3 table
  above did not carry, because it lives on B, not encL. I'm flagging it here
  rather than folding it into Step 3's table, since Step 3 was scoped to
  encL specifically and this is a different control with a different owner
  (per-applet B, not the global encL).

Both are real, but they are exactly the case the `OC_app_base.cpp` comment
already accounts for: A+B is **scoped per app**, so two *different*
Hemisphere applets each giving A+B a different meaning — including ASR's own
internal A-means-octave/B-means-freeze asymmetry — is not a global-chord
collision, it's legal local polymorphism within each applet's own screen.

**What changes:** nothing structural. The fix is documentation, not
rebinding: each applet's own chord card (generalized in Step 1 to cover
Hemisphere-family apps once Step 0 unblocks the sim there) states its own
A+B meaning, so "ASR's A+B" and "Chords' A+B" each self-disclose and never
have to be told apart by a user holding one memory across two applets.
**What breaks:** nothing. This is the cheapest possible resolution and it is
cheap *because* the audit already settled that per-app A+B polymorphism is
sanctioned grammar, not an accident to fix.
**Who notices:** someone who was confused switching between the two applets
and expected one global A+B meaning — the card now tells them there isn't
one, immediately, instead of them discovering it by trial.
**Rollback:** trivial (card content, Hemisphere-family, Step 0-gated for sim
verification).
**xeno-sim:** blocked until Step 0 (Hemisphere-family). Flag explicitly:
this is one of the concrete reasons Step 0 is a prerequisite, not a nice-to-have.

### Step 5 — The 200e arm/confirm carve-out

This is the one place the instrument correctly does **not** follow "confirm
screens mask all four face buttons and accept only the designated commit
control" verbatim — the preset-bus overlay's STORE/RECALL holds are the
commit gesture itself, not a separate confirm screen layered on top, and
`store_needs_release`/`recall_needs_release` (cited as "airtight" under
adversarial testing) is the release-first implementation that makes a hold-
to-commit safe without a second screen. **I am not touching this.** It is
the reference implementation the rest of Step 2's screens should eventually
resemble, not a special case to reconcile with the confirm-screen rule — a
hold-to-commit *is* the commit, by design, and adding a second confirm
screen on top would be redundant ceremony on the one gesture in the
instrument already proven safe by a live incident and its fix.

`docs/Panel-Binding-Matrix.md` independently names this exact carve-out as
**must-not-rebind**: in `Bus200eApp.h:2589-2860`, A (press) *arms* a whole-
bank Write and encR (after the dead-window) *confirms* it — deliberately
different buttons, "because a fumbled `A+encR` is also the global
app-switcher chord, and a mis-commit writes 63KB to the wrong module." On the
confirm screens A+B is deliberately inert (`Bus200eApp.h:242-246, 421`) — not
an oversight, a second layer of the same guard. Any future step that
"standardizes" arm/confirm onto one button, or reactivates A+B there for
consistency with some other screen's grammar, breaks this carve-out on
purpose while believing it's cleaning something up. I'm naming it as
explicitly out of scope for every later step in this document, not just this
one.
**What changes:** nothing. This step is a call-out, not a code change:
future steps must not "fix" this carve-out into conformance with the
generic confirm-screen rule.
**xeno-sim:** already covered — the 200e write-path tests in
`tools/xeno-sim/selfcheck.sh` (write-fault injection, state-misreport checks)
exercise exactly this path today.

---

## 2. The minimum viable routing UI

### Where I start, and why

The constraints doc is explicit that the smallest real increment is:
generalize the shipped applet-hiding bitmask (`HS::hidden_applets[2]`,
`software/src/applets/_config.h:257`, a `uint64_t[2]` — 128 bits, currently
holding ~31 declared applets with room to spare) **to apps**, and let ANY app
opt its inputs into the **existing, shipped, type-safe** `CVInputMap` /
`DigitalInputMap` source-selection UI — the real screen at
`Hemisphere.h:1425-1458`, `DrawInputMappings()` — still **one app at a time,
no concurrency.**

I am not proposing a new routing screen. I am proposing that the screen that
already exists stops being Hemisphere-only.

### Mockup — reusing the shipped screen verbatim

The real screen (verified today, `Hemisphere.h:1425-1458`): header
`< Input Mapping >`, TR/CV icons over a 4-column grid, each cell a **3-char
type code** — `C 1` (physical CV 1), `D A` (self-patched from slot A's
output — `TYPE_DAC`), `M 1` (MIDI map slot 1), ` - ` (disabled) — a vertical
divider at x=64 splitting channels 1-2 from 3-4, and an inverted-box
attenuverter readout when editing. This is the grandchild pattern my
addendum flagged: reuse the 3-char code, not a longer label.

Extended to a non-Hemisphere app (21x8 budget; this is the SAME renderer,
called from a new site, not a new draw path):

```
<  Input Mapping  >
 [TR]      [CV]
 C1  M3    D A  - 
 T2  - -   C4  C2
--------------------
       125.0%
```

Row 1: header (unchanged). Rows 2-3: TR/CV icon row + the 4-column grid,
unchanged from Hemisphere's own screen. Row 4: divider. Row 5: attenuverter
readout when a cell is being edited (unchanged — `gfxDisplayInputMapEditor()`
already draws exactly this). Six rows used of eight; two spare, deliberately
— headroom for the "which app is this" indicator most non-Hemisphere apps
will need, since Hemisphere itself never has to ask (it *is* the applet
host).

### What the increment actually is

1. **Generalize `hidden_applets[2]` to apps.** The constraints doc's own
   math: T41_audio compiles 15 apps against a 128-bit mask built for ~31
   declared ones. A `uint32_t` (or reuse the existing `uint64_t[2]`
   directly) covers the full roster with room to spare — this is a toggle
   screen (`encR` toggles one, `encL` inverts all — already shipped grammar
   from the applet version, `OC_app_base.cpp` SHOWHIDELIST case) applied to
   one more list.
2. **Let any app declare it has CV/trigger inputs worth mapping**, and route
   its input-mapping gesture to the *same* `DrawInputMappings()` /
   `gfxDisplayInputMapEditor()` renderer Hemisphere already owns, rather than
   each app growing its own screen. This is the direct promotion the
   constraints doc calls for (avoid two different routing screens in one
   instrument) — the same convergence Quadrants' own B+Y prototype
   (`Quadrants.h:637-642`, `config_page = INPUT_SETTINGS`) already points at.
3. **Scope stays exactly where it is today: one app, no concurrency.** This
   increment does not touch the 60 us ISR budget, does not add a second live
   app, and does not require anything from the audio graph. It is a menu and
   a screen, not a scheduler.

### What breaks, who notices, rollback

**What breaks:** nothing existing. Hemisphere's own input-mapping screen is
untouched; this is additive — new call sites into an existing renderer, plus
a curation bitmask extended to a second list. **Who notices:** an app that
previously had no way to remap its physical inputs now has one, behind a
gesture that mirrors Quadrants' own B+Y precedent. **Rollback:** revert the
app-side call sites; the renderer and `CVInputMap` are untouched, so rollback
is at the call-site layer, not the shared-code layer — exactly the property
you want from "generalize, don't fork."
**xeno-sim:** the renderer is testable today wherever it's reachable from an
already-buildable app; testing it from Hemisphere-family apps is Step-0-gated
like everything else Hemisphere-adjacent.

### Extending later without redesigning what shipped

This is the part I was told is my key contribution, so I want to be explicit
about *why* it holds, not just assert that it does:

- **Outputs.** Today an applet's output is fixed by `io_offset` (`hemisphere
  * 2`) — position-owns-channel. The constraints doc calls this "the single
  biggest genuinely missing piece." Adding an output map does not touch the
  input screen above at all — it is a second, symmetric grid (`C1 D3 - -`
  for *destinations* instead of sources) using the same 3-char convention,
  same divider-at-x=64 layout, same toggle-and-invert curation pattern. The
  input screen does not get redesigned to make room for it; it gets a
  sibling.
- **Audio.** The audio graph (`AudioSummingRoute<2,3>`) is already a runtime-
  rewireable mixing primitive, proven in production by Quadrants/Tweighty/
  Sampler, and it is DMA-driven and fully decoupled from the CV-rate ISR.
  Exposing it as rows on this same screen (audio sources get a live meter
  where CV/trigger get a static type code, since audio truly is always-on
  concurrent and CV truly is not) is additive rows on the same grid, not a
  new mental model.
- **Multiple apps.** The screen stays single-app-scoped by design — this
  increment never promises otherwise. When cross-app routing is designed
  (out of scope here), the type-safe, position-addressed `CVInputMap` grid
  is exactly the substrate a multi-app view would read from; nothing about
  this increment has to be torn up to get there, because it never claimed
  concurrency it can't deliver.

### Where I deviate from CORDLESS, and why it matters to continuity

CORDLESS's "wiggle-to-patch" and live-meter picker are genuinely good ideas —
I'm not contesting the invention. But CORDLESS's Part 1 proposes
**generalizing hold-to-700ms into a universal rule for every button and both
encoder switches**, and reusing the newly-freed **A** button as a modifier
again (`hold-A -> card`, `A+encR-click -> PATCH`). That is the single
riskiest thing in CORDLESS from a continuity standpoint, and I want to be
concrete about why:

- **A was deliberately freed.** The constraints doc states plainly: "the
  chord card is now Z-only. Holding A draws nothing; A is a free ordinary app
  button." That wasn't an oversight — Established-Rules' PATCHBAY addendum
  independently documents that A-as-modifier caused a **real production
  collision already** (the splash-screen factory-erase chord had to be moved
  off A+encR onto A+B specifically because it collided with the app-switcher
  gesture, `OC_ui.cpp:296-305`). Re-overloading A as a global modifier
  reopens a class of bug this instrument has already been bitten by once.
- **It costs every app that currently treats A as an ordinary button its
  first tap.** CORDLESS's own end-to-end walkthrough opens with "holds A ->
  card appears." On an instrument where A is currently a free, ordinary,
  per-app button (used however each app likes), making "hold A" a global
  discovery gesture means every app's *existing* A binding now has to
  survive being preceded by a hold-detection window it didn't have before —
  a migration cost CORDLESS doesn't cost out, and one that touches every app
  that uses A, not just the routing screens.
- I'd ship CORDLESS's wiggle-to-patch picker — it's a real, testable, cheap-
  to-prototype idea, and CORDLESS itself proposes prototyping it standalone
  first, which I agree with — but I would **not** ship it riding on a
  reopened A-as-global-modifier. Land it as a picker-row behavior on the
  Step-2 screen above instead: same live-meter, same activity-sort, same
  "wiggle the thing you mean" law, invoked from the existing gesture that
  already reaches `DrawInputMappings()`, not from a new global hold.

---

## 3. Honest concurrency framing

Two completely different stories, and the proposal must not blur them:

**Audio concurrency is free and already works.** The audio graph runs DMA-
driven on ~2.9 ms / 128-sample blocks, fully decoupled from app switching.
`AudioSummingRoute<2,3>` already lets Quadrants, Tweighty and Sampler sound
simultaneously while another app owns the screen, and live audio-graph
rewiring inside `AudioNoInterrupts()` brackets is proven safe in production.
Nothing in this proposal needs to invent audio concurrency — it needs to
*expose* what's already running, per Step 2's later extension.

**CV-rate concurrency is not free, and no UI language can make it free.**
Every app's logic runs inside a single 16.666 kHz core ISR with a shared
**60 microsecond** budget covering display flush, DAC write, ADC scan and
digital-input scan. `AppSwitcher::current_app_` is one `RuntimeSlot` — one
seat. Quadrants is the only existing proof concurrency works there at all,
and only because Hemisphere applets are deliberately tiny; full apps own
exclusive resources (SD, USB MIDI, audio engine) with no arbitration layer,
and there is a documented real bug (the Tweighty background pump) from two
unsynchronized writers racing app state. Both FW and SW audits independently
recommend generalizing the existing lightweight-applet system rather than
building a concurrent-app runtime — which is exactly what Section 2's
increment does, and exactly what it does *not* try to do (it stays single-
app, on purpose).

Any button language that implies "run several full apps at CV rate at once"
is making a promise the ISR budget cannot keep. RATIFY makes none: routing
stays single-app; the only "several things running" claim it makes is about
audio, which is the one place the claim is actually true today.

---

## 4. Menu curation

Reuses the shipped pattern verbatim, no new mechanism:

- `hidden_applets[2]` (`software/src/applets/_config.h:257-263`) is a
  `uint64_t[2]` — 128-bit bitmask — with a toggle screen: encR toggles the
  item under cursor, encL inverts the whole selection
  (`OC_app_base.cpp` SHOWHIDELIST case, mirrored at `Hemisphere.h:1409-1411`
  for the applet-family's own copy). Persisted in bank globals.
- **Generalizing it to apps is the same data structure, applied to a second
  list.** T41_audio compiles 15 apps against a roster of ~31 declared
  classes — one bitmask this size covers it with room to spare; no new
  storage key, no format change.
- Curation UI: identical toggle screen, second entry point (or a second tab
  on the existing one), same `CAPS = active / lowercase = inactive`
  convention already established for the rest of the instrument.

```
 SHOW / HIDE APPS
>captain midi   HI
 calibr8or      HI
 SCALE EDITOR   
 scenery        
 tweighty       
 + 10 more...
L:invert     R:toggle
```

(Illustrative row content; exact roster pulled from the real app table once
Step 1's content pass runs. Footer deliberately does **not** reuse the A/B
Tweighty-style legend — this screen's own shipped logic
(`OC_app_base.cpp` SHOWHIDELIST case) only ever dispatches on encL/encR
turns, never A or B, so the footer follows the encoder-only precedent
already in the tree at `HSUtils.cpp:656,686`: `"L:cursor     R:adjust"`.
Labeling a button that does nothing on this screen would itself be an L-06-
adjacent lie — stating a control means something it doesn't.)

---

## 5. Test plan

**Is the const-correctness bug a prerequisite?** Yes, explicitly, and I want
to be blunt about the scope of what it blocks: Captain MIDI (the **default
boot app**), Calibr8or, Scale Editor, and — because it's the same root cause
— Hemisphere and Quadrants **in their entirety** are unreachable by
`xeno-sim`. That is not a corner of this proposal; Steps 2, 3 and 4 above are
each partially or fully gated on it, and Section 2's routing screen lives
inside `Hemisphere.h`. `docs/Panel-Binding-Matrix.md` names the exact sites: a
non-const member called from a const draw path at `CaptainMIDI.h:393-394`,
`Calibr8or.h:755,757`, `ScaleEditor.h:192-213` — `arm-none-eabi-g++` accepts
it, Apple clang (and therefore the simulator's host build) rejects it. Fixing
it (Step 0) is small and named, not an architecture change, and is the one
piece of this whole document I'd put ahead of every visible UI change,
because every later verification claim in this section is conditional on it.

**A related, not-a-prerequisite item: the `CONTROL_BUTTON_A`/`UP` alias
trap.** `OC_ui.h:53-56` aliases `CONTROL_BUTTON_A` to `CONTROL_BUTTON_UP` and
`B` to `DOWN`, a leftover from 2-button-shield hardware this panel doesn't
have. Three apps (`NeuralNetwork.h:657-678`, `WaveformEditor.h:414-436`,
`ScaleEditor.h:380-402`) bind `CONTROL_BUTTON_A` to a function *named*
`OnDownButtonPress()` — verified directly in `NeuralNetwork.h`. This is not
something xeno-sim catches for you (it exercises the binding, which is
correct; the trap is purely in a human reading the *name* while writing new
code against these three files as a template). It's a documentation/process
risk, not a code-under-test risk, so it doesn't gate any step above — but it
belongs in this test plan because "does the sim cover it" is the wrong
question for it. I'd handle it by never adding these three files to a
"reference implementation" list in any implementation ticket without a
one-line warning attached.

| What | xeno-sim can verify today | Needs hardware | Needs Step 0 first |
|---|---|---|---|
| Chord-card content (Step 1) | Yes — `--dump-fb` + `fbtext.py`, exact glyph decode | — | Only for Hemisphere-family app cards |
| L-06 inversion migration (Step 2) | Yes, per screen, for Setup/About, 200e, Scenery, Pong, Tweighty, Back It Up! | Confirm it *reads* right at arm's length (subjective) | Yes, for Captain MIDI/Calibr8or/Scale Editor/Hemisphere/Quadrants screens |
| encL-long-press visibility (Step 3) | Yes, for app-switcher DebugStats and preset-bus STORE only | — | Yes, for QQ/DQ/SETTINGS/H1200/Automatonnetz sites |
| ASR/Chords A/B card content (Step 4) | No — both are Hemisphere-family | Yes, until Step 0 | Yes |
| 200e arm/confirm carve-out (Step 5) | Yes — already covered by existing write-fault/state-misreport tests | Confirms real 251e BACKUP/RESTORE timing | No |
| Routing screen reuse (Section 2) | Yes, wherever the call site is in an already-buildable app | Confirm the physical CV jack labeling matches on-panel silkscreen, if any | Yes, for Hemisphere-hosted verification |
| Curation bitmask generalized to apps (Section 4) | Yes — same pattern the applet toggle already regression-tests | — | No |
| Z's real-panel reachability | No — this is a hardware-only unknown the whole chord-card and Step-1 design depends on | **Yes — unconfirmed today**, two prior bench attempts inconclusive | — |
| Audio concurrency claims (Section 3) | Partial — graph wiring logic, not audible correctness | Yes, for actual audio-quality verification | — |
| `CONTROL_BUTTON_A`/`UP` alias trap | N/A — not a behavior bug, a naming trap for implementers; the sim exercises the (correct) binding, not the (misleading) name | — | — |

**Two standing unverified items this proposal inherits rather than resolves**
(flagging, not re-litigating): Z's reachability on the real panel, and
Calibr8or's CV voltmeter sign/scale. Both are cited in the constraints doc as
cheap bench checks; both are load-bearing for pieces of this document (Z for
the entire chord-card premise; the voltmeter for anything that shows a live
CV meter, including CORDLESS's picker and my own Section 2 extension notes).

---

## Where I deviate from the owner's stated vision, and why

1. **I am not proposing a new button language.** The brief for this seat and
   the owner's own framing both point toward "design the button language,"
   and my answer is substantially "the button language already exists,
   write it down and finish it" rather than "here is a new one." I think
   that is the correct answer for a pragmatist seat, but it is a deviation
   from the shape of what was asked, and I want that named rather than
   buried in the concept name.
2. **I decline to unify encL-long-press into one meaning.** The brief lists
   this as something to "handle"; I handle it by making each site legible
   rather than collapsing six destructive/service actions into one gesture
   with one meaning, because collapsing them would itself invent a new
   cross-cutting grammar rule the way L-06 warns against, for a much smaller
   payoff than L-06 itself.
3. **I do not reopen A as a global modifier**, where CORDLESS does. This is
   the sharpest concept-level disagreement in this document, argued in full
   in Section 2.

## Position on per-app vs. global routing

**Keep the split, and label it — with a stated bias toward global for new
rows.** Today gain/filter/scaling are per-app (serialized inside each app's
own EEPROM chunk, `OC_app_base.cpp:468/474/481` — verified: `Save()`/
`Restore()` call `io_settings_.Save/Restore` then `SaveAppData/RestoreAppData`
separately, confirming the two are genuinely separate persistence homes, not
merely documented as such); cvmap/trigmap are HS globals. Un-splitting either
direction has a real migration cost against **existing saved presets**:

- **Everything-global** breaks the mental model that gain/filter/scaling are
  a per-instrument-slot performance setting (Tweighty's filter cutoff, say,
  tuned for that patch) — it would make switching apps re-point a tuned
  parameter, which is a worse surprise than the current split.
- **Everything-per-app** means cvmap/trigmap — proven type-safe, proven
  self-patching (`TYPE_DAC`), currently one small global table — becomes N
  copies, one per app, each independently driftable, and requires a genuine
  storage-format migration for every existing preset that currently reads
  the global table. That is real cost for a very small win (the only
  benefit is symmetry).
- **Keep the split, label it on screen** costs nothing in storage or
  migration — literally a text label change, since Step 0/Section 2's screen
  is new UI surface anyway — and it's the only option that doesn't touch a
  single byte of what's already saved on 30 preset-bus slots. Given this
  seat's mandate is "what does a storage change actually cost, and do
  existing presets survive it," the option that costs zero migration and
  keeps 100% of existing presets valid is the one I'd ship, even though it's
  the least architecturally satisfying of the three.

---

## SHIP SEQUENCE

1. **Fix the const-correctness bug** (Step 0). Risk: **none** — signature fix,
   zero user-visible behavior change. Value: unblocks xeno-sim on Captain
   MIDI/Calibr8or/Scale Editor/Hemisphere/Quadrants — the prerequisite for
   verifying everything after it.
2. **Chord-card content pass** (Step 1). Risk: **very low** — content only,
   on an already-shipped mechanism. Value: the instrument's existing chord
   grammar becomes discoverable for the first time.
3. **Curation bitmask generalized to apps** (Section 4). Risk: **very low** —
   identical data structure and UI to the already-shipped applet toggle.
   Value: Oren can hide apps he doesn't use from the switcher, same as
   applets today.
4. **encL-long-press visibility** (Step 3). Risk: **low** — footer/card text
   at six sites, no behavior change. Value: directly reduces accidental-
   destruction risk (D-01..D-06 class) without touching the destructive
   logic itself.
5. **ASR/Chords A/B card disclosure** (Step 4). Risk: **low**, gated on Step
   0 for verification. Value: resolves the named collision by disclosure,
   not rebinding.
6. **Minimum viable routing UI** (Section 2): generalize `hidden_applets`-
   style opt-in for CVInputMap access, promote `DrawInputMappings()` to a
   shared renderer any app can call. Risk: **medium** — new call sites into
   shared code, though the renderer itself is untouched. Value: the biggest
   single feature in this document — any app gets type-safe input remapping
   for the first time, on a screen Oren already knows from Hemisphere.
7. **L-06 inversion migration, screen by screen** (Step 2), in the order:
   200e overlay (already correct, document only) -> state-warning screens ->
   cursor-inversion screens. Risk: **medium-to-high** on the state-warning
   round specifically (two meanings resolving on one screen at once is the
   riskiest single change in this document); **low** on the cursor round.
   Value: closes the instrument's single most-cited open UI defect (L-06),
   the one this whole team was pointed at first. **Caution carried from the
   binding matrix:** this step is the one most likely to have an implementer
   reading many apps side-by-side as reference, which is exactly the
   situation the `CONTROL_BUTTON_A`/`UP` alias trap bites in — and
   `ScaleEditor.h` is both a Step-0 const-bug site (`:192-213`) and an
   alias-trap site (`:380-402`), so it will be touched twice in this
   sequence by two different people for two different reasons. Flag it once,
   here, so neither pass rediscovers it the hard way.
8. **200e arm/confirm carve-out**: explicitly no change — call it done, keep
   it as the reference implementation. Risk: **none** (this step is a
   decision, not a diff). Value: prevents a future well-intentioned
   "consistency" pass from breaking the one gesture already proven safe by a
   live incident.
