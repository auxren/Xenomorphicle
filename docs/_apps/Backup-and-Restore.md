---
layout: default
---
# Backup / Restore

Backup/Restore is a utility that allows you to transfer complete sets of app and/or calibration data to and from your module. Possible uses include:

- Transferring app data to a new O_C module,
- Moving calibration to a replacement Teensy for the same O_C module,
- Changing settings between pieces during live shows,
- Intercontinental internet-based collaboration with other Hemisphere Suite users, or
- Simply preparing for inevitable disaster.

## Backing Up Your Module

Backup/Restore backs up data as it exists in the EEPROM. So if you want to take a backup of the O_C's current state, save the module's state by long-pressing the right button from the main menu.

Connect your computer or tablet to the module and enter the Backup/Restore app on your module. Turn either encoder to choose "Data" or "Calibration." Enable recording on your SysEx Librarian software, and then press the right encoder button ([BACKUP]). The size of the data dump will be 172 bytes for Calibration, and about 2.5K for Data.

## Restoring Your Module
Connect your computer or tablet to the module and enter the Backup/Restore app on your module. Press the left encoder button ([RESTORE]). Backup/Restore will indicate that it is listening for SysEx data. From your SysEx Librarian software, initiate the dump. The module's screen should indicate a progress bar. When the restore is successful, the module returns to the saved state.

A restored calibration takes effect at once; no power cycle is needed.

## What each region carries

- **Calibration** carries the DAC and ADC calibration page and, on a unit calibrated by [Cal Routine](Cal-Routine), its per-input curves. The curves are sent first, as their own packets; firmware without Cal Routine ignores them. They are stored only once the whole calibration region has arrived, so an interrupted restore leaves the old curves in place.
- **Data** carries the app settings. It does not carry any calibration: the per-input line fits and the "calibrated by Cal Routine" marker stay as they are on the receiving unit, and a Data restore never overwrites them.
- A Calibration restore replaces the output tables, so the receiving unit no longer counts them as Cal Routine tables, and Setup stops warning before its DAC steps replace them.
- Calibration is per unit and per orientation. If the two units differ in Setup's controls-and-IO reversal (the left/right arrow in [Setup / About](../Setup-About)), the restored calibration lands on different physical jacks than the ones measured: run Cal Routine (or Setup's calibration) again instead.

*© 2018-2022, Jason Justian and Beige Maze Laboratories.*
