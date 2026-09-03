#ifndef XENOSIM_SIM_BUS_H_
#define XENOSIM_SIM_BUS_H_

#include <stdint.h>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The fake 200e bus behind OC::PresetBus.
//
// The bus MASTER is not faked: src/Bus200eMaster.cpp is linked and driven for
// real, through its own Bus200eMasterOps transport hooks, so the QUERY
// send/reply/timeout FSM, the card-address claim, the activity-based
// done-detection and every timeout constant are the firmware's own. What is
// faked is only what is on the other end of the wire: three modules that answer
// (or don't), and their preset banks, which come from real bench captures.
//
// The simulated bus is the user's actual bus:
//     0x20 -> "210"    answers QUERY, has no bank the simulator can serve
//     0x28 -> "259 A"  answers QUERY, 30 x 33-byte records from a real capture
//     0x5C -> "251 A"  answers QUERY, 30 x 2104-byte slots from a real capture
// Every other address is silent, and costs the firmware's real 1000ms QUERY
// reply timeout -- see SimBusSetRealTiming().
// ---------------------------------------------------------------------------

struct SimBusConfig {
  std::string capture_251e;
  std::string capture_259e;
  bool bus_enabled = true;    // false exercises the "preset bus disabled" screen
  bool case_off = false;      // bus enabled but no module answers: the module
                              // on USB with the case unplugged, or the scan
                              // run before the case has powered up
  bool real_timing = false;   // see SimBusSetRealTiming()
};

// Loads the captures (falling back to synthetic banks, loudly, if they are
// gone) and installs the fake transport into the real Bus200eMaster.
void SimBusInit(const SimBusConfig &cfg);

// True when the captures could not be loaded and synthetic banks are in use.
bool SimBusUsingSyntheticBanks();

// Pump the real master FSM and the fake modules. Call once per simulated tick.
void SimBusTask();

// True while any master or query job is in flight, i.e. while advancing the
// virtual clock would actually change something.
bool SimBusBusy();

// Fast (default): the virtual clock is advanced as fast as the loop can run, so
// a 61-address scan still costs its honest ~60 SIMULATED seconds but returns
// immediately in wall-clock. Real: the virtual clock is pinned to wall-clock,
// so scan pacing feels the way it feels on the module.
void SimBusSetRealTiming(bool on);
bool SimBusRealTiming();

// Push a note-on into the 200e bus MIDI RX ring, which is one of the four
// sources AppBus200e::Controller() drains while a recording is armed.
void SimBusInjectMidiNote(uint8_t note, uint8_t vel);

// What a simulated module does with a RESTORE.
//
// FAITHFUL is the truth for a working module and the default: the bank the
// master sent is what the module now holds, so a subsequent BACKUP serves it
// back. The rest exist so the firmware's post-write read-back verification can
// be exercised without a real module and without a real mis-write -- each one
// is a shape a bad transfer actually takes.
enum SimWriteFault {
  SIM_WRITE_FAITHFUL = 0,
  SIM_WRITE_IGNORE,        // reports done, stores nothing (the silent no-op)
  // Stores all but the last record: the short write. Note this is only
  // DETECTABLE when the edit was in that last record -- truncating away
  // records the user did not change leaves the module matching the intent
  // exactly, and the firmware is right to call that verified.
  SIM_WRITE_DROP_TAIL,
  SIM_WRITE_FLIP_FIRST,    // one byte wrong in the FIRST slot
  SIM_WRITE_FLIP_LAST,     // one byte wrong in the LAST slot -- collateral
  // Stores the bank perfectly, then TRUNCATES THE READ-BACK: the BACKUP that
  // follows the RESTORE serves one slot instead of thirty and still goes
  // quiet, so the master reports DONE having moved 2104 of 63120 bytes.
  //
  // This is NOT drop-tail with a different name, and the distinction is the
  // whole point of the mode. drop-tail is a bad STORE observed through a GOOD
  // read-back: the firmware sees all 63120 bytes and they legitimately match
  // the intent, so "VERIFIED" is the true answer. This is a GOOD store
  // observed through a BAD read-back: the firmware sees 3% of the bank, and
  // the other 97% of the image is still the bytes the RESTORE was sourced
  // from -- so a whole-bank hash of it matches the intent no matter what the
  // module actually holds. Believing that hash is how a write earns
  // "VERIFIED" on evidence it never received.
  SIM_WRITE_SHORT_READBACK,
};
void SimBusSetWriteFault(SimWriteFault f);

// Make the fault above apply to the FIRST restore only, after which the module
// behaves. This is what makes the recovery path testable end to end: the undo
// write is an ordinary write, and through a permanently broken module it can
// only ever land as BAD -- so "the pre-write bank really does come back" could
// not be asserted at all without it.
void SimBusSetWriteFaultOnce(bool once);
SimWriteFault SimBusWriteFault();
const char *SimWriteFaultName(SimWriteFault f);

// A one-line description of what the bus is doing, for the terminal chrome.
std::string SimBusStatusLine();

// The rolling log lives in sim_host.h -- one log for the whole simulator.
// Writes are logged loudly here because a simulated RESTORE changes nothing
// anywhere.
#include "sim_host.h"

#endif  // XENOSIM_SIM_BUS_H_
