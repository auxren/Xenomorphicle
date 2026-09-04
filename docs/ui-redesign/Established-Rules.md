# Established UI rules a new proposal must build on, not re-derive

Distilled from the three existing artifacts. A design proposal for this module
is not writing on a blank page: 26 numbered findings have already been made,
adversarially verified, and mostly shipped. **A proposal should state which
findings it assumes as baseline rather than treating the instrument as
pre-audit.**

## The three artifacts, and which is which

| Artifact | What it actually is |
| --- | --- |
| **Xenomorpher UI Review** (`9d39f0b8`) | A Claude Design canvas, 6 artboards (Main / Navigation / ModuleSelect / ActionRow / WriteConfirm / WriteScope). A review **plus** a concrete redesign proposal for whole-instrument navigation and the 200e app. |
| **Xenomorpher Panel Audit** (`a13f0023`) | **The load-bearing one.** A formal ~800-line audit in plain prose, dated 2026-09-01 against `preset-bus @ 79bda444`: four independent UX reviewers plus an adversarial code-trace pass, 26 numbered findings (D-01..D-06 unguarded destruction, L-01..L-06 screen lies, F-01..F-07 friction, C-01..C-06 code), a "what is already right" section, and a shipped/deferred status table. |
| **Tweighty Screens** (`e831c90f`) | Not a review. Two pixel-exact 128x64 mockups (SCR_HOME, SCR_EDIT) annotated with real firmware draw calls and real field values. Pure screen-spec reference. |

## L-06 — the single most important open finding

`PresetBusUI.cpp:355` **already states the intended rule**:

> one focus grammar: invert exactly what the right encoder will change.

The preset overlay honours it. **Nothing else does.** Inversion currently means
**six different things** across the instrument — encR target, encL cursor, state
warning, loop point, mode, and banner — two of them on the same screen at once.

The recommended global fix, status **DEFERRED, still open**:

- Inversion means "encR changes this", period.
- Cursors get a leading `>`.
- State gets a suffix or bracket.
- Banners get a drawn box.

**Any new proposal must adopt that rule or explicitly argue against it. It
cannot invent a different inversion convention without becoming the seventh
meaning.**

> **This is a live problem for [CORDLESS](CORDLESS.md).** That proposal leans
> hard on visual state — filled/hollow/blinking jack glyphs, live meters on
> every picker row, an occupied-jack render, a carried-cord indicator. Its
> author was **not** briefed on L-06, because the briefing session did not know
> the finding existed. CORDLESS needs an explicit pass against the inversion
> grammar before it goes in front of an owner. Brief any re-run seat on L-06 up
> front.

## Other established rules

- **Release-first / entry-gesture-leak.** Identified from a real hardware bug:
  the both-encoder chord left encR held, and the overlay's 250 ms RECALL fired
  on it, causing an unrequested **bus-wide recall** across every 200e module in
  the case. The recommended fix is a **global** rule applied at screen entry,
  not per-screen patches. The current `store_needs_release` /
  `recall_needs_release` implementation was called "airtight" under adversarial
  testing — treat it as the reference implementation for any hold-to-commit
  gesture.
- **Confirm screens mask all four face buttons** and accept only the designated
  commit control.
- **Write vocabulary distinguishes states precisely:** WRITING -> VERIFYING ->
  WROTE + VERIFIED; "WRITE LOST" vs "WRITE FAILED"; "BAD: OTHER PRESETS!" vs
  "BAD: N bytes wrong".
- **Refusals state fact + remedy in one line.** `busy: scan (L stops)` is cited
  as the best example in the tree.
- **The overlay's 27px zero-padded 7-segment slot numeral** is "the standard to
  measure against" for anything read at arm's length.
- **Footer legend grammar** `A:toggle  B:env  R:edit` is established practice,
  seen independently in both the 200e app and Tweighty. Do not introduce a
  competing footer convention.
- **Screen budget: 21 characters x 8 rows** at the 6x8 font. The chord card's
  own rows are 9px-pitched, which is why *that* screen caps at ~6 rows and hard-
  caps at `rows[5]`. Both figures are right in their own context — they are not
  contradictory.

## Hazards that must not be reintroduced

- A two-step confirm **whose two steps are the same button** (arm = A, commit =
  encR — both on the primary navigation chord); measured committing in as little
  as **6-51 ms** of contact.
- **encL means "cancel" everywhere EXCEPT on `[RESTORE]`**, where it is
  destructive.
- **Unlabeled B in the app switcher** silently flips a global (encoder
  acceleration) with 4 pixels of feedback.
- **Encoder acceleration threshold (~150 detents/sec) is unreachable by hand.**
  Any encoder-driven list design must address wrap and acceleration explicitly.
- **The bus-wide RECALL hold (250 ms) is SHORTER than the local, harmless STORE
  hold (500 ms).** The audit calls that backwards.

## A protocol constraint that kills a whole class of UI

Decoded from the 251e's own AVR32 firmware (see the WriteScope artboard):

**There is no per-slot write.** `RECALL(0x01)` / `SAVE(0x02)` touch only live
panel state and carry no data. `BACKUP(0x04)` / `RESTORE(0x05)` are the only
data-moving commands and they move **the whole 63,120-byte bank**. So every
write is inherently *read-all-30 -> modify-1 -> write-all-30*.

Two unexplored commands (`0x07`/`0x08`, 32-bit BE arg) **might** be a per-slot
route, but the 259e's equivalent setters turned out to be dead or miscompiled,
so that needs disassembly before anyone trusts it.

**Any "save just this slot" UI is architecturally wrong, not merely redundant.**

## Status ledger

Most D/L/F/C findings are marked **shipped** in commits after the audit.
Explicitly still **DEFERRED**:

1. Inversion-grammar unification (L-06).
2. A panel gesture for Export / Import.

## Coverage gap

**Quadrants, Hemisphere, Captain MIDI, Calibr8or and Scale Editor were NOT
reviewed** — a build/simulator limitation caused by the same const-correctness
bug described in [UI-Redesign-Constraints.md](../UI-Redesign-Constraints.md)
section 5. **Captain MIDI is the default app on hardware.**

This is an explicit gap, not an implied endorsement of those apps' UI. It is the
same gap the QA audit flagged, from the same root cause — which is the strongest
argument for fixing the const bug before any large gesture refactor.
