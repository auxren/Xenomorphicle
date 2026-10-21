# 200e bus bridge — SysEx firmware-card, scan and bus-status commands (protocol v2, bridge level 1)

An extension of the Xenomorpher's 200e bridge protocol (`software/src/Bus200eSysEx.h`,
`Bus200eBridge.{h,cpp}`). It lets a desktop app do over USB MIDI SysEx what the
serial console key `H` does today: stream a v1/v2 firmware-card image into
`FWCARD.BIN`, arm firmware-card mode, and watch a module read it — plus a QUERY
sweep of the bus and a bus-status poll. The protocol version byte stays `02`;
the additions are new commands and new NAK codes, announced by
`BUS200E_SYSEX_BRIDGE_LEVEL` = **1** appended to INFO_R (a host that sees no
level byte, or 0, is talking to a device without any of this).

Ground rules (same family as `docs/hoc-midi-sysex.md`, which this reuses):

- Every message is at most **60 bytes** including `F0`/`F7`.
- All payload bytes are plain 7-bit values. The one packed field is the
  firmware image inside `FW_DATA`, exactly as `DUMP_DATA` packs a bank
  (`Bus200eSysExPack`: per 7 raw bytes, one high-bits byte then seven low-7
  bytes).
- **One outstanding request at a time.** Wait for the reply (ACK, NAK or
  response) before sending the next command. `SCAN` is the one request that
  answers more than once (ACK, then `SCAN_R`* and `SCAN_END`); do not send
  another command until `SCAN_END`.
- Multi-byte numbers are septets, **low first**: packet counters are 14 bits
  (two septets, max 16383), lengths are 21 bits (three septets), CRCs are
  35 bits (five septets).

## Frame format

```
F0 7D 62 35 02 <cmd> <payload...> F7
```

| byte | value | meaning |
|---|---|---|
| 0 | `F0` | SysEx start |
| 1 | `7D` | non-commercial manufacturer ID |
| 2 | `62` | "Beige Maze" family |
| 3 | `35` | 251e/200e bridge application |
| 4 | `02` | protocol version |
| 5 | cmd | command byte (below) |
| 6.. | payload | command-specific, 7-bit |
| last | `F7` | SysEx end |

A version mismatch is answered with NAK 1; frames whose byte 3 is another
application are ignored.

## Commands added

H→D = host to device, D→H = device to host. Existing commands (`01` INFO,
`04` GET_DUMP, `05` PUT_DUMP, `10` STATUS, `44` DUMP_DATA, `45` DUMP_END, `40`
ACK, `41` INFO_R, `50` STATUS_R, `7E` NAK) are unchanged.

| cmd | name | dir | payload | reply |
|---|---|---|---|---|
| `20` | FW_BEGIN | H→D | `kind len0 len1 len2 crc0 crc1 crc2 crc3 crc4` | `40` ACK `{20}` or NAK |
| `21` | FW_DATA | H→D | `seq_lo seq_hi total_lo total_hi packed(≤48)` | `40` ACK `{21 seq_lo seq_hi}` or NAK |
| `22` | FW_END | H→D | `n_lo n_hi xor7` | `40` ACK `{22 n_lo n_hi}` after the commit, or NAK |
| `23` | FW_ARM | H→D | `on` (0 disarm, 1 arm) | `40` ACK `{23 on}` |
| `24` | FW_STATUS | H→D | — | `64` FW_STATUS_R |
| `25` | SCAN | H→D | `first last` | `40` ACK `{25 first last}`, then `65` SCAN_R × n, then `66` SCAN_END |
| `26` | POLL_DONE | H→D | — | `40` ACK `{26}` |
| `27` | BUS_STATUS | H→D | — | `67` BUS_STATUS_R |
| `64` | FW_STATUS_R | D→H | see below | — |
| `65` | SCAN_R | D→H | `idx count addr ver_valid v0..v6` | — |
| `66` | SCAN_END | D→H | `n_found` | — |
| `67` | BUS_STATUS_R | D→H | `wpm_present bus_quiet dialect last_error` | — |

**No collisions.** hOC uses `01`–`0B`, `40`–`42`, `44`, `45`, `4B` and `7E`;
the existing bridge uses `01`, `04`, `05`, `10`, `40`, `41`, `44`, `45`, `50`,
`7E`. The new `20`–`27` and `64`–`67` clear every one of those (and the new
NAK codes 10–16 clear hOC's 1–6 and the bridge's 7–9).

### INFO_R addition

`INFO_R` is append-only. The level byte is appended after `card_serving`:

```
schema n_sequences max_steps len0 len1 len2 chunk_bytes max_pk_lo max_pk_hi card_serving bridge_level
```

`bridge_level` = `01` for everything in this document.

## FW_BEGIN (`20`)

```
F0 7D 62 35 02 20 <kind> <len0 len1 len2> <crc0 crc1 crc2 crc3 crc4> F7
```

| offset | field | meaning |
|---|---|---|
| 0 | `kind` | image kind: `01` = v1 card image, `02` = v2 |
| 1..3 | `len0..len2` | image length in bytes, **21 bits**, three septets low first: `len = len0 + (len1 << 7) + (len2 << 14)` (max 2,097,151) |
| 4..8 | `crc0..crc4` | CRC-32 of the whole image (zlib polynomial, same as `Buchla200eCrc32`), **35 bits**, five septets low first: `crc = crc0 + (crc1 << 7) + (crc2 << 14) + (crc3 << 21) + (crc4 << 28)`. The CRC is 32 bits; `crc4` is therefore 0..15 |

Example — a 54,186-byte image (`0xD3AA`) with CRC `0x12345678`, kind 2:

```
F0 7D 62 35 02 20 02 2A 27 03 78 2C 51 11 01 F7
                  │  └len┘    └───crc────┘
                  kind
```

`54186 = 0x2A + (0x27 << 7) + (0x03 << 14)`; `0x12345678` = `78 2C 51 11 01`.

FW_BEGIN is **refused** (NAK, device stays IDLE) when:

- a WPM answers `0x50` on the bus: **NAK 7 NO_FREE_CARD** (two cards on one bus
  corrupt each other, so the device will not serve one — the same rule the
  console `H` key follows);
- firmware-card serving is on right now, or a load is already in progress:
  **NAK 10 FW_BUSY**;
- `FwCardBegin()` cannot allocate the receive buffer: **NAK 11 FW_NOMEM**.

On success it calls `FwCardBegin()` and answers ACK `{20}`; the device is now
FW_RECEIVING.

## FW_DATA (`21`)

```
F0 7D 62 35 02 21 <seq_lo seq_hi> <total_lo total_hi> <packed...> F7
```

| offset | field | meaning |
|---|---|---|
| 0..1 | `seq_lo seq_hi` | this packet's index, 14 bits, **starting at 0** |
| 2..3 | `total_lo total_hi` | number of packets in the whole image, 14 bits (`ceil(len / 42)`) |
| 4.. | `packed` | the chunk, 7-bit packed: at most **42 raw bytes** → at most 48 packed bytes. The chunk is at raw offset `seq × 42`; every chunk but the last is exactly 42 raw bytes |

Packing: for each group of up to 7 raw bytes, one byte whose bit *j* is the
top bit of raw byte *j*, then the seven bytes with their top bit cleared. 42
raw bytes make 48 packed; a 6-byte tail makes 7.

Frame size: 7 (F0 + mfr + family + app + ver + cmd + F7) + 4 header + 48 =
**59**, inside the 60-byte ceiling. (Group granularity means no chunk size
lands exactly on 60; the spare byte is not an off-by-one.)

Example — the last packet of the 54,186-byte image: 54186 = 1290 × 42 + 6,
so `total` = 1291, the final `seq` = 1290, and it carries 6 raw bytes
`DE AD BE EF 00 7F`:

```
F0 7D 62 35 02 21 0A 0A 0B 0A 0F 5E 2D 3E 6F 00 7F F7
                  └seq─┘ └total┘ │  └───low 7 bits───┘
                                  hi-bits byte: DE AD BE EF have the top bit
```

`1290 = 0x0A + (0x0A << 7)`, `1291 = 0x0B + (0x0A << 7)`. The reply is ACK
`{21 0A 0A}`; each packet is ACKed before the next is sent.

Each accepted packet is fed to `FwCardPush()` byte by byte. Anything that is
not exactly the next packet is **NAK 16 FW_SEQUENCE**, and the load is
aborted (below): a `seq` other than the expected one, a `total` that changes
mid-load, a short chunk before the last one, a chunk that would push the
received bytes past `len`, or an FW_DATA with no FW_BEGIN before it (device
IDLE). A malformed packed chunk is NAK 16 as well.

## FW_END (`22`)

```
F0 7D 62 35 02 22 <n_lo n_hi> <xor7> F7
```

| offset | field | meaning |
|---|---|---|
| 0..1 | `n_lo n_hi` | number of FW_DATA packets sent (14 bits). Must equal `total` and the number received |
| 2 | `xor7` | XOR of every raw (pre-pack) image byte, folded to 7 bits (`Bus200eSysExXor7`) — same definition as DUMP_END |

Example (n = 1291, xor7 = `5A`):

```
F0 7D 62 35 02 22 0B 0A 5A F7
```

A packet-count or xor7 mismatch is **NAK 6 CHECKSUM** (abort). Otherwise the
device calls `FwCardCommit(len, crc32)`, which verifies the length, the
`#!200EFU` header and the CRC-32, writes `FWCARD.BIN`, reads it back, and arms
firmware-card mode. Its return code selects the reply; ACK `{22 n_lo n_hi}`
means the image is on flash **and** armed.

| `FwCardCommit` returns | meaning | NAK |
|---|---|---|
| `0` | ok | ACK |
| `-1` | no buffer / not in a load | **11 FW_NOMEM** |
| `-2` | length does not match `len` (or is out of range) | **12 FW_LENGTH** |
| `-3` | bad `#!200EFU` header / geometry | **13 FW_MAGIC** |
| `-4` | CRC-32 mismatch | **14 FW_CRC** |
| `-5` | write, or read-back after write, failed | **15 FW_WRITE** |
| `-6` | out of memory while committing | **11 FW_NOMEM** |

## FW_ARM (`23`)

`on` = `01` arms, `00` disarms (`FwCardArm`). ACK `{23 on}`. Arming without an
image on flash is allowed (it is the same flag the console `M` key toggles)
but there will be nothing to serve; read FW_STATUS to see `len`. The arm takes
effect at the **next boot**.

**The arm is one-shot.** Once the module has read the whole image (the TOC
data end) and the bus has then been quiet for **30 s**, the device disarms
itself and the following boot is normal. The host must not assume the mode
survives a successful flash; re-arm to flash again.

## FW_STATUS (`24`) → FW_STATUS_R (`64`)

```
F0 7D 62 35 02 64 <armed> <serving> <kind> <served0 served1 served2> <age_lo age_hi> <len0 len1 len2> <ver_addr> <ver_valid> <v0..v6> F7
```

| offset | field | meaning |
|---|---|---|
| 0 | `armed` | 0/1: `FwCardArmed()` |
| 1 | `serving` | 0/1: `FwCardMode()` — answering `0x50` right now |
| 2 | `kind` | `00` none on flash, `01` v1, `02` v2 |
| 3..5 | `served0..2` | bytes the module has read so far, **21 bits**, low first |
| 6..7 | `age_lo age_hi` | seconds since the module's first read, **14 bits**; `7F 7F` (`0x3FFF`) = no read yet |
| 8..10 | `len0..2` | length of the image on flash, 21 bits |
| 11 | `ver_addr` | address of the module whose version frame was last seen (0 if none) |
| 12 | `ver_valid` | 0/1: `v0..v6` hold a real frame |
| 13..19 | `v0..v6` | the seven ASCII bytes of the last `0x13` frame seen from `ver_addr` (zeros if `ver_valid` is 0) |

Example — armed, serving a v2 image of 54,186 bytes, 12,345 bytes read
(`12345 = 39 + 96×128` → `39 60 00`), 4 s since the first read, module `0x42`
reporting `"31.0b1 "`:

```
F0 7D 62 35 02 64 01 01 02 39 60 00 04 00 2A 27 03 42 01 33 31 2E 30 62 31 20 F7
```

A host watches `served` climb to `len` to know the module has read the whole
image, and `ver_*` to see the new version after the module reboots.

## SCAN (`25`) → SCAN_R (`65`) × n, SCAN_END (`66`)

`SCAN [first, last]`: the device QUERYs each address `first..=last`
inclusive, **one at a time**, through `MasterQuery`. Both bytes are 7-bit
module addresses; `first > last` is an empty sweep. The device ACKs `SCAN`
immediately (`{25 first last}`), then runs the sweep and sends one `SCAN_R`
per address that answered (QUERY job DONE), then `SCAN_END`. **NAK 5 BUSY** if
a master job or a scan is already running.

```
SCAN_R:   F0 7D 62 35 02 65 <idx> <count> <addr> <ver_valid> <v0..v6> F7
SCAN_END: F0 7D 62 35 02 66 <n_found> F7
```

| field | meaning |
|---|---|
| `idx` | 0-based index of this responder in the sweep |
| `count` | **always `7F` ("unknown") at this protocol level.** The device cannot know the responder count before the sweep ends. The real total is `SCAN_END`'s `n_found` |
| `addr` | the module address that answered |
| `ver_valid` | 0/1: `v0..v6` hold a real version frame |
| `v0..v6` | the seven ASCII bytes of that module's last `0x13` frame |

Example — one responder at `0x42`, then the end:

```
F0 7D 62 35 02 65 00 7F 42 01 33 31 2E 30 62 31 20 F7
F0 7D 62 35 02 66 01 F7
```

The sweep is bounded by the host's `first..=last`; a full sweep of the 7-bit
space is slow, so the host should expect `SCAN_END` to take seconds and
should use its own timeout (it is the host's call whether to abandon the
wait; there is no scan-cancel command at level 1).

## POLL_DONE (`26`)

Asks the device to send the bus's POLL_DONE now (the message the boot sequence
holds back while firmware-card mode is armed). ACK `{26}`. Use it after a flash
to restore normal bus behaviour without a power cycle.

## BUS_STATUS (`27`) → BUS_STATUS_R (`67`)

```
F0 7D 62 35 02 67 <wpm_present> <bus_quiet> <dialect> <last_error> F7
```

| field | meaning |
|---|---|
| `wpm_present` | 0/1: something answers `0x50` (the reason FW_BEGIN would answer NAK 7) |
| `bus_quiet` | 0/1: the bus is quiet enough to master a frame (the `tx_gate_open` condition) |
| `dialect` | framing last seen on the bus: `00` none yet, `01` short/V2, `02` long/v1 (PRIMO). Append-only; unknown values mean "newer than you" |
| `last_error` | `Bus200eMasterLastError()`: `0` none, `1` busy, `2` bad args, `3` no free card, `4` send timeout, `5` no response |

Example: `F0 7D 62 35 02 67 00 01 01 00 F7` — no WPM, bus quiet, short framing,
no error.

## State diagram — a firmware load

```
                         FW_BEGIN ok
          ┌──────┐  ─────────────────────▶  ┌──────────────┐
  boot ──▶│ IDLE │                          │ FW_RECEIVING │◀─┐ FW_DATA ok
          └──────┘  ◀─────────────────────  └──────┬───────┘  │ (ACK, next seq)
             ▲  ▲        fw_abort                  │ ─────────┘
             │  │   (every NAK exit, and           │
             │  │    the 10 s stall)               │ FW_END ok → FwCardCommit → 0
             │  │                                  ▼
             │  │                        ┌──────────────────┐
             │  └────────────────────────│ committed + armed│
             │       FW_ARM 0, or the    └──────────────────┘
             │       one-shot disarm        │ (device is IDLE again; the image
             └──────────────────────────────┘  is on flash and the arm is set)
```

Transitions, with their NAKs:

| from | event | to | reply |
|---|---|---|---|
| IDLE | FW_BEGIN, WPM answers `0x50` | IDLE | NAK 7 NO_FREE_CARD |
| IDLE | FW_BEGIN, card serving on | IDLE | NAK 10 FW_BUSY |
| IDLE | FW_BEGIN, `FwCardBegin` alloc fails | IDLE | NAK 11 FW_NOMEM |
| IDLE | FW_BEGIN ok | FW_RECEIVING | ACK |
| IDLE | FW_DATA / FW_END | IDLE | NAK 16 FW_SEQUENCE |
| FW_RECEIVING | FW_BEGIN again | IDLE (aborted) | NAK 10 FW_BUSY |
| FW_RECEIVING | FW_DATA in order | FW_RECEIVING | ACK `{21 seq}` |
| FW_RECEIVING | FW_DATA out of order / bad | IDLE (aborted) | NAK 16 FW_SEQUENCE |
| FW_RECEIVING | FW_END, count or xor7 wrong | IDLE (aborted) | NAK 6 CHECKSUM |
| FW_RECEIVING | FW_END, `FwCardCommit` fails | IDLE (aborted) | NAK 11 / 12 / 13 / 14 / 15 (table above) |
| FW_RECEIVING | FW_END, commit ok | IDLE, **image on flash, armed** | ACK `{22 n}` |
| FW_RECEIVING | 10 s with no FW_DATA/FW_END | IDLE (aborted) | none; the next FW_DATA gets NAK 16 |

**Every NAK exit calls `FwCardAbort()` (`fw_abort`) and returns the device to
IDLE** — a failed or stalled load never leaves a half-written image armed,
and the host restarts from FW_BEGIN. FW_STATUS, BUS_STATUS, INFO and FW_ARM
may be sent during FW_RECEIVING (they do not touch the load; FW_ARM 1 with
nothing committed is the console's `M`). While a load is in flight, SCAN and
the master commands answer NAK 10 FW_BUSY.

## Rules, collected

- **One outstanding request.** No pipelining: wait for the ACK of packet *n*
  before sending *n+1*.
- **60-byte ceiling** including `F0`/`F7`; FW_DATA is 59 at full chunk (42 raw
  bytes).
- **10 s stall rule.** In FW_RECEIVING the device aborts if 10 s pass with no
  frame from the host. The host's own per-reply timeout must be shorter.
- **14-bit counters** (`seq`, `total`, `n`): 16383 packets max; at 42 bytes
  that is 687,846 bytes, far past any 200e image.
- **21-bit length, 35-bit CRC** as septets, low first, as laid out above.
- **`FW_BEGIN` is refused** while a WPM answers `0x50` (NAK 7) or while card
  serving is on (NAK 10). Take the WPM off the bus first.
- **The arm is one-shot:** it disarms itself once the whole image has been
  read and the bus has been quiet for 30 s. It takes effect at the next boot.
- Commit is slow (a flash write plus read-back); the ACK of FW_END may take
  several seconds. Use a long timeout for that one reply only.

## NAK error codes

`7E <cmd> <errcode>`. Codes 1, 2, 5, 6 are hOC's exact meanings; 7–9 were added
for foreign-module mastering; 10–16 are new here.

| code | name | meaning |
|---|---|---|
| 1 | VERSION | protocol version mismatch |
| 2 | UNKNOWN_CMD | unknown command |
| 5 | BUSY | a master job (or a scan) is already running |
| 6 | CHECKSUM | packet-count / xor7 mismatch |
| 7 | NO_FREE_CARD | no free card address; for FW_BEGIN, a WPM answers `0x50` |
| 8 | SEND_TIMEOUT | the bus never went quiet |
| 9 | NO_RESPONSE | the target never touched the card |
| 10 | FW_BUSY | card serving is on, or a load is in progress |
| 11 | FW_NOMEM | `FwCardBegin` alloc failed, or `FwCardCommit` -1 / -6 |
| 12 | FW_LENGTH | `FwCardCommit` -2 |
| 13 | FW_MAGIC | `FwCardCommit` -3 (header / geometry) |
| 14 | FW_CRC | `FwCardCommit` -4 |
| 15 | FW_WRITE | `FwCardCommit` -5 (write or read-back) |
| 16 | FW_SEQUENCE | FW_DATA / FW_END out of order, or without FW_BEGIN |
