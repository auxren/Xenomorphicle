#pragma once
// ---------------------------------------------------------------------------
// The target-side wiring for Bus200eBridge: live USB MIDI SysEx RX/TX, and
// the Bus200eBridgeOps table backed by OC::PresetBus's card image + foreign-
// module bus mastering.
//
// Everything here is the part Bus200eBridge.h deliberately does not do (it
// is BSP-free and host-tested); this file is the small, unavoidable "touch
// the hardware" half.
//
// ---- how it hears SysEx without stealing anyone's MIDI ---------------------
// Init() registers a Teensy usbMIDI SysEx completion handler
// (usbMIDI.setHandleSystemExclusive). Teensy's usb_midi_read() calls that
// handler for a completed frame and STILL returns the message to whoever
// called read() (cores/teensy4/usb_midi.c: the handler call is followed by
// `return 1`, with getType()/getSysExArray() intact). So the bridge listens
// through every existing poller -- Captain MIDI's poll_midi(), Quadrants',
// Hemisphere's, HSMIDI's ListenForSysEx() -- without consuming a single
// message from them. Captain MIDI, the standing app on this rig, already
// ignores frames whose app byte isn't 'M' (CaptainMIDI::HandleSysEx), so
// our 0x35 frames pass by it untouched.
//
// The handler may run in ISR context (ScaleEditor/Backup/Enigma call
// ListenForSysEx() from Controller()), so it does nothing but memcpy one
// frame into a staging buffer -- not FLASHMEM, no parsing, no I/O. Task()
// (loop context) does the work, the same discipline Captain MIDI uses.
//
// SetPolling(true) additionally drains usbMIDI from Task() itself, for apps
// that never poll USB MIDI at all. It is OFF by default and must stay that
// way: an app that DOES poll would race us for the queue and lose notes.
// Console 'y' toggles it.
// ---------------------------------------------------------------------------

namespace OC {
namespace Bus200eBridgeUsb {

#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)

void Init();   // after PresetBus::Init()
void Task();   // from loop()

bool Polling();
void SetPolling(bool on);

void DebugDump();  // one-line session/master status to Serial

#else

inline void Init() {}
inline void Task() {}
inline bool Polling() { return false; }
inline void SetPolling(bool) {}
inline void DebugDump() {}

#endif

}  // namespace Bus200eBridgeUsb
}  // namespace OC
