# Syncing the 252e over the 200e preset bus

The owner's ask: the clock that drives this module's music must also appear
on the 200e bus, so the 252e Polyphonic Rhythm Generator can follow it.

## How clock rides the bus (verified from source)

MIDI realtime rides the preset bus as ordinary bus-MIDI frames. Ground
truth is the 2WIRELESS source (github.com/studiohsoftware/2WIRELESS) via
the MARF port (`marf/docs/DESIGN-200e-bus.md`): `0xF8/0xFA/0xFC` = MIDI
clock/start/stop, carried in the same framing as note/CC traffic —

    PRIMO (long):  [08] [00] [22] [0F] [F8] [00] [00] [00] [00]
    V2 (short):    status-first, [F8 ...]

Our TX (`PresetBus.cpp pump_midi_tx`) masters the PRIMO long frame, the
dialect the current 225e firmware speaks. Our own RX parser accepts both
(`test_bus200e.cpp`), so a Xenomorpher also *follows* bus clock.

## The paths that put clock on the bus

Both are gated by the MIDI settings rows (`ClkTx Bus`, `Thru Bus`), all on
by default, and by the TX quiet-gate (WPM card transfers hold TX off for
1.5 s — see `200e-conformance.md`).

1. **External clock, thru** (the rig's normal case: the Orin sends 0xF8
   over USB). Captain MIDI's `midi_thru` forwards every incoming realtime
   byte to the bus. External clock also flags `midi_sync`, which disables
   the internal clock's own MIDI out — so the bus never sees two clocks.
2. **Internal clock** (`clock_m` running, this module is the master).
   Fixed 2026-08-27: the tick engine (`SyncTrig` + `MIDITock` fan-out)
   lived only in ClockSetup's Controller, which runs only inside
   HEMISPHERE and Quadrants — with Captain MIDI as the standing app a
   running internal clock was silent everywhere. Captain now pumps the
   same engine (`PumpTransport()` in CaptainMIDI.h): transport
   start/stop/queue, sync, and the 24 ppqn realtime fan-out to every
   enabled ClkTx interface, the bus included.

Start (0xFA) and Stop (0xFC) go out on transport changes from the same
fan-out (`HSClockManager Start()/Stop()`), and thru forwards external ones.

## Burst-after-holdoff is deliberate

When the quiet-gate blocks TX (WPM transfer, arbitration), queued 0xF8
ticks burst out afterwards rather than being dropped. A 24 ppqn receiver
*counts* ticks to hold musical phase; dropping stale ticks would slip the
252e permanently, while a burst catches its count back up. The TX ring
never coalesces realtime (`MidiTxRing::is_continuous`) for the same reason.

## Verified / unverified

- Framing + gating: host-tested (`test_bus200e` 88 checks, `test_miditxring`
  12584 checks); green on every CI build environment (`T41`, `T41_audio`,
  `T41_audio_dbg`, `T41_console` — all T4.1/Xenomorpher; CI no longer
  builds T40 or other older-hardware environments on this fork).
- **Unverified on hardware:** whether 252Ev302 actually follows bus MIDI
  clock, and in which dialect. Only the .hex exists locally (Buchla_FW);
  no source. Live test: run a clock, watch console `b` (`midi_tx`
  climbing at 24 ppqn), see whether the 252e locks. If the 252e only
  hears the short/V2 dialect, the lever is `pump_midi_tx` — teach it a
  per-message or global dialect switch; the RX side already speaks both.
- Whether the 252e needs 0xFA to arm before following 0xF8: unknown;
  we send it on transport start regardless.
