#ifndef BUS200ESYSEX_H_
#define BUS200ESYSEX_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// SysEx framing for carrying a captured/edited 200e preset dump to and from
// a browser applet over the Xenomorpher's existing USB MIDI interface.
// Piggybacked on USB MIDI (manufacturer-ID-prefixed, chunked, checksummed)
// rather than a new WebUSB descriptor or a serial console, per this
// project's foundation requirement: USB MIDI+audio stays best-quality/
// lowest-latency, and FLASHMEM must never sit in a hot USB/audio ISR path.
//
// Nothing in this file touches USB, MIDI libraries, or interrupts -- it is
// pure encode/decode/chunk/checksum logic, host-testable exactly like
// PresetBus200e.cpp (see test_bus200e_sysex.cpp). The live device-side USB
// MIDI SysEx RX handler and the multi-message session state that calls into
// this now exist ONE LAYER UP: Bus200eBridge.{h,cpp} owns the FSM (and is
// host-tested in test_bus200e_bridge.cpp), Bus200eBridgeUsb.{h,cpp} owns the
// usbMIDI registration and the OC::PresetBus-backed ops. Neither has been
// run against real hardware yet.
//
// THIS IS THE CONTRACT a web-app developer needs to match exactly. It is
// deliberately the SAME FAMILY as this repo's one other real, shipped SysEx
// protocol -- hOC Captain MIDI's, spec'd in docs/hoc-midi-sysex.md and
// implemented in tools/hoc_sysex.py -- not a fresh invention: same
// manufacturer ID, same "Beige Maze" family byte, same frame shape, same
// INFO/GET_DUMP/DUMP_DATA/DUMP_END/ACK/NAK command shape and flow-control
// discipline (one outstanding request at a time). A prototype web applet
// already exists at tools/251e-sequencer/ (sysex-transport.js,
// sequence-codec.js) built against a self-declared PLACEHOLDER version of
// this same shape, explicitly waiting for firmware's real framing to land;
// this header formalizes that hand-off. APP_ID below (0x35) matches the
// placeholder's own guess so its swap-in is the "small, isolated diff" it
// was written to allow.
//
// ---- THE ONE DELIBERATE DEVIATION from hOC's ground rule ------------------
// hOC's spec rule #2 is "all payload bytes are plain 7-bit values (no 8-bit
// packing, no struct dumps)" -- true there because a Captain MIDI dump is
// STRUCTURED PARAMETER RECORDS the device already only ever stores as
// 0..127 values (see hoc-midi-sysex.md's "Dump format"). A 200e/251e
// preset dump is not that: it is raw bytes captured verbatim off a real
// module's FRAM/EEPROM via BACKUP (Bus200eMaster.h) -- undecoded, and with
// no guarantee any byte is < 0x80. SysEx payload bytes MUST be 7-bit
// (MIDI's own hardware-level constraint, not a house style choice), so
// unlike hOC this protocol 7-bit-packs DUMP_DATA/RESTORE_DATA payloads
// (Bus200eSysExPack/Unpack below) and unpacks on receipt. Every other
// message type (INFO/GET_DUMP/PUT_DUMP/ACK/NAK/...) carries small
// already-7-bit fields and is NOT packed, matching hOC exactly.
// (tools/251e-sequencer/sequence-codec.js's PLACEHOLDER dump format
// currently avoids this entirely by inventing an already-7-bit-safe fake
// 251e layout -- fine for exercising the UI/transport against synthetic
// data, but a REAL captured dump will not stay under 0x80, so the real
// integration must switch sysex-transport.js's packDump/DumpAssembler over
// to Bus200eSysExPack/Unpack's scheme, not keep assuming raw pass-through.)
//
// ---- SysEx message shape ---------------------------------------------------
// Standard MIDI SysEx bytes: F0 <payload> F7. Payload (what this module
// builds/parses -- F0/F7 belong to the MIDI transport layer, not here):
//
//   [0] manufacturer ID   = 0x7D  (MIDI's reserved "non-commercial / school
//                                   use" ID -- same one hOC uses)
//   [1] family ID         = 0x62  ("Beige Maze", shared with other o_C apps
//                                   per hoc-midi-sysex.md)
//   [2] application ID    = 0x35  ("251e sequencer bridge" -- hOC's Captain
//                                   MIDI app is 0x4D ('M'); this is a
//                                   distinct, non-colliding assignment)
//   [3] protocol version  = 0x02
//   [4] command           = one of BUS200E_SYSEX_CMD_*
//   [5..] payload, command-specific (see below), all 7-bit
//
// Frames whose bytes [0..2] don't match are not ours (ignored, not an
// error -- matches hOC's "frames for a different application are ignored"
// stance). A version mismatch (byte 3) answers NAK error 1, same as hOC.
//
// ---- VERSION 2: 14-bit packet counters (why the bump) ----------------------
// v1 carried DUMP_DATA's [seq, total] and DUMP_END's [n_packets] as SINGLE
// 7-bit bytes. That capped a transfer at 127 packets * 44 raw bytes = 5588
// bytes, and Bus200eBridge.cpp refused (clean NAK 6) anything larger rather
// than truncate it. A REAL 251e preset bank is 30 records * 2104 bytes =
// 63120 bytes (docs in Buchla_FW/docs/251e-SEQUENCE-FORMAT.md; confirmed
// four independent ways, including a live MasterBackup(0x5C) that reported
// exactly 63120) -- 11.3x over that ceiling. The whole point of this bridge
// is to move a 251e bank, so v1 could not do its job.
//
// v2 widens every packet counter to FOURTEEN bits, carried as two 7-bit
// septets, LOW FIRST. That is not a new invention for this protocol: v1's
// own INFO_R already split its dump length into a lo/hi septet pair, and
// septet-pair counters are how MIDI itself has always carried a >7-bit
// number (pitch bend, RPN/NRPN, song position). 14 bits = 16383 packets =
// 687 KB, so the wire is no longer the binding limit; the device's card
// image (BUSCARD_SIZE, 64 KB -> BUS200E_BRIDGE_MAX_PACKETS) is.
//
// This was chosen over the alternative -- keeping the 7-bit counters and
// wrapping several <=127-packet "batches" in a new outer offset/total
// layer -- because it keeps ONE dump == ONE session. The batching design
// adds a second, independent framing layer with its own sequencing, its own
// restart/resume semantics and its own failure modes, all to avoid changing
// two field widths in a protocol nothing has shipped against yet. Widening
// the field is the smaller change to the model, not just to the code.
//
// The one real cost: DUMP_DATA's header grows from 2 bytes to 4, so the
// chunk that still fits the 60-byte frame ceiling shrinks 44 -> 42 raw
// bytes (see below). 63120 bytes is then 1503 packets instead of 1435 --
// a 4.7% frame-count tax for removing the ceiling entirely.
//
// ---- frame size ceiling -----------------------------------------------------
// 60 bytes total (F0..F7 inclusive) -- hOC's own ground rule ("keeps every
// frame well inside any USB-MIDI SysEx buffer a Teensy build is likely to
// use"), reused verbatim rather than re-deriving a number for the same
// hardware (and Bus200eBridgeUsb.cpp's rx_buf is sized 64 on that basis).
// BUS200E_SYSEX_CHUNK_BYTES (42 raw bytes) is the largest chunk whose
// PACKED form still fits: 7 non-payload bytes (F0+mfr+family+app+ver+cmd+
// F7) + 4 payload header bytes (seq lo/hi, total lo/hi) + pack(42) =
// 7+4+48 = 59. 59, not 60: pack() grows in 8-byte groups (pack(42)=48,
// pack(43)=50), so no chunk size lands exactly on the ceiling here. The
// spare byte is group granularity, not an off-by-one.
//
// ---- commands ----------------------------------------------------------
//   INFO         (H->D) no payload. -> INFO_R: [schema, n_sequences,
//                max_steps, len0, len1, len2, chunk_bytes, max_pk_lo,
//                max_pk_hi, card_serving]. len0..len2 are the last dump's
//                raw length as THREE septets, low first (21 bits -- 14
//                would not reach 63120), max_pk_* the device's packet
//                ceiling as a septet pair. Fields are APPEND-ONLY.
//   GET_DUMP     (H->D) payload = [mod_addr]: capture a dump from the
//                named 200e module (via Bus200eMaster's BACKUP) and stream
//                it back as DUMP_DATA*/DUMP_END.
//   PUT_DUMP     (H->D) payload = [mod_addr]: announce an incoming
//                write-back for the named module. ACKed, then the host
//                streams DUMP_DATA*/DUMP_END; on a clean DUMP_END the
//                device masters RESTORE (Bus200eMaster) at that module.
//   DUMP_DATA    (both) payload = [seq_lo, seq_hi, total_lo, total_hi,
//                ...7-bit-packed chunk]: one chunk, at raw offset
//                seq*BUS200E_SYSEX_CHUNK_BYTES. H->D per packet is ACKed
//                with {seq_lo, seq_hi} (matches hOC's per-packet ACK, its
//                confirmation that flow control may proceed to the next
//                chunk -- ACK is NOT a checksum).
//   DUMP_END     (both) payload = [n_lo, n_hi, xor7]: n_packets as a
//                septet pair, then xor7 -- the XOR of every RAW (pre-pack)
//                dump byte across the whole transfer, folded to 7 bits
//                (Bus200eSysExXor7 below) -- identical definition to hOC's
//                DUMP_END checksum, just over raw 200e bytes instead of
//                parameter records. H->D: ACKed or NAK 6 (packet-count/
//                checksum mismatch).
//   STATUS       (H->D) no payload. -> STATUS_R: [state, error, mod_addr,
//                is_restore] (Bus200eMasterState/Error, one byte each) --
//                no hOC analog; a foreign-module BACKUP/RESTORE runs on
//                the bus over real time, so the browser needs somewhere
//                to poll progress. Command bytes (0x10/0x50) are chosen
//                clear of every byte hOC's spec already uses.
//   ACK          (D->H) payload = [echo_cmd, ...ctx]. ctx is one byte
//                (mod_addr) for GET_DUMP/PUT_DUMP, and a septet PAIR for
//                DUMP_DATA (the seq being acknowledged) and DUMP_END (the
//                accepted n_packets) -- both of which are now 14-bit.
//   NAK          (D->H) payload = [cmd, errcode] (BUS200E_SYSEX_NAK_*).
// ---------------------------------------------------------------------------

#define BUS200E_SYSEX_MFR_ID     0x7D
#define BUS200E_SYSEX_FAMILY_ID  0x62
#define BUS200E_SYSEX_APP_ID     0x35
#define BUS200E_SYSEX_PROTO_VER  0x02

typedef enum {
  BUS200E_SYSEX_CMD_INFO      = 0x01,
  BUS200E_SYSEX_CMD_GET_DUMP  = 0x04,
  BUS200E_SYSEX_CMD_PUT_DUMP  = 0x05,
  BUS200E_SYSEX_CMD_STATUS    = 0x10,
  // Firmware-card / scan / bus-status commands (docs/bus200e-sysex-fw.md).
  // 0x20-0x27 and 0x64-0x67 clear every byte hOC and this enum already use.
  BUS200E_SYSEX_CMD_FW_BEGIN   = 0x20,  // H->D [kind, len0,len1,len2, crc0..crc4]
  BUS200E_SYSEX_CMD_FW_DATA    = 0x21,  // H->D [seq_lo,seq_hi,total_lo,total_hi, packed(<=42)]
  BUS200E_SYSEX_CMD_FW_END     = 0x22,  // H->D [n_lo,n_hi, xor7]
  BUS200E_SYSEX_CMD_FW_ARM     = 0x23,  // H->D [on]
  BUS200E_SYSEX_CMD_FW_STATUS  = 0x24,  // H->D none
  BUS200E_SYSEX_CMD_SCAN       = 0x25,  // H->D [first, last]
  BUS200E_SYSEX_CMD_POLL_DONE  = 0x26,  // H->D none
  BUS200E_SYSEX_CMD_BUS_STATUS = 0x27,  // H->D none
  BUS200E_SYSEX_CMD_ACK       = 0x40,
  BUS200E_SYSEX_CMD_INFO_R    = 0x41,
  BUS200E_SYSEX_CMD_DUMP_DATA = 0x44,
  BUS200E_SYSEX_CMD_DUMP_END  = 0x45,
  BUS200E_SYSEX_CMD_STATUS_R  = 0x50,
  BUS200E_SYSEX_CMD_FW_STATUS_R  = 0x64, // [armed, serving, kind, served0..2, age_lo, age_hi, len0..2, ver_addr, ver_valid, v0..v6]
  BUS200E_SYSEX_CMD_SCAN_R       = 0x65, // one per responder: [idx, count(0x7F=unknown), addr, ver_valid, v0..v6]
  BUS200E_SYSEX_CMD_SCAN_END     = 0x66, // [n_found]
  BUS200E_SYSEX_CMD_BUS_STATUS_R = 0x67, // [wpm_present, bus_quiet, dialect, last_error]
  BUS200E_SYSEX_CMD_NAK       = 0x7E,
} Bus200eSysExCmd;

// NAK error codes: 1/2/5/6 keep hOC's exact meanings (reused, not
// reassigned); 7-9 are new, mapped from Bus200eMasterError (Bus200eMaster.h)
// for failures with no hOC analog (foreign-module bus mastering).
typedef enum {
  BUS200E_SYSEX_NAK_VERSION      = 1,  // protocol version mismatch
  BUS200E_SYSEX_NAK_UNKNOWN_CMD  = 2,
  BUS200E_SYSEX_NAK_BUSY         = 5,  // a master job is already running
  BUS200E_SYSEX_NAK_CHECKSUM     = 6,  // packet-count/checksum mismatch
  BUS200E_SYSEX_NAK_NO_FREE_CARD = 7,
  BUS200E_SYSEX_NAK_SEND_TIMEOUT = 8,
  BUS200E_SYSEX_NAK_NO_RESPONSE  = 9,
  BUS200E_SYSEX_NAK_FW_BUSY      = 10,  // card serving on / load in progress
  BUS200E_SYSEX_NAK_FW_NOMEM     = 11,  // FwCardCommit -6 / -1, FwCardBegin alloc
  BUS200E_SYSEX_NAK_FW_LENGTH    = 12,  // FwCardCommit -2
  BUS200E_SYSEX_NAK_FW_MAGIC     = 13,  // FwCardCommit -3 (header/geometry)
  BUS200E_SYSEX_NAK_FW_CRC       = 14,  // FwCardCommit -4
  BUS200E_SYSEX_NAK_FW_WRITE     = 15,  // FwCardCommit -5 (write or read-back)
  BUS200E_SYSEX_NAK_FW_SEQUENCE  = 16,  // FW_DATA out of order / without FW_BEGIN
} Bus200eSysExNakReason;

// Appended to INFO_R: which command set this firmware speaks (1 = the
// FW_*/SCAN/POLL_DONE/BUS_STATUS commands of docs/bus200e-sysex-fw.md).
#define BUS200E_SYSEX_BRIDGE_LEVEL 1

#define BUS200E_SYSEX_CHUNK_BYTES 42   // raw bytes per DUMP_DATA chunk

// A chunk's packed payload never exceeds this many bytes (pack(42) = 48).
#define BUS200E_SYSEX_MAX_PACKED 48

// DUMP_DATA's unpacked header: seq_lo, seq_hi, total_lo, total_hi.
#define BUS200E_SYSEX_DUMP_HDR_BYTES 4

// A full built message (header + payload + no trailing bytes) never
// exceeds this many bytes, between the F0 and F7 a MIDI layer adds --
// header(5) + seq/total(4) + packed(48) = 57; 57 + F0 + F7 = 59.
#define BUS200E_SYSEX_MAX_MESSAGE \
  (5 + BUS200E_SYSEX_DUMP_HDR_BYTES + BUS200E_SYSEX_MAX_PACKED)

// ---- 14-bit septet pairs (v2's widened packet counters) --------------------
// Low septet first, matching MIDI's own long-standing convention for a
// >7-bit number on the wire (pitch bend, song position, RPN/NRPN) and v1's
// own INFO_R dump-length split. Every counter that rides a pair is capped
// at BUS200E_SYSEX_MAX_COUNT; a device may (and does) impose a smaller one.
#define BUS200E_SYSEX_MAX_COUNT 16383u

#define BUS200E_SYSEX_LO7(v) ((uint8_t) ((v) & 0x7Fu))
#define BUS200E_SYSEX_HI7(v) ((uint8_t) (((v) >> 7) & 0x7Fu))
#define BUS200E_SYSEX_FROM14(lo, hi) \
  ((uint16_t) (((uint16_t) ((lo) & 0x7Fu)) | (uint16_t) (((uint16_t) ((hi) & 0x7Fu)) << 7)))

// ---- 7-bit packing (the one deviation from hOC -- see file header) --------

// Packs raw_len raw bytes into out (7-bit clean). Returns the packed length,
// or -1 if out_cap is too small. out_cap must be >= ((raw_len+6)/7)*8.
int Bus200eSysExPack(const uint8_t *raw, uint32_t raw_len,
                      uint8_t *out, uint32_t out_cap);

// Unpacks packed_len packed bytes (must be a valid pack() output -- a
// multiple of 8, or a valid partial final group) into out. Returns the raw
// length, or -1 on a malformed input or an undersized out_cap.
int Bus200eSysExUnpack(const uint8_t *packed, uint32_t packed_len,
                        uint8_t *out, uint32_t out_cap);

// ---- message build/parse ----------------------------------------------------

// Builds one message payload (the bytes between F0 and F7) into `out`.
// For DUMP_DATA: field_payload/field_len is [seq_lo, seq_hi, total_lo,
// total_hi] (exactly BUS200E_SYSEX_DUMP_HDR_BYTES, NOT packed -- build it
// with BUS200E_SYSEX_LO7/HI7) and raw_chunk/raw_chunk_len is the raw
// (unpacked) chunk data (<= BUS200E_SYSEX_CHUNK_BYTES) -- this function
// packs it internally. For every other command: field_payload/field_len is
// the whole (already 7-bit, unpacked) payload as documented above, and
// raw_chunk must be NULL (raw_chunk_len 0). Returns the built length, or
// -1 on a bad size or an undersized out_cap.
int Bus200eSysExBuildMessage(uint8_t cmd,
                              const uint8_t *field_payload, uint32_t field_len,
                              const uint8_t *raw_chunk, uint32_t raw_chunk_len,
                              uint8_t *out, uint32_t out_cap);

// Parses one received message payload (bytes between F0 and F7, exclusive).
// Verifies manufacturer/family/app ID and protocol version. On success
// (return 0): *cmd is filled. For BUS200E_SYSEX_CMD_DUMP_DATA, out_seq and
// out_total are filled from the septet-pair header (already recombined to
// 14-bit values) and the unpacked chunk is written to out_raw/*out_raw_len;
// for every other command, out_seq/out_total are untouched and the raw
// (already-7-bit, unpacked) payload is written to out_raw/*out_raw_len
// directly. Any output pointer may be NULL to skip it. On failure, returns:
//  -1  too short / malformed framing
//  -2  manufacturer/family/app ID mismatch (not one of our messages)
//  -3  protocol version mismatch
//  -4  malformed packed chunk (DUMP_DATA only)
//  -5  out_raw_cap too small for the unpacked payload
int Bus200eSysExParseMessage(const uint8_t *in, uint32_t in_len,
                              uint8_t *cmd, uint16_t *out_seq, uint16_t *out_total,
                              uint8_t *out_raw, uint32_t out_raw_cap,
                              uint32_t *out_raw_len);

// XOR of every byte in raw[0..len), folded to 7 bits -- the DUMP_END
// checksum, same definition as hOC's (over raw/pre-pack bytes). Pass a
// nonzero `acc` (a prior call's return value) to accumulate across chunks
// without buffering the whole dump; 0 to start a fresh accumulation.
uint8_t Bus200eSysExXor7(uint8_t acc, const uint8_t *raw, uint32_t len);

// ---- firmware-card / scan / bus-status codec (docs/bus200e-sysex-fw.md) -----
// All Build* functions write the whole message payload (the 5 header bytes
// plus the command's fields, between F0 and F7) and return its length, or -1
// on a null pointer, an undersized cap or a field that does not fit its
// septets. All Parse* functions take the FIELD payload (the bytes after the
// 5 header bytes, as Bus200eSysExParseMessage hands them back) and return 0,
// or -1 on a wrong length, a null pointer or a non-7-bit byte.

typedef struct {
  uint8_t  armed;             // FwCardArmed()
  uint8_t  serving;           // FwCardMode(): answering 0x50 right now
  uint8_t  kind;              // 0 none on flash, 1 v1, 2 v2
  uint32_t served;            // bytes the module has read (21 bits)
  uint16_t first_read_age_s;  // seconds since the first read (14 bits); 0x3FFF = none yet
  uint32_t image_len;         // length of the image on flash (21 bits)
  uint8_t  ver_addr;          // module whose version frame was last seen
  uint8_t  ver_valid;         // 0/1: ver[] holds a real frame (else sent as seven spaces)
  uint8_t  ver[7];            // ASCII bytes of the last 0x13 frame (masked & 0x7F on the wire)
} Bus200eFwStatus;

#define BUS200E_SYSEX_FW_BEGIN_FIELDS     9   // kind + 3 length septets + 5 CRC septets
#define BUS200E_SYSEX_FW_STATUS_R_FIELDS  20

// n septets, low first <-> value. Five septets carry 35 bits, enough for a
// CRC-32; ToSeptets drops bits above 7*n, Septets reads at most n <= 5.
uint32_t Bus200eSysExSeptets(const uint8_t *p, int n);
void     Bus200eSysExToSeptets(uint32_t v, uint8_t *out, int n);

int Bus200eSysExBuildFwBegin(uint8_t kind, uint32_t len, uint32_t crc32,
                              uint8_t *out, uint32_t cap);
int Bus200eSysExParseFwBegin(const uint8_t *payload, uint32_t len,
                              uint8_t *kind, uint32_t *image_len, uint32_t *crc32);
int Bus200eSysExBuildFwStatusR(const Bus200eFwStatus *s, uint8_t *out, uint32_t cap);
int Bus200eSysExParseFwStatusR(const uint8_t *payload, uint32_t len, Bus200eFwStatus *s);
int Bus200eSysExBuildScanR(uint8_t idx, uint8_t count, uint8_t addr, uint8_t ver_valid,
                            const uint8_t ver[7], uint8_t *out, uint32_t cap);
int Bus200eSysExBuildBusStatusR(uint8_t wpm, uint8_t quiet, uint8_t dialect, uint8_t err,
                                 uint8_t *out, uint32_t cap);

#endif  // BUS200ESYSEX_H_
