#ifndef CALRUNACTIVE_H_
#define CALRUNACTIVE_H_

// One question, asked from places that must not pull in the Cal Routine's flow
// engines: is a Cal Routine run open (any target), or is a cancelled 259e run
// still waiting for its COMMIT verdict? While it is, nothing else may write to a
// 259e or restore presets over the 200e bus: the console 'W' (PUT/COMMIT frames),
// 'm' and 'x', the USB SysEx bridge's GET_DUMP / PUT_DUMP, and the 200e app's
// reads and writes all refuse. Defined in CalRoutinePorts.cpp; the host and
// simulator builds that do not link it answer false (see CalRunActive.cpp users).
namespace CalRoutinePorts {
bool CalRunActive();

// True while a console '=' read of a 259e/251e (its own bus client) has a job in
// flight. The USB bridge's MOD_XFER refuses (NAK 17) while it is, so the two bus
// clients never interleave frames. False in builds without the console.
bool Cal259JobActive();
}

#endif  // CALRUNACTIVE_H_
