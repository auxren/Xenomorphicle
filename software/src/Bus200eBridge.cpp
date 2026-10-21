// The browser <-> 200e preset-dump session layer. See Bus200eBridge.h for
// what this joins together and why it is its own file. Pure logic: no USB,
// no MIDI, no Wire, no Arduino -- everything platform-specific arrives
// through Bus200eBridgeOps.
#include <string.h>

// On target this is cold, loop-context glue (a browser transfer is a
// once-in-a-while human action, never an audio/USB hot path), so it lives in
// flash and leaves the scarce ITCM bank alone -- same treatment as
// Bus200eSysEx.cpp. Host builds compile it bare.
#if defined(__IMXRT1062__)
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

// inbound (PUT_DUMP) reassembly. One bit per packet: 1561 packets = 196
// bytes of .bss, the price of tracking completeness/duplicates for a whole
// 64K card image rather than v1's 127-packet, 5588-byte window.
static uint8_t g_rx_seen[(BUS200E_BRIDGE_MAX_PACKETS + 7) / 8];
static uint16_t g_rx_count = 0;     // distinct packets stored
static uint32_t g_rx_len = 0;       // highest end-offset written
static uint32_t g_rx_last_ms = 0;

// outbound (GET_DUMP) streaming
static uint32_t g_tx_len = 0;
static uint16_t g_tx_seq = 0;
static uint16_t g_tx_total = 0;
static uint8_t g_tx_xor = 0;

// firmware-card load (FW_BEGIN .. FW_END)
#define FW_MIN_LEN        0x60u
#define FW_CAP_V1         65536u
#define FW_CAP_V2         0x38000u
static uint8_t  g_fw_kind = 0;
static uint32_t g_fw_len = 0;       // announced image length
static uint32_t g_fw_crc = 0;
static uint16_t g_fw_total = 0;     // ceil(len / 42)
static uint16_t g_fw_next = 0;      // next expected seq == packets received
static uint32_t g_fw_pushed = 0;    // raw bytes handed to fw_push
static uint8_t  g_fw_xor = 0;
static uint8_t  g_fw_magic[8];   // first 8 raw bytes of the stream, for the kind/magic check
static uint32_t g_fw_last_ms = 0;   // last FW_DATA/FW_END (polls do not count)

// sweep (SCAN .. SCAN_END)
static uint8_t  g_scan_next = 0;    // address to query next (may reach last + 1)
static uint8_t  g_scan_last = 0;
static uint8_t  g_scan_found = 0;   // responders reported so far == next SCAN_R idx
static uint8_t  g_scan_active = 0;  // a query_start was accepted and is unresolved
static uint8_t  g_scan_have_done = 0;
static uint32_t g_scan_done_ms = 0; // when the previous query was resolved
static uint8_t  g_scan_trying = 0;  // query_start for g_scan_next has been refused busy
static uint32_t g_scan_try_ms = 0;  // when that first attempt was made

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

// ACK{cmd, mod_addr} -- the one-byte context form, for GET_DUMP/PUT_DUMP.
BRIDGE_CODE static void send_ack(uint8_t echo_cmd, uint8_t ctx) {
  const uint8_t p[2] = { echo_cmd, ctx };
  send_msg(BUS200E_SYSEX_CMD_ACK, p, 2);
}

// ACK{cmd, lo, hi} -- the septet-pair form, for the two contexts that are
// 14-bit as of protocol v2: DUMP_DATA's seq and DUMP_END's n_packets.
BRIDGE_CODE static void send_ack14(uint8_t echo_cmd, uint16_t ctx) {
  const uint8_t p[3] = { echo_cmd, BUS200E_SYSEX_LO7(ctx), BUS200E_SYSEX_HI7(ctx) };
  send_msg(BUS200E_SYSEX_CMD_ACK, p, 3);
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
  g_fw_kind = 0;
  g_fw_len = 0;
  g_fw_crc = 0;
  g_fw_total = 0;
  g_fw_next = 0;
  g_fw_pushed = 0;
  g_fw_xor = 0;
  memset(g_fw_magic, 0, sizeof(g_fw_magic));
  g_fw_last_ms = 0;
  g_scan_next = 0;
  g_scan_last = 0;
  g_scan_found = 0;
  g_scan_active = 0;
  g_scan_have_done = 0;
  g_scan_done_ms = 0;
  g_scan_trying = 0;
  g_scan_try_ms = 0;
}

// The scan shortens the master's QUERY reply timeout; every way out of a scan
// puts the default back (NULL-tolerant: the op is optional).
BRIDGE_CODE static void scan_timeout_restore(void) {
  if (g_ops && g_ops->query_set_reply_timeout_ms)
    g_ops->query_set_reply_timeout_ms(BUS200E_MASTER_QUERY_REPLY_TIMEOUT_MS);
}

// Leaving a session from outside its own handlers (Abort, re-Init): release
// whatever the session holds on the far side, then drop to IDLE.
BRIDGE_CODE static void end_session(void) {
  if (g_ops) {
    if (g_state == BUS200E_BRIDGE_FW_RECEIVING && g_ops->fw_abort) g_ops->fw_abort();
    if (g_state == BUS200E_BRIDGE_SCANNING) {
      if (g_ops->query_reset) g_ops->query_reset();
      scan_timeout_restore();
    }
  }
  session_reset();
}

BRIDGE_CODE static int busy(void) { return g_state != BUS200E_BRIDGE_IDLE; }

// ---- public API ----------------------------------------------------------------

BRIDGE_CODE void Bus200eBridgeInit(const Bus200eBridgeOps *ops) {
  end_session();   // against the PREVIOUS ops, before they are replaced
  g_ops = ops;
  g_mod_addr = 0;
  g_dump_bytes = 0;
  g_last_nak = 0;
  session_reset();
}

BRIDGE_CODE void Bus200eBridgeAbort(void) { end_session(); }

BRIDGE_CODE Bus200eBridgeState Bus200eBridgeGetState(void) { return g_state; }
BRIDGE_CODE uint8_t Bus200eBridgeModAddr(void) { return g_mod_addr; }
BRIDGE_CODE uint32_t Bus200eBridgeDumpBytes(void) { return g_dump_bytes; }
BRIDGE_CODE uint8_t Bus200eBridgeLastNak(void) { return g_last_nak; }

// ---- command handlers ----------------------------------------------------------

// Cached version frame of `addr`: 1 and ver[7] filled if known, else 0.
BRIDGE_CODE static int version_for(uint8_t addr, uint8_t ver[7]) {
  memset(ver, 0, 7);
  if (addr == 0 || !g_ops->fw_version_get) return 0;
  return g_ops->fw_version_get(addr, ver) == 1 ? 1 : 0;
}

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
    2,                                     // schema (2 = protocol v2 layout)
    0,                                     // n_sequences: unknown to firmware
    0,                                     // max_steps:   unknown to firmware
    // Last dump length, low septet first. THREE septets, not v1's two: the
    // transfer this bridge exists for is 63120 bytes, and 14 bits stops at
    // 16383. 21 bits covers the whole 64K card image with room to spare.
    (uint8_t) (g_dump_bytes & 0x7F),
    (uint8_t) ((g_dump_bytes >> 7) & 0x7F),
    (uint8_t) ((g_dump_bytes >> 14) & 0x7F),
    BUS200E_SYSEX_CHUNK_BYTES,             // appended: raw bytes per DUMP_DATA
    BUS200E_SYSEX_LO7(BUS200E_BRIDGE_MAX_PACKETS),   // appended: packet ceiling,
    BUS200E_SYSEX_HI7(BUS200E_BRIDGE_MAX_PACKETS),   //           lo/hi septets
    (uint8_t) (g_ops->card_serving() ? 1 : 0),
    BUS200E_SYSEX_BRIDGE_LEVEL,            // appended: bridge feature level
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
  if (g_state == BUS200E_BRIDGE_FW_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_FW_BUSY);
    return;
  }
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
  if (g_state == BUS200E_BRIDGE_FW_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, BUS200E_SYSEX_NAK_FW_BUSY);
    return;
  }
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

BRIDGE_CODE static void handle_dump_data(uint16_t seq, uint16_t total,
                                          const uint8_t *raw, uint32_t raw_len) {
  if (g_state != BUS200E_BRIDGE_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_DATA, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  // seq is what indexes g_rx_seen, so the ceiling check is what keeps a
  // hostile/garbled 14-bit seq out of the bitmap -- load-bearing, not tidy.
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
  send_ack14(BUS200E_SYSEX_CMD_DUMP_DATA, seq);
}

BRIDGE_CODE static void handle_dump_end(const uint8_t *payload, uint32_t len) {
  if (g_state != BUS200E_BRIDGE_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_END, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  if (len < 3) {
    send_nak(BUS200E_SYSEX_CMD_DUMP_END, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }
  const uint16_t n_packets = BUS200E_SYSEX_FROM14(payload[0], payload[1]);
  const uint8_t xor7 = payload[2];

  // every packet 0..n_packets-1 present, and nothing beyond it
  int complete = (n_packets != 0) && (g_rx_count == n_packets) &&
                 (n_packets <= BUS200E_BRIDGE_MAX_PACKETS);
  for (uint16_t i = 0; complete && i < n_packets; ++i)
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
  // real bus time (seconds). The outcome arrives later as an unsolicited
  // ACK/NAK PUT_DUMP frame from pump_restore (STATUS reads IDLE afterward).
  g_state = BUS200E_BRIDGE_RESTORING;
  send_ack14(BUS200E_SYSEX_CMD_DUMP_END, n_packets);
}

// ---- firmware-card load (docs/bus200e-sysex-fw.md) -----------------------------

BRIDGE_CODE static int fw_ops_ok(void) {
  return g_ops->fw_begin && g_ops->fw_push && g_ops->fw_commit && g_ops->fw_abort &&
         g_ops->fw_armed && g_ops->fw_arm && g_ops->fw_serving &&
         g_ops->fw_served_bytes && g_ops->fw_first_read_age_s &&
         g_ops->fw_image_len && g_ops->fw_image_kind && g_ops->wpm_present;
}

// Every NAK exit of a load: abort the card side, drop to IDLE, then NAK.
BRIDGE_CODE static void fw_fail(uint8_t cmd, uint8_t err) {
  g_ops->fw_abort();
  session_reset();
  send_nak(cmd, err);
}

BRIDGE_CODE static void handle_fw_begin(const uint8_t *payload, uint32_t len) {
  if (g_state == BUS200E_BRIDGE_FW_RECEIVING) {   // a load is already in flight
    fw_fail(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_NAK_FW_BUSY);
    return;
  }
  uint8_t kind = 0;
  uint32_t image_len = 0, crc = 0;
  if (Bus200eSysExParseFwBegin(payload, len, &kind, &image_len, &crc) != 0) {
    send_nak(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_NAK_FW_LENGTH);
    return;
  }
  if (kind != 1 && kind != 2) {
    send_nak(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_NAK_FW_MAGIC);
    return;
  }
  const uint32_t cap = (kind == 1) ? FW_CAP_V1 : FW_CAP_V2;
  if (image_len < FW_MIN_LEN || image_len > cap) {
    send_nak(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_NAK_FW_LENGTH);
    return;
  }
  if (g_state == BUS200E_BRIDGE_SCANNING) {
    send_nak(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_NAK_BUSY);
    return;
  }
  if (g_ops->card_serving() || g_ops->fw_serving() || busy() ||
      g_ops->master_state() != BUS200E_MASTER_IDLE) {
    send_nak(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_NAK_FW_BUSY);
    return;
  }
  if (g_ops->wpm_present()) {
    send_nak(BUS200E_SYSEX_CMD_FW_BEGIN, BUS200E_SYSEX_NAK_NO_FREE_CARD);
    return;
  }
  const int rc = g_ops->fw_begin(kind);
  if (rc < 0) {
    send_nak(BUS200E_SYSEX_CMD_FW_BEGIN,
             rc == -6 ? BUS200E_SYSEX_NAK_FW_NOMEM : BUS200E_SYSEX_NAK_FW_BUSY);
    return;
  }
  session_reset();
  g_fw_kind = kind;
  g_fw_len = image_len;
  g_fw_crc = crc;
  g_fw_total = (uint16_t) ((image_len + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
  g_fw_last_ms = g_ops->now_ms();
  g_state = BUS200E_BRIDGE_FW_RECEIVING;
  const uint8_t p[1] = { BUS200E_SYSEX_CMD_FW_BEGIN };
  send_msg(BUS200E_SYSEX_CMD_ACK, p, 1);
}

BRIDGE_CODE static void handle_fw_data(uint16_t seq, uint16_t total,
                                        const uint8_t *raw, uint32_t raw_len) {
  if (g_state != BUS200E_BRIDGE_FW_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_FW_DATA, BUS200E_SYSEX_NAK_FW_SEQUENCE);
    return;
  }
  const int last = (g_fw_total != 0 && seq == g_fw_total - 1);
  if (seq != g_fw_next || total != g_fw_total || seq >= g_fw_total ||
      raw_len > BUS200E_SYSEX_CHUNK_BYTES ||
      (!last && raw_len != BUS200E_SYSEX_CHUNK_BYTES) ||
      g_fw_pushed + raw_len > g_fw_len) {
    fw_fail(BUS200E_SYSEX_CMD_FW_DATA, BUS200E_SYSEX_NAK_FW_SEQUENCE);
    return;
  }
  for (uint32_t i = 0; i < raw_len; ++i) {
    if (g_fw_pushed + i < sizeof(g_fw_magic)) g_fw_magic[g_fw_pushed + i] = raw[i];
    g_ops->fw_push(raw[i]);
  }
  g_fw_xor = Bus200eSysExXor7(g_fw_xor, raw, raw_len);
  g_fw_pushed += raw_len;
  g_fw_next++;
  g_fw_last_ms = g_ops->now_ms();
  send_ack14(BUS200E_SYSEX_CMD_FW_DATA, seq);   // after the whole chunk is pushed
}

BRIDGE_CODE static void handle_fw_end(const uint8_t *payload, uint32_t len) {
  if (g_state != BUS200E_BRIDGE_FW_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_FW_END, BUS200E_SYSEX_NAK_FW_SEQUENCE);
    return;
  }
  if (len != 3) { fw_fail(BUS200E_SYSEX_CMD_FW_END, BUS200E_SYSEX_NAK_CHECKSUM); return; }
  const uint16_t n = BUS200E_SYSEX_FROM14(payload[0], payload[1]);
  if (n != g_fw_total || n != g_fw_next || payload[2] != g_fw_xor) {
    fw_fail(BUS200E_SYSEX_CMD_FW_END, BUS200E_SYSEX_NAK_CHECKSUM);
    return;
  }
  // The card layer sizes its buffer from `kind`; a stream whose header says the
  // other format (or neither) must never reach the commit.
  static const uint8_t kMagicV1[8] = { '#', '!', '2', '0', '0', 'E', 'F', 'U' };
  static const uint8_t kMagicV2[8] = { '#', '$', '2', '0', '0', 'E', 'F', 'U' };
  const uint8_t *want = (g_fw_kind == 1) ? kMagicV1 : (g_fw_kind == 2) ? kMagicV2 : NULL;
  if (!want || g_fw_pushed < sizeof(g_fw_magic) || memcmp(g_fw_magic, want, 8) != 0) {
    fw_fail(BUS200E_SYSEX_CMD_FW_END, BUS200E_SYSEX_NAK_FW_MAGIC);
    return;
  }
  const uint32_t image_len = g_fw_len, crc = g_fw_crc;
  const int rc = g_ops->fw_commit(image_len, crc);
  if (rc == 0) {
    session_reset();
    send_ack14(BUS200E_SYSEX_CMD_FW_END, n);
    return;
  }
  uint8_t err;
  switch (rc) {
    case -2: err = BUS200E_SYSEX_NAK_FW_LENGTH; break;
    case -3: err = BUS200E_SYSEX_NAK_FW_MAGIC;  break;
    case -4: err = BUS200E_SYSEX_NAK_FW_CRC;    break;
    case -5: err = BUS200E_SYSEX_NAK_FW_WRITE;  break;
    case -1:
    case -6:
    default: err = BUS200E_SYSEX_NAK_FW_NOMEM;  break;
  }
  fw_fail(BUS200E_SYSEX_CMD_FW_END, err);
}

BRIDGE_CODE static void handle_fw_arm(const uint8_t *payload, uint32_t len) {
  if (len != 1 || payload[0] > 1) {
    send_nak(BUS200E_SYSEX_CMD_FW_ARM, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  g_ops->fw_arm(payload[0]);
  const uint8_t p[2] = { BUS200E_SYSEX_CMD_FW_ARM, payload[0] };
  send_msg(BUS200E_SYSEX_CMD_ACK, p, 2);
}

BRIDGE_CODE static void handle_fw_status(void) {
  Bus200eFwStatus st;
  memset(&st, 0, sizeof(st));
  st.armed = g_ops->fw_armed() ? 1 : 0;
  st.serving = g_ops->fw_serving() ? 1 : 0;
  st.kind = (uint8_t) (g_ops->fw_image_kind() & 0x7F);
  uint32_t served = g_ops->fw_served_bytes();
  st.served = served > 0x1FFFFFu ? 0x1FFFFFu : served;
  const uint32_t age = g_ops->fw_first_read_age_s();
  st.first_read_age_s = (age == 0xFFFFFFFFu) ? 0x3FFF : (age > 0x3FFE ? 0x3FFE : (uint16_t) age);
  const uint32_t il = g_ops->fw_image_len();
  st.image_len = il > 0x1FFFFFu ? 0x1FFFFFu : il;
  // The LAST 0x13 version frame seen on the bus, from the version cache. The
  // codec sends seven spaces when ver_valid is 0.
  const uint8_t va = g_ops->last_version_addr ? g_ops->last_version_addr() : 0;
  st.ver_addr = va;
  st.ver_valid = (uint8_t) version_for(va, st.ver);
  st.ver_gen = g_ops->fw_version_gen ? (uint16_t) (g_ops->fw_version_gen() & 0x3FFF) : 0;
  uint8_t out[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildFwStatusR(&st, out, sizeof(out));
  if (n < 0) return;
  g_ops->send_message(out, (uint32_t) n);
}

// ---- SCAN / POLL_DONE / BUS_STATUS (docs/bus200e-sysex-fw.md) -------------------

BRIDGE_CODE static void handle_scan(const uint8_t *payload, uint32_t len) {
  if (!g_ops->query_start || !g_ops->query_state || !g_ops->query_reset ||
      len != 2 || payload[0] > 0x7F || payload[1] > 0x7F) {
    send_nak(BUS200E_SYSEX_CMD_SCAN, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  if (g_state == BUS200E_BRIDGE_FW_RECEIVING) {   // the doc: a load in flight is FW_BUSY
    send_nak(BUS200E_SYSEX_CMD_SCAN, BUS200E_SYSEX_NAK_FW_BUSY);
    return;
  }
  if (busy() || g_ops->master_state() != BUS200E_MASTER_IDLE) {
    send_nak(BUS200E_SYSEX_CMD_SCAN, BUS200E_SYSEX_NAK_BUSY);
    return;
  }
  session_reset();
  g_scan_next = payload[0];
  g_scan_last = payload[1];
  g_state = BUS200E_BRIDGE_SCANNING;
  if (g_ops->query_set_reply_timeout_ms)
    g_ops->query_set_reply_timeout_ms(BUS200E_BRIDGE_SCAN_REPLY_TIMEOUT_MS);
  const uint8_t p[3] = { BUS200E_SYSEX_CMD_SCAN, payload[0], payload[1] };
  send_msg(BUS200E_SYSEX_CMD_ACK, p, 3);
}

BRIDGE_CODE static void handle_poll_done(void) {
  if (!g_ops->send_poll_done) {
    send_nak(BUS200E_SYSEX_CMD_POLL_DONE, BUS200E_SYSEX_NAK_UNKNOWN_CMD);
    return;
  }
  if (g_state == BUS200E_BRIDGE_FW_RECEIVING) {
    send_nak(BUS200E_SYSEX_CMD_POLL_DONE, BUS200E_SYSEX_NAK_FW_BUSY);
    return;
  }
  if (busy() || g_ops->master_state() != BUS200E_MASTER_IDLE) {
    send_nak(BUS200E_SYSEX_CMD_POLL_DONE, BUS200E_SYSEX_NAK_BUSY);
    return;
  }
  if (g_ops->send_poll_done() != 0) {
    send_nak(BUS200E_SYSEX_CMD_POLL_DONE, BUS200E_SYSEX_NAK_SEND_TIMEOUT);
    return;
  }
  const uint8_t p[1] = { BUS200E_SYSEX_CMD_POLL_DONE };
  send_msg(BUS200E_SYSEX_CMD_ACK, p, 1);
}

BRIDGE_CODE static void handle_bus_status(void) {
  const uint8_t wpm = (g_ops->wpm_present && g_ops->wpm_present()) ? 1 : 0;
  const uint8_t quiet = (!g_ops->bus_quiet || g_ops->bus_quiet()) ? 1 : 0;
  const uint8_t dialect = g_ops->bus_dialect ? (uint8_t) (g_ops->bus_dialect() & 0x7F) : 0;
  uint8_t out[BUS200E_SYSEX_MAX_MESSAGE];
  const int n = Bus200eSysExBuildBusStatusR(wpm, quiet, dialect,
                                            (uint8_t) g_ops->master_error(),
                                            out, sizeof(out));
  if (n > 0) g_ops->send_message(out, (uint32_t) n);
}

// ---- RX entry point --------------------------------------------------------------

BRIDGE_CODE void Bus200eBridgeHandleSysEx(const uint8_t *sysex, uint32_t len) {
  if (!ops_ok() || !sysex) return;

  // Accept the frame with or without its F0/F7 wrapper.
  if (len && sysex[0] == 0xF0) { sysex++; len--; }
  if (len && sysex[len - 1] == 0xF7) len--;
  if (len < 5) return;

  uint8_t cmd = 0;
  uint16_t seq = 0, total = 0;
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
    if (sysex[4] == BUS200E_SYSEX_CMD_FW_DATA) {
      // A garbled FW_DATA chunk is a sequence failure and ends the load.
      if (!fw_ops_ok()) send_nak(sysex[4], BUS200E_SYSEX_NAK_UNKNOWN_CMD);
      else if (g_state == BUS200E_BRIDGE_FW_RECEIVING) fw_fail(sysex[4], BUS200E_SYSEX_NAK_FW_SEQUENCE);
      else send_nak(sysex[4], BUS200E_SYSEX_NAK_FW_SEQUENCE);
      return;
    }
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
    case BUS200E_SYSEX_CMD_SCAN:       handle_scan(raw, raw_len); break;
    case BUS200E_SYSEX_CMD_POLL_DONE:  handle_poll_done(); break;
    case BUS200E_SYSEX_CMD_BUS_STATUS: handle_bus_status(); break;
    case BUS200E_SYSEX_CMD_FW_BEGIN:
    case BUS200E_SYSEX_CMD_FW_DATA:
    case BUS200E_SYSEX_CMD_FW_END:
    case BUS200E_SYSEX_CMD_FW_ARM:
    case BUS200E_SYSEX_CMD_FW_STATUS:
      if (!fw_ops_ok()) { send_nak(cmd, BUS200E_SYSEX_NAK_UNKNOWN_CMD); break; }
      if (cmd == BUS200E_SYSEX_CMD_FW_BEGIN)       handle_fw_begin(raw, raw_len);
      else if (cmd == BUS200E_SYSEX_CMD_FW_DATA)   handle_fw_data(seq, total, raw, raw_len);
      else if (cmd == BUS200E_SYSEX_CMD_FW_END)    handle_fw_end(raw, raw_len);
      else if (cmd == BUS200E_SYSEX_CMD_FW_ARM)    handle_fw_arm(raw, raw_len);
      else                                         handle_fw_status();
      break;
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
    // Only reachable on a build whose card image is larger than the address
    // space a card pointer can name -- i.e. never, today. Kept because the
    // alternative is handing back a truncated dump the browser would happily
    // write straight back to the module.
    send_nak(BUS200E_SYSEX_CMD_GET_DUMP, BUS200E_SYSEX_NAK_CHECKSUM);
    session_reset();
    return;
  }

  g_dump_bytes = n;
  g_tx_len = n;
  g_tx_seq = 0;
  g_tx_xor = 0;
  g_tx_total = (uint16_t) ((n + BUS200E_SYSEX_CHUNK_BYTES - 1) / BUS200E_SYSEX_CHUNK_BYTES);
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
    const uint8_t hdr[BUS200E_SYSEX_DUMP_HDR_BYTES] = {
      BUS200E_SYSEX_LO7(g_tx_seq),   BUS200E_SYSEX_HI7(g_tx_seq),
      BUS200E_SYSEX_LO7(g_tx_total), BUS200E_SYSEX_HI7(g_tx_total),
    };
    const int n = Bus200eSysExBuildMessage(BUS200E_SYSEX_CMD_DUMP_DATA, hdr, sizeof(hdr),
                                            image + off, chunk, out, sizeof(out));
    if (n > 0) g_ops->send_message(out, (uint32_t) n);
    g_tx_xor = Bus200eSysExXor7(g_tx_xor, image + off, chunk);
    g_tx_seq++;
  }
  if (g_tx_seq >= g_tx_total) {
    const uint8_t end[3] = {
      BUS200E_SYSEX_LO7(g_tx_total), BUS200E_SYSEX_HI7(g_tx_total), g_tx_xor,
    };
    send_msg(BUS200E_SYSEX_CMD_DUMP_END, end, 3);
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

// One step of the sweep. Timeouts (2 s send + 1 s reply) live inside the
// master query; the only timer here is the gap between two queries.
BRIDGE_CODE static void pump_scan(void) {
  if (!g_ops->query_start || !g_ops->query_state || !g_ops->query_reset) {
    end_session();   // ops swapped under us: nothing left to drive
    return;
  }
  const uint32_t now = g_ops->now_ms();
  if (g_scan_active) {
    const Bus200eQueryState qs = g_ops->query_state();
    if (qs == BUS200E_QUERY_SENDING || qs == BUS200E_QUERY_WAITING) return;
    if (qs == BUS200E_QUERY_DONE) {
      uint8_t ver[7];
      const int valid = version_for(g_scan_next, ver);
      uint8_t out[BUS200E_SYSEX_MAX_MESSAGE];
      const int n = Bus200eSysExBuildScanR(g_scan_found, 0x7F, g_scan_next,
                                           (uint8_t) valid, ver, out, sizeof(out));
      if (n > 0) g_ops->send_message(out, (uint32_t) n);
      g_scan_found++;
    }
    // DONE, FAILED, or an IDLE that should not happen: resolved either way.
    g_ops->query_reset();
    g_scan_active = 0;
    g_scan_have_done = 1;
    g_scan_done_ms = now;
    g_scan_next++;
    g_scan_trying = 0;
  }
  if (g_scan_next > g_scan_last) {
    const uint8_t n_found = g_scan_found;
    scan_timeout_restore();
    session_reset();
    send_msg(BUS200E_SYSEX_CMD_SCAN_END, &n_found, 1);
    return;
  }
  if (g_scan_have_done && (uint32_t) (now - g_scan_done_ms) < BUS200E_BRIDGE_SCAN_GAP_MS) return;
  const int rc = g_ops->query_start(g_scan_next);
  if (rc == 0) {
    g_scan_active = 1;
    g_scan_trying = 0;
    return;
  }
  if (-rc == (int) BUS200E_MASTER_ERR_BUSY) {
    // Busy: retry on later ticks, but only for BUS200E_BRIDGE_SCAN_ADDR_DEADLINE_MS
    // from the first refusal, so a foreign query that is never reset cannot
    // hold the sweep (and the host's SCAN_END) hostage.
    if (!g_scan_trying) { g_scan_trying = 1; g_scan_try_ms = now; return; }
    if ((uint32_t) (now - g_scan_try_ms) < BUS200E_BRIDGE_SCAN_ADDR_DEADLINE_MS) return;
  }
  // Refused outright (bad address, ...) or the deadline expired: skip the
  // address, no SCAN_R.
  g_scan_trying = 0;
  g_scan_have_done = 1;
  g_scan_done_ms = now;
  g_scan_next++;
}

BRIDGE_CODE void Bus200eBridgeTask(void) {
  if (!ops_ok()) return;
  switch (g_state) {
    case BUS200E_BRIDGE_CAPTURING: pump_capture(); break;
    case BUS200E_BRIDGE_SENDING:   pump_send(); break;
    case BUS200E_BRIDGE_RESTORING: pump_restore(); break;
    case BUS200E_BRIDGE_SCANNING:  pump_scan(); break;
    case BUS200E_BRIDGE_RECEIVING:
      if (g_ops->now_ms() - g_rx_last_ms > BUS200E_BRIDGE_RX_TIMEOUT_MS) {
        send_nak(BUS200E_SYSEX_CMD_PUT_DUMP, BUS200E_SYSEX_NAK_SEND_TIMEOUT);
        session_reset();
      }
      break;
    case BUS200E_BRIDGE_FW_RECEIVING:
      // Stall: no FW_DATA/FW_END for 10 s. Silent; the next FW_DATA gets NAK 16.
      if (g_ops->now_ms() - g_fw_last_ms > BUS200E_BRIDGE_RX_TIMEOUT_MS) {
        if (g_ops->fw_abort) g_ops->fw_abort();
        session_reset();
      }
      break;
    case BUS200E_BRIDGE_IDLE:
    default:
      break;
  }
}
