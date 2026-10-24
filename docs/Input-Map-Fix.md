# Input map fix (2026-10-24)

## What was wrong

On the Xenomorpher every CV input was read one input over. A CV patched into
panel input n showed up in the firmware as input n+1, and panel input 8
showed up as input 1. Every app saw it: a CV into panel 3 drove CV4 in
Quadrants. The outputs were never affected.

It was found on the bench by holding one output at 8.4 V and patching it into
each input in turn while watching the console CV stream. The result was the
same on every boot.

The cause was in how the readings from the 8-channel ADC are sorted into
inputs. The ADC hands back each reading one conversion after the input was
selected, and the old sorting table did not allow for that. The delay is
fixed by the hardware, so the fix is a fixed correction to the table. It
applies only to hardware with the external 8-channel ADC, which is every
Xenomorpher. Hardware that uses the Teensy's own ADC is unchanged.

## What changes when you update

- Each input now reads its own jack: panel input n is CVn in every app, and
  column n in the console's `v` stream.
- **Your stored input calibration moves with it, once, automatically.** Older
  firmware recorded each input's data one place over: the 0 V offsets from
  Setup, the per-input `Q` fits and the calibration curves. The first boot
  of the fixed firmware moves each entry to the input it was measured on and
  saves it. Nothing to do: an input calibrated on panel 1 is still panel 1's
  calibration afterwards. For example, a `Q` fit that older firmware listed
  as input 2, because it was made with the cable in panel 1, is now listed as
  input 1.
- The move happens once per module. It is safe against losing power: every
  stored page records whether it has been moved, so an interrupted move
  finishes on the next boot and never moves anything twice.
- In flip mode the move follows the flipped input order, so a flipped module
  ends up right too.
- If you go back to older firmware, recalibrate there (Setup's ADC steps or
  `Q`), and then update again, re-run that calibration after the update: the
  fixed firmware cannot tell that it was recorded the old way.

## Backups

| backup made by | restored on | result |
|---|---|---|
| older firmware | fixed firmware | the calibration is moved on restore, as at the first boot |
| fixed firmware | fixed firmware | restored as it is |
| fixed firmware | older firmware | restored, but older firmware reads every input one over again, as it always did |

## Worth re-checking

Setup's two ADC octave steps (`CV1: Input 1.xV` / `3.xV`) read the input the
firmware called CV1, which on older firmware was really panel input 8. If you
ran those steps on older firmware with the voltage patched into panel 1, run
them again: the shared input scale may not be what you meant to set. The
other calibrations (0 V offsets, `Q` fits, curves) were recorded per input
and are moved for you.
