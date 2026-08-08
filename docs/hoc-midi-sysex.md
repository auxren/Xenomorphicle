# hOC Captain MIDI — SysEx configuration protocol v1

Remote configuration of the hOC's Captain MIDI app (firmware env
`nlm_hoc_midi`). Every I/O mapping the on-device UI can edit is also
readable and writable over USB MIDI SysEx; changes apply **live** and the
screen reflects them immediately. Designed to be driven from a host
(e.g. Jetson Orin) via `tools/hoc_sysex.py`.

Ground rules:

- Every message is at most **60 bytes** including `F0`/`F7`.
- All payload bytes are plain 7-bit values (no 8-bit packing, no struct dumps).
- One outstanding request at a time; wait for the reply (ACK/NAK/response)
  before sending the next command.
- Live edits are RAM-only until `SAVE` (or an on-device save) commits them.

## Frame format

```
F0 7D 62 4D <ver> <cmd> <payload...> F7
```

| byte | value | meaning |
|---|---|---|
| 0 | `F0` | SysEx start |
| 1 | `7D` | non-commercial manufacturer ID |
| 2 | `62` | "Beige Maze" family (shared with other o_C apps) |
| 3 | `4D` (`'M'`) | Captain MIDI application |
| 4 | `01` | protocol version |
| 5 | cmd | command byte (table below) |
| 6.. | payload | command-specific, 7-bit bytes |
| last | `F7` | SysEx end |

A version mismatch is answered with NAK error 1. Frames whose byte 3 is a
different application code are ignored (logged on the device).

### Universal Device Inquiry

The device also answers the standard identity request:

```
host:   F0 7E <dev> 06 01 F7
device: F0 7E 7F 06 02 7D 68 4F 43 01 02 00 01 00 F7
                        │  └─"hOC"─┘ │  └fw 2.0.1┘
                        └mfr 7D      └protocol ver
```

## Commands

Direction H→D = host to device, D→H = device to host.

| cmd | name | dir | payload | reply |
|---|---|---|---|---|
| `01` | INFO | H→D | — | `41` INFO_R |
| `02` | GET_PARAM | H→D | `class idx param` | `42` GET_R: `class idx param value`, or NAK 3 |
| `03` | SET_PARAM | H→D | `class idx param value` | `40` ACK echoing all 4 bytes, or NAK. Applies live. |
| `04` | GET_DUMP | H→D | `setup#` (`7F` = active) | stream of `44` packets, then `45` |
| `44` | DUMP_DATA | both | `setup# seq total {records}` | on H→D: `40` ACK `{seq}` per packet |
| `45` | DUMP_END | both | `setup# n_packets xor7` | on H→D: `40` ACK / NAK 6 |
| `06` | SAVE | H→D | — | `40` ACK `{06}` after the write completes |
| `07` | LOAD | H→D | `setup#` | `40` ACK `{07 setup#}`. Discards live edits, activates and reloads that setup from the stored state. |
| `08` | FACTORY | H→D | `21 42` (magic guard) | `40` ACK `{08}`. All setups reset to defaults. |
| `09` | PANIC | H→D | — | `40` ACK. Note-offs for tracked notes + CC120/123 sweep on all 16 channels. |
| `0A` | SELECT_SETUP | H→D | `setup#` | `40` ACK `{0A? no — {setup#}}` — see note. Stores live edits into the current setup, then switches. |
| `40` | ACK | D→H | echo bytes (see each cmd) | — |
| `41` | INFO_R | D→H | see below | — |
| `42` | GET_R | D→H | `class idx param value` | — |
| `7E` | NAK | D→H | `cmd errcode` | — |

Note on `0A` SELECT_SETUP: ACK payload is `{setup#}`.

`SELECT_SETUP` vs `LOAD`: SELECT preserves your unsaved edits (packs them
into the setup you're leaving); LOAD throws unsaved edits away and reloads
the target setup as last stored.

### INFO_R payload (`41`)

```
schema fw_maj fw_min fw_pat n_inmaps n_cv_out n_trig_out n_setups active dirty
```

`dirty` = 1 when the live configuration differs from the stored active
setup (same condition as the `*` next to the setup number on screen).

### NAK error codes

| code | meaning |
|---|---|
| 1 | protocol version mismatch |
| 2 | unknown command |
| 3 | bad address (class/idx/param out of range) |
| 4 | bad value (out of range, or read-only parameter) |
| 5 | busy |
| 6 | dump checksum/packet-count mismatch |

## Parameter address space

Parameters are addressed as `(class, idx, param)`. All values 0–127.

### class 0 — GLOBAL (idx must be 0)

| param | name | range | notes |
|---|---|---|---|
| 0 | schema | — | read-only, = 1 |
| 1 | active_setup | 0..n_setups-1 | writing behaves like SELECT_SETUP |
| 2 | setup_count | — | read-only |
| 3 | bend_range | 1..24 | semitones, for MIDI-in pitch bend |
| 4 | poly_mode | 0..2 | 0 Reset, 1 Rotate, 2 Reuse |
| 5 | pc_channel | 0..16 | program-change listen channel, 0 = omni |
| 6 | trig_length | 1..127 | trigger/note pulse length, ms |

### class 1 — MIDI-IN map (idx 0..n_inmaps-1; 8 on the hOC)

Each map translates incoming MIDI to one behavior; maps 0-3 drive DAC
outputs A-D directly (map i → output i).

| param | name | range | notes |
|---|---|---|---|
| 0 | type | 0..5 | 0 None, 1 Pitch, 2 Gate, 3 Trigger, 4 Modulator, 5 CC |
| 1 | subtype / CC# | 0..127 | subtype index for types 1-4 (see below); CC number for type 5. **127 = auto-learn.** |
| 2 | channel | 0..16 | 16 = omni |
| 3 | transpose | 16..112 | semitones, stored +64 (64 = 0) |
| 4 | range_low | 0..127 | note window |
| 5 | range_high | 0..127 | |
| 6 | polyvoice | 0..3 | poly voice assignment |

Subtypes — Pitch: 0 Note, 1 PolyN, 2 LoNote, 3 HiNote, 4 PdlNote,
5 InvNote. Gate: 0 Gate, 1 PolyG, 2 GateRT, 3 InvGate, 4 RunGate,
5 Reset. Trigger: 0 Trig, 1 Trig1st, 2 TrgAlws, 3 Start, 4 Stop,
5 Clk-1, 6 Clk-2, 7 Clk-4, 8 Clk-8, 9 Clk24. Modulator: 0 Veloc,
1 PolyV, 2 ChnAft, 3 KeyAft, 4 Bend.

### class 2 — CV-OUT port (idx 0..3 = CV inputs 1-4)
### class 3 — TRIG-OUT port (idx 0..3 = trigger inputs 1-4)

Trigger port *i* is paired with CV port *i*: a trig port in Note mode
takes its pitch from the paired CV port when that port's function is
Pitch (transpose/quantize from the CV port; channel, velocity and range
from the trigger port).

| param | name | range | notes |
|---|---|---|---|
| 0 | function | see tables | |
| 1 | channel | 0..15 | |
| 2 | data1 | 0..127 | note# / CC# / NRPN LSB |
| 3 | data2 | 0..127 | on-value / fixed velocity / NRPN MSB |
| 4 | transpose | 16..112 | stored +64 |
| 5 | range_low | 0..127 | output clip window |
| 6 | range_high | 0..127 | |
| 7 | flags | 0..127 | bitfield below |
| 8 | clkdiv | 0..15 | clock multiplier index |

CV functions (class 2 param 0): 0 Off, 1 Pitch (gated by paired trig),
2 FreeNote (note-on whenever pitch changes), 3 CC7, 4 CC14 (MSB data1,
LSB data1+32), 5 Velocity source, 6 Bend, 7 Aftertouch, 8 NRPN7,
9 NRPN14 (address MSB=data2 LSB=data1), 10 ProgramChange, 11 GateNote
(CV threshold gates fixed note data1).

TRIG functions (class 3 param 0): 0 Off, 1 Note, 2 NoteTrig (fixed
length), 3 NoteLatch, 4 CC momentary (data1←data2/0), 5 CC latch,
6 Start, 7 Stop, 8 Continue, 9 Start/Stop toggle, 10 Clock, 11 Panic.

Flags bitfield (param 7): bit0 quantize pitch CV, bit1 legato (overlap
instead of retrigger), bit2 CV gate source (trigger ports only, below),
bits3-5 velocity source (0 = fixed data2, 1-4 = sample CV port 1-4 at
note-on).

Param 8 is dual-purpose on trigger ports:
- function = Clock: clock multiplier, index 0-9 → x1 x2 x3 x4 x6 x8 x12
  x24 x48 x96 MIDI clocks per rising edge; 10-15 reserved (= x1).
- any other function with flags bit2 set: **gate source** — the port
  takes its gate from CV input (param8 mod n_cv) via the ADC's 1.25V
  threshold instead of the TR jack. Use this for pulse sources too weak
  for the digital input's hardware threshold (measured on real Buchla
  gear: a 281's pulse arrives ~1.8V, a 244's ~2.4V — both below the TR
  jack's threshold, both comfortably above 1.25V). Pitch still comes
  from the paired CV port when it is set to Pitch. The on-device "Gate
  Src" row edits the same bits. Not available when function = Clock.

## Dump format

`GET_DUMP` streams the full configuration of one setup as `44` DUMP_DATA
packets. Packet payload:

```
setup# seq total  {record}{record}...
```

Each record is `class idx v0 v1 ... vN` with the full parameter list for
that class (7 values for class 0 and 1, 9 for classes 2/3), encoded
exactly as GET_PARAM returns them. Records never split across packets.
The stream ends with `45` DUMP_END: `setup# n_packets xor7`, where `xor7`
is the XOR of every record byte (classes, indices and values), masked to
7 bits.

**Restore** is the same stream sent host→device: each DUMP_DATA is
applied through the SET_PARAM path (read-only globals are skipped,
per-record; `seq 0` resets the receiver state) and ACKed with its `seq`;
DUMP_END verifies packet count + checksum and ACKs, or NAKs with error 6.
Send to the target setup by SELECT_SETUP first; dumps apply to the live
(active) configuration. Follow with `SAVE` to persist.

## Persistence model

- Live config = what the engine runs and the screen shows. RAM only.
- 4 stored setups live in RAM blobs; `SELECT_SETUP` packs live edits into
  the outgoing setup.
- `SAVE` commits everything (all setups + active index + the rest of the
  o_C app data) to EEPROM. Power-cycling without SAVE loses edits.
- On-device saves (the o_C settings menu) commit the same data.

## Typical Jetson flow per patch

```
identity? (optional sanity check)
INFO                      -> confirm schema/ports, note active setup
SELECT_SETUP n            -> pick the patch's setup slot
SET_PARAM ... (xN)        -> configure I/O, each ACKed
SAVE                      -> persist (optional if setups are transient)
```

or restore a whole stored patch: `SELECT_SETUP n`, replay a saved dump
stream, `SAVE`.

See `tools/hoc_sysex.py` for a reference implementation of all of this.
