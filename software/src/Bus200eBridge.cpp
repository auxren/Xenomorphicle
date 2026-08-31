// The browser <-> 200e preset-dump session layer. See Bus200eBridge.h for
// what this joins together and why it is its own file. Pure logic: no USB,
// no MIDI, no Wire, no Arduino -- everything platform-specific arrives
// through Bus200eBridgeOps.
#include <string.h>

// On target this is cold, loop-context glue (a browser transfer is a
// once-in-a-while human action, never an audio/USB hot path), so it lives in
// flash and leaves the scarce ITCM bank alone -- same treatment as
// Bus200eSysEx.cpp. Host builds compile it bare.
#if defined(__IMXRT1062__) || defined(__MK20DX256__)
#include <Arduino.h>
#define BRIDGE_CODE FLASHMEM
#else
#define BRIDGE_CODE
#endif

#include "Bus200eBridge.h"

// ---- session state -----------------------------------------------------------

static const Bus200eBridgeOps *g_ops = 0;

static Bus200eBridgeState g_state = BUS200E_BRIDGE_IDLE;
static uint8_t g_mod_addr = 0;
static uint32_t g_dump_bytes = 0;   // last dump sent/received, raw length
static uint8_t g_last_nak = 0;

// inbound (PUT_DUMP) reassembly
static uint8_t g_rx_seen[(BUS200E_BRIDGE_MAX_PACKETS + 7) / 8];
static uint16_t g_rx_count = 0;     // distinct packets stored
static uint32_t g_rx_len = 0;       // highest end-offset written
static uint32_t g_rx_last_ms = 0;

// outbound (GET_DUMP) streaming
static uint32_t g_tx_len = 0;
static uint8_t g_tx_seq = 0;
static uint8_t g_tx_total = 0;
static uint8_t g_tx_xor = 0;

// ---- small helpers -------------------------------------------------------------

BRIDGE_CODE static int ops_ok(void) {
  return g_ops && g_ops->now_ms && g_ops->card_serving && g_ops->card_serve_enable &&
         g_ops->card_image && g_ops->card_size && g_ops->master_backup &&
         g_ops->master_restore && g_ops->master_state && g_ops->master_error &&
         g_ops->master_bytes && g_ops->master_reset && g_ops->send_message;
}

BRIDGE_CODE static void send_msg(uint8_t cmd, const uint8_t *payload, uint32_t len) {
  uint8_t out[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildMessage(cmd, payload, len, 0, 0, out, sizeof(out));
  if (n > 0) g_ops->send_message(out, (uint32_t) n);
}

BRIDGE_CODE static void send_ack(uint8_t echo_cmd, uint8_t ctx) {
  const uint8_t p[2] = { echo_cmd, ctx };
  send_msg(BUS200E_SYSEX_CMD_ACK, p, 2);
}

BRIDGE_CODE static void send_nak(uint8_t cmd, uint8_t err) {
  g_last_nak = err;
  const uint8_t p[2] = { cmd, err };
  send_msg(BUS200E_SYSEX_CMD_NAK, p, 2);
}

// Bus200eMasterError -> Bus200eSysExNakReason. 7/8/9 exist precisely for
// this mapping (see Bus200eSysEx.h); BAD_ARGS has no dedicated code and
// means "the device refused the request as malformed", which is what NAK 2
// already says.
BRIDGE_CODE static uint8_t nak_for_master(Bus200eMasterError e) {
  switch (e) {
    case BUS200E_MASTER_ERR_BUSY:          return BUS200E_SYSEX_NAK_BUSY;
    case BUS200E_MASTER_ERR_NO_FREE_CARD:  return BUS200E_SYSEX_NAK_NO_FREE_CARD;
    case BUS200E_MASTER_ERR_SEND_TIMEOUT:  return BUS200E_SYSEX_NAK_SEND_TIMEOUT;
    case BUS200E_MASTER_ERR_NO_RESPONSE:   return BUS200E_SYSEX_NAK_NO_RESPONSE;
    case BUS200E_MASTER_ERR_BAD_ARGS:
    case BUS200E_MASTER_ERR_NONE:
    default:                               return BUS200E_SYSEX_NAK_UNKNOWN_CMD;
  }
}

BRIDGE_CODE static void session_reset(void) {
  g_state = BUS200E_BRIDGE_IDLE;
  memset(g_rx_seen, 0, sizeof(g_rx_seen));
  g_rx_count = 0;
  g_rx_len = 0;
  g_rx_last_ms = 0;
  g_tx_len = 0;
  g_tx_seq = 0;
  g_tx_total = 0;
  g_tx_xor = 0;
}

BRIDGE_CODE static int busy(void) { return g_state != BUS200E_BRIDGE_IDLE; }

// ---- public API ----------------------------------------------------------------

BRIDGE_CODE void Bus200eBridgeInit(const Bus200eBridgeOps *ops) {
  g_ops = ops;
  g_mod_addr = 0;
  g_dump_bytes = 0;
  g_last_nak = 0;
  session_reset();
}

BRIDGE_CODE void Bus200eBridgeAbort(void) { session_reset(); }

BRIDGE_CODE Bus200eBridgeState Bus200eBridgeGetState(void) { return g_state; }
BRIDGE_CODE uint8_t Bus200eBridgeModAddr(void) { return g_mod_addr; }
BRIDGE_CODE uint32_t Bus200eBridgeDumpBytes(void) { return g_dump_bytes; }
BRIDGE_CODE uint8_t Bus200eBridgeLastNak(void) { return g_last_nak; }

// ---- command handlers ----------------------------------------------------------

BRIDGE_CODE static void handle_info(void) {
  // Field order is fixed by Bus200eSysEx.h's INFO_R note; extra fields are
  // APPENDED ONLY, so an older host that zips names against the payload just
  // stops early (hOC's own rule -- see CaptainMIDI's PEND_INFO reply).
  //
  // n_sequences / max_steps are 0 ON PURPOSE and will stay 0: the firmware
  // does NOT decode the 251e byte layout. A dump is captured verbatim off
  // the module's FRAM via BACKUP and handed back byte-for-byte; whatever it
  // means is the browser codec's business (tools/251e-sequencer/
  // sequence-codec.js), and that layout is still being reverse-engineered.
  // Reporting an invented 4/16 here would be firmware asserting a format it
  // has never parsed.
  const uint8_t info[] = {
    1,                                     // schema
    0,                                     // n_sequences: unknown to firmware
    0,                                     // max_steps:   unknown to firmware
    (uint8_t) (g_dump_bytes & 0x7F),       // last dump length, 7 bits at a time
    (uint8_t) ((g_dump_bytes >> 7) & 0x7F),
    BUS200E_SYSEX_CHUNK_BYTES,             // appended: raw bytes per DUMP_DATA
    BUS200E_BRIDGE_MAX_PACKETS,            // appended: transfer packet ceiling
    (uint8_t) (g_ops->card_serving() ? 1 : 0),
  };
  send_msg(BUS200E_SYSEX_CMD_INFO_R, info, sizeof(info));
}

BRIDGE_CODE static void handle_status(void) {
  const uint8_t p[] = {
    (uint8_t) g_ops->master_state(),
    (uint8_t) g_ops->master_error(),
    g_mod_addr,
    (uint8_t) (g_state == BUS200E_BRIDGE_RESTORING ? 1 : 0),
    (uint8_t) g_state,   // appended: this module's own session state
  };
  send_msg(BUS200E_SYSEX_CMD_STATUS_R, p, sizeof(p));
}

BRIDGE_CODE static void handle_get_dump(const uint8_t *payload, uint32_t len) {
  if (busy()) { send_nak(BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_BUSY); return; }
  if (len < 1) {
    // No default module: guessing which 200e on the bus to master a BACKUP
    // at is exactly the kind of blind bus traffic this project refuses.
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  const uint8_t mod_addr = payload[0];
  // MasterBackup() claims a card address itself (CardServeEnable's self-test
  // and the 0x50/WPM gate both still apply) -- no CardServeEnable here.
  const int rc = g_ops->master_backup(mod_addr);
  if (rc < 0) {
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, nak_for_master((Bus200eMasterError) (-rc)));
    return;
  }
  g_mod_addr = mod_addr;
  g_state = BUS200E_BRIDGE_CAPTURING;
  send_ack(BUS200E_SYSEX_CMD_GET_DUMP, mod_addr);
}

BRIDGE_CODE static void handle_put_dump(const uint8_t *payload, uint32_t len) {
  if (busy()) { send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, BUS200E_SYSEX_NAK_BUSY); return; }
  if (len < 1) {
    send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  // The card image is where the incoming bytes are staged, so it must exist
  // and be served BEFORE the first DUMP_DATA lands -- and MasterRestore()
  // refuses outright unless we are already CardServing() (PresetBus.h).
  if (!g_ops->card_serving() && g_ops->card_serve_enable(1) != 0) {
    send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, BUS200E_SYSEX_NAK_NO_FREE_CARD);
    return;
  }
  if (!g_ops->card_image()) {
    send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, BUS200E_SYSEX_NAK_NO_FREE_CARD);
    return;
  }
  session_reset();
  g_mod_addr = payload[0];
  g_state = BUS200E_BRIDGE_RECEIVING;
  g_rx_last_ms = g_ops->now_ms();
  send_ack(BUS200E_SYSEX_CMD_PUT_DUMP, g_mod_addr);
}

BRIDGE_CODE static void handle_dump_data(uint8_t seq, uint8_t total,
                                          const uint8_t *raw, uint32_t raw_len) {
  if (g_state != BUS200E_BRIDGE_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_DATA, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  if (seq >= BUS200E_BRIDGE_MAX_PACKETS || total == 0 || seq >= total ||
      total > BUS200E_BRIDGE_MAX_PACKETS || raw_len > BUS200E_SYSEX_CHUNK_BYTES) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_DATA, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }
  uint8_t *image = g_ops->card_image();
  const uint32_t cap = g_ops->card_size();
  const uint32_t off = (uint32_t) seq * BUS200E_SYSEX_CHUNK_BYTES;
  if (!image || off + raw_len > cap) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_DATA, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }

  memcpy(image + off, raw, raw_len);
  // The checksum is recomputed from the image at DUMP_END rather than
  // accumulated here, so a retransmitted or out-of-order packet can't
  // double-count into it.
  const uint8_t bit = (uint8_t) (1u << (seq & 7));
  if (!(g_rx_seen[seq >> 3] & bit)) {
    g_rx_seen[seq >> 3] |= bit;
    g_rx_count++;
  }
  if (off + raw_len > g_rx_len) g_rx_len = off + raw_len;
  g_rx_last_ms = g_ops->now_ms();
  send_ack(BUS200E_SYSEX_CMD_DUMP_DATA, seq);
}

BRIDGE_CODE static void handle_dump_end(const uint8_t *payload, uint32_t len) {
  if (g_state != BUS200E_BRIDGE_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_END, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  if (len < 2) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_END, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }
  const uint8_t n_packets = payload[0];
  const uint8_t xor7 = payload[1];

  // every packet 0..n_packets-1 present, and nothing beyond it
  int complete = (n_packets != 0) && (g_rx_count == n_packets) &&
                 (n_packets <= BUS200E_BRIDGE_MAX_PACKETS);
  for (uint8_t i = 0; complete && i < n_packets; ++i)
    if (!(g_rx_seen[i >> 3] & (uint8_t) (1u << (i & 7)))) complete = 0;

  uint8_t *image = g_ops->card_image();
  if (!complete || !image) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_END, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }
  if (Bus200eSysExXor7(0, image, g_rx_len) != (xor7 & 0x7F)) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_END, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }

  // The bytes are in the card image and verified -- exactly the precondition
  // PresetBus.h's MasterRestore comment says the caller must establish.
  if (g_ops->card_mark_dirty) g_ops->card_mark_dirty();
  g_dump_bytes = g_rx_len;

  const int rc = g_ops->master_restore(g_mod_addr);
  if (rc < 0) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_END, nak_for_master((Bus200eMasterError) (-rc)));
    session_reset();
    return;
  }
  // ACK means "accepted and started", not "finished": a RESTORE runs over
  // real bus time (seconds). The host polls STATUS for the outcome.
  g_state = BUS200E_BRIDGE_RESTORING;
  send_ack(BUS200E_SYSEX_CMD_DUMP_END, n_packets);
}

// ---- RX entry point --------------------------------------------------------------

BRIDGE_CODE void Bus200eBridgeHandleSysEx(const uint8_t *sysex, uint32_t len) {
  if (!ops_ok() || !sysex) return;

  // Accept the frame with or without its F0/F7 wrapper.
  if (len && sysex[0] == 0xF0) { sysex++; len--; }
  if (len && sysex[len - 1] == 0xF7) len--;
  if (len < 5) return;

  uint8_t cmd = 0, seq = 0, total = 0;
  uint8_t raw[BUS200E_SYSEX_CHUNK_BYTES];
  uint32_t raw_len = 0;
  const int rc = Bus200eSysExParseMessage(sysex, len, &cmd, &seq, &total,
                                          raw, sizeof(raw), &raw_len);
  if (rc == -2) return;  // another application's SysEx; not an error
  if (rc == -3) {        // version mismatch: cmd byte is still readable
    send_nak(sysex[4], BUS200E_SYSEX_NAK_VERSION);
    return;
  }
  if (rc < 0) {
    // -1 malformed, -4 bad packed chunk, -5 oversized payload. Everything
    // else in this protocol is small and fixed, so an oversized/garbled
    // frame can only be a dump packet gone wrong.
    send_nak(len >= 5 ? sysex[4] : 0, BUS200E_SYSEX_NAK_CHECKSUM);
    if (g_state == BUS200E_BRIDGE_RECEIVING) session_reset();
    return;
  }

  switch (cmd) {
    case BUS200E_SYSEX_CMD_INFO:      handle_info(); break;
    case BUS200E_SYSEX_CMD_STATUS:    handle_status(); break;
    case BUS200E_SYSEX_CMD_GET_DUMP:  handle_get_dump(raw, raw_len); break;
    case BUS200E_SYSEX_CMD_PUT_DUMP:  handle_put_dump(raw, raw_len); break;
    case BUS200E_SYSEX_CMD_DUMP_DATA: handle_dump_data(seq, total, raw, raw_len); break;
    case BUS200E_SYSEX_CMD_DUMP_END:  handle_dump_end(raw, raw_len); break;
    default:
      send_nak(cmd, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
      break;
  }
}

// ---- pump -------------------------------------------------------------------------

BRIDGE_CODE static void pump_capture(void) {
  const Bus200eMasterState ms = g_ops->master_state();
  if (ms == BUS200E_MASTER_FAILED) {
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, nak_for_master(g_ops->master_error()));
    g_ops->master_reset();
    session_reset();
    return;
  }
  if (ms != BUS200E_MASTER_DONE) return;

  uint32_t n = g_ops->master_bytes();
  const uint32_t cap = g_ops->card_size();
  if (n > cap) n = cap;
  g_ops->master_reset();

  if (n == 0 || !g_ops->card_image()) {
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_NO_RESPONSE);
    session_reset();
    return;
  }
  if (n > BUS200E_BRIDGE_MAX_DUMP_BYTES) {
    // n_packets is one 7-bit field; a bigger capture cannot be described by
    // this protocol version. Refuse rather than hand back a truncated dump
    // the browser would happily write straight back to the module.
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }

  g_dump_bytes = n;
  g_tx_len = n;
  g_tx_seq = 0;
  g_tx_xor = 0;
  g_tx_total = (uint8_t) ((n + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
  g_state = BUS200E_BRIDGE_SENDING;
}

BRIDGE_CODE static void pump_send(void) {
  const uint8_t *image = g_ops->card_image();
  if (!image) {  // card serving was turned off underneath us
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_NO_FREE_CARD);
    session_reset();
    return;
  }
  uint8_t out[BUS200E_SYSEX_MAX_MESSAGE];
  for (int budget = 0; budget < BUS200E_BRIDGE_SEND_BUDGET && g_tx_seq < g_tx_total;
       ++budget) {
    const uint32_t off = (uint32_t) g_tx_seq * BUS200E_SYSEX_CHUNK_BYTES;
    uint32_t chunk = g_tx_len - off;
    if (chunk > BUS200E_SYSEX_CHUNK_BYTES) chunk = BUS200E_SYSEX_CHUNK_BYTES;
    const uint8_t hdr[2] = { g_tx_seq, g_tx_total };
    const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, hdr, 2,
                                            image + off, chunk, out, sizeof(out));
    if (n > 0) g_ops->send_message(out, (uint32_t) n);
    g_tx_xor = Bus200eSysExXor7(g_tx_xor, image + off, chunk);
    g_tx_seq++;
  }
  if (g_tx_seq >= g_tx_total) {
    const uint8_t end[2] = { g_tx_total, g_tx_xor };
    send_msg(BUS200E_SYSEX_CMD_DUMP_END, end, 2);
    session_reset();
  }
}

BRIDGE_CODE static void pump_restore(void) {
  const Bus200eMasterState ms = g_ops->master_state();
  if (ms == BUS200E_MASTER_FAILED) {
    // The host already got its DUMP_END ACK ("accepted"); this is how it
    // learns the bus job itself failed without having to poll STATUS.
    send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, nak_for_master(g_ops->master_error()));
    g_ops->master_reset();
    session_reset();
    return;
  }
  if (ms != BUS200E_MASTER_DONE) return;
  g_ops->master_reset();
  send_ack(BUS200E_SYSEX_CMD_PUT_DUMP, g_mod_addr);
  session_reset();
}

BRIDGE_CODE void Bus200eBridgeTask(void) {
  if (!ops_ok()) return;
  switch (g_state) {
    case BUS200E_BRIDGE_CAPTURING: pump_capture(); break;
    case BUS200E_BRIDGE_SENDING:   pump_send(); break;
    case BUS200E_BRIDGE_RESTORING: pump_restore(); break;
    case BUS200E_BRIDGE_RECEIVING:
      if (g_ops->now_ms() - g_rx_last_ms > BUS200E_BRIDGE_RX_TIMEOUT_MS) {
        send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, BUS200E_SYSEX_NAK_SEND_TIMEOUT);
        session_reset();
      }
      break;
    case BUS200E_BRIDGE_IDLE:
    default:
      break;
  }
}
