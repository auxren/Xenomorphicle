# L-06 inversion-grammar unification — ordered migration plan

Branch `preset-bus`, read 2026-09-03. **Every line number below was opened and
read in the current working tree.** Where I could not construct a concrete
failure I say so and downgrade.

> **Line-number drift warning.** Another agent is actively editing
> `software/src/` right now. `git status` at read time showed `Main.cpp`,
> `OC_app_base.cpp`, `OC_strings.cpp`, `OC_ui.cpp`, `apps/Calibr8or.h`,
> `apps/SETTINGS.h` modified. Line numbers in those six files may have moved by
> the time this is actioned; the *identities* (function + surrounding comment)
> are stable and are quoted where it matters.

---

## 0. Three facts that change the shape of the plan

These were not in the brief and each one moves work between steps.

### 0.1 `invertRect` is XOR, and two overlapping inversions CANCEL

`software/src/src/drivers/weegfx.cpp:204-210`:

```cpp
void Graphics::invertRect(coord_t x, coord_t y, coord_t w, coord_t h)
{
  CLIPX(x, w); CLIPY(y, h);
  draw_rect<PIXEL_OP_XOR>(get_frame_ptr(x, y), y, w, h);
}
```

(`drawRect` at `:190-195` is `PIXEL_OP_OR` — a *solid* fill, not an outline.)

This turns H-2 from a grammar violation into a **functional defect**. See §2.1.

### 0.2 The real blast radius of H-1 / H-2 is the AUDIO applets, not Hemisphere

The brief says "inherited by every Hemisphere/Quadrants applet". Inherited yes;
**called** almost nowhere in `applets/`. I grepped every call:

- `HemisphereApplet::gfxPrint(CVInputMap&)` is called from `applets/Combin8.h:124`
  and from **~30 audio applets** (`gfxPrint(mix_cv)`, `gfxPrint(pitch_cv)`, …
  — each of those identifiers is a `CVInputMap` member; verified for
  `audio_applets/DelayApplet.h:459-467`, `GlitchApplet.h:363-373`,
  `ModalResonatorApplet.h:322`).
- `HemisphereApplet::gfxPrint(DigitalInputMap&)` has exactly **11** call sites,
  all in `audio_applets/`:
  `AdvKrpsStrngApplet.h:101`, `AnimorfApplet.h:273`, `DelayApplet.h:193`,
  `FMDrumApplet.h:406`, `GlitchApplet.h:152`, `:163`, `:168`,
  `MistierApplet.h:183`, `ModalResonatorApplet.h:192`,
  `OneShotPlayerApplet.h:269`, `WAVPlayerApplet.h:157`, `WAVRecorderApplet.h:138`.

So H-1/H-2 ship inside the **Audio Applet subapp** (T41_audio), which is the
default-ish surface on this instrument, not inside the 60 CV applets.

### 0.3 Every one of the three shared functions exists TWICE

There is a second, near-identical copy of each in the `HSApplication` /
free-function family, which the brief does not mention:

| Defect | HemisphereApplet copy | Second copy |
|---|---|---|
| H-2 gate-high inversion | `HemisphereApplet.cpp:235-238` | `HSUtils.cpp:792-795` |
| H-1 24px CV line | `HemisphereApplet.cpp:239-245` | `HSUtils.cpp:796-802` |
| H-3 inverted banner | `HemisphereApplet.h:345-370` | `HSApplication.h:265-290` |

`HSUtils.cpp:792-802` sits **outside** the `#ifndef NO_HEMISPHERE` block (that
block is `:708-736`), so it is compiled by `xeno-sim` — but its only reachable
callers are `HSUtils.cpp:365` (`DrawMidiMaps`) and `HSUtils.cpp:392`
(`DrawConfigRow` case 3), and both are reached only from `apps/Hemisphere.h` and
`apps/Quadrants.h`. **Compile coverage in the sim, zero render coverage.**
`HSApplication.h:288` is an in-class (implicitly inline) member; no sim app
odr-uses it, so the sim does not even compile it.

Fixing only the `HemisphereApplet` copy leaves the violation live on the
Hemisphere/Quadrants config screens. Both copies must move together.

---

## 1. Taxonomy — what inversion means in this tree today

I found **176 `gfxInvert`/`invertRect` lines**; 7 are declarations, definitions
or commented-out, leaving **169 live draw sites**. They fall into ten meanings,
not six:

| Code | Meaning | L-06 verdict | Prescribed replacement |
|---|---|---|---|
| **T** | encR will change this | **LEGAL — keep** | — |
| **C** | encL / selection cursor, encR does something else | violation | leading `>` (or an existing non-inverting bullet) |
| **S** | latched state (frozen, armed, enabled, bypassed, follow, write-mode) | violation | suffix / bracket / filled-vs-hollow box |
| **G** | live gate/trigger is high (**the seventh meaning**) | violation + XOR bug | see §2.1 |
| **P** | playhead / step index / loop point | violation | caret or bullet |
| **M** | mode / page / which-half-of-the-app | violation | drawn box or suffix |
| **B** | banner / header emphasis | violation | drawn box |
| **V** | meter or bar **fill** (inversion used as a drawing primitive) | out of scope | leave |
| **F** | transient flash / animation (< ~250 ms) | mostly leave | leave, except where it cancels a cursor |
| **D** | decoration / progress | leave | leave |

**Reference implementations already in the tree** (copy these, do not invent):

1. `PresetBusUI.cpp:433-450` — "one focus grammar: invert exactly what the right
   encoder will change". Cited in Established-Rules; verified at line 433.
2. `HSUtils.cpp:527-530` — `if (cur) { gfxIcon(73, y, RIGHT_ICON); if (editing)
   gfxInvert(82, y-1, 45, 10); }`. Cursor = `>` icon, inversion = encR target.
   **This is L-06 already shipped, in shared code, on the Hemisphere config
   menu.** It is the single best template for the `C` class.
3. `apps/Bus200eApp.h:1503-1554` (`DrawModuleSelect`) — encR target (`Addr`
   digits) inverted at `:1505`; encL list selection marked with a non-inverting
   3x3 bullet `graphics.drawRect(0, y + 2, 3, 3)` at `:1553`.
4. `applets/TruthCat4.h:337-346` — the author already migrated off the inverted
   header, and left the reasoning in the comment ("Simplest: just fill the
   region black with gfxRect then draw text in white").

---

## 2. The three shared-code sites, re-verified

### 2.1 H-2 — `gfxPrint(DigitalInputMap&)` — **CRITICAL, and worse than reported**

`software/src/HemisphereApplet.cpp:235-238`:

```cpp
FLASHMEM void HemisphereApplet::gfxPrint(DigitalInputMap &map) const {
  gfxPrintIcon(map.Icon());
  if (map.Gate()) gfxInvert(gfxGetPrintPosX()-8, gfxGetPrintPosY(), 8, 8);
}
```
Duplicate at `HSUtils.cpp:792-795`. Brief's claim **confirmed**: this is a
seventh meaning.

**But there is a concrete failure path, not just a grammar complaint.** Take
`audio_applets/GlitchApplet.h:162-164`, page row 1:

```cpp
gfxStartCursor();                 // records cursor_start_x/y = current print pos
gfxPrint(hold_input);             // -> icon, then XOR the 8x8 cell if gate high
gfxEndCursor(cursor == HOLD_SRC, false, hold_input.InputName());
```

Trace the rects:
- `gfxPrintIcon` (`HemisphereApplet.cpp:342-345`) draws the icon at
  `(cursor_start_x, cursor_start_y)` and advances printPosX by 8.
- H-2 therefore inverts **exactly** `(printPosX-8, printPosY, 8, 8)` =
  `(cursor_start_x, cursor_start_y, 8, 8)`.
- `gfxEndCursor` (`:290-296`) computes `w = printPosX - cursor_start_x` = 8,
  `y = printPosY + 8`, `h = y - cursor_start_y` = 8, and calls `gfxCursor`,
  whose edit branch (`:157-158`) is `gfxInvert(x, y - h, w, h)` =
  `(cursor_start_x, cursor_start_y, 8, 8)`.

**Identical rect. XOR twice. 100% cancellation.**

Reproduction: patch a sustained gate into Glitch's HOLD source, put the cursor
on that row, press encR to enter edit mode. The edit highlight **disappears for
as long as the gate is high** — i.e. exactly while you are performing. The same
shape exists at `MistierApplet.h:183` (spicy cursor), `ModalResonatorApplet.h:192`,
`GlitchApplet.h:152`, `:168`, `DelayApplet.h:193`, `AnimorfApplet.h:273`,
`WAVRecorderApplet.h:138`, `WAVPlayerApplet.h:157`, `OneShotPlayerApplet.h:269`,
`FMDrumApplet.h:406`, `AdvKrpsStrngApplet.h:101`.

**The fix prescribed in Audio-Apps-Screens.md §6.4 is WRONG for this call site.**
It says `drawRect(x,y,8,8)` high / `drawFrame(x,y,8,8)` low. That spec was
written for a *bare gutter box at x=34*. Here the 8x8 cell **already contains
the icon glyph**: `drawRect` is an OR fill (`weegfx.cpp:190-195`), so it would
paint the cell solid and erase the icon; `drawFrame` would overwrite the icon's
outer ring. Do not apply §6.4's text verbatim.

Minimal fixes that actually work, in order of my preference:

- **(a) Corner pip.** `if (map.Gate()) gfxRect(gfxGetPrintPosX()-3, gfxGetPrintPosY()+6, 2, 2);`
  — 2x2 OR-fill in the icon's bottom-right corner. Zero width cost, no XOR, and
  it survives being inverted by the cursor (contrast is preserved either way).
  Needs a pixel check that no icon in `PARAM_MAP_ICONS` / `DIGITAL_INPUT_ICONS`
  already lights that corner; **unverified**, confirm by dumping the icon table
  or an `--dump-fb` capture.
- **(b) Keep the inversion but make the cursor not cancel it.** Would require
  `gfxCursor` to use a non-XOR emphasis. Much larger blast radius (every applet).
  Not recommended as a first move.

**Risk: cosmetic + bug-fixing.** No bounding box changes, no print position
changes, no width change. **Hardware-only verification** (`HemisphereApplet.cpp`
is not in `xeno-sim`'s `FW_SRCS`, Makefile `:47-93`).

### 2.2 H-3 — `gfxDisplayInputMapEditor()` inverted banner — **confirmed, and cheap**

`software/src/HemisphereApplet.h:345-370`, ending at **`:368`
`gfxInvert(0, 0, 63, 11);`**. Confirmed exactly as briefed. Duplicate at
`HSApplication.h:265-290`, ending at **`:288`**.

Note what the code actually does: `gfxClear(0,0,63,11)` at `:347`, print the
value, then invert → white bar, black text. And `gfxHeader` bails out early when
this is up (`HemisphereApplet.cpp:363: if (IsEditingInputMap()) return;`), so
nothing underneath is being flipped. So the banner is self-contained.

Replacement `gfxFrame(0, 0, 63, 11)` gives white text on black inside a 1px box.
Geometry check: the frame's border occupies x=0, x=62, y=0, y=10. Text is
printed at y=2 (`:351`, `:357`) so it spans rows 2..9 — inside. Horizontally the
CV case starts near x=11 and the digital case at x=20 — inside. **No collision.**

**Risk: purely cosmetic.** Same 63x11 bounding box, no print position moves, no
width change. **Hardware-only** for both copies (see §0.3 — the `HSApplication`
copy is not even compiled by the sim).

Reach: 37 call sites of `gfxDisplayInputMapEditor()` — 33 audio applets,
`applets/Combin8.h:60`, `apps/Quadrants.h:499`, `:1365`, `:1474`,
`apps/Hemisphere.h:811`, `:1457`, `:1538`.

### 2.3 H-1 — `gfxPrint(CVInputMap&)` 24px line — **confirmed, and worse at 8px pitch**

`software/src/HemisphereApplet.cpp:239-245` (duplicate `HSUtils.cpp:796-802`):

```cpp
  const int xpos = gfxGetPrintPosX() - 1;
  const int ypos = gfxGetPrintPosY() + 4;
  const int height = map.InRescaled(24);
  gfxLine(xpos, ypos, xpos, ypos - height);
```

`InRescaled` (`CVInputMap.h:109-111`) is a plain `Proportion(In(), FS, 24)` — it
is **signed**, so a negative CV makes `height` negative and the line is drawn
*downward* instead. So the bleed is bidirectional, up to 24px each way.

The brief says "10px pitch". The worst real case is tighter: `GlitchApplet.h:135`
draws rows at **`15 + i * 8`** — an 8px pitch. A full-scale CV on row 2 (y=31)
draws from y=35 up to y=11: through row 1, row 0, and into the header strip.
**Three rows plus the header.**

**Risk: NOT purely cosmetic.** Any bounded replacement changes what the row looks
like and, if you adopt the `fxDrawBipolarBar` from Audio-Apps-Screens §1.5, it
changes the *width* of the field from 1px to 24px, which does not fit a 64px
half-screen applet. Minimal in-place fix that changes no width:
clamp the height to the row, e.g. `const int height = constrain(map.InRescaled(24), -6, 6);`
— 1px wide, 6px tall, stays inside an 8px row. **Hardware-only.**

I rank H-1 **below** H-2 and H-3: it is ugly and it is a real misread, but I
cannot construct a case where it destroys information the way H-2's cancellation
does. Downgraded to High-minus.

---

## 3. Two more shared-code sites the brief did not list

### 3.1 `HemisphereApplet::DrawConfigHelp()` — six inverted label chips, EVERY applet

`HemisphereApplet.cpp:62-85` inverts three 19x9 chips per channel, six per
screen: `:64` (trigger map name), `:73` (CV map name), `:82` (output letter).
These are **labels** (class `B`), and this screen is raised by
`BaseView(full_screen=true, parked=false)` (`:36-41`) for **every applet in the
build** — a larger literal reach than H-2 or H-3.

Nothing on that screen is an encR target, so there is no ambiguity *within* the
screen; the cost is that a user who has learned "inverted = encR" elsewhere reads
six false targets. Replacement: `graphics.drawFrame(ch*64, y-1, 19, 9)`, same
rect. **Purely cosmetic**, hardware-only. Medium priority.

### 3.2 `HS::DrawPopup()` inverts the WHOLE popup, and does it inconsistently

`HSUtils.cpp:531-694`. Two sites:

- `:653` (QUANTIZER_POPUP) — `gfxInvert(px, py, pw, ph)` **only when `q_edit`**.
- `:692` (MIDI_POPUP) — the same call, **unconditionally**, outside the
  `if (midi_edit)` block that closes at `:690`.

So the quantizer popup inverts while editing and the MIDI popup inverts always.
Two sibling popups, same geometry (`:521-525` gives both `px=14, py=23, pw=100,
ph=28`), opposite rules. Meanwhile the *actual* encR target inside each popup is
marked by a separate `UP_BTN_ICON` caret (`:647`, `:651`, `:681`, `:683`), which
is the correct L-06 mark — and the whole-box inversion then flips that caret too.

Class `B`/`M`. Callers: `apps/Quadrants.h:1699`, `apps/Hemisphere.h:887`,
`apps/CaptainMIDI.h:457`, `apps/Calibr8or.h:503`. **Hardware only.**

**Risk: not purely cosmetic.** The right fix is to drop both inversions and let
the existing `drawFrame(px+1, py+1, pw-2, ph-2)` at `:575` carry the box — that
frame is already drawn. That is a one-line delete each and it *removes* pixels
rather than adding, so no width hazard. But it changes the look of four apps'
most-used overlay, so it wants an owner's eye before it ships.

---

## 4. Full enumeration — all 169 live sites

Legend as §1. **Sim** = does `tools/xeno-sim` build *and* reach it. The sim
builds with `-DNO_HEMISPHERE -DENABLE_APP_BUS200E -DENABLE_APP_PONG
-DENABLE_APP_SCENES -DENABLE_APP_TWEIGHTY` (Makefile `:41-43`) plus the two
unconditional apps (`apps/_config.h:67-68`: `Backup.h`, `SETTINGS.h`).
`HemisphereApplet.cpp` is **not** in `FW_SRCS` (Makefile `:47-93`).

### 4.1 Shared code / framework

| file:line | meaning | reaches | sim | verdict |
|---|---|---|---|---|
| `HemisphereApplet.cpp:64` | B | every applet, config-help screen | no | fix (§3.1) |
| `HemisphereApplet.cpp:73` | B | " | no | fix |
| `HemisphereApplet.cpp:82` | B | " | no | fix |
| `HemisphereApplet.cpp:158` | **T** | `gfxCursor` edit branch — every applet | no | **KEEP** |
| `HemisphereApplet.cpp:179` | **T** | `gfxSpicyCursor` edit branch | no | **KEEP** |
| `HemisphereApplet.cpp:237` | **G** | 11 audio-applet rows | no | **H-2, critical** |
| `HemisphereApplet.cpp:289` | **T** | `gfxEndCursor` labelled branch | no | **KEEP** |
| `HemisphereApplet.cpp:355` | V | `gfxSkyline` output bar fill | no | leave |
| `HemisphereApplet.cpp:401` | **T** | `DrawSlider` edit branch | no | **KEEP** |
| `HemisphereApplet.h:368` | B | 37 call sites | no | **H-3** |
| `HSApplication.h:124` | V | HSApplication channel meter | Scenery/Tweighty | leave |
| `HSApplication.h:204` | **T** | `gfxCursor` edit branch | yes | **KEEP** |
| `HSApplication.h:215` | **T** | `gfxSpicyCursor` edit branch | yes | **KEEP** |
| `HSApplication.h:288` | B | Hemisphere/Quadrants only | no | **H-3 duplicate** |
| `HSUtils.cpp:367` | S | MIDI-map "enabled"; cursor on same screen is `LEFT_ICON` (`:371`) | no | fix |
| `HSUtils.cpp:529` | **T** | config-menu value edit | no | **KEEP — reference impl** |
| `HSUtils.cpp:598` | S | `(auto)` autosave state inside MENU_POPUP | no | fix |
| `HSUtils.cpp:653` | B/M | whole quantizer popup while `q_edit` | no | fix (§3.2) |
| `HSUtils.cpp:692` | B/M | whole MIDI popup, **unconditional** | no | fix (§3.2) |
| `HSUtils.cpp:732` | **T** | applet show/hide list; encR toggles this row | no | **KEEP** |
| `HSUtils.cpp:794` | **G** | `DrawMidiMaps`/`DrawConfigRow` | compiled, not reached | **H-2 duplicate** |
| `HSUtils.cpp:796-802` (line) | H-1 | " | compiled, not reached | **H-1 duplicate** |
| `AudioAppletSubapp.h:152` | **T** | encR picks the applet for that slot | no | **KEEP** |
| `AudioAppletSubapp.h:695` | V | 1px peak meter | no | leave |
| `HemisphereAudioApplet.h:140` | S | `offset == 0` (no detune) | no | low |
| `OC_menus.h:265` | **T** | title-bar column selection | yes | **KEEP** |
| `OC_menus.h:328,344,366,388,403,415,425,437` | **T** | settings-list `selected` row; encR edits it | yes | **KEEP (8 sites)** |
| `OC_app_base.cpp:369` | B | chord-card title, inside an existing `drawFrame` (`:365`) | yes | fix, low risk |
| `OC_apps.cpp:1076` | **T** | app switcher; encR scrolls + commits | yes | **KEEP** |
| `OC_ui.cpp:326` | S | splash: which boot branch the held button gives | yes | **leave, see §6** |
| `OC_ui.cpp:332` | S | " | yes | **leave** |
| `OC_ui.cpp:352` | D | splash progress bar | yes | **leave** |
| `OC_scale_edit.cpp:123` | **T** | `EDIT_ROOT` field | compiled, not reached | **KEEP** |
| `OC_scale_edit.cpp:125` | **T** | transpose field | compiled, not reached | **KEEP** |
| `OC_chords_edit.h:226` | C | quality-grid cursor cell | no | medium |
| `OC_chords_edit.h:228` | **T** | value cell; already `drawFrame` when not editing (`:230`) | no | **KEEP** |
| `PresetBusUI.cpp:435,437,444,447,448` | **T** | the reference implementation | yes | **KEEP (5 sites)** |
| `Main.cpp:216,221,226,231` | S | USB boot-mode choice; **no encoder on that screen** | no (sim has its own `main.cpp`) | **leave, see §6** |

### 4.2 Apps

| file:line | meaning | screen | sim | verdict |
|---|---|---|---|---|
| `Bus200eApp.h:1505` | **T** | MODULE_SELECT `Addr`; encR changes `addr_` (`:2948-2951`) | yes | **KEEP — reference impl** |
| `Bus200eApp.h:1635,1658,1666,1687,1694,1727,1740` | S | refusal / WRITING / VERIFY / BAD / FAIL banners at y=45 | yes | leave or box; see §6 |
| `Bus200eApp.h:1707` | S | `EDITED*` | yes | leave |
| `Bus200eApp.h:1764` | B | WRITE-confirm title | yes | leave or box |
| `Bus200eApp.h:1887,1890` | **C** | `recover_cursor_`, moved by **encL** (`:2925-2931`) | yes | **fix — see §5 step 1** |
| `Bus200eApp.h:1901` | **C** | action row, moved by **encL** (`:2937-2941`), while encR changes `slot_` (`:2898-2921`) and `slot_` is NOT inverted (`:1823`) | yes | **fix — highest-value app finding** |
| `Bus200eApp.h:1927` | B | UNDO-write title | yes | leave or box |
| `Bus200eApp.h:2027` | S | `END` marker present | yes | low |
| `Bus200eApp.h:2071` | **C** | generator cursor, **encL** (`:2883-2887`), encR adjusts value | yes | fix |
| `Bus200eApp.h:2122` | S | `ARMED` | yes | low |
| `Bus200eApp.h:2131` | S | `FULL` | yes | low |
| `Bus200eApp.h:2324` | **C** | 259e row cursor, **encL** scroll (`:2932-2936`) | yes | fix |
| `Bus200eApp.h:2347` | D | single "Read" entry, always inverted | yes | fix (says nothing) |
| `SETTINGS.h:880` | B | REFLASH/RESET title; comment already says "inversion is the only emphasis here" | yes | leave or box |
| `SETTINGS.h:994` | **T** | `bus_addr_edit`; the comment at `:988-991` states L-06 verbatim | yes | **KEEP — reference impl** |
| `Backup.h:267` | B | "OVERWRITE EEPROM" | yes | leave or box |
| `TweightyApp.h:646` | **T** | encL row = encR value (Audio-Apps §1.1) | yes | **KEEP** |
| `TweightyApp.h:685` | **T** | selected tap; encR adjusts it | yes | **KEEP** |
| `Scenery.h:566` | S | *active* scene (CV-driven), while encR edits a value | yes | fix, low radius |
| `Scenery.h:592` | V | output meter | yes | leave |
| `UsbDriveApp.h:195,199` | C | menu cursor; actuated by B, no encR target on screen | no | low |
| `UsbDriveApp.h:208` | B | confirm title | no | low |
| `SamplerApp.h:458` | **T** | Tweighty idiom | no | **KEEP** |
| `SamplerApp.h:470` | **BUG** | see §4.4 | no | **fix** |
| `TunerApp.h:272` | S | MIDI-out off | no | low |
| `Quadrants.h:452` | **M** | "applets 3 and 4 get inverted titles", full-screen | no | fix |
| `Quadrants.h:463` | **T** | input-map edit target (+ dotted `gfxFrame` at `:464`) | no | **KEEP** |
| `Quadrants.h:1492` | S | `!SDcard_Ready` | no | low |
| `Quadrants.h:1682` | **M** | same title inversion on the 2-up main screen, **co-resident with every applet's own T-class cursor** | no | **fix — this is the "two meanings on one screen" case** |
| `Hemisphere.h:775` | **T** | input-map edit target | no | **KEEP** |
| `Calibr8or.h:746` | **C** | channel tab = `sel_chan` | no | **fix — co-resident with `:827`** |
| `Calibr8or.h:827` | **T** | `HS::q_edit` scale field | no | **KEEP** |
| `ScaleEditor.h:205` | C/T | current note; verify binding | no | medium, **unverified** |
| `Enigma.h:479` | S | memory >32 steps warning | no | low |
| `Enigma.h:525` | **T** | step-number cursor, blinking | no | KEEP |
| `Enigma.h:660` | B | selector-box header row | no | low |
| `NeuralNetwork.h:319` | **G** | input gate high | no | fix with H-2 |
| `NeuralNetwork.h:331` | **G** | output high | no | fix with H-2 |
| `Automatonnetz.h:650` | P | grid playhead | no | low |
| `TheDarkestTimeline.h:432` | F | record-mode blink | no | leave |

### 4.3 Applets (`applets/`) — the long tail

All hardware-only. None of these is co-resident with a *different* inversion
meaning except where noted. **My recommendation is to touch almost none of them
in this migration** (see §6).

- **T (keep):** `BugCrack.h:415`, `Scope.h:205`, `Seq32.h:382`, `SequenceX.h:200`,
  `EbbAndLfo.h:252`, `WTVCOApplet.h:295`.
- **S (state):** `Tuner.h:168` (in tune), `Brancher.h:120` (flipflop mode),
  `TwoRings.h:564` (reset active), `Shredder.h:240`,`:250` (shred-on-reset),
  `EnvSeq.h:1099` (follow), `CVSeq.h:729` (channel follow), `Voltage.h:128`
  (view), `Carpeggio.h:204` (shuffle), `Seq32.h:332` (write mode),
  `MidiLoop.h:273` (overdub), `Scope.h:105` (freeze — inverts a 64x40 region),
  `Fungen.h:446`, `VectorEG.h:167`, `VectorLFO.h:198`, `VectorMod.h:122`,
  `VectorMorph.h:132`, `ADSREG.h:293` (channel letter chips).
- **P (step/loop):** `DivSeq.h:251`, `DivSeq10.h:242`, `MidiLoop.h:270`,
  `ProbabilityDivider.h:251`, `PolyDiv.h:188`.
- **F (flash):** `MarkoV.h:255`, `MarkovPerc.h:282`, `ClockSetupT4.h:364`,
  `ProbabilityDivider.h:259`, `ProbabilityMelody.h:440`,`:449`, `Seq32.h:386`,
  `DrumMap.h:355`, `CVRecV2.h:232`.
- **V (meter/graphic fill):** `BootsNCat.h:196`, `BugCrack.h:419`,`:511`,
  `LowerRenz.h:131`, `RndWalk.h:246`,`:248`, `Relabi.h:321`, `Stairs.h:330`,
  `Strum.h:158`, `Xfader.h:44`, `MultiScale.h:181`, `ScaleDuet.h:163`,`:171`,
  `ProbabilityMelody.h:356`, `hMIDIIn.h:273`,`:310`, `hMIDIOut.h:395`.
- **Audio applets:** `GlitchApplet.h:161` (S), `MistierApplet.h:182` (S),
  `ModalResonatorApplet.h:190` (S), `WAVRecorderApplet.h:135` (S), `:179` (S clip),
  `WTVCOApplet.h:377` (S osc reverse), `AnimorfApplet.h:307` (V),
  `InputApplet.h:118` (V), `WAVPlayerApplet.h:174` (V),
  `HarmOscApplet.h:99`,`:114`,`:128`,`:157` (M — which partial page),
  `HarmOscApplet.h:170` (T).
- **Commented out, no action:** `Stairs.h:325`,`:329`, `Burst.h:310`,
  `DrumMap.h:304`, `DelayApplet.h:229`.
- **Comment only:** `TruthCat4.h:340`, `TweightyApp.h:600`.

### 4.4 One outright rendering bug found in passing

`apps/SamplerApp.h:466-474`:

```cpp
const bool playing = file_loaded_[s] && players_[s].isPlaying();
if (playing) {
  graphics.drawRect(x, kRingY, 8, 6);
  graphics.invertRect(x, kRingY, 8, 6);
} else if (file_loaded_[s]) {
  graphics.drawRect(x, kRingY, 8, 6);
} else {
  graphics.drawFrame(x, kRingY, 8, 6);
}
```

`drawRect` is an OR fill and `invertRect` is XOR (`weegfx.cpp:190-210`), so
**playing = solid, then XOR = every pixel off**. A playing slot renders as a
*blank hole*; a loaded-but-idle slot renders solid. The header comment at
`:409-413` states the intent ("A slot currently playing gets its box inverted")
— so the comment documents the bug. Also note `:478` draws the selection frame
at `(x-2, kRingY-2, 12, 10)` which is unaffected.

Minimal fix: `playing` → `drawRect`; `file_loaded_ && !playing` → `drawFrame` +
`drawRect(x+3, kRingY+2, 2, 2)`; empty → `drawFrame`. Zero width change.
**Hardware only** (Sampler is not in the sim's app set).

---

## 5. The ship sequence

Each step is independently shippable, independently revertible, and touches a
disjoint set of files.

### Step 0 — instrument the sim first (SIM)
Add `--dump-fb` golden captures for the Bus200e MODULE_HOME action row, the
Tweighty edit screen and the Setup/About confirm screen, and run
`edgecheck.py` on each. Cost: none to the firmware. Value: every later
sim-verifiable step becomes a diff instead of an opinion, and it establishes the
21-column clip baseline before anything gets wider.

### Step 1 — Bus200e MODULE_HOME: put the inversion on the encR target (SIM)
**Files:** `apps/Bus200eApp.h` only.
This is the strongest single fix in the tree that the simulator can prove, and
the app is internally contradictory today: `DrawModuleSelect` (`:1505`, `:1553`)
is L-06-perfect, `DrawModuleHome` is L-06-backwards.

- `:1823` `Slot %d` (the encR target, `:2898-2921`) gets
  `graphics.invertRect(79, 12, 44, 10)`. Width check: `Slot 30` is 7 chars from
  x=80 → x=80..121; 79+44=123 < 128. **Safe.**
- `:1901` action-row cursor becomes non-inverting.
  **WIDTH HAZARD — do not use a leading `>` here.** The action row is exactly
  124px wide today: `(4*6+4)+(4*6+4)+(3*6+4)+(3*6+4)+(4*6) = 124`, and the
  author's own comment at `:1894-1895` records that a 6px gap already spilled to
  132. A 6px `>` pushes it to 130 and the last glyph of "Save" is drawn and
  clipped — invisible to `fbtext.py`, catchable only by `edgecheck.py`.
  Use `graphics.drawFrame(x - 1, 55, w + 2, 9)` instead: identical bounding box,
  zero width cost. **Pixel check required:** the frame's bottom rule lands on
  y=63, the same row as the glyph cell's last row. If any of
  `Read/Edit/Gen/Rec/Save` lights row 7 of its 8-row cell the two will touch.
  I did not read `gfx_font_6x8.h` to confirm — **unverified**; the sim capture
  from Step 0 answers it in one diff.
- `:1887`, `:1890` (`recover_cursor_`, also encL) and `:2071` (`gen_cursor_`,
  also encL) and `:2324` (`row_cursor_`, encL) get the same treatment. `:2324`
  in particular already carries a long comment about how much damage a
  one-pixel-taller band did; a frame is strictly safer than a band there.
- `:2347` — the 259e single "Read" entry is inverted unconditionally and
  therefore communicates nothing. Delete the inversion.

**Risk:** not purely cosmetic — bounding boxes are preserved but the *rendering*
of five cursors changes. No print position moves, no field widens.
**Verification: SIMULATOR**, end to end.

### Step 2 — H-3, the inverted banner (HARDWARE)
**Files:** `HemisphereApplet.h:368`, `HSApplication.h:288`. Both, together.
`gfxInvert(0,0,63,11)` → `gfxFrame(0,0,63,11)`.
**Risk: purely cosmetic.** Same rect, no reflow (geometry checked in §2.2).
**Verification: HARDWARE ONLY.** `HemisphereApplet.cpp` is absent from
`FW_SRCS`, and `HSApplication.h:288` is an in-class member no sim app odr-uses,
so the sim will not even type-check the edit. Bench test: open any audio applet,
put the cursor on a CV row, press encR twice to enter the attenuverter editor.

### Step 3 — H-2, the gate inversion (HARDWARE)
**Files:** `HemisphereApplet.cpp:237`, `HSUtils.cpp:794`. Both, together.
Replace with the corner pip of §2.1(a). Optionally fold in
`NeuralNetwork.h:319`/`:331`, which express the same `G` meaning.
**Risk: cosmetic, and it fixes the XOR cancellation.** No width change.
**Verification: HARDWARE ONLY**, and the *specific* regression test is the one
from §2.1: sustained gate + cursor on that row + edit mode; the highlight must
now stay visible.
Ship this **after** Step 2, not before: Step 2 proves the round trip (edit →
flash → look) on a change with zero behavioural surface, so if Step 3's pip
lands on top of an icon pixel you already know your test loop is good.

### Step 4 — Quadrants' inverted titles (HARDWARE)
**Files:** `apps/Quadrants.h:452`, `:1682`.
This is the canonical "two meanings on one screen": the title band means "this
is slot 3 or 4" (`M`) while every applet drawn underneath is simultaneously
using inversion to mean "encR edits this" (`HemisphereApplet.cpp:158`, `:289`,
`:401`).
Replacement must not widen: the band is `(h*64, 0, 63, 10)`, and the applet name
is right-aligned to x=62 on odd hemispheres (`HemisphereApplet.cpp:374-376`), so
there is **no left margin for a `>`** and no right margin either. Use a suffix
glyph or a 1px `gfxFrame(h*64, 0, 63, 10)`.
**Risk: not purely cosmetic** — the frame's rules at y=0 and y=9 sit adjacent to
the header text row and to the applet's first content row at y=13. Needs a
framebuffer look. **HARDWARE ONLY.**

### Step 5 — Calibr8or channel tabs (HARDWARE)
**Files:** `apps/Calibr8or.h:746` (and leave `:827` alone).
`:746` inverts the whole 11px channel tab for `sel_chan` (an encL/button
selection) while `:827` correctly inverts the encR target. Replace `:746` with
`gfxFrame(1 + x, y, w - 1, 11)` — the tab already has hand-drawn rules at
`:728`, `:748`, `:749`, so a frame fits the existing idiom.
**NOTE:** `Calibr8or.h` is currently being edited by another agent; re-verify
before touching. **Risk: cosmetic.** **HARDWARE ONLY** (Calibr8or is blocked
from the sim by the const bug described in UI-Redesign-Constraints §5).

### Step 6 — `DrawConfigHelp`'s six label chips (HARDWARE)
`HemisphereApplet.cpp:64`, `:73`, `:82` → `graphics.drawFrame(ch*64, y-1, 19, 9)`.
**Risk: cosmetic** — but this is the highest *literal* reach of any change here
(every applet), so ship it after 2 and 3 have proven the bench loop.

### Step 7 — `HS::DrawPopup` (HARDWARE, wants an owner's eye)
`HSUtils.cpp:653`, `:692`. Delete both; the `drawFrame` at `:575` already boxes
the popup. Also fixes the quantizer-vs-MIDI inconsistency of §3.2.
**Risk: behavioural-looking** — four apps' most-used overlay changes appearance
in one commit. Not a code risk; a taste risk. **HARDWARE ONLY.**

### Step 8 — H-1, the 24px CV line (HARDWARE)
`HemisphereApplet.cpp:243`, `HSUtils.cpp:800`. Clamp to the row
(`constrain(map.InRescaled(24), -6, 6)`), do **not** import the 24px-wide
`fxDrawBipolarBar` — it does not fit a 64px half screen.
**Risk: cosmetic, information-reducing** (the meter loses resolution). Last
because it is the only one of the four shared fixes that trades away a feature.

### Step 9 — Sampler ring bug (HARDWARE)
`apps/SamplerApp.h:466-474`, §4.4. Independent of everything else; can be
pulled forward at will. **HARDWARE ONLY.**

### Steps NOT scheduled
The ~90 `S`/`P`/`V`/`F` applet sites in §4.3. See §6.

---

## 6. What NOT to do

1. **Do not add a leading `>` to the Bus200e action row.** Verified 124px of 128
   already used (§5 step 1). The 22nd character is drawn and clipped and
   `fbtext.py` cannot see it — that is exactly the failure `edgecheck.py`'s
   header documents. The same hazard applies to `Quadrants.h:452`/`:1682` (a
   full 63px half-width band with a right-aligned title) and to
   `Bus200eApp.h:2324` (`invertRect(0, y, 124, ...)` — 4px of slack).

2. **Do not "fix" the boot screens.** `Main.cpp:216-231` (USB boot mode) and
   `OC_ui.cpp:326`/`:332` (splash `[L]`/`[R]`) invert to show which branch the
   currently-held button leads to. **Neither screen has an encoder binding at
   all** — `OC_ui.cpp:299-303` reads buttons directly — so there is no encR to
   be ambiguous with, and the inversion is the clearest possible statement of
   "this is what happens if you let go now". Changing them makes them worse.
   `OC_ui.cpp:352` is a progress bar; leave it.

3. **Do not apply Audio-Apps-Screens §6.4's H-2 fix verbatim.** `drawRect` is an
   OR *fill* (`weegfx.cpp:190`), and the 8x8 cell already holds the icon glyph.
   The spec's filled/hollow box was written for an empty gutter cell. See §2.1.

4. **Do not touch the `V` class (meter/bar fills).** ~18 sites where
   `gfxInvert` is being used as a *drawing primitive* to fill a bar
   (`BootsNCat.h:196`, `LowerRenz.h:131`, `RndWalk.h:246`, `Relabi.h:321`,
   `Strum.h:158`, `InputApplet.h:118`, `HSApplication.h:124`,
   `HemisphereApplet.cpp:355`, …). Replacing them with `drawRect` changes what
   happens where the bar crosses existing content and is a pure regression risk
   for zero grammar gain. Nobody reads a 3px-wide moving bar as "encR edits
   this".

5. **Do not touch the `F` class (flashes under ~250 ms).** `MarkoV.h:255`,
   `MarkovPerc.h:282`, `ClockSetupT4.h:364`, `ProbabilityDivider.h:259`,
   `DivSeq.h:251`, `DrumMap.h:355`, `CVRecV2.h:232`, `ProbabilityMelody.h:440`.
   A transient flash is temporally distinct from a persistent highlight; the
   grammar does not actually collide.
   **One exception worth a look, but not in this migration:** `Seq32.h:386`
   flashes `gfxInvert(0, 22, 63, 41)` over 41 rows, which XOR-cancels the edit
   cursor at `:382` for the duration of the flash. Same class of defect as H-2,
   one applet, low stakes. Note it; do not bundle it.

6. **Do not convert the `T` class.** 30 sites are already correct and several
   are load-bearing references: `PresetBusUI.cpp:435-448`, `SETTINGS.h:994`
   (whose comment already states the rule), `OC_menus.h:328-437` (8 sites — the
   entire stock o_C settings-menu grammar), `OC_apps.cpp:1076`,
   `TweightyApp.h:646`,`:685`, `SamplerApp.h:458`, `HSUtils.cpp:529`,
   `Bus200eApp.h:1505`, `Calibr8or.h:827`, `Quadrants.h:463`, `Hemisphere.h:775`,
   `HSApplication.h:204`,`:215`, `HemisphereApplet.cpp:158`,`:179`,`:289`,`:401`.
   Any change here *creates* a seventh meaning rather than removing one.

7. **Do not ship any change to `HemisphereApplet.cpp` / `HemisphereApplet.h` /
   `HSApplication.h` on the strength of a green sim build.** The sim's
   `FW_SRCS` (Makefile `:47-93`) does not list `HemisphereApplet.cpp`, and the
   two headers' inverting members are not odr-used by any of the six sim apps.
   `make && ./selfcheck.sh` passing after such an edit means **nothing was
   compiled**, in exactly the same way the FLASHMEM/LTO trap makes a
   byte-identical rebuild diagnostic rather than reassuring.

8. **Do not fold the state banners into this work.** `Bus200eApp.h:1635`,
   `:1658`, `:1666`, `:1687`, `:1694`, `:1727`, `:1740`, `SETTINGS.h:880`,
   `Backup.h:267` are worded refusal/progress banners whose whole design intent
   is to shout, and two of them carry comments saying so ("inversion is the only
   emphasis"). They are `B`-class by the letter of L-06 and a drawn box is the
   letter of the fix, but boxing them costs 2px of vertical margin on rows that
   are already at y=45 and y=12-14, and there is no encR on those screens to be
   confused with. If the owner wants them boxed, that is a separate, purely
   aesthetic commit — not part of a disambiguation migration.

---

## 7. Could not verify

1. **Whether any 8x8 input-map icon lights its bottom-right 2x2 corner**, which
   is where §2.1(a) proposes to put the gate pip. Confirm by reading
   `PARAM_MAP_ICONS` / `DIGITAL_INPUT_ICONS` in the bitmap table, or with an
   `--dump-fb` capture of an audio applet's CV row.
2. **Whether the 6x8 font lights row 7 of its cell** for `a c d e n v R E G S`,
   which decides whether Step 1's `drawFrame(x-1, 55, w+2, 9)` touches the
   glyphs. One sim capture answers it.
3. **`apps/ScaleEditor.h:205`** — I did not trace which control moves
   `current_note`, so I cannot say whether that inversion is `T` or `C`. Check
   `ScaleEditor.h`'s encoder handler before touching it.
4. **`OC_chords_edit.h:226`** — same question for `cursor_quality_pos_`. Note
   that `:228`/`:230` already switch between `invertRect` and `drawFrame` on
   `edit_page_`, which suggests the author had the right model and only the
   top cell at `:226` is unconditional.
5. **Whether `HS::DrawPopup`'s `MESSAGE_POPUP` is reachable at all outside
   Hemisphere/Quadrants/CaptainMIDI/Calibr8or.** `PokePopup(MESSAGE_POPUP, …)`
   is called from `PhzConfig.cpp:666`, `:672`, `:691`, `:807`, `:818`,
   `PresetEngine.cpp:917`, `:1089`, `:1240`, `:1249`, `:1260`, `:1293`, `:1356`
   and `PresetBusUI.cpp:150` — i.e. from code that runs under *every* app — but
   `DrawPopup()` itself is only called from those four apps
   (`Quadrants.h:1699`, `CaptainMIDI.h:457`, `Hemisphere.h:887`,
   `Calibr8or.h:503`). If that is right, then "Corrupt File!!", "Disk full !!"
   and "Bus recall OK" are **silently swallowed** in Tweighty, Sampler, the 200e
   app and Setup/About. That is not an L-06 finding and I have not chased it,
   but it looks like a real one and it is worth its own pass.
