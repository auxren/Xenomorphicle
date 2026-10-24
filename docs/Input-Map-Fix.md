# Input map fix (2026-10-24)

## What was wrong

On the Xenomorpher the CV inputs could be read one input over: a CV patched
into panel input n showed up in the firmware as input n+1, and panel input 8
as input 1. Every app saw it; a CV into panel 3 drove CV4 in Quadrants. The
outputs were never affected.

Whether a given firmware build had the problem depended on the build itself.
The 8-channel ADC is read in a fixed rotation of eight inputs, and the old
start-up code began that rotation at a point that depended on how long the
start-up took. The same build always started the same way, so it looked
stable from boot to boot, but a different build could start one step
earlier or later. On the bench, two builds that differed only in the code
that sorts the readings started the rotation one step apart.

## What the fix does

The rotation now always starts from the same point, whatever the build: the
ADC capture is fully set up before its clock is allowed to run. Panel input
n is CVn in every app, and column n in the console's `v` stream, on every
build. If the capture ever loses a reading (which would shift every input
by one), the firmware notices and restarts it in step.

It applies only to hardware with the external 8-channel ADC, which is every
Xenomorpher. Hardware that uses the Teensy's own ADC is unchanged.

## Your stored calibration

Stored per-input calibration is **not** moved. Calibration recorded on
older firmware was read through whichever start-up that build happened to
have, so it may be on the right inputs or one input over, and the firmware
has no way to tell which.

- If the module was calibrated on older firmware (Setup's ADC steps, the
  console's `Q`, or the Cal Routine), **run that calibration again** after
  updating. At boot the console prints a line naming any calibration stored
  before the fix.
- Calibration made after the update is recorded as such, and the line goes
  away once each kind has been re-run.
- If you go back to older firmware, recalibrate there, and then update again,
  re-run that calibration after the update too. The fixed firmware cannot
  tell that it was recorded the old way.

## Backups

Backups and restores work as before. A calibration backup made on older
firmware restores as it was made, which means it carries the same
uncertainty as above: re-run the calibration after restoring it.
