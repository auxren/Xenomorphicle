---
title: 200e Modules App
nav_order: 12
---

# 200e Modules

This app reads and writes the preset banks of **other manufacturers' modules**
over the Buchla 200e preset bus. That is unusual enough to state plainly before
anything else, because it shapes every design decision in the app.

Beta-testing this on the `preset-bus` branch? See the [Beta Guide](Xenomorpher-Beta-Guide)
for setup and, importantly, what's still unverified — writing to another
module is the newer half of this app.

## The thing to understand first

The 200e preset bus has **no command that writes one preset**. The only
transfers it defines are BACKUP and RESTORE, and both move a module's *entire*
bank. A 251e bank is **63,120 bytes — thirty slots of 2,104**.

So when you change one step of one sequence and press Save, the module sends
**all thirty presets back**. The other twenty-nine are re-sent verbatim from
the copy it read seconds earlier. There is no way to send fewer.

That is why this app is built the way it is: the danger was never the edit, it
was a stale, short, or wrong-module image being used as the baseline for a
whole-bank rewrite.

## Screens

Six controls, and the app teaches one grammar throughout:
**encL is where you are, encR is what it is.**

| screen | what it is |
|---|---|
| **Module select** | pick an address. `encL` scans the bus, `B` probes one address, `encR` enters. |
| **Module home** | the slot you are looking at, its sequence, and the action row. |
| **Edit** | per-stage editor. `encL` picks the stage, `encR` the value, `A` toggles the loop point, `X`/`Y` shift an octave. |
| **Gen** | Euclidean generator. `encR` applies and stays put, drawing the result below the parameters. |
| **Rec** | records stages from incoming MIDI. |
| **Write confirm** | the only screen that can put bytes on the wire. |

The action row on the module home is `Read · Edit · Gen · Rec · Save`. `encL`
moves the cursor along it; `encR` runs whatever it sits on. `A` is a shortcut
straight to arming a write.

## Reading

Nothing works until you Read. The app will not let you write a bank it has not
seen, and the refusal says so (`Read the module first`).

A Read pulls the whole 63,120-byte bank once, so browsing slots afterwards
costs no bus traffic at all — changing the slot re-decodes from the copy in
memory.

The **provenance line** above the action row is the only honest account of what
the module holds:

| it says | it means |
|---|---|
| `NO DATA - Read first` | nothing has been read |
| `LIVE 45s ago` | this is the module's bank, as of 45 seconds ago |
| `EDITED*` (inverted) | **these bytes are not what the module holds** |
| `WROTE + VERIFIED` | the whole bank was read back and matched byte for byte |
| `BAD: ...` | the module holds something other than what was sent |

Age matters. Banks go stale — someone can turn a knob on the 251e — and the
app re-checks the fingerprint before it will write.

## Writing

Two steps, deliberately:

1. **Arm.** `A` from the module home, or `encR` with the cursor on `Save`.
2. **Confirm.** The confirm screen states the blast radius:

```
WRITE to 5C slot 1
6 bytes change
Rewrites ALL 30 slots
29 others re-sent
encR:CONFIRM  encL:no
```

The confirm screen **ignores its "yes" for 350 ms after appearing**. That is
not politeness: the app switcher is *hold A, press encR*, `A` alone arms the
write, and `encR` confirms it — so a fumbled navigation chord used to commit
63,120 bytes with the prompt on screen for 51 ms.

After the transfer the app reads the whole bank **back** and compares it.
`WROTE + VERIFIED` is earned, never assumed, and it requires the full bank to
have returned — a truncated read-back reports `BAD: read-back short`, because
the read lands in the same buffer the write came from and would otherwise
agree with itself.

## Recovering from BAD

Before any write goes out, the app copies the module's current bank to
`PBSNAP.BIN` on internal flash — one 64 KB block, tagged with the address it
came from.

If the write ends `BAD`, the action row is replaced by:

```
BAD: 6 bytes wrong
keep        UNDO
```

`encL` moves between them, `encR` runs the one you are on, and the cursor
starts on **keep** every time. Choosing `UNDO` re-sends the pre-write bank
through the same confirm, transfer and read-back path — an undo that could not
be verified would be no better than the damage.

`A` still arms Save from this state, so retrying an edit is unchanged. A
snapshot taken before a BAD verdict is **kept**, not overwritten by a retry.

`BAD: OTHER PRESETS!` means the twenty-nine you did not touch came back
different. That is the outcome the whole design exists to make impossible, and
it is the one to reach for UNDO on.

## The module's own presets are a different thing

Confusingly, this instrument has two kinds of preset:

* **200e bus presets** — the thirty slots *inside another module*, which this
  app reads and writes.
* **Xenomorpher presets** — thirty snapshots of *this* module's whole state,
  stored as `PB_NN.PBS` on internal flash and reached through the preset-bus
  overlay (hold both encoder buttons).

They are unrelated. See [Xenomorpher Presets](Xenomorpher-Presets).

## Bench console

The console is **locked at boot** and discards every byte until it receives the
literal string `pew!`. It re-locks on every reset, including after a flash.
Output is not gated, so a locked module prints boot traces while ignoring
commands — which looks exactly like a dead module and is not.

Read-only: `t` self-test · `l` internal filesystem · `s` SD card · `b` bus
status.

Local storage, this module only, no bus traffic: `V` then two digits saves to
that slot, `N` then two digits recalls it.

**These put bytes on the bus and affect other people's modules:** `S` and `R`
(broadcast save/recall to every remote-enabled module), and `x`
(`MasterRestore` of the resident card image). Know what they do before you
type them.
