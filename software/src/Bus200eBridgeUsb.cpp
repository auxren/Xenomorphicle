// Target-side USB MIDI wiring for Bus200eBridge. See Bus200eBridgeUsb.h for
// how this listens without stealing MIDI from the app that owns the port.
#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)

#include <Arduino.h>
#include <string.h>

#include "Bus200eBridge.h"
#include "Bus200eBridgeUsb.h"
#include "Bus200eMaster.h"
#include "PresetBus.h"
#include "PresetBusCard.h"

namespace OC {
namespace Bus200eBridgeUsb {

// ---- RX staging (the handler may run in ISR context) -----------------------
// One frame in flight, exactly like CaptainMIDI::OnReceiveSysEx: a second
// frame arriving before Task() drains the first is dropped, and the host's
// own flow control (one outstanding request, per-packet ACK) is what keeps
// that from happening -- and it is the reason a 1503-packet 251e bank can
// stream through a single-frame staging buffer at all. Sized for
// BUS200E_SYSEX_MAX_MESSAGE + F0 + F7 (59 under protocol v2, 60 under v1),
// rounded up to 64; hOC's 60-byte frame ceiling is what keeps that true.
static volatile uint16_t rx_len = 0;
static uint8_t rx_buf[64];
static volatile uint32_t rx_dropped = 0;

static bool polling = false;
static bool inited = false;

// NOT FLASHMEM: this can be entered from an app's Controller()-context
// usbMIDI.read() (HSMIDI::ListenForSysEx), and flash-resident code has no
// business in an ISR path on this project (USB/audio foundation rule).
static void usb_sysex_handler(uint8_t *data, unsigned int size) {
  if (rx_len) { rx_dropped++; return; }  // one in flight
  if (size > sizeof(rx_buf)) { rx_dropped++; return; }
  memcpy(rx_buf, data, size);
  rx_len = (uint16_t) size;
}

// ---- ops table -------------------------------------------------------------

static uint32_t op_now_ms() { return millis(); }

static int op_card_serving() { return OC::PresetBus::CardServing() ? 1 : 0; }
static int op_card_serve_enable(int on) { return OC::PresetBus::CardServeEnable(on != 0); }
static uint8_t *op_card_image() { return OC::PresetBus::MasterCardImage(); }
static uint32_t op_card_size() { return BUSCARD_SIZE; }

static int op_master_backup(uint8_t mod_addr) { return OC::PresetBus::MasterBackup(mod_addr); }
static int op_master_restore(uint8_t mod_addr) { return OC::PresetBus::MasterRestore(mod_addr); }
static Bus200eMasterState op_master_state() { return OC::PresetBus::MasterState(); }
static Bus200eMasterError op_master_error() { return OC::PresetBus::MasterError(); }
static uint32_t op_master_bytes() { return Bus200eMasterBytesTransferred(); }
static void op_master_reset() { OC::PresetBus::MasterReset(); }

static void op_send_message(const uint8_t *payload, uint32_t len) {
  if (!usb_configuration) return;   // nobody is listening; don't stall the TX ring
  uint8_t frame[BUS200E_SYSEX_MAX_MESSAGE + 2];
  if (len > BUS200E_SYSEX_MAX_MESSAGE) return;
  frame[0] = 0xF0;
  memcpy(frame + 1, payload, len);
  frame[len + 1] = 0xF7;
  usbMIDI.sendSysEx(len + 2, frame, true);
}

// card_mark_dirty is deliberately NULL. PresetBus.h flags it as an open
// design question that a foreign-module capture shares PBCARD.BIN with
// ordinary local card serving; leaving the image undirtied means the bytes a
// browser pushes are staged in RAM for the RESTORE and are never written
// over the user's local backup file. If a future pass gives foreign dumps
// their own image/file, this is where the hook goes.
static const Bus200eBridgeOps kOps = {
  op_now_ms,
  op_card_serving, op_card_serve_enable, op_card_image, op_card_size, nullptr,
  op_master_backup, op_master_restore, op_master_state, op_master_error,
  op_master_bytes, op_master_reset,
  op_send_message,
};

// ---- public ----------------------------------------------------------------

FLASHMEM void Init() {
  Bus200eBridgeInit(&kOps);
  // The (uint8_t*, unsigned int) overload is the "one call per COMPLETE
  // message" handler (usb_midi_handleSysExComplete); the three-argument one
  // is the chunked/partial variant we do not want.
  usbMIDI.setHandleSystemExclusive(usb_sysex_handler);
  inited = true;
}

void Task() {
  if (!inited) return;

  // Opt-in fallback drain for apps that never touch usbMIDI themselves.
  // OFF by default -- see the header for why enabling it under a
  // MIDI-consuming app costs that app messages.
  if (polling) {
    int budget = 8;
    while (budget-- > 0 && usbMIDI.read()) {}
  }

  if (rx_len) {
    const uint16_t n = rx_len;
    Bus200eBridgeHandleSysEx(rx_buf, n);
    rx_len = 0;
  }
  Bus200eBridgeTask();
}

bool Polling() { return polling; }
void SetPolling(bool on) { polling = on; }

FLASHMEM void DebugDump() {
  static const char *kState[] = { "idle", "capturing", "sending", "receiving", "restoring" };
  const int s = (int) Bus200eBridgeGetState();
  Serial.printf("bridge: %s  mod=%02X  last dump=%lu B  last NAK=%u  "
                "poll=%s  rx dropped=%lu\n",
                (s >= 0 && s < 5) ? kState[s] : "?",
                Bus200eBridgeModAddr(),
                (unsigned long) Bus200eBridgeDumpBytes(),
                Bus200eBridgeLastNak(),
                polling ? "on" : "off",
                (unsigned long) rx_dropped);
  Serial.printf("  master state=%d err=%d bytes=%lu   card serving=%s\n",
                (int) OC::PresetBus::MasterState(),
                (int) OC::PresetBus::MasterError(),
                (unsigned long) Bus200eMasterBytesTransferred(),
                OC::PresetBus::CardServing() ? "yes" : "no");
}

}  // namespace Bus200eBridgeUsb
}  // namespace OC

#endif  // ARDUINO_TEENSY41 && PRESET_BUS
