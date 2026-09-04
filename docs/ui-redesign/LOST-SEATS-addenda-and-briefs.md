# The two lost proposals: surviving addenda, and the briefs to re-run them

Of the three UI/UXR proposals, only [CORDLESS](CORDLESS.md) survives complete.
PATCHBAY (opus lead) and the sonnet senior's proposal are **gone** — their
subagent transcripts became unreachable ("No transcript found for agent ID")
once the parent session compacted and its job directory moved out from under
them. What survives of each is an addendum: errata written *against* a main
document that no longer exists.

**A proposal reconstructed from its own errata should not be put in front of an
owner for a greenlight.** These are preserved because the addenda are
independently substantive — they cite real code and correct real errors — not as
stand-ins for the proposals.

---

## PATCHBAY addendum (opus lead) — verbatim

> The audio-graph agent returned. It confirms the proposal's foundations and
> adds five corrections worth folding in — two of which strengthen the argument
> and one of which opens a genuine design question I got wrong.
>
> **1. App count correction — curation is smaller than briefed.** The ~31 figure
> is the DECLARED roster (33 classes, 32 reachable). The actual T41_audio build
> compiles 15 apps; plain T41 is 10. So menu curation needs exactly ONE
> `uint32_t`, with room to spare — my P3 estimate holds, and it covers the full
> declared roster even if every app is enabled.
>
> **2. Quadrants already has a Patchbay prototype — and I mis-assigned it.** B+Y
> in Quadrants opens `config_page = INPUT_SETTINGS; config_cursor = TRIGMAP1` —
> an existing input-mapping page over trigmap/cvmap (`apps/Quadrants.h:637-642`).
> This is the single most important addendum: **P0 should not invent a layout,
> it should PROMOTE this one.** The global Patchbay and Quadrants' input page
> must converge on one renderer, or the instrument ships two different routing
> screens. Recommend P0 extracts that page's drawing into a shared component and
> Quadrants' B+Y becomes a shortcut INTO the global Patchbay rather than a
> parallel screen.
>
> **3. Z chords are safe inside Quadrants by construction.**
> `CheckButtonCombo()` is `mask == combo && mask != last_mask`
> (`Quadrants.h:822-824`) — an EXACT mask match, not a subset test. Holding Z
> and pressing X yields `mask == Z|X`, which matches no Quadrants combo. So Z+X
> / Z+Y cannot collide with the largest app's eight bindings without any
> defensive work. This removes the main risk from the new-gesture proposal.
>
> **4. Retiring the A aliases has direct precedent in this codebase.** The
> splash-screen factory-erase chord was ALREADY MOVED OFF A+encR onto A+B
> specifically because it collided with the app-switcher gesture
> (`OC_ui.cpp:296-305` documents this). A-as-modifier has caused a real
> collision once already. That is the argument for retirement, and it is the
> project's own.
>
> **5. A correction I need to flag: persistence is split, and the brief was half
> right.** IOSettings is NOT in PhzConfig bank globals — it is a per-app member
> serialized inside each app's own app-data chunk
> (`OC_app_base.cpp:468/474/481`). Only cvmap/trigmap are HS globals. So the
> Patchbay spans two persistence homes with different scoping semantics:
> gain/filter/scaling are PER-APP, while CV source routing is GLOBAL. That is a
> real, user-visible inconsistency — one screen where half the rows follow you
> between apps and half don't. **This needs an owner decision.**
>
> One free win from the same report: `IO_SETTING_TRn` is already
> `STORAGE_TYPE_NOP` — a display-only row. The trigger-source assignment can
> land there with ZERO storage migration.
>
> Unchanged: the port-is-the-row model, the Z-only modifier rule, the
> outmap-at-gather-stage call for outputs, the three-tier concurrency honesty,
> and the P0-first sequencing. The agent's `AudioSummingRoute` findings confirm
> the unity-gain/no-normalization constraint (and add that Sampler nests its own
> `<2,8>` route inside its single slot), so the audio page's per-source level
> control stands.
>
> **REVISED OPEN QUESTIONS** (replacing item 4 of the earlier list, and adding
> one):
>
> - *(new, most important)* **Should routing be per-app or global?** Today
>   gain/filter/scaling are per-app and CV sources are global. Options: make
>   everything per-app (routing follows the app, more storage, breaks the "patch
>   the module once" mental model), everything global (one patch for the
>   instrument, but switching apps re-points live signals), or keep the split and
>   LABEL it on screen. I lean global-with-labels, but this is the single biggest
>   mental-model decision in the design and it is yours.
> - Does Quadrants' B+Y input page get absorbed into the global Patchbay, or kept
>   as a fast path into it? Absorbing is cleaner but reaches into the app with no
>   automated test net until the sim's const bug is fixed (my P4).
>
> The remaining six questions from the proposal stand as written.

**Note:** those "remaining six questions", the port-is-the-row model, the Z-only
modifier rule, the outmap-at-gather-stage design and the P0-first sequencing all
lived in the main document. They are referenced here but not recoverable.

---

## Sonnet senior addendum — verbatim

> Background verification finished and confirms the proposal I delivered — no
> corrections needed, but it fills in citations I'd left as "illustrative" or
> partially verified.
>
> **REAL HEMISPHERE INPUT MAPPING SCREEN** (`apps/Hemisphere.h:1425-1458`,
> `DrawInputMappings()`) — my section 2 mockup approximated the module's
> list-menu style; the actual shipped screen is different and more compact:
> header `< Input Mapping >`, TR/CV icons over a 4-column x 32px grid, each cell
> a 3-char code (`C 1` = physical CV 1, `D A` = self-patch from slot A's output,
> `M 1` = MIDI map slot 1, ` - ` = disabled), with a vertical divider at x=64
> splitting channels 1-2 from 3-4, and an inverted-box attenuverter readout
> (e.g. 125.0%) when editing. If we want the new app-level Source row's picker to
> feel like THIS screen's grandchild rather than a fresh invention, the 3-char
> type-code convention (C/D/M/T/-) is the thing to reuse verbatim, not the longer
> "Physical CV 4" text I wrote.
>
> **APP-SWITCHER CHORD DOC** (`OC_app_base.cpp:44-51`) states the grammar in one
> sentence I should have quoted directly: "A (or Z) plus a push of the RIGHT
> encoder opens the app switcher... A/Z plus the LEFT encoder opens I/O
> settings... A+B means a different thing in each app that binds it" — this is
> the exact two-layer rule my section 1 policy proposes, already half-written as
> a comment in the firmware. Adopting it as policy is closer to "ratify what's
> already documented" than "invent a new rule."
>
> **encL-LONG-PRESS NOW HAS FOUR MORE CONFIRMED SITES** beyond the two I'd
> cited, with exact locations: `QQ.h:1484-1493` and `DQ.h:1393` (copy scale/root
> to other channels), `SETTINGS.h:516-532` (bootloader arm), `H1200.h:1037-1039`
> (reset to defaults), `Automatonnetz.h:747-749` (clear grid) — plus the
> app-switcher's DebugStats (`OC_apps.cpp:967-977`) and preset-bus STORE I'd
> already cited. That's all six from the brief, fully grounded now — I'd flagged
> four as an "open bench-audit item"; they're actually code-confirmed and
> simulator-reachable only for the app-switcher/preset-bus pair (the other four
> live in apps the simulator doesn't build).
>
> **RELEASE-FIRST GUARD'S OWN DOC COMMENT** (`OC_ui.h:152-172`) independently
> states the same five-chord list my proposal treats as the closed set — good
> corroboration that I haven't missed a sixth global chord that would change the
> gesture-budget argument in my Disagreement #1.
>
> I'd treat these as citation upgrades to the proposal already delivered, not
> scope changes.

---

## The briefs, to re-run the two seats

The constraint section was **identical** across all three seats — it is the
content of [UI-Redesign-Constraints.md](../UI-Redesign-Constraints.md). Only the
ROLE differed.

Both seats were additionally told: give concrete text mockups (~21 chars x 6
rows) for every key screen, name the concept, and say explicitly where they
deviate from the owner's stated vision and why.

### Opus lead

> You are the UI/UX LEAD (most senior)... your proposal should be the most
> architecturally coherent.

Deliverable sections demanded:

1. **The button language** — a single consistent grammar; what each of
   A/B/X/Y/Z/encL/encR means by default in EVERY app and screen; what
   modifiers/chords mean; how an app extends it without breaking it; stating
   explicitly which existing gestures are kept / changed / retired and what each
   change costs.
2. **The routing model** — where the UI lives, what gesture opens it, concrete
   128x64 text mockups, how type-safety is communicated, and how a user sees
   what is already patched ("the hardest part on a screen this small").
3. **The concurrency story**, presented honestly.
4. **Menu curation.**
5. **Migration / sequencing** — what ships first, second, third, and the
   smallest increment delivering real value.

Ends with "OPEN QUESTIONS FOR THE OWNER."

### Sonnet senior

> the pragmatist and the continuity advocate... optimize for shipping
> incrementally without breaking the instrument for its existing user, and for
> respecting muscle memory.

Deliverable sections:

1. **A migration-first button language as a SEQUENCE of safe steps**, each with
   what changes / what breaks / who notices / rollback / whether the simulator
   can verify it — explicitly handling the ASR-and-Chords A/B collision, the
   encL-long-press overload, and the 200e arm/confirm carve-out.
2. **The minimum viable routing UI**, built on the SW lead's recommended
   smallest increment (generalize the applet-hiding bitmask to apps; let ANY app
   opt its inputs into the existing type-safe `CVInputMap` source-selection UI,
   still one app at a time, no concurrency) with mockups — plus how it extends
   later to outputs/audio/multiple apps WITHOUT redesigning what shipped
   (forward-compatibility was named as its key contribution).
3. **Honest concurrency framing.**
4. **Menu curation**, reusing the shipped applet pattern.
5. **A test plan** mapped to what the simulator can verify vs. what needs
   hardware, including whether the const-correctness bug is a prerequisite.

Ends with a numbered "SHIP SEQUENCE", each increment with user-visible value and
risk level.

---

## Also held by the other session, not yet relayed

Three feasibility audits in full — FW (ISR/audio/MIDI reality), SW (app-vs-applet
architecture and existing routing precedents), and QA (**a complete per-app
button-binding matrix across all 33 apps**, plus a doc-vs-reality discrepancy
table for `docs/Hemisphere-Gestures.md`). The load-bearing findings are digested
in [UI-Redesign-Constraints.md](../UI-Redesign-Constraints.md); the QA binding
matrix in particular was compressed hard and can be relayed in full on request.
