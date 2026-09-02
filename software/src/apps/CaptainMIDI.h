// Copyright (c) 2018, Jason Justian
//
// Menu & screen cursor Copyright (c) 2016 Patrick Dowling
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// See https://www.pjrc.com/teensy/td_midi.html
//
// Adapted for T4.x and 8-channel hardware by djphazer

#pragma once

#include "PresetEngine.h"

static constexpr int MIDI_SETUP_COUNT = 4;
static constexpr int MIDI_PARAMETER_COUNT = 40;
static constexpr int MIDI_CURRENT_SETUP = (MIDI_PARAMETER_COUNT * MIDI_SETUP_COUNT);
static constexpr int MIDI_SETTING_COUNT = (MIDI_CURRENT_SETUP + 1);
static constexpr int MIDI_LOG_MAX_SIZE = 101;

enum MIDI_IN_FUNCTION : uint8_t {
  // T32 only, deprecated
    MIDI_IN_OFF,
    MIDI_IN_NOTE = HEM_MIDI_NOTE_OUT,
    MIDI_IN_GATE = HEM_MIDI_GATE_OUT,
    MIDI_IN_TRIGGER = HEM_MIDI_TRIG_OUT,
    MIDI_IN_VELOCITY = HEM_MIDI_VEL_OUT,
    MIDI_IN_MOD = HEM_MIDI_CC_OUT,
    MIDI_IN_AFTERTOUCH = HEM_MIDI_AT_CHAN_OUT,
    MIDI_IN_PITCHBEND = HEM_MIDI_PB_OUT,
    MIDI_IN_EXPRESSION,
    MIDI_IN_PAN,
    MIDI_IN_HOLD,
    MIDI_IN_BREATH,
    MIDI_IN_Y_AXIS,

    // clock divisions
    MIDI_IN_CLOCK_4TH = HEM_MIDI_CLOCK_OUT,
    MIDI_IN_CLOCK_8TH = HEM_MIDI_CLOCK_8_OUT,
    MIDI_IN_CLOCK_16TH = HEM_MIDI_CLOCK_16_OUT,
    MIDI_IN_CLOCK_24PPQN = HEM_MIDI_CLOCK_24_OUT,
};

using Type = MIDIMapSettings::Type;
using PitchType = MIDIMapSettings::PitchType;
using GateType = MIDIMapSettings::GateType;

// labels for the CV/trigger -> MIDI engine functions (HSMIDITypes.h)
const char* const cv_out_fn_names[HS::CVFN_COUNT] = {
    "--", "Pitch", "FreeN", "CC", "CC14", "Veloc", "Bend", "Aft", "NRPN", "NRP14", "PgmC", "GateN"
};
const char* const trig_out_fn_names[HS::TRFN_COUNT] = {
    "--", "Note", "Trig", "Latch", "CC", "CCLat", "Start", "Stop", "Cont", "Run", "Clock", "Panic"
};

const char* const midi2cv_label[] = {
  "MIDI > A", "MIDI > B", "MIDI > C", "MIDI > D",
  "MIDI > E", "MIDI > F", "MIDI > G", "MIDI > H",
};
const char* const cv2midi_label[] = {
  "CV1>MIDI", "CV2>MIDI", "CV3>MIDI", "CV4>MIDI",
  "CV5>MIDI", "CV6>MIDI", "CV7>MIDI", "CV8>MIDI",
};
const char* const trig2midi_label[] = {
  "TR1>MIDI", "TR2>MIDI", "TR3>MIDI", "TR4>MIDI",
  "TR5>MIDI", "TR6>MIDI", "TR7>MIDI", "TR8>MIDI",
};

const char* const captain_off_on[2] = { "Off", "On" };
const char* const captain_vel_src[9] = {
  "Fixed", "CV1", "CV2", "CV3", "CV4", "CV5", "CV6", "CV7", "CV8"
};
const char* const captain_gate_src[9] = {
  "TRjack", "CV1", "CV2", "CV3", "CV4", "CV5", "CV6", "CV7", "CV8"
};
const char* const captain_clkdiv[16] = {
  "x1", "x2", "x3", "x4", "x6", "x8", "x12", "x24", "x48", "x96",
  "x1", "x1", "x1", "x1", "x1", "x1"
};

// per channel
const settings::ValueAttributes CaptainSettings[] = {
  // 0: Assigned function, MIDI-to-CV
  { 0, 0, HEM_MIDI_MAX_FUNCTION, "", midi_fn_name, settings::STORAGE_TYPE_U8 },
  // 1: CV-to-MIDI
  { 0, 0, HS::CVFN_COUNT-1, "", cv_out_fn_names, settings::STORAGE_TYPE_U8 },
  // 2: Trigger-to-MIDI
  { 0, 0, HS::TRFN_COUNT-1, "", trig_out_fn_names, settings::STORAGE_TYPE_U8 },

  // 3: Channel
  { 0, 0, 16, "", midi_channels, settings::STORAGE_TYPE_U8 },
  // 4: Transpose
  { 0, -48, 48, "", NULL, settings::STORAGE_TYPE_I8 },
  // 5: Range Low
  { 0, 0, 127, "", midi_note_numbers, settings::STORAGE_TYPE_U8 },
  // 6: Range High
  { 0, 0, 127, "", midi_note_numbers, settings::STORAGE_TYPE_U8 },

  // 7: Data 1 as note number
  { 0, 0, 127, "", midi_note_numbers, settings::STORAGE_TYPE_U8 },
  // 8: Data 1 as plain number (CC#/NRPN LSB)
  { 0, 0, 127, "", NULL, settings::STORAGE_TYPE_U8 },
  // 9: Data 2 (on-value / velocity / NRPN MSB)
  { 0, 0, 127, "", NULL, settings::STORAGE_TYPE_U8 },
  // 10: Off/On (quantize, legato)
  { 0, 0, 1, "", captain_off_on, settings::STORAGE_TYPE_U8 },
  // 11: Velocity source
  { 0, 0, ADC_CHANNEL_COUNT < 7 ? ADC_CHANNEL_COUNT : 7, "", captain_vel_src, settings::STORAGE_TYPE_U8 },
  // 12: Clock multiplier
  { 0, 0, 15, "", captain_clkdiv, settings::STORAGE_TYPE_U8 },
  // 13: Poly voice
  { 0, 0, DAC_CHANNEL_COUNT - 1, "", NULL, settings::STORAGE_TYPE_U8 },
  // 14: Trigger gate source (TR jack or a CV input via ADC threshold)
  { 0, 0, ADC_CHANNEL_COUNT, "", captain_gate_src, settings::STORAGE_TYPE_U8 },
};

enum CaptainsKeys : uint16_t {
  SETUP_KEY = 0,
  // The SysEx class-0 globals (bend range, poly mode, PC channel, trigger
  // length), packed one per byte in that order. They were in the checksum
  // that decides whether Suspend rewrites CAPTAIN.DAT, but not in the file:
  // a SET from the host bought a 64 KB erase that stored nothing, and the
  // value died at the next Resume. Signed with kMidiGlobalsMagic in bits
  // 32-39 so a file that predates the key, or a foreign key, is ignored
  // whole rather than half-applied (PC channel 0 is a legal value).
  MIDI_GLOBALS_KEY = 1,

  // upper 7 bits of mapping key
  INPUT_MAP_KEY = 1 << 9,
  OUTPUT_MAP_KEY = 2 << 9,
};
// USB device profiles are MODULE-level config, not preset state: they live
// in GLOBALS.CFG ((9<<8) namespace). CAPTAIN.DAT is captured into preset
// slots and restored by recalls (including boot recall), which would wipe
// anything stored there. Values signed with 0xA5<<48 so stale or foreign
// keys can never parse as profiles. Names at +16.
static constexpr uint16_t DEVICE_PROFILE_KEY = 9 << 8;
static constexpr uint64_t kProfileMagic = 0xA5ULL << 48;
static constexpr uint64_t kMidiGlobalsMagic = 0xC1ULL << 32;
static constexpr int kDeviceProfiles = 8;

const char* const midi_messages[7] = {
    "Note", "Off", "CC#", "Aft", "Bend", "SysEx", "Diag"
};

// Teensy USB device state: nonzero once a host has configured us,
// cleared on bus reset. The closest thing to "is the computer there".
extern "C" volatile uint8_t usb_configuration;

//#define MIDI_DIAGNOSTIC
struct CaptainMIDILog {
    bool midi_in; // 0 = out, 1 = in
    char io; // 1, 2, 3, 4, A, B, C, D
    uint8_t message; // 0 = Note On, 1 = Note Off, 2 = CC, 3 = Aftertouch, 4 = Bend, 5 = SysEx, 6 = Diagnostic
    uint8_t channel; // MIDI channel
    int16_t data1;
    int16_t data2;

    void DrawAt(int y) {
        if (message == 5) {
            int app_code = static_cast<char>(data1);
            if (app_code > 0) {
                graphics.setPrintPos(1, y);
                graphics.print("SysEx: ");
                if (app_code == 'M') graphics.print("Captain MIDI");
                if (app_code == 'H') graphics.print("Hemisphere");
                if (app_code == 'D') graphics.print("D. Timeline");
                if (app_code == 'E') graphics.print("Scale Ed");
                if (app_code == 'T') graphics.print("Enigma");
                if (app_code == 'W') graphics.print("Waveform Ed");
                if (app_code == '_') graphics.print("O_C EEPROM");
                if (app_code == 'B') graphics.print("Backup");
                if (app_code == 'N') graphics.print("Neural Net");
            }
        } else {
            graphics.setPrintPos(1, y);
            if (midi_in) graphics.print(">");
            graphics.print(io);
            if (!midi_in) graphics.print(">");
            graphics.print(" ");
            graphics.print(midi_channels[channel]);
            graphics.setPrintPos(37, y);

            graphics.print(midi_messages[message]);
            graphics.setPrintPos(73, y);

            uint8_t x_offset = (data2 < 100) ? 6 : 0;
            x_offset += (data2 < 10) ? 6 : 0;

            if (message == 0 || message == 1) {
                graphics.print(midi_note_numbers[data1]);
                graphics.setPrintPos(91 + x_offset, y);
                graphics.print(data2); // Velocity
            }

            if (message == 2 || message == 3) {
                if (message == 2) graphics.print(data1); // Controller number
                graphics.setPrintPos(91 + x_offset, y);
                graphics.print(data2); // Value
            }

            if (message == 4) {
                if (data2 > 0) graphics.print("+");
                graphics.print(data2); // Aftertouch or bend value
            }

            if (message == 6) {
                graphics.print(data1);
                graphics.print("/");
                graphics.print(data2);
            }
        }
    }
};

OC_APP_CLASS(AppCaptainMIDI, TWOCCS("MI"), "Captain MIDI", "MIDI I/O"),
  public HSApplication, public SystemExclusiveHandler
{
public:
    // number of stored setups, and the serialized size of one setup:
    // all in-maps + all out-ports, one packed uint64 each.
    // T3 EEPROM cost scales with this — builds with many apps sharing the
    // pool override it (see platformio.ini; the static_assert in
    // apps/_config.h is the budget gate)
#ifndef CAPTAIN_SETUP_COUNT
#define CAPTAIN_SETUP_COUNT 1
#endif
    static constexpr int kNumSetups = CAPTAIN_SETUP_COUNT;
    static constexpr size_t kSetupBlobWords =
        MIDIMAP_MAX + HS::MIDIFrame::kOutPorts;
    // version/active/reserved header + setups (see SaveAppData)
    static constexpr size_t kCaptainStorageSize =
        4 + kNumSetups * kSetupBlobWords * sizeof(uint64_t);

#ifdef __IMXRT1062__
  // T4.x persists to PhzConfig (LittleFS) instead of the EEPROM pool
  OC_APP_INTERFACE_DECLARE(AppCaptainMIDI, 0);
#else
  OC_APP_INTERFACE_DECLARE(AppCaptainMIDI, AppCaptainMIDI::kCaptainStorageSize);
#endif

    static constexpr int MIDI_INDICATOR_COUNTDOWN = 2000;

    OC::menu::ScreenCursor<OC::menu::kScreenLines> cursor;       // overview rows
    OC::menu::ScreenCursor<OC::menu::kScreenLines> param_cursor; // detail rows

#if defined(ARDUINO_TEENSY41)
    static constexpr int kHostRows = 2;   // Host1/Host2 device-profile rows
#else
    static constexpr int kHostRows = 0;
#endif
    static constexpr int kNumRows =
        DAC_CHANNEL_COUNT + HS::MIDIFrame::kOutPorts + kHostRows;
    static constexpr int kFirstHostRow = kNumRows - kHostRows;

    void Start() {
        detail_port = -1;
        display = 0;
        cursor.Init(0, kNumRows - 1);
        param_cursor.Init(0, 0);
        log_index = 0;
        log_view = 0;
        active_setup = 0;
        // seed all setups from the freshly-initialized live state
        for (int s = 0; s < kNumSetups; ++s) PackSetup(s);
        Reset();
    }

    // pack the live mapping/outports state into setups[s]
    void PackSetup(int s);  // out-of-class (FLASHMEM + LTO)

    // Clamp a packed in-map blob so stale/garbage EEPROM data can never
    // index a name table out of bounds (that reads a wild pointer:
    // garbled draws at best, a hard fault at worst).
    static uint64_t SanitizeInMapPacked(uint64_t d) {
        auto byte = [&d](int n) { return uint8_t((d >> (n * 8)) & 0xff); };
        auto setbyte = [&d](int n, uint8_t v) {
            d = (d & ~(uint64_t(0xff) << (n * 8))) | (uint64_t(v) << (n * 8));
        };
        // function: type in upper 3 bits, CCONTROL (5) is the highest
        if ((byte(1) >> 5) > 5) setbyte(1, 0);
        if (byte(2) > 16) setbyte(2, 16);                       // channel
        const int8_t t = int8_t(byte(4));                       // transpose
        if (t < -48 || t > 48) setbyte(4, 0);
        if (byte(5) > 127) setbyte(5, 0);                       // range_low
        if (byte(6) > 127) setbyte(6, 127);                     // range_high
        if (byte(3) >= DAC_CHANNEL_COUNT) setbyte(3, 0);        // polyvoice
        return d;
    }

    // apply setups[s] to the live state
    void UnpackSetup(int s);  // out-of-class (FLASHMEM + LTO)

    void Suspend() {
        clock_router = false;  // never come back to a modal screen mid-app-switch
        // Capture dirtiness BEFORE PackSetup: packing copies the live frame
        // into setups[] which is exactly what LiveConfigDirty compares, so
        // checking after packing is always-false (review C1 - edits were
        // silently revertible via menu round-trips). The checksum term also
        // covers setup switches and SysEx edits to non-active setups.
        const bool dirty = LiveConfigDirty();
        PackSetup(active_setup);
#ifdef __IMXRT1062__
        // Only hit flash when something is actually unsaved: this runs on
        // every app-menu entry, and an unconditional CAPTAIN.DAT rewrite
        // (sector erases included) made the menu gesture take seconds.
        // And not at all when a bus recall is about to replace CAPTAIN.DAT
        // with the slot's copy: that erase bought nothing but 250 ms of
        // frozen audio. The edits are gone either way -- that is what a
        // 200e recall means -- so no bytes are lost that were not already.
        const uint8_t recall = OC::PresetEngine::RecallReplacing();
        const bool replaced = recall & OC::PresetEngine::REPLACES_CAPTAIN;
        if (!replaced && (dirty || LiveChecksum() != saved_checksum)) StoreData();
#else
        const uint8_t recall = OC::PresetEngine::RecallReplacing();
        const bool replaced = false;
#endif
#if defined(ARDUINO_TEENSY41)
        // T41 only: device profiles need the USB host ports
        if (profiles_dirty) PersistProfiles();  // debounce dies with DoLoop
#endif
        if (replaced) {
            // The slot's CAPTAIN.DAT is about to replace the live one, so a
            // SET the host is still waiting on has been undone: an ACK sent
            // at the next Resume would vouch for an edit that no longer
            // exists. Drop it, and any frame the ISR staged since DoLoop
            // last ran, so the host's timeout tells it the truth.
            syx_pending = 0;
            syx_rx_len = 0;
        }
        // dump-to-host on suspend is the Orin workflow contract, but only
        // when a host is actually configured to hear it. Send it here:
        // this runs in loop context, and DoLoop -- where a queued PEND_DUMP
        // would have gone out -- does not run again until Resume, which
        // turned "dump on suspend" into an unsolicited stale dump on every
        // resume. Not on a bus recall, which is a performance gesture and
        // must not wait on the host draining a multi-packet SysEx.
        if (usb_configuration && !(recall & OC::PresetEngine::RECALL_SUSPEND))
            SendDump(active_setup);
    }

    // the class-0 SysEx globals as one word, byte order = parameter order
    uint32_t PackMidiGlobals() const {
        auto &M = frame.MIDIState;
        return uint32_t(M.bend_range) | (uint32_t(M.poly_mode) << 8)
             | (uint32_t(M.pc_channel) << 16) | (uint32_t(HS::trig_length) << 24);
    }
    // same ranges as set_param: a file cannot smuggle a value SysEx refuses
    void ApplyMidiGlobals(uint32_t g) {
        auto &M = frame.MIDIState;
        const uint8_t bend = g & 0xff, poly = (g >> 8) & 0xff,
                      pc = (g >> 16) & 0xff, trig = (g >> 24) & 0xff;
        if (bend >= 1 && bend <= 24) M.bend_range = bend;
        if (poly <= 2) { M.poly_mode = poly; M.UpdateMaxPolyphony(); }
        if (pc <= 16) M.pc_channel = pc;
        if (trig >= 1) HS::trig_length = trig;
    }

#ifdef __IMXRT1062__
    void StoreData();  // out-of-class (FLASHMEM + LTO)
    void Resume() {
        PhzConfig::load_config("CAPTAIN.DAT");
        uint64_t data = 0;
        PhzConfig::getValue(SETUP_KEY, data);
        active_setup = constrain((int)data, 0, kNumSetups - 1);
        for (int s = 0; s < kNumSetups; ++s) {
          for (int i = 0; i < MIDIMAP_MAX; ++i)
            if (PhzConfig::getValue(INPUT_MAP_KEY + i + s*MIDIMAP_MAX, data))
              setups[s].inmaps[i] = data;
          for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
            if (PhzConfig::getValue(OUTPUT_MAP_KEY + i + s*HS::MIDIFrame::kOutPorts, data))
              setups[s].outports[i] = data;
        }
        if (PhzConfig::getValue(MIDI_GLOBALS_KEY, data) && (data >> 32) == (kMidiGlobalsMagic >> 32))
            ApplyMidiGlobals(uint32_t(data));
        UnpackSetup(active_setup);
        LeaveDetail();
        saved_checksum = live_checksum_prev = LiveChecksum();
#if defined(ARDUINO_TEENSY41)
        // Coming back from another app, a device that never unplugged is not
        // an "arrival", so its bound setup was never re-selected - the bind
        // looked dead until you physically replugged. Re-run the check once,
        // quietly, against whatever is actually connected. (This sat in the
        // T3.x Resume below for a week, where ARDUINO_TEENSY41 is never
        // defined: the fix had been shipped to the one branch it cannot run on.)
        reapply_bindings_ = true;
        next_dev_poll_ms = millis() + 250;   // let the app finish resuming
#endif
    }
#else
    // T3.x: setups[] persist via SaveAppData/RestoreAppData (EEPROM pool)
    void Resume() {
        UnpackSetup(active_setup);
        LeaveDetail();
        saved_checksum = live_checksum_prev = LiveChecksum();
    }
#endif

    void Controller() {
        // MIDI polling lives in DoLoop() (loop context): USBHost_t36 is NOT
        // ISR-safe -- reading usbHostMIDI from this 16.67kHz ISR corrupts
        // queues shared with the USB host interrupts and locks the module
        // the moment a real device delivers traffic.

        PumpTransport();

        // Convert CV/trigger inputs to outgoing MIDI messages
        frame.MIDIState.ProcessOutputs(frame);

        // set CV outputs from MIDI mappings
        for (int ch = 0; ch < DAC_CHANNEL_COUNT; ch++)
        {
            // Handle clock timing
            if (indicator_in[ch] > 0) --indicator_in[ch];

            MIDIMapping &map = frame.MIDIState.mapping[ch];

            // ViewOut, not output: the bend term lives there. Driving the
            // raw value meant pitch bend was received, stored and DRAWN --
            // the row's level bar moved -- while the jack never carried it.
            // Found by measurement: a -2.5 cent/semitone correction sent
            // over bend changed a tuning fit by exactly nothing while the
            // display swore it was happening (2026-08-28, tune_check
            // --corrected). ViewOut returns raw output for non-pitch maps
            // and is a plain multiply, safe at 16.67kHz.
            Out(ch, map.ViewOut());
        }
    }

    void View() const;
    void MainView() const {
        if (clock_router) DrawClockRouter();
        else if (copy_mode) DrawCopyScreen();
        else if (display) DrawLogScreen();
        else if (detail_port >= 0) DrawDetail();
        else DrawOverview();
        // popups poked from this app (device recognition, preset engine)
        // must be drawn here too - only Quadrants drew them before, so
        // they fired invisibly with Captain active
        if (OC::CORE::ticks - HS::popup_tick < HEMISPHERE_CURSOR_TICKS * 4)
            HS::DrawPopup(0, 0, CursorBlink());
    }

    // ---- two-level navigation ----
    // Overview: one row per port; turn R to scroll, press R to open the port.
    // Detail: all parameters of one port; turn R scrolls, press R edits,
    // press L goes back. Turn L: switch setup (overview) / switch port
    // (detail) / scroll (log view).

    int DetailParamCount(int row) const {
#if defined(ARDUINO_TEENSY41)
        // Device / Id / Bind / Bus + one row per STORED profile (no clutter)
        if (row >= kFirstHostRow) return 4 + StoredProfileCount();
#endif
        if (row < DAC_CHANNEL_COUNT) return 6;
        const int p = row - DAC_CHANNEL_COUNT;
        return (p >= HS::MIDIFrame::kCVOutPorts) ? 10 : 10;
    }

    void EnterDetail(int row) {
        detail_port = row;
        param_cursor.Init(0, DetailParamCount(row) - 1);
    }
    void LeaveDetail() { detail_port = -1; }

    const char* DetailParamLabel(int row, int par) const {
#if defined(ARDUINO_TEENSY41)
        if (row >= kFirstHostRow) {
            static const char* const host_labels[4] = { "Device", "Id", "Bind", "Bus" };
            if (par < 4) return host_labels[par];
            // stored-profile rows: the captured device name
            const DeviceProfile &pr = profiles[NthProfile(par - 4)];
            return pr.name[0] ? pr.name : "device";
        }
#endif
        static const char* const in_labels[6] = {
            "Assign", "Channel", "Transpose", "Range Low", "Range High", "Voice" };
        static const char* const cv_labels[10] = {
            "Function", "Channel", "Data 1", "Data 2", "Transpose",
            "Range Low", "Range High", "Quantize", "Legato", "Vel Source" };
        static const char* const tr_labels[10] = {
            "Function", "Channel", "Data 1", "Data 2", "Transpose",
            "Range Low", "Range High", "Vel Source", "Clock Mult",
            "Gate Src" };
        if (row < DAC_CHANNEL_COUNT) return in_labels[par];
        const int p = row - DAC_CHANNEL_COUNT;
        return (p >= HS::MIDIFrame::kCVOutPorts) ? tr_labels[par] : cv_labels[par];
    }

    // attr index into CaptainSettings for the value column / edit icon
    int DetailParamAttr(int row, int par) const {
#if defined(ARDUINO_TEENSY41)
        if (row >= kFirstHostRow) return 0;
#endif
        if (row < DAC_CHANNEL_COUNT) {
            static const uint8_t a[6] = { 0, 3, 4, 5, 6, 13 };
            return a[par];
        }
        const int p = row - DAC_CHANNEL_COUNT;
        const bool is_trig = p >= HS::MIDIFrame::kCVOutPorts;
        const HS::MIDIOutPort &o = frame.MIDIState.outports[p];
        if (par == 0) return is_trig ? 2 : 1;
        if (par == 2) { // data1: note names when it denotes a note
            const bool note_d1 = is_trig
                ? (o.function >= HS::TRFN_NOTE && o.function <= HS::TRFN_NOTE_LATCH)
                : (o.function == HS::CVFN_GATE_NOTE);
            return note_d1 ? 7 : 8;
        }
        if (is_trig) {
            static const uint8_t a[10] = { 2, 3, 8, 9, 4, 5, 6, 11, 12, 14 };
            return a[par];
        }
        static const uint8_t a[10] = { 1, 3, 8, 9, 4, 5, 6, 10, 10, 11 };
        return a[par];
    }

    int DetailParamValue(int row, int par) const {
#if defined(ARDUINO_TEENSY41)
        if (row >= kFirstHostRow) return 0;   // custom-drawn in DrawDetail
#endif
        if (row < DAC_CHANNEL_COUNT) {
            MIDIMapping &m = frame.MIDIState.mapping[row];
            switch (par) {
              case 0: return m.get_type() | m.get_subtype();
              case 1: return m.get_channel();
              case 2: return m.get_transpose();
              case 3: return m.get_low();
              case 4: return m.get_high();
              case 5: return m.get_voice();
            }
            return 0;
        }
        const int p = row - DAC_CHANNEL_COUNT;
        const bool is_trig = p >= HS::MIDIFrame::kCVOutPorts;
        const HS::MIDIOutPort &o = frame.MIDIState.outports[p];
        switch (par) {
          case 0: return o.function;
          case 1: return o.channel;
          case 2: return o.data1;
          case 3: return o.data2;
          case 4: return o.transpose;
          case 5: return o.range_low;
          case 6: return o.range_high;
        }
        if (is_trig) {
            if (par == 7) return o.velocity_source();
            if (par == 8) return o.clkdiv;
            if (par == 9) // gate source: 0 = TR jack, 1..N = CV input
                return (o.flags & HS::MIDIOutSettings::FLAG_CV_GATE)
                    ? (o.clkdiv % HS::MIDIFrame::kCVOutPorts) + 1 : 0;
        } else {
            if (par == 7) return (o.flags & HS::MIDIOutSettings::FLAG_QUANTIZE) ? 1 : 0;
            if (par == 8) return (o.flags & HS::MIDIOutSettings::FLAG_LEGATO) ? 1 : 0;
            if (par == 9) return o.velocity_source();
        }
        return 0;
    }

    void EditDetailParam(int row, int par, int dir) {
#if defined(ARDUINO_TEENSY41)
        if (row >= kFirstHostRow) {
            const int port = row - kFirstHostRow;
            if (par == 2) {                     // Bind: off / Setup 1..4
                const int cur = PortBinding(port);
                int next = constrain(cur + dir, 0, kNumSetups);
                if (cur == 0 && dir != 0)       // first detent, either way:
                    next = active_setup + 1;    // bind to what you're hearing
                if (next == 0) {
                    UnbindPort(port);
                    EnterDetail(row);   // list shrank: re-seat the cursor
                } else {
                    BindPort(port, uint8_t(next - 1));
                    // binding an UNKNOWN device stores a new profile, so the
                    // row list just grew: extend the cursor's range in place
                    // (AdjustEnd keeps the cursor and screen line where they
                    // are; Init would yank it back to the top mid-edit)
                    param_cursor.AdjustEnd(DetailParamCount(row) - 1);
                    char msg[16];
                    snprintf(msg, sizeof(msg), "Bound: Set %d", next);
                    HS::PokePopup(HS::MESSAGE_POPUP, msg);
                }
            } else if (par == 3) {              // Bus thru for THIS device
                if (!usbHostMIDI[port].idVendor() || dir == 0) return;
                const uint8_t off = dir > 0 ? 0 : 1;   // right = on, left = off
                if (off == port_no_bus_thru[port]) return;
                port_no_bus_thru[port] = off;
                // remember it on the device's profile so it follows the
                // device, not the port it happened to land on
                const int pi = FindProfile(usbHostMIDI[port].idVendor(),
                                           usbHostMIDI[port].idProduct());
                if (pi >= 0 && profiles[pi].no_bus_thru != off) {
                    profiles[pi].no_bus_thru = off;
                    MarkProfilesDirty();
                }
                HS::PokePopup(HS::MESSAGE_POPUP, off ? "Bus: off" : "Bus: on");
            } else if (par >= 4 && par - 4 < StoredProfileCount()) {
                DeviceProfile &pr = profiles[NthProfile(par - 4)];
                const int next = constrain(pr.setup + 1 + dir, 0, kNumSetups);
                if (next == 0) {                // off = delete, worded not hidden
                    pr = {};
                    MarkProfilesDirty();
                    EnterDetail(row);           // list shrank: re-seat cursor
                } else {
                    pr.setup = uint8_t(next - 1);
                    MarkProfilesDirty();
                }
            }
            return;                             // Device/Id are read-only
        }
#endif
        if (row < DAC_CHANNEL_COUNT) {
            MIDIMapping &m = frame.MIDIState.mapping[row];
            switch (par) {
              case 0: m.AdjustFunction(dir); break;
              case 1: m.AdjustChannel(dir); break;
              case 2: m.AdjustTranspose(dir); break;
              case 3: m.AdjustRangeLow(dir); break;
              case 4: m.AdjustRangeHigh(dir); break;
              case 5: m.AdjustVoice(dir); break;
            }
            return;
        }
        const int p = row - DAC_CHANNEL_COUNT;
        const bool is_trig = p >= HS::MIDIFrame::kCVOutPorts;
        HS::MIDIOutPort &o = frame.MIDIState.outports[p];
        const int fn_count = is_trig ? HS::TRFN_COUNT : HS::CVFN_COUNT;
        const int max_vel_src = ADC_CHANNEL_COUNT < 7 ? ADC_CHANNEL_COUNT : 7;
        auto edit_vel_src = [&](int d) {
            int src = constrain(int(o.velocity_source()) + d, 0, max_vel_src);
            o.flags = (o.flags & ~HS::MIDIOutSettings::VEL_SOURCE_MASK)
                    | (src << HS::MIDIOutSettings::VEL_SOURCE_SHIFT);
        };
        switch (par) {
          case 0:
            o.function = constrain(o.function + dir, 0, fn_count - 1);
            o.ResetRuntime();
            break;
          case 1: o.channel = constrain(o.channel + dir, 0, 15); break;
          case 2: o.data1 = constrain(o.data1 + dir, 0, 127); break;
          case 3: o.data2 = constrain(o.data2 + dir, 0, 127); break;
          case 4: o.transpose = constrain(o.transpose + dir, -48, 48); break;
          case 5: o.range_low = constrain(o.range_low + dir, 0, o.range_high); break;
          case 6: o.range_high = constrain(o.range_high + dir, o.range_low, 127); break;
          default:
            if (is_trig) {
                if (par == 7) edit_vel_src(dir);
                if (par == 8) o.clkdiv = constrain(o.clkdiv + dir, 0, 15);
                if (par == 9) {
                    const int cur = (o.flags & HS::MIDIOutSettings::FLAG_CV_GATE)
                        ? (o.clkdiv % HS::MIDIFrame::kCVOutPorts) + 1 : 0;
                    const int v = constrain(cur + dir, 0, ADC_CHANNEL_COUNT);
                    if (v == 0) {
                        o.flags &= ~HS::MIDIOutSettings::FLAG_CV_GATE;
                    } else {
                        o.flags |= HS::MIDIOutSettings::FLAG_CV_GATE;
                        o.clkdiv = v - 1;
                    }
                }
            } else {
                if (par == 7 && dir) o.flags ^= HS::MIDIOutSettings::FLAG_QUANTIZE;
                if (par == 8 && dir) o.flags ^= HS::MIDIOutSettings::FLAG_LEGATO;
                if (par == 9) edit_vel_src(dir);
            }
            break;
        }
    }

    void SelectSetup(int setup_number) {
        // moving to another setup?
        if (setup_number != get_setup_number()) {
          PackSetup(active_setup); // store current settings
          active_setup = setup_number;
          UnpackSetup(active_setup);
          Reset();
        }
    }

    void LeftEncoderTurn(int dir) {
        if (display) {
            // Scroll Log view
            if (log_index > 6) log_view = constrain(log_view + dir, 0, log_index - 6);
        } else if (detail_port >= 0) {
            // browse adjacent ports without leaving the editor
            const int row = constrain(detail_port + dir, 0, kNumRows - 1);
            if (row != detail_port) EnterDetail(row);
        } else {
            SwitchSetup(dir);
        }
    }

    void SwitchSetup(int dir) {
        if (copy_mode) {
            copy_setup_target = constrain(copy_setup_target + dir, 0, kNumSetups - 1);
        } else {
            int new_setup = constrain(get_setup_number() + dir, 0, kNumSetups - 1);
            SelectSetup(new_setup);
        }
    }

    void ToggleDisplay() {
        if (copy_mode) copy_mode = 0;
        else if (detail_port >= 0) LeaveDetail(); // back to overview
        else display = 1 - display;
    }

    // ---- Clock Router: dual-press A+B opens it -- the SAME gesture
    // Quadrants already dedicates to its own Clock Setup overlay
    // (AppQuadrants::CheckButtonCombo, "dual press A+B for Clock Setup"),
    // so this is consistent with the one gesture this whole app family
    // already trains a player to reach for when they want clock controls.
    // L closes it (matches ToggleDisplay's existing "L goes back" meaning
    // for the log view / copy mode). While open, it owns every other
    // button/encoder input (see HandleButtonEvent/HandleEncoderEvent).
    // One shared clock engine (HS::clock_m) drives every clocked screen in
    // the app family, so what this edits -- HS::clock_sync_jack,
    // HS::midi_clkrx_disable, HS::midi_clktx_disable -- are globals, not
    // Captain-local state; HS::ClockRoutingPump() (Main.cpp loop()) is what
    // actually persists them, debounced, from wherever they were changed.
    enum RouterRow : int8_t {
        ROUTER_SYNC,
        ROUTER_RX_SERIAL, ROUTER_RX_USBDEV, ROUTER_RX_HOST1,
        ROUTER_RX_HOST2, ROUTER_RX_BUS,
        ROUTER_TX_SERIAL, ROUTER_TX_USBDEV, ROUTER_TX_HOST1,
        ROUTER_TX_HOST2, ROUTER_TX_BUS,
        ROUTER_ROW_COUNT
    };

    void ToggleClockRouter() {
        clock_router = !clock_router;
        if (clock_router) router_cursor.Init(0, ROUTER_ROW_COUNT - 1);
    }

    // Turn = edit immediately, no separate edit-mode/commit step. Sync
    // Source is the one radio group: turn cycles Off->TR1..4, wrapping
    // both ways. Every Rx/Tx row is directional rather than a flip
    // (right = On, left = Off) so repeated same-direction turns land
    // predictably instead of toggling back and forth.
    void RouterEditFocused(int dir);  // out-of-class (FLASHMEM + LTO)

    void DrawClockRouter() const;  // out-of-class (FLASHMEM survives LTO)

    void Reset() {
        // Reset the interface states
        for (int ch = 0; ch < DAC_CHANNEL_COUNT; ch++)
        {
            note_in[ch] = -1;
            indicator_in[ch] = 0;
            Out(ch, 0);
        }
        frame.MIDIState.clock_count = 0;
    }

    // Queue a panic; the MIDI traffic is sent from Loop(), not the ISR/UI
    void Panic() {
        Reset();
        frame.MIDIState.panic_request = true;
    }

    // build+send one protocol frame: F0 7D 62 4D <ver> <cmd> <payload> F7
    void SendSyxFrame(uint8_t cmd, const uint8_t *payload, uint8_t plen) {
        uint8_t buf[64];
        uint8_t n = 0;
        buf[n++] = 0xF0; buf[n++] = 0x7D; buf[n++] = 0x62; buf[n++] = 'M';
        buf[n++] = SYX_PROTO_VERSION;
        buf[n++] = cmd;
        for (uint8_t i = 0; i < plen && n < 62; ++i) buf[n++] = payload[i] & 0x7f;
        buf[n++] = 0xF7;
        usbMIDI.sendSysEx(n, buf, true);
    }

    // the 7 in-map / 9 out-port parameter values from a packed blob,
    // encoded exactly as GET_PARAM returns them
    static void InMapParams(uint64_t d, uint8_t out[7]) {
        const uint8_t fcc = d & 0xff;
        out[0] = uint8_t((d >> 8) & 0xff) >> 5;              // type
        out[1] = (fcc == 0xff) ? 127 : (fcc & 0x7f);         // subtype/CC
        out[2] = (d >> 16) & 0xff;                           // channel
        out[3] = uint8_t(int8_t((d >> 32) & 0xff) + 64);     // transpose+64
        out[4] = (d >> 40) & 0x7f;                           // range_low
        out[5] = (d >> 48) & 0x7f;                           // range_high
        out[6] = (d >> 24) & 0xff;                           // polyvoice
    }
    static void OutPortParams(uint64_t d, uint8_t out[9]) {
        HS::MIDIOutSettings tmp;
        tmp.Unpack(d);
        out[0] = tmp.function; out[1] = tmp.channel;
        out[2] = tmp.data1; out[3] = tmp.data2;
        out[4] = uint8_t(tmp.transpose + 64);
        out[5] = tmp.range_low; out[6] = tmp.range_high;
        out[7] = tmp.flags; out[8] = tmp.clkdiv;
    }

    // full-setup dump as a stream of DUMP_DATA packets + DUMP_END
    void SendDump(uint8_t s);  // out-of-class (FLASHMEM + LTO)

    // ---- auto-save ----
    // Persist every change (UI or SysEx) once edits settle for a few
    // seconds. Debounced so encoder sweeps don't hammer the EEPROM.
    static constexpr uint8_t kAutoSaveSettleSecs = 3;
    uint32_t autosave_last_check = 0;
    uint32_t live_checksum_prev = 0;
    uint32_t saved_checksum = 0;
    uint8_t settle_seconds = 0;

    uint32_t LiveChecksum() const;  // out-of-class (FLASHMEM + LTO)

    void PersistNow() {
        PackSetup(active_setup);
#ifdef __IMXRT1062__
        StoreData();
#else
        OC::save_app_data(); // global settings + every app's EEPROM chunk
#endif
        saved_checksum = live_checksum_prev = LiveChecksum();
        settle_seconds = 0;
    }

    void AutoSaveTick() {
        const uint32_t now = millis();
        if (now - autosave_last_check < 1000) return;
        autosave_last_check = now;
        const uint32_t cs = LiveChecksum();
        if (cs != live_checksum_prev) {
            live_checksum_prev = cs; // still being edited; restart settle timer
            settle_seconds = 0;
        } else if (cs != saved_checksum && ++settle_seconds >= kAutoSaveSettleSecs) {
            PersistNow();
        }
    }

    bool LiveConfigDirty() const {
        for (int i = 0; i < MIDIMAP_MAX; ++i)
            if (frame.MIDIState.mapping[i].Pack() != setups[active_setup].inmaps[i]) return true;
        for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
            if (frame.MIDIState.outports[i].Pack() != setups[active_setup].outports[i]) return true;
        return false;
    }

#if defined(ARDUINO_TEENSY41)
    // ---- USB device profiles -------------------------------------------
    struct DeviceProfile {
        uint16_t vid = 0, pid = 0;
        uint8_t setup = 0;
        // Suppress (not enable) so a profile stored before this existed
        // reads 0 and keeps the old always-on behavior.
        uint8_t no_bus_thru = 0;
        char name[9] = "";   // 8 chars from the USB product string
    };
    DeviceProfile profiles[kDeviceProfiles];
    // live per-port copy of the profile's bus-thru choice; a device with no
    // profile plays to the bus, as it always did
    uint8_t port_no_bus_thru[2] = {0, 0};
    // stable copy of each port's product string, taken once when the device
    // settles - the draw path must never touch the live USB buffer
    char port_name_[2][9] = {{0}, {0}};
    uint16_t seen_vid[2] = {0, 0}, seen_pid[2] = {0, 0};
    // set on resume: re-apply bindings for devices that were already there,
    // quietly (no arrival popups for something that never left)
    bool reapply_bindings_ = false;
    uint32_t next_dev_poll_ms = 0;

    // profiles persist in GLOBALS.CFG; after writing, CAPTAIN.DAT is
    // reloaded so this app's expected map residency is restored.
    // Deferred: edits mark dirty, DoLoop persists after they settle, so
    // Bind detents never do file I/O in the UI path.
    bool profiles_dirty = false;
    uint32_t profiles_dirty_ms = 0;
    void MarkProfilesDirty() {
        profiles_dirty = true;
        profiles_dirty_ms = millis();
    }
    void PersistProfiles();  // out-of-class (FLASHMEM + LTO)
    // boot-time: other apps' Init may have loaded their own files first,
    // so take the GLOBALS map explicitly rather than assuming residency
    FLASHMEM void LoadProfiles() {
        PhzConfig::load_config();
        uint64_t data = 0;
        for (int i = 0; i < kDeviceProfiles; ++i) {
            profiles[i] = {};
            if (PhzConfig::getValue(DEVICE_PROFILE_KEY + i, data) &&
                (data >> 48) == 0xA5) {
                profiles[i].vid = (data >> 32) & 0xFFFF;
                profiles[i].pid = (data >> 16) & 0xFFFF;
                profiles[i].setup = constrain((int)(data & 0xFF), 0, kNumSetups - 1);
                profiles[i].no_bus_thru = (data >> 8) & 1;
                if (PhzConfig::getValue(DEVICE_PROFILE_KEY + 16 + i, data)) {
                    memcpy(profiles[i].name, &data, 8);
                    profiles[i].name[8] = 0;
                }
            }
        }
    }

    int StoredProfileCount() const {
        int n = 0;
        for (int i = 0; i < kDeviceProfiles; ++i)
            if (profiles[i].vid) ++n;
        return n;
    }
    int NthProfile(int n) const {   // index of the nth stored profile
        for (int i = 0; i < kDeviceProfiles; ++i)
            if (profiles[i].vid && n-- == 0) return i;
        return 0;
    }

    int FindProfile(uint16_t vid, uint16_t pid) const {
        for (int i = 0; i < kDeviceProfiles; ++i)
            if (profiles[i].vid == vid && profiles[i].pid == pid && vid) return i;
        return -1;
    }
    // bind the device on a host port to a setup; creates or updates the
    // profile (name captured from the USB product string). -1 = no device
    // or table full.
    FLASHMEM int BindPort(int port, uint8_t setup) {
        const uint16_t vid = usbHostMIDI[port].idVendor();
        const uint16_t pid = usbHostMIDI[port].idProduct();
        if (!vid) return -1;
        int slot = FindProfile(vid, pid);
        if (slot < 0)
            for (int i = 0; i < kDeviceProfiles && slot < 0; ++i)
                if (!profiles[i].vid) slot = i;
        if (slot < 0) return -1;   // table full: worded as 'full' in the UI
        // re-binding a known device keeps its bus-thru choice
        const uint8_t keep_bus = profiles[slot].no_bus_thru;
        profiles[slot] = { vid, pid, uint8_t(setup % kNumSetups), keep_bus, "" };
        const uint8_t *pn = usbHostMIDI[port].product();
        for (int c = 0; pn && pn[c] && c < 8; ++c)
            profiles[slot].name[c] = (char)pn[c];
        MarkProfilesDirty();
        return slot;
    }
    FLASHMEM void UnbindPort(int port) {
        const int i = FindProfile(usbHostMIDI[port].idVendor(),
                                  usbHostMIDI[port].idProduct());
        if (i >= 0) {
            profiles[i] = {};
            MarkProfilesDirty();
        }
    }
    // bind state of the device on a host port: 0 = unbound, 1..4 = setup+1
    int PortBinding(int port) const {
        const int i = FindProfile(usbHostMIDI[port].idVendor(),
                                  usbHostMIDI[port].idProduct());
        return (i >= 0) ? profiles[i].setup + 1 : 0;
    }

    FLASHMEM void DumpProfiles() {
        for (int i = 0; i < kDeviceProfiles; ++i)
            if (profiles[i].vid)
                serial_printf("profile %d: %04X:%04X setup=%d name=%s\n", i,
                              profiles[i].vid, profiles[i].pid,
                              profiles[i].setup, profiles[i].name);
        serial_printf("PortBinding(0)=%d PortBinding(1)=%d active_setup=%d\n",
                      PortBinding(0), PortBinding(1), active_setup);
    }

    // loop context: watch the host ports; a newly arrived known device
    // switches to its bound setup
    FLASHMEM void PollDeviceProfiles() {
        if (millis() < next_dev_poll_ms) return;
        next_dev_poll_ms = millis() + 500;   // let the product string arrive
        const bool quiet = reapply_bindings_;   // resume: no arrival popups
        reapply_bindings_ = false;
        for (int port = 0; port < 2; ++port) {
            const uint16_t vid = usbHostMIDI[port].idVendor();
            const uint16_t pid = usbHostMIDI[port].idProduct();
            if (!quiet && vid == seen_vid[port] && pid == seen_pid[port]) continue;
            seen_vid[port] = vid;
            seen_pid[port] = pid;
            if (!vid) {                            // departure
                port_no_bus_thru[port] = 0;        // next device starts on
                port_name_[port][0] = 0;
                continue;
            }
            // snapshot the product string here, where the device has had time
            // to settle, so the draw path never reads the live USB buffer
            const uint8_t *pn = usbHostMIDI[port].product();
            if (pn && pn[0]) {
                int c = 0;
                for (; c < 8 && pn[c]; ++c) port_name_[port][c] = (char)pn[c];
                port_name_[port][c] = 0;
            } else {
                port_name_[port][0] = 0;
            }
            const int i = FindProfile(vid, pid);
            // bus-thru follows the DEVICE: applied even when the setup
            // switch below is held back by unsaved edits
            port_no_bus_thru[port] = (i >= 0) ? profiles[i].no_bus_thru : 0;
            if (i < 0) {
                // recognition feedback only, no action implied
                if (!quiet)
                    HS::PokePopup(HS::MESSAGE_POPUP, RowFunctionName(kFirstHostRow + port));
            } else if (profiles[i].setup == active_setup) {
                if (!quiet) HS::PokePopup(HS::MESSAGE_POPUP, "Setup active");
            } else if (LiveConfigDirty()) {
                // auto-anything must never eat unsaved edits
                if (!quiet) HS::PokePopup(HS::MESSAGE_POPUP, "Held: unsaved");
            } else {
                SelectSetup(profiles[i].setup);
                if (!quiet) {
                    char msg[16];
                    snprintf(msg, sizeof(msg), "%s>Set%d",
                             profiles[i].name[0] ? profiles[i].name : "Device",
                             profiles[i].setup + 1);
                    HS::PokePopup(HS::MESSAGE_POPUP, msg);
                }
            }
        }
    }
#endif  // ARDUINO_TEENSY41

    // The internal transport, kept alive while Captain is front.
    //
    // ClockSetup's Controller is the clock engine everywhere else, but it
    // runs only inside HEMISPHERE and Quadrants. With Captain as the
    // standing app the internal clock froze silently: nothing called
    // SyncTrig, so a running clock never tocked, and a 252e listening on
    // the 200e bus had nothing to follow. This is the engine half of that
    // Controller -- transport queue, sync, and the realtime TX fan-out
    // with the same ClkTx gates; the applet keeps its UI, flashers and
    // virtual outputs. Same ISR context as ClockSetup under Quadrants,
    // with the sends deferred for the same reason.
    //
    // Externally clocked (the rig: the Orin sends 0xF8 over USB), clock_q
    // disables MIDI-out, so the bus copy of that clock comes from
    // midi_thru below and is never doubled here. Internally clocked, this
    // is the only source of bus clock.
    void PumpTransport() {
        auto &ms = frame.MIDIState;
        bool midi_sync = false;
        bool clock_sync = HS::frame.synctrig;

        if (ms.clock_q) {
            ms.clock_q = 0;
            clock_sync = 1;
            midi_sync = 1;
            HS::clock_m.DisableMIDIOut();
        }
        if (ms.start_q) {
            ms.start_q = 0;
            HS::clock_m.DisableMIDIOut();
            HS::clock_m.Start();
        }
        if (ms.stop_q) {
            ms.stop_q = 0;
            HS::clock_m.Stop();
            HS::clock_m.EnableMIDIOut();
        }

        // Paused means wait for clock-sync to start
        if (HS::clock_m.IsPaused() && clock_sync)
            HS::clock_m.Start();

        if (HS::clock_m.IsRunning())
            HS::clock_m.SyncTrig(clock_sync, midi_sync);

        if (HS::clock_m.IsRunning() && HS::clock_m.MIDITock()) {
          OC::CORE::DeferTask([](){
            if (~HS::midi_clktx_disable & HS::mMaskUSBDev)
              usbMIDI.sendRealTime(usbMIDI.Clock);
#ifdef ARDUINO_TEENSY41
            if (~HS::midi_clktx_disable & HS::mMaskUSBHost)
              usbHostMIDI[0].sendRealTime(usbMIDI.Clock);
            if (~HS::midi_clktx_disable & HS::mMaskUSBHost2)
              usbHostMIDI[1].sendRealTime(usbMIDI.Clock);
            if (~HS::midi_clktx_disable & HS::mMaskSerial)
              MIDI1.sendRealTime(midi::MidiType(usbMIDI.Clock));
            if (~HS::midi_clktx_disable & HS::mMaskBus)
              OC::PresetBus::QueueMidiTx(usbMIDI.Clock, 0, 0, 0);
#endif
          });
        }
    }

    // forward an incoming message to the other interfaces (source excluded),
    // honoring the same thru mask Quadrants uses -- the bus included, so a
    // keyboard on the USB host jack can play bus-MIDI modules (259e etc.)
    void midi_thru(const HS::MIDIMessage &msg, uint8_t exclude);  // out-of-class (FLASHMEM + LTO)

    // runs in the main loop (not the ISR): drains deferred work
public:
    // Drain every MIDI source into the (global) MIDIState and run thru.
    // Split out of DoLoop so ANOTHER app can keep the MIDI->CV engine alive
    // while Captain is suspended - the Tuner calls this so you can play the
    // note you are tuning. Loop context only: USBHost_t36 is not ISR-safe.
    void PollMidiSources();  // out-of-class (FLASHMEM + LTO)

    // (this region was already public; restore it rather than privatize
    // everything below, which includes the selftest instrumentation)
    FLASHMEM void DoLoop() {
        auto &hMIDI = frame.MIDIState;

#if defined(ARDUINO_TEENSY41)
        PollDeviceProfiles();
        if (profiles_dirty && millis() - profiles_dirty_ms > 1500)
            PersistProfiles();
#endif

        PollMidiSources();

        if (syx_rx_len) {
            HandleSysEx(syx_rx_buf, syx_rx_len);
            syx_rx_len = 0;
        }

        AutoSaveTick();

        // surface engine sends in the log display
        HS::MIDIFrame::OutLogEvent e;
        while (hMIDI.PopOutLog(e)) {
            if (e.message == 4) { // bend: reassemble signed value
                const int bend = ((e.data2 << 7) | e.data1) - 8192;
                UpdateLog(0, e.port, 4, e.channel, 0, bend);
            } else {
                UpdateLog(0, e.port, e.message, e.channel, e.data1, e.data2);
            }
        }

        if (hMIDI.panic_request) {
            hMIDI.panic_request = false;
            hMIDI.PanicOutputs(); // targeted note-offs for engine notes
            for (int ch = 0; ch < 16; ++ch) {
                hMIDI.SendCC(ch, 120, 0); // All Sound Off
                hMIDI.SendCC(ch, 123, 0); // All Notes Off
            }
        }

        if (!syx_pending) return;
        const uint8_t pending = syx_pending;
        syx_pending = 0;

        if (pending & PEND_REPLY) {
            SendSyxFrame(syx_reply[0], &syx_reply[1], syx_reply_len - 1);
        }
        if (pending & PEND_IDENTITY) {
            // Universal Device Inquiry reply: mfr 7D, family "hOC", fw version
            const uint8_t reply[] = {
                0xF0, 0x7E, 0x7F, 0x06, 0x02,
                0x7D, 'h', 'O', 'C', SYX_PROTO_VERSION,
                2, 0, 1, 0, 0xF7,
            };
            usbMIDI.sendSysEx(sizeof(reply), reply, true);
        }
        if (pending & PEND_INFO) {
            const uint8_t info[] = {
                2, // schema: 2 adds n_dac_out and class 4 (DAC)
                2, 0, 1, // firmware version
                MIDIMAP_MAX,
                HS::MIDIFrame::kCVOutPorts, HS::MIDIFrame::kTrigOutPorts,
                kNumSetups, uint8_t(active_setup),
                uint8_t(LiveConfigDirty() ? 1 : 0),
                // How many CV outputs this board actually has. Everything
                // else in this payload counts something else -- in_maps is
                // 32, and kCVOutPorts/kTrigOutPorts are the jacks you patch
                // INTO -- so a host had no way to ask this and had to assert
                // it instead. Four on an hOC, eight on the Xenomorpher.
                // Appended last: older hosts zip field names against the
                // payload and simply stop early.
                DAC_CHANNEL_COUNT,
            };
            SendSyxFrame(SYX_INFO_R, info, sizeof(info));
        }
        if (pending & PEND_DUMP) {
            SendDump(syx_dump_setup);
        }
        if (pending & PEND_SAVE) {
            PersistNow();
            const uint8_t echo = SYX_SAVE;
            SendSyxFrame(SYX_ACK, &echo, 1);
        }
        if (pending & PEND_LOAD) {
            // revert to the stored blob, discarding live edits
            active_setup = syx_load_target;
            UnpackSetup(active_setup);
            Reset();
            const uint8_t echo[2] = {SYX_LOAD, syx_load_target};
            SendSyxFrame(SYX_ACK, echo, 2);
        }
        if (pending & PEND_FACTORY) {
            hMIDI.Init();
            for (int s = 0; s < kNumSetups; ++s) PackSetup(s);
            active_setup = 0;
            Reset();
            const uint8_t echo = SYX_FACTORY;
            SendSyxFrame(SYX_ACK, &echo, 1);
        }
    }

    // ------------------------------------------------------------------
    // SysEx protocol v1 — see docs/hoc-midi-sysex.md
    // Frame: F0 7D 62 4D <ver> <cmd> <body...> F7, all bytes 7-bit, <=60 total
    // ------------------------------------------------------------------
    static constexpr uint8_t SYX_PROTO_VERSION = 1;
    enum SyxCmd : uint8_t {
      SYX_INFO = 0x01, SYX_GET = 0x02, SYX_SET = 0x03, SYX_GET_DUMP = 0x04,
      SYX_SAVE = 0x06, SYX_LOAD = 0x07, SYX_FACTORY = 0x08, SYX_PANIC = 0x09,
      SYX_SELECT = 0x0A, SYX_ECHO = 0x0B,
      SYX_ACK = 0x40, SYX_INFO_R = 0x41, SYX_GET_R = 0x42,
      SYX_DUMP_DATA = 0x44, SYX_DUMP_END = 0x45, SYX_ECHO_R = 0x4B,
      SYX_NAK = 0x7E,
    };
    enum SyxErr : uint8_t {
      SYXERR_VERSION = 1, SYXERR_CMD = 2, SYXERR_ADDR = 3,
      SYXERR_VALUE = 4, SYXERR_BUSY = 5, SYXERR_CHECKSUM = 6,
    };
    // CLS_DAC is the physical CV output (A..H), as against CLS_IN which is
    // the MIDI map that feeds it. It exists because output voltage scaling
    // lived only in the I/O settings menu: a host could bind a pitch to
    // output A over SysEx but could not tell the module that A drives a
    // Buchla at 1.2V/oct, so every interval played 20% flat until somebody
    // walked over and set it by hand. Nothing about that was a hardware
    // limit -- the per-channel setting already existed and IOFrameToChannel
    // already consults it every tick; the wire simply had no word for it.
    enum SyxClass : uint8_t { CLS_GLOBAL = 0, CLS_IN = 1, CLS_CV = 2,
                              CLS_TR = 3, CLS_DAC = 4 };
    static constexpr uint8_t kGlobalParamCount = 7;
    static constexpr uint8_t kInParamCount = 7;
    static constexpr uint8_t kOutParamCount = 9;
    static constexpr uint8_t kDacParamCount = 2;   // scaling, autotune

    // parameter access; value encoding matches the spec (transpose stored +64,
    // in-map subtype 127 = auto-learn)
    bool get_param(uint8_t cls, uint8_t idx, uint8_t par, uint8_t &value) {
        auto &M = frame.MIDIState;
        switch (cls) {
          case CLS_GLOBAL:
            if (idx != 0) return false;
            switch (par) {
              case 0: value = 2; return true; // schema (2 = has class 4 / n_dac_out)
              case 1: value = active_setup; return true;
              case 2: value = kNumSetups; return true;
              case 3: value = M.bend_range; return true;
              case 4: value = M.poly_mode; return true;
              case 5: value = M.pc_channel; return true;
              case 6: value = HS::trig_length; return true;
              default: return false;
            }
          case CLS_IN: {
            if (idx >= MIDIMAP_MAX || par >= kInParamCount) return false;
            uint64_t d = M.mapping[idx].Pack();
            const uint8_t b[7] = {
              uint8_t(d & 0xff),         // function_cc
              uint8_t((d >> 8) & 0xff),  // function (type in upper 3 bits)
              uint8_t((d >> 16) & 0xff), // channel
              uint8_t((d >> 24) & 0xff), // dac_polyvoice
              uint8_t((d >> 32) & 0xff), // transpose (raw int8)
              uint8_t((d >> 40) & 0xff), // range_low
              uint8_t((d >> 48) & 0xff), // range_high
            };
            switch (par) {
              case 0: value = b[1] >> 5; return true;                 // type
              case 1: value = (b[0] == 0xff) ? 127 : (b[0] & 0x7f); return true; // subtype/CC (127=learn)
              case 2: value = b[2]; return true;                      // channel
              case 3: value = uint8_t(int8_t(b[4]) + 64); return true; // transpose+64
              case 4: value = b[5]; return true;
              case 5: value = b[6]; return true;
              case 6: value = b[3]; return true;                      // polyvoice
            }
            return false;
          }
          case CLS_CV:
          case CLS_TR: {
            const int p = (cls == CLS_TR) ? HS::MIDIFrame::kCVOutPorts + idx : idx;
            const int limit = (cls == CLS_TR) ? HS::MIDIFrame::kTrigOutPorts
                                              : HS::MIDIFrame::kCVOutPorts;
            if (idx >= limit || par >= kOutParamCount) return false;
            const HS::MIDIOutPort &o = M.outports[p];
            switch (par) {
              case 0: value = o.function; return true;
              case 1: value = o.channel; return true;
              case 2: value = o.data1; return true;
              case 3: value = o.data2; return true;
              case 4: value = uint8_t(o.transpose + 64); return true;
              case 5: value = o.range_low; return true;
              case 6: value = o.range_high; return true;
              case 7: value = o.flags; return true;
              case 8: value = o.clkdiv; return true;
            }
            return false;
          }
          case CLS_DAC: {
            if (idx >= DAC_CHANNEL_COUNT || par >= kDacParamCount) return false;
            const OC::IOSettings &io = io_settings();
            switch (par) {
              case 0: value = uint8_t(io.get_output_scaling(idx)); return true;
              case 1: value = io.autotune_data_enabled(idx) ? 1 : 0; return true;
            }
            return false;
          }
          default: return false;
        }
    }

    // returns 0 on success, else a SyxErr
    uint8_t set_param(uint8_t cls, uint8_t idx, uint8_t par, uint8_t value) {
        auto &M = frame.MIDIState;
        switch (cls) {
          case CLS_GLOBAL:
            if (idx != 0) return SYXERR_ADDR;
            switch (par) {
              case 1:
                if (value >= kNumSetups) return SYXERR_VALUE;
                SelectSetup(value);
                return 0;
              case 3:
                if (value < 1 || value > 24) return SYXERR_VALUE;
                M.bend_range = value; return 0;
              case 4:
                if (value > 2) return SYXERR_VALUE;
                M.poly_mode = value;
                M.UpdateMaxPolyphony();
                return 0;
              case 5:
                if (value > 16) return SYXERR_VALUE;
                M.pc_channel = value; return 0;
              case 6:
                if (value < 1) return SYXERR_VALUE;
                HS::trig_length = value; return 0;
              case 0: case 2: return SYXERR_VALUE; // read-only
              default: return SYXERR_ADDR;
            }
          case CLS_IN: {
            if (idx >= MIDIMAP_MAX || par >= kInParamCount) return SYXERR_ADDR;
            uint64_t d = M.mapping[idx].Pack();
            auto setbyte = [&d](int n, uint8_t v) {
              d = (d & ~(uint64_t(0xff) << (n * 8))) | (uint64_t(v) << (n * 8));
            };
            switch (par) {
              case 0:
                if (value > 5) return SYXERR_VALUE;
                setbyte(1, value << 5);
                break;
              case 1: setbyte(0, (value == 127) ? 0xff : value); break;
              case 2:
                if (value > 16) return SYXERR_VALUE;
                setbyte(2, value); break;
              case 3:
                if (value < 16 || value > 112) return SYXERR_VALUE;
                setbyte(4, uint8_t(int8_t(value) - 64)); break;
              case 4: setbyte(5, value & 0x7f); break;
              case 5: setbyte(6, value & 0x7f); break;
              case 6:
                if (value >= DAC_CHANNEL_COUNT) return SYXERR_VALUE;
                setbyte(3, value); break;
            }
            M.mapping[idx].Unpack(d);
            M.UpdateMidiChannelFilter();
            M.UpdateMaxPolyphony();
            return 0;
          }
          case CLS_CV:
          case CLS_TR: {
            const int p = (cls == CLS_TR) ? HS::MIDIFrame::kCVOutPorts + idx : idx;
            const int limit = (cls == CLS_TR) ? HS::MIDIFrame::kTrigOutPorts
                                              : HS::MIDIFrame::kCVOutPorts;
            const int fn_count = (cls == CLS_TR) ? HS::TRFN_COUNT : HS::CVFN_COUNT;
            if (idx >= limit || par >= kOutParamCount) return SYXERR_ADDR;
            HS::MIDIOutPort &o = M.outports[p];
            switch (par) {
              case 0:
                if (value >= fn_count) return SYXERR_VALUE;
                o.function = value;
                o.ResetRuntime();
                break;
              case 1:
                if (value > 15) return SYXERR_VALUE;
                o.channel = value; break;
              case 2: o.data1 = value & 0x7f; break;
              case 3: o.data2 = value & 0x7f; break;
              case 4:
                if (value < 16 || value > 112) return SYXERR_VALUE;
                o.transpose = int8_t(value) - 64; break;
              case 5: o.range_low = value & 0x7f; break;
              case 6: o.range_high = value & 0x7f; break;
              case 7: o.flags = value; break;
              case 8:
                if (value > 15) return SYXERR_VALUE;
                o.clkdiv = value; break;
            }
            o.Sanitize();
            return 0;
          }
          case CLS_DAC: {
            if (idx >= DAC_CHANNEL_COUNT || par >= kDacParamCount)
              return SYXERR_ADDR;
            OC::IOSettings &io = mutable_io_settings();
            switch (par) {
              case 0:
                // Scaling applies BEFORE the clamp in PitchToScaledDAC, so
                // 1.2x on an output carrying a 0..10V controller pins every
                // value over ~106 to full scale. That is the caller's
                // business, not ours: a module cannot know whether output E
                // is a note or a filter sweep. Range-check and obey.
                if (value >= OC::VOLTAGE_SCALING_LAST) return SYXERR_VALUE;
                io.apply_value(
                    OC::IOSettings::channel_setting(OC::IO_SETTING_A_SCALING, idx),
                    value);
                break;
              case 1:
                if (value > 1) return SYXERR_VALUE;
                io.apply_value(
                    OC::IOSettings::channel_setting(OC::IO_SETTING_A_TUNING, idx),
                    value);
                break;
            }
            return 0;
          }
          default: return SYXERR_ADDR;
        }
    }

    // pending SysEx work: flags set in ISR context, drained in DoLoop()
    enum SyxPending : uint8_t {
      PEND_REPLY = 1, PEND_IDENTITY = 2, PEND_INFO = 4,
      PEND_DUMP = 8, PEND_SAVE = 16, PEND_LOAD = 32, PEND_FACTORY = 64,
    };
    volatile uint8_t syx_pending = 0;
    uint8_t syx_reply[8]; // cmd + payload for ACK/NAK/GET_R
    uint8_t syx_reply_len = 0;
    uint8_t syx_dump_setup = 0;
    uint8_t syx_load_target = 0;
    // incoming dump bookkeeping
    uint8_t syx_rx_xor = 0;
    uint8_t syx_rx_packets = 0;

    void QueueReply(uint8_t cmd, std::initializer_list<uint8_t> payload) {
        syx_reply[0] = cmd;
        uint8_t n = 1;
        for (uint8_t b : payload) {
            if (n >= sizeof(syx_reply)) break;
            syx_reply[n++] = b;
        }
        syx_reply_len = n;
        syx_pending |= PEND_REPLY;
    }
    void QueueReplyBuf(uint8_t cmd, const uint8_t *body, uint8_t n) {
        syx_reply[0] = cmd;
        uint8_t m = 1;
        for (uint8_t i = 0; i < n && m < sizeof(syx_reply); ++i)
            syx_reply[m++] = body[i];
        syx_reply_len = m;
        syx_pending |= PEND_REPLY;
    }
    void QueueAck(std::initializer_list<uint8_t> echo) { QueueReply(SYX_ACK, echo); }
    void QueueNak(uint8_t cmd, uint8_t err) { QueueReply(SYX_NAK, {cmd, err}); }

    /* Suspend hook / manual [DUMP]: send the active setup over SysEx */
    void OnSendSysEx() {
        syx_dump_setup = active_setup;
        syx_pending |= PEND_DUMP;
    }

    // ISR context: stage the frame; DoLoop() parses it.
    void OnReceiveSysEx() {
        if (syx_rx_len) return;  // one frame in flight; next one re-triggers
        const uint8_t *sx = usbMIDI.getSysExArray();
        unsigned len = usbMIDI.getSysExArrayLength();
        if (len > sizeof(syx_rx_buf)) len = sizeof(syx_rx_buf);
        memcpy((void *)syx_rx_buf, sx, len);
        syx_rx_len = len;
    }
    volatile uint16_t syx_rx_len = 0;
    uint8_t syx_rx_buf[96];

    // selftest instrumentation: worst gap between MIDI poll passes (the
    // dominant term in device-side MIDI latency) and echo probes served
    uint32_t poll_last_us = 0;
    uint32_t poll_gap_max_us = 0;
    uint32_t echo_count = 0;
    uint32_t dump_count = 0;   // SendDump calls (suspend hook, [DUMP], GET_DUMP)

    void HandleSysEx(const uint8_t *sx, unsigned len) {
        // Universal Device Inquiry: F0 7E <dev> 06 01 F7
        if (len >= 6 && sx[1] == 0x7E && sx[3] == 0x06 && sx[4] == 0x01) {
            syx_pending |= PEND_IDENTITY;
            return;
        }
        if (len < 7 || sx[1] != 0x7D || sx[2] != 0x62) return; // not ours
        if (sx[3] != 'M') { // another app's dump passing by; just log it
            UpdateLog(1, 0, 5, 0, sx[3], 0);
            return;
        }
        const uint8_t ver = sx[4];
        const uint8_t cmd = sx[5];
        const uint8_t *body = sx + 6;
        const unsigned blen = (len >= 7) ? len - 7 : 0; // strip F0..cmd and F7
        if (ver != SYX_PROTO_VERSION) { QueueNak(cmd, SYXERR_VERSION); return; }

        UpdateLog(1, 0, 5, 0, 'M', cmd);

        switch (cmd) {
          case SYX_INFO: syx_pending |= PEND_INFO; break;

          case SYX_GET: {
            uint8_t value = 0;
            if (blen >= 3 && get_param(body[0], body[1], body[2], value))
                QueueReply(SYX_GET_R, {body[0], body[1], body[2], value});
            else QueueNak(cmd, SYXERR_ADDR);
            break;
          }
          case SYX_SET: {
            if (blen < 4) { QueueNak(cmd, SYXERR_ADDR); break; }
            const uint8_t err = set_param(body[0], body[1], body[2], body[3]);
            if (err) QueueNak(cmd, err);
            else QueueAck({body[0], body[1], body[2], body[3]});
            break;
          }
          case SYX_GET_DUMP: {
            uint8_t s = (blen >= 1) ? body[0] : 0x7f;
            if (s == 0x7f) s = active_setup;
            if (s >= kNumSetups) { QueueNak(cmd, SYXERR_VALUE); break; }
            syx_dump_setup = s;
            syx_pending |= PEND_DUMP;
            break;
          }
          case SYX_DUMP_DATA: { // host -> device restore, applied live
            if (blen < 3) { QueueNak(cmd, SYXERR_ADDR); break; }
            const uint8_t seq = body[1];
            if (seq == 0) { syx_rx_xor = 0; syx_rx_packets = 0; }
            unsigned i = 3; // past setup#/seq/total
            uint8_t err = 0;
            while (i + 1 < blen) {
                const uint8_t cls = body[i], idx = body[i + 1];
                const uint8_t np = (cls == CLS_GLOBAL) ? kGlobalParamCount
                                 : (cls == CLS_IN) ? kInParamCount : kOutParamCount;
                if (i + 2 + np > blen) { err = SYXERR_ADDR; break; }
                for (uint8_t par = 0; par < np; ++par) {
                    const uint8_t v = body[i + 2 + par];
                    // skip read-only globals; tolerate per-param rejects
                    if (cls == CLS_GLOBAL && (par == 0 || par == 1 || par == 2)) continue;
                    set_param(cls, idx, par, v);
                }
                for (unsigned b = i; b < i + 2 + np; ++b) syx_rx_xor ^= body[b];
                i += 2 + np;
            }
            ++syx_rx_packets;
            if (err) QueueNak(cmd, err);
            else QueueAck({seq});
            break;
          }
          case SYX_DUMP_END: {
            if (blen < 3) { QueueNak(cmd, SYXERR_ADDR); break; }
            const bool ok = (body[1] == syx_rx_packets) && (body[2] == (syx_rx_xor & 0x7f));
            syx_rx_xor = 0;
            syx_rx_packets = 0;
            if (ok) QueueAck({body[0]});
            else QueueNak(cmd, SYXERR_CHECKSUM);
            break;
          }
          case SYX_SAVE: syx_pending |= PEND_SAVE; break;
          case SYX_LOAD:
            if (blen < 1 || body[0] >= kNumSetups) { QueueNak(cmd, SYXERR_VALUE); break; }
            syx_load_target = body[0];
            syx_pending |= PEND_LOAD;
            break;
          case SYX_FACTORY:
            if (blen >= 2 && body[0] == 0x21 && body[1] == 0x42)
                syx_pending |= PEND_FACTORY;
            else QueueNak(cmd, SYXERR_VALUE);
            break;
          case SYX_PANIC:
            frame.MIDIState.panic_request = true;
            QueueAck({});
            break;
          case SYX_SELECT:
            if (blen < 1 || body[0] >= kNumSetups) { QueueNak(cmd, SYXERR_VALUE); break; }
            SelectSetup(body[0]);
            QueueAck({body[0]});
            break;
          case SYX_ECHO:  // latency probe: bounce the caller's token back
            ++echo_count;
            // syx_reply holds cmd + 7 body bytes: cap at 7 so a max-size
            // token comes back whole instead of silently short
            QueueReplyBuf(SYX_ECHO_R, body, blen > 7 ? 7 : (uint8_t)blen);
            break;
          default:
            QueueNak(cmd, SYXERR_CMD);
            break;
        }
    }

   void ToggleCopyMode() {
       copy_mode = 1 - copy_mode;
       copy_setup_source = get_setup_number();
       copy_setup_target = copy_setup_source + 1;
       if (copy_setup_target > 3) copy_setup_target = 0;
   }

   void ToggleCursor() {
       if (copy_mode) CopySetup(copy_setup_target, copy_setup_source);
       else if (detail_port < 0) EnterDetail(cursor.cursor_pos());
#if defined(ARDUINO_TEENSY41)
       // host detail: Device/Id are read-only - engaging edit mode there
       // silently eats encoder turns ("it won't let me scroll")
       else if (detail_port >= kFirstHostRow && param_cursor.cursor_pos() < 2)
           return;
#endif
       else param_cursor.toggle_editing();
   }

   /* Perform a copy or sysex dump */
   void CopySetup(int target, int source) {
       if (source == target) {
           OnSendSysEx();
       } else {
           PackSetup(active_setup); // capture live edits first
           setups[target] = setups[source];
           if (target == active_setup) UnpackSetup(active_setup);
           else SelectSetup(target);
       }
       copy_mode = 0;
   }

private:
    // Housekeeping
    int8_t detail_port; // -1 = overview list, else the open port row
    bool display; // 0=Setup Edit 1=Log
    bool copy_mode; // Copy mode on/off
    bool clock_router = false; // Clock Router screen open (A+B combo)
    OC::menu::ScreenCursor<OC::menu::kScreenLines> router_cursor;
    uint16_t mask = 0, last_mask = 0; // combo edge-detect, mirrors Quadrants
    int active_setup; // index of current setup
    int copy_setup_source; // Which setup is being copied?
    int copy_setup_target; // Which setup is being copied to?

    CaptainMIDILog log[MIDI_LOG_MAX_SIZE];
    int log_index; // Index of log for writing
    int log_view; // Current index for viewing

    // MIDI In
    // TODO: replace with data from MIDIFrame
    int note_in[DAC_CHANNEL_COUNT]; // track active note per DAC channel
    uint16_t indicator_in[DAC_CHANNEL_COUNT]; // A MIDI indicator will display next to MIDI In assignment

    // MIDI Out state lives in frame.MIDIState.outports[] (engine-owned)

    // stored setups: packed in-maps + out-ports (see PackSetup/UnpackSetup)
    struct SetupBlob {
        uint64_t inmaps[MIDIMAP_MAX];
        uint64_t outports[HS::MIDIFrame::kOutPorts];
    };
    SetupBlob setups[kNumSetups];

    // 8-char port names for the overview rows
    const char* RowLabel(int pos) const {
#if defined(ARDUINO_TEENSY41)
        if (pos >= kFirstHostRow) return (pos - kFirstHostRow) ? "Host2" : "Host1";
#endif
        if (pos < DAC_CHANNEL_COUNT) return midi2cv_label[pos];
        const int p = pos - DAC_CHANNEL_COUNT;
        if (p < HS::MIDIFrame::kCVOutPorts) return cv2midi_label[p];
        return trig2midi_label[p - HS::MIDIFrame::kCVOutPorts];
    }

    // the function currently assigned to a row, as a safe display string
    const char* RowFunctionName(int pos) const {
#if defined(ARDUINO_TEENSY41)
        if (pos >= kFirstHostRow) {
            const int port = pos - kFirstHostRow;
            if (!usbHostMIDI[port].idVendor()) return "none";
            // Read our OWN snapshot, never the live USB string: the host
            // stack rewrites that buffer while a device enumerates, and this
            // runs in the draw path - which is exactly how a replug painted
            // garbage across the row for a few seconds.
            return port_name_[port][0] ? port_name_[port] : "device";
        }
#endif
        if (pos < DAC_CHANNEL_COUNT)
            return frame.MIDIState.mapping[pos].get_label();
        const int p = pos - DAC_CHANNEL_COUNT;
        const HS::MIDIOutPort &o = frame.MIDIState.outports[p];
        if (p >= HS::MIDIFrame::kCVOutPorts)
            return trig_out_fn_names[o.function < HS::TRFN_COUNT ? o.function : 0];
        return cv_out_fn_names[o.function < HS::CVFN_COUNT ? o.function : 0];
    }

    const char* RouterRowLabel(int row) const;  // out-of-class (FLASHMEM + LTO)
    const char* RouterRowValue(int row) const;  // out-of-class (FLASHMEM + LTO)

    // live physical-input state, left of the activity area:
    // CV rows get a level bar, trigger rows a gate lamp, MIDI-in rows
    // a bar for the CV they are currently outputting
    void DrawRowInputState(int pos, int y) const {
        const int fullscale = HSAPPLICATION_5V * 2; // 10V unipolar
        if (pos < DAC_CHANNEL_COUNT) {
            const int out = frame.MIDIState.mapping[pos].ViewOut();
            const int h = constrain(out * 8 / fullscale, 0, 8);
            if (h > 0) graphics.drawRect(50, y + 9 - h, 3, h);
            return;
        }
        const int p = pos - DAC_CHANNEL_COUNT;
        if (p < HS::MIDIFrame::kCVOutPorts) {
            const int h = constrain(HS::cvmap[p].In() * 8 / fullscale, 0, 8);
            if (h > 0) graphics.drawRect(50, y + 9 - h, 3, h);
        } else {
            const int k = p - HS::MIDIFrame::kCVOutPorts;
            if (HS::trigmap[k].Gate()) graphics.drawRect(49, y + 2, 5, 6);
            else graphics.drawFrame(49, y + 3, 5, 5);
        }
    }

    // draw MIDI activity for a row: icon, or the sounding note name
    void DrawRowActivity(int pos, int y) const {
        if (pos < DAC_CHANNEL_COUNT) {
            if (indicator_in[pos] > 0 || note_in[pos] > -1)
                graphics.drawBitmap8(70, y + 2, 8, MIDI_ICON);
            return;
        }
        const HS::MIDIOutPort &o = frame.MIDIState.outports[pos - DAC_CHANNEL_COUNT];
        if (o.gated && o.last_note <= 127) {
            graphics.setPrintPos(70, y + 2);
            graphics.print(midi_note_numbers[o.last_note]);
        } else if (o.indicator > 0) {
            graphics.drawBitmap8(70, y + 2, 8, MIDI_ICON);
        }
    }

    // header: presence dot (filled = computer connected) + caps when active.
    // Inversion stays reserved for focus (design-system: one mark, one
    // meaning); this is the same dot idiom as the bus WPM indicator.
    void DrawUSBStatus(int x) const {
        graphics.drawCircle(x + 1, 4, 2);
        if (usb_configuration) graphics.drawRect(x, 3, 3, 3);
        graphics.setPrintPos(x + 6, 1);
        graphics.print(usb_configuration ? "USB" : "usb");
    }

    void DrawOverview() const {
        gfxHeader("Captain");
        DrawUSBStatus(48);
        gfxPrint(128 - 48, 1, "Setup ");
        gfxPrint(get_setup_number() + 1);
        if (LiveConfigDirty()) gfxPrint("*"); // unsaved changes

        OC::menu::SettingsList<OC::menu::kScreenLines, 0, 76> settings_list(cursor);
        OC::menu::SettingsListItem list_item;
        while (settings_list.available()) {
            const int current = settings_list.Next(list_item);
            if (current >= kNumRows) { list_item.DrawCustom(); continue; }

            DrawRowInputState(current, list_item.y);
            DrawRowActivity(current, list_item.y);
            list_item.SetPrintPos();
            graphics.print(RowLabel(current));
            // right column: assigned function; press to open the editor
            graphics.setPrintPos(0, list_item.y + 2); // reset for DrawDefault path
            list_item.DrawDefault(RowFunctionName(current), 0, CaptainSettings[0]);
        }
    }

    void DrawDetail() const {
        const int row = detail_port;
        gfxHeader(RowLabel(row));
        DrawUSBStatus(52);
        gfxPrint(128 - 48, 1, "Setup ");
        gfxPrint(get_setup_number() + 1);
        if (LiveConfigDirty()) gfxPrint("*");

        OC::menu::SettingsList<OC::menu::kScreenLines, 0, 76> settings_list(param_cursor);
        OC::menu::SettingsListItem list_item;
        const int n_params = DetailParamCount(row);
        while (settings_list.available()) {
            const int current = settings_list.Next(list_item);
            if (current >= n_params) { list_item.DrawCustom(); continue; }

            list_item.SetPrintPos();
            graphics.print(DetailParamLabel(row, current));
            const int value = DetailParamValue(row, current);
            const auto &attr = CaptainSettings[DetailParamAttr(row, current)];
#if defined(ARDUINO_TEENSY41)
            if (row >= kFirstHostRow) {
                const int port = row - kFirstHostRow;
                char buf[12];
                const char *v = buf;
                if (current == 0) {             // Device
                    v = RowFunctionName(row);
                } else if (current == 1) {      // Id
                    if (usbHostMIDI[port].idVendor())
                        snprintf(buf, sizeof(buf), "%04X:%04X",
                                 usbHostMIDI[port].idVendor(),
                                 usbHostMIDI[port].idProduct());
                    else v = "-";
                } else if (current == 2) {      // Bind
                    const int b = PortBinding(port);
                    if (!usbHostMIDI[port].idVendor()) v = "-";
                    else if (b) snprintf(buf, sizeof(buf), "Setup %d", b);
                    else v = "off";
                } else if (current == 3) {      // Bus: play the 200e bus?
                    if (!usbHostMIDI[port].idVendor()) v = "-";
                    else v = port_no_bus_thru[port] ? "off" : "on";
                } else {                        // stored profile rows
                    const DeviceProfile &pr = profiles[NthProfile(current - 4)];
                    snprintf(buf, sizeof(buf), "Setup %d", pr.setup + 1);
                }
                list_item.DrawDefault(v, 0, attr);
                continue;
            }
#endif
            if (row < DAC_CHANNEL_COUNT && current == 0) {
                // in-map Assign: composite value; print its label string
                list_item.DrawDefault(frame.MIDIState.mapping[row].get_label(), value, attr);
            } else {
                list_item.DrawDefault(value, attr);
            }
        }
    }

    void DrawLogScreen() {
        gfxHeader("IO Ch Type  Values");
        if (log_index) {
            for (int l = 0; l < 6; l++)
            {
                int ix = l + log_view; // Log index
                if (ix < log_index) {
                    log[ix].DrawAt(l * 8 + 15);
                }
            }

            // Draw scroll
            if (log_index > 6) {
                graphics.drawFrame(122, 14, 6, 48);
                int y = Proportion(log_view, log_index - 6, 38);
                y = constrain(y, 0, 38);
                graphics.drawRect(124, 16 + y, 2, 6);
            }
        }
    }

    void DrawCopyScreen() {
        gfxHeader("Copy");

        graphics.setPrintPos(8, 28);
        graphics.print("Setup ");
        graphics.print(copy_setup_source + 1);
        graphics.print(" -");
        graphics.setPrintPos(58, 28);
        graphics.print("> ");
        if (copy_setup_source == copy_setup_target) graphics.print("SysEx");
        else {
            graphics.print("Setup ");
            graphics.print(copy_setup_target + 1);
        }

        graphics.setPrintPos(0, 55);
        graphics.print("[CANCEL]");

        graphics.setPrintPos(90, 55);
        graphics.print(copy_setup_source == copy_setup_target ? "[DUMP]" : "[COPY]");
    }

    int get_setup_number() const {
        return active_setup;
    }

    // CV/trigger -> MIDI conversion lives in HS::MIDIFrame::ProcessOutputs
    // (HSIOFrame.cpp); the old per-app implementation is gone.
    // read one message from the device; returns whether one was processed
    template <typename T1>
    bool poll_midi(T1 &device, uint8_t srcmask) {
        if (!device.read()) return false;

        uint8_t message = device.getType();
        // per-interface RX filters (same gates Quadrants applies)
        if (message >= 0xF8) {
            if (HS::midi_clkrx_disable & srcmask) return true;
        } else if (message != HEM_MIDI_SYSEX) {
            if (HS::midi_msgrx_disable & srcmask) return true;
        }
        uint8_t channel = device.getChannel();
        uint8_t data1 = device.getData1();
        uint8_t data2 = device.getData2();

        // Handle system exclusive dump for Setup data.
        // Copy only — parsing runs from Loop() so the whole SysEx stack
        // stays out of the ISR (and out of ITCM on Teensy 4).
        if (message == HEM_MIDI_SYSEX) {
            OnReceiveSysEx();
            return true;   // never thru our own config traffic
        }

        const HS::MIDIMessage msg = {channel, message, data1, data2};
        HS::frame.MIDIState.ProcessMIDIMsg(msg);
        uint8_t exclude = srcmask;
#if defined(ARDUINO_TEENSY41) && defined(PRESET_BUS)
        // per-device bus thru (Host detail "Bus" row): keep a chatty
        // controller off the 200e bus without muting it everywhere else
        if ((srcmask == mMaskUSBHost && port_no_bus_thru[0]) ||
            (srcmask == mMaskUSBHost2 && port_no_bus_thru[1]))
            exclude |= mMaskBus;
#endif
        midi_thru(msg, exclude);
        return true;
    }

    // read one message received over the 200e preset bus (PRESET_BUS builds;
    // the stub ReadMidiRx returns false elsewhere). Status low nibble is the
    // 200e bus mask: A (0x8) -> channel 1, B (0x4) -> channel 2.
    bool poll_bus_midi() {
        uint8_t status, d1, d2;
        if (!OC::PresetBus::ReadMidiRx(status, d1, d2)) return false;

        const uint8_t type = (status >= 0xF8) ? status : (status & 0xF0);
        const bool clk = type >= 0xF8;
        if (clk && (HS::midi_clkrx_disable & mMaskBus)) return true;
        if (!clk && (HS::midi_msgrx_disable & mMaskBus)) return true;

        const uint8_t buses = clk ? 0x8 : (status & 0x0F);
        for (uint8_t bit = 0; bit < 4; ++bit) {
            if (!(buses & (0x8 >> bit))) continue;
            const HS::MIDIMessage msg =
                {uint8_t(bit + 1), type, uint8_t(d1 & 0x7F), uint8_t(d2 & 0x7F)};
            HS::frame.MIDIState.ProcessMIDIMsg(msg);
            midi_thru(msg, mMaskBus);
            if (clk) break;
        }
        return true;
    }

    uint8_t get_in_assign(int ch) {
      return frame.MIDIState.get_in_assign(ch);
    }

    uint8_t get_in_channel(int ch) {
      return frame.MIDIState.get_in_channel(ch);
    }

    int8_t get_in_transpose(int ch) {
      return frame.MIDIState.get_in_transpose(ch);
    }

    bool in_in_range(int ch, int note) {
      return frame.MIDIState.in_in_range(ch, note);
    }

    void UpdateLog(bool midi_in, int ch, uint8_t message, uint8_t channel, int16_t data1, int16_t data2) {
        // Don't log SysEx unless the user is on the log display screen
        if (message == 5 && display == 0) return;

        char io = midi_in ? ('A' + ch) : ('1' + ch);
        log[log_index++] = {midi_in, io, message, channel, data1, data2};
        if (log_index == MIDI_LOG_MAX_SIZE) {
            for (int i = 0; i < MIDI_LOG_MAX_SIZE - 1; i++)
            {
                memcpy(&log[i], &log[i+1], sizeof(log[i + 1]));
            }
            log_index--;
        }
        log_view = log_index - 6;
        if (log_view < 0) log_view = 0;
    }

};

////////////////////////////////////////////////////////////////////////////////
//// App Functions
////////////////////////////////////////////////////////////////////////////////
FLASHMEM void AppCaptainMIDI::PackSetup(int s) {
    for (int i = 0; i < MIDIMAP_MAX; ++i)
        setups[s].inmaps[i] = frame.MIDIState.mapping[i].Pack();
    for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
        setups[s].outports[i] = frame.MIDIState.outports[i].Pack();
}

FLASHMEM void AppCaptainMIDI::UnpackSetup(int s) {
    for (int i = 0; i < MIDIMAP_MAX; ++i) {
        setups[s].inmaps[i] = SanitizeInMapPacked(setups[s].inmaps[i]);
        frame.MIDIState.mapping[i].Unpack(setups[s].inmaps[i]);
    }
    for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i) {
        HS::MIDIOutPort &o = frame.MIDIState.outports[i];
        o.Unpack(setups[s].outports[i]);
        // clamp per port class: garbage functions crash the label lookup
        const uint8_t fn_count = (i >= HS::MIDIFrame::kCVOutPorts)
            ? HS::TRFN_COUNT : HS::CVFN_COUNT;
        if (o.function >= fn_count) o.function = 0;
        if (o.velocity_source() > ADC_CHANNEL_COUNT)
            o.flags &= ~HS::MIDIOutSettings::VEL_SOURCE_MASK;
        o.ResetRuntime();
        setups[s].outports[i] = o.Pack(); // keep the blob canonical too
    }
    frame.MIDIState.UpdateMidiChannelFilter();
    frame.MIDIState.UpdateMaxPolyphony();
}

FLASHMEM uint32_t AppCaptainMIDI::LiveChecksum() const {
    uint32_t h = 2166136261u ^ active_setup;
    auto mix = [&h](uint64_t d) {
        h = (h ^ uint32_t(d)) * 16777619u;
        h = (h ^ uint32_t(d >> 32)) * 16777619u;
    };
    for (int i = 0; i < MIDIMAP_MAX; ++i)
        mix(frame.MIDIState.mapping[i].Pack());
    for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
        mix(frame.MIDIState.outports[i].Pack());
    for (int s = 0; s < kNumSetups; ++s) {
        for (int i = 0; i < MIDIMAP_MAX; ++i) mix(setups[s].inmaps[i]);
        for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i) mix(setups[s].outports[i]);
    }
    mix(PackMidiGlobals());
    return h;
}

static AppCaptainMIDI *g_captain_instance = nullptr;
FLASHMEM void CaptainDumpProfiles() {
#if defined(ARDUINO_TEENSY41)
    if (g_captain_instance) g_captain_instance->DumpProfiles();
#endif
}

// Keep the MIDI->CV engine running from another app (the Tuner): one
// implementation, so thru, filters and per-device bus masking cannot drift
// between the two callers. No-op when Captain has never been initialized.
FLASHMEM void CaptainPollMidi() {
    if (g_captain_instance) g_captain_instance->PollMidiSources();
}

// selftest hook: MIDI poll cadence + echo probe stats (gap max resets on read)
FLASHMEM void CaptainMidiHealth() {
    if (!g_captain_instance) {
        Serial.println("captain: not initialized");
        return;
    }
    Serial.printf("captain: poll_gap_max=%luus echoes=%lu dumps=%lu\n",
                  (unsigned long)g_captain_instance->poll_gap_max_us,
                  (unsigned long)g_captain_instance->echo_count,
                  (unsigned long)g_captain_instance->dump_count);
    g_captain_instance->poll_gap_max_us = 0;
}

void AppCaptainMIDI::Init() {
    g_captain_instance = this;
#if defined(ARDUINO_TEENSY41)
    LoadProfiles();
    // First device evaluation waits out the boot recall: a device already
    // plugged at power-up counts as "arriving" AFTER the preset restore,
    // so its bound setup wins the boot (the 225e-ish expectation), instead
    // of being overwritten by the recall finishing a moment later.
    next_dev_poll_ms = millis() + 3000;
#endif
    BaseStart();
}

FLASHMEM size_t AppCaptainMIDI::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
#ifdef __IMXRT1062__
  return 0; // T4.x persists via PhzConfig (see StoreData)
#else
  // NOTE: a save captures the live state only if Suspend()/PackSetup ran;
  // the framework saves from the settings menu, which suspends the app first.
  stream_buffer.Write(uint8_t(1)); // schema version
  stream_buffer.Write(uint8_t(active_setup));
  stream_buffer.Write(uint16_t(0)); // reserved
  for (int s = 0; s < kNumSetups; ++s) {
    for (int i = 0; i < MIDIMAP_MAX; ++i)
      stream_buffer.Write(setups[s].inmaps[i]);
    for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
      stream_buffer.Write(setups[s].outports[i]);
  }
  return stream_buffer.written();
#endif
}
FLASHMEM size_t AppCaptainMIDI::RestoreAppData(util::StreamBufferReader &stream_buffer) {
#ifdef __IMXRT1062__
  return 0;
#else
  const uint8_t version = stream_buffer.Read<uint8_t>();
  if (version != 1) return stream_buffer.read();
  // NB: constrain() is a macro that re-evaluates its argument — never
  // feed it a stream read directly (it desyncs the whole stream)
  const uint8_t stored_setup = stream_buffer.Read<uint8_t>();
  active_setup = constrain(stored_setup, 0, kNumSetups - 1);
  stream_buffer.Read<uint16_t>(); // reserved
  for (int s = 0; s < kNumSetups; ++s) {
    for (int i = 0; i < MIDIMAP_MAX; ++i)
      setups[s].inmaps[i] = stream_buffer.Read<uint64_t>();
    for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
      setups[s].outports[i] = stream_buffer.Read<uint64_t>();
  }
  UnpackSetup(active_setup);
  return stream_buffer.read();
#endif
}

void AppCaptainMIDI::Process(OC::IOFrame *ioframe) {
  BaseController(ioframe);
}

void AppCaptainMIDI::GetIOConfig(OC::IOConfig &ioconfig) const
{
  using namespace OC;
  ioconfig.digital_inputs[DIGITAL_INPUT_1].set("Gate1");
  ioconfig.digital_inputs[DIGITAL_INPUT_2].set("Gate2");
  ioconfig.digital_inputs[DIGITAL_INPUT_3].set("Gate3");
  ioconfig.digital_inputs[DIGITAL_INPUT_4].set("Gate4");

  // Bounded by the board: the arrays are ADC_/DAC_CHANNEL_COUNT long, and
  // eight literal sets on a four-channel build (T40, hOC) wrote past
  // IOConfig into whatever the caller had next to it on the stack.
  static const char *const cv_names[] = {
    "Ch1 CV", "Ch2 CV", "Ch3 CV", "Ch4 CV", "Ch5 CV", "Ch6 CV", "Ch7 CV", "Ch8 CV",
  };
  static const char *const out_names[] = {
    "Ch1", "Ch2", "Ch3", "Ch4", "Ch5", "Ch6", "Ch7", "Ch8",
  };
  static_assert(ADC_CHANNEL_COUNT <= 8 && DAC_CHANNEL_COUNT <= 8, "extend the name tables");
  for (int i = 0; i < ADC_CHANNEL_COUNT; ++i) ioconfig.cv[i].set(cv_names[i]);
  for (int i = 0; i < DAC_CHANNEL_COUNT; ++i) ioconfig.outputs[i].set(out_names[i], OUTPUT_MODE_PITCH);
}

FLASHMEM
void AppCaptainMIDI::HandleAppEvent(OC::AppEvent event) {
  if (event == OC::APP_EVENT_SUSPEND) {
    Suspend();
  }
  if (event == OC::APP_EVENT_RESUME) {
    Resume();
  }
  if (event == OC::APP_EVENT_FLUSH) {
    PackSetup(active_setup);
#ifdef __IMXRT1062__
    StoreData();  // preset-bus capture: persist CAPTAIN.DAT
#endif
  }
}

FLASHMEM void AppCaptainMIDI::Loop() { DoLoop(); } // deferred (non-ISR) work

FLASHMEM
void AppCaptainMIDI::DrawMenu() const { BaseView(); }

FLASHMEM
void AppCaptainMIDI::View() const { MainView(); }

#ifdef __IMXRT1062__
FLASHMEM void AppCaptainMIDI::StoreData() {
    PhzConfig::load_config("CAPTAIN.DAT");  // own the map before writing
    PhzConfig::setValue(SETUP_KEY, active_setup);
    for (int s = 0; s < kNumSetups; ++s) {
      for (int i = 0; i < MIDIMAP_MAX; ++i)
        PhzConfig::setValue(INPUT_MAP_KEY + i + s*MIDIMAP_MAX, setups[s].inmaps[i]);
      for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
        PhzConfig::setValue(OUTPUT_MAP_KEY + i + s*HS::MIDIFrame::kOutPorts, setups[s].outports[i]);
    }
    PhzConfig::setValue(MIDI_GLOBALS_KEY, kMidiGlobalsMagic | PackMidiGlobals());
    PhzConfig::save_config("CAPTAIN.DAT");
}
#endif  // __IMXRT1062__

FLASHMEM void AppCaptainMIDI::SendDump(uint8_t s) {
    ++dump_count;
    if (s == active_setup) PackSetup(s); // canonicalize live edits
    constexpr int kRecBytes =
        (2 + kGlobalParamCount) + MIDIMAP_MAX * (2 + kInParamCount)
        + HS::MIDIFrame::kOutPorts * (2 + kOutParamCount);
    uint8_t rec[kRecBytes];
    int rn = 0;

    rec[rn++] = CLS_GLOBAL; rec[rn++] = 0;
    for (uint8_t par = 0; par < kGlobalParamCount; ++par) {
        uint8_t v = 0;
        get_param(CLS_GLOBAL, 0, par, v);
        rec[rn++] = v;
    }
    for (int i = 0; i < MIDIMAP_MAX; ++i) {
        rec[rn++] = CLS_IN; rec[rn++] = i;
        InMapParams(setups[s].inmaps[i], &rec[rn]);
        rn += kInParamCount;
    }
    for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i) {
        const bool trig = i >= HS::MIDIFrame::kCVOutPorts;
        rec[rn++] = trig ? CLS_TR : CLS_CV;
        rec[rn++] = trig ? (i - HS::MIDIFrame::kCVOutPorts) : i;
        OutPortParams(setups[s].outports[i], &rec[rn]);
        rn += kOutParamCount;
    }

    // chunk whole records into <=49-byte packet payloads
    uint8_t xorsum = 0;
    for (int i = 0; i < rn; ++i) xorsum ^= rec[i];
    // pre-count packets
    uint8_t total = 0;
    for (int i = 0; i < rn; ) {
        int take = 0;
        while (i + take < rn) {
            const uint8_t cls = rec[i + take];
            const int rlen = 2 + ((cls == CLS_GLOBAL) ? kGlobalParamCount
                           : (cls == CLS_IN) ? kInParamCount : kOutParamCount);
            if (take + rlen > 49) break;
            take += rlen;
        }
        i += take;
        ++total;
    }
    uint8_t seq = 0;
    for (int i = 0; i < rn; ) {
        uint8_t pkt[52];
        int n = 0;
        pkt[n++] = s; pkt[n++] = seq; pkt[n++] = total;
        while (i < rn) {
            const uint8_t cls = rec[i];
            const int rlen = 2 + ((cls == CLS_GLOBAL) ? kGlobalParamCount
                           : (cls == CLS_IN) ? kInParamCount : kOutParamCount);
            if (n - 3 + rlen > 49) break;
            memcpy(&pkt[n], &rec[i], rlen);
            n += rlen;
            i += rlen;
        }
        SendSyxFrame(SYX_DUMP_DATA, pkt, n);
        ++seq;
    }
    const uint8_t endpkt[3] = {s, seq, uint8_t(xorsum & 0x7f)};
    SendSyxFrame(SYX_DUMP_END, endpkt, 3);
}

FLASHMEM void AppCaptainMIDI::midi_thru(const HS::MIDIMessage &msg, uint8_t exclude) {
    exclude |= HS::midi_thru_disable;
    if (~exclude & mMaskUSBDev)
        usbMIDI.send(msg.message, msg.data1, msg.data2, msg.channel, 0);
#ifdef ARDUINO_TEENSY41
    if (~exclude & mMaskUSBHost)
        usbHostMIDI[0].send(msg.message, msg.data1, msg.data2, msg.channel);
    if (~exclude & mMaskUSBHost2)
        usbHostMIDI[1].send(msg.message, msg.data1, msg.data2, msg.channel);
    if (~exclude & mMaskSerial)
        MIDI1.send((midi::MidiType)msg.message, msg.data1, msg.data2, msg.channel);
#endif
    if (~exclude & mMaskBus)
        OC::PresetBus::QueueMidiTx(msg.message, msg.channel, msg.data1, msg.data2);
}

FLASHMEM void AppCaptainMIDI::PollMidiSources() {
    {   // worst-case poll cadence, for the selftest latency budget.
        // Gaps over 1s are app-suspension time (menu, other app), not
        // poll latency - skip them so the metric stays meaningful.
        const uint32_t now_us = micros();
        const uint32_t gap = now_us - poll_last_us;
        if (poll_last_us && gap < 1000000 && gap > poll_gap_max_us)
            poll_gap_max_us = gap;
        poll_last_us = now_us;
    }

    int budget = 8;
    while (budget-- > 0 && poll_midi(usbMIDI, mMaskUSBDev)) {}
#ifdef ARDUINO_TEENSY41
    budget = 8;
    while (budget-- > 0 && poll_midi(usbHostMIDI[0], mMaskUSBHost)) {}
    budget = 8;
    while (budget-- > 0 && poll_midi(usbHostMIDI[1], mMaskUSBHost2)) {}
    budget = 8;
    while (budget-- > 0 && poll_midi(MIDI1, mMaskSerial)) {}
    budget = 8;
    while (budget-- > 0 && poll_bus_midi()) {}
#endif
}

#if defined(ARDUINO_TEENSY41)
FLASHMEM void AppCaptainMIDI::PersistProfiles() {
    profiles_dirty = false;
    PhzConfig::load_config();
    for (int i = 0; i < kDeviceProfiles; ++i) {
        uint64_t v = 0;
        if (profiles[i].vid)
            v = kProfileMagic | (uint64_t(profiles[i].vid) << 32) |
                (uint64_t(profiles[i].pid) << 16) | profiles[i].setup |
                (profiles[i].no_bus_thru ? (uint64_t(1) << 8) : 0);
        PhzConfig::setValue(DEVICE_PROFILE_KEY + i, v);
        uint64_t nm = 0;
        memcpy(&nm, profiles[i].name, 8);
        PhzConfig::setValue(DEVICE_PROFILE_KEY + 16 + i, nm);
    }
    PhzConfig::save_config();
    PhzConfig::load_config("CAPTAIN.DAT");
}
#endif  // ARDUINO_TEENSY41

FLASHMEM void AppCaptainMIDI::RouterEditFocused(int dir) {
    const int row = router_cursor.cursor_pos();
    if (row == ROUTER_SYNC) {
        int v = ((int)HS::clock_sync_jack + 1 + dir) % 5;
        if (v < 0) v += 5;
        HS::clock_sync_jack = (int8_t)(v - 1);
        return;
    }
    static const uint8_t kBit[5] = {
        HS::mMaskSerial, HS::mMaskUSBDev, HS::mMaskUSBHost,
        HS::mMaskUSBHost2, HS::mMaskBus
    };
    const bool tx = row >= ROUTER_TX_SERIAL;
    const int idx = row - (tx ? (int)ROUTER_TX_SERIAL : (int)ROUTER_RX_SERIAL);
    uint8_t &mask = tx ? HS::midi_clktx_disable : HS::midi_clkrx_disable;
    if (dir > 0) mask &= ~kBit[idx];       // right = On (bit clear)
    else if (dir < 0) mask |= kBit[idx];   // left  = Off (bit set)
}

FLASHMEM const char* AppCaptainMIDI::RouterRowLabel(int row) const {
    static const char* const kLabels[ROUTER_ROW_COUNT] = {
        "Sync Src",
        "Rx  Serial", "Rx  USBDev", "Rx  Host1", "Rx  Host2", "Rx  Bus",
        "Tx  Serial", "Tx  USBDev", "Tx  Host1", "Tx  Host2", "Tx  Bus",
    };
    return kLabels[row];
}

FLASHMEM const char* AppCaptainMIDI::RouterRowValue(int row) const {
    if (row == ROUTER_SYNC) {
        static const char* const kSync[5] = { "Off", "TR1", "TR2", "TR3", "TR4" };
        return kSync[HS::clock_sync_jack + 1];
    }
    static const uint8_t kBit[5] = {
        HS::mMaskSerial, HS::mMaskUSBDev, HS::mMaskUSBHost,
        HS::mMaskUSBHost2, HS::mMaskBus
    };
    const bool tx = row >= ROUTER_TX_SERIAL;
    const int idx = row - (tx ? (int)ROUTER_TX_SERIAL : (int)ROUTER_RX_SERIAL);
    const uint8_t mask = tx ? HS::midi_clktx_disable : HS::midi_clkrx_disable;
    return captain_off_on[!(mask & kBit[idx])];
}

// Sync Src / Rx.. / Tx.. rows, reusing the app's own SettingsList idiom
// (DrawOverview/DrawDetail) so focus/scroll/wording stay consistent with
// every other screen -- inversion still means exactly one thing.
FLASHMEM void AppCaptainMIDI::DrawClockRouter() const {
    gfxHeader("Clock Router");
    // whether MIDI clock bytes are CURRENTLY arriving on any interface --
    // MIDI clock always wins over TR sync on a shared tick (PumpTransport),
    // so this is what explains a TR-sync row that looks selected but idle
    const bool midi_live = millis() - frame.MIDIState.last_midi_clock_ms < 300;
    graphics.drawCircle(94, 4, 2);
    if (midi_live) graphics.drawRect(93, 3, 3, 3);
    graphics.setPrintPos(99, 1);
    graphics.print(midi_live ? "MIDI" : "midi");

    OC::menu::SettingsList<OC::menu::kScreenLines, 0, 76> settings_list(router_cursor);
    OC::menu::SettingsListItem list_item;
    while (settings_list.available()) {
        const int current = settings_list.Next(list_item);
        if (current >= ROUTER_ROW_COUNT) { list_item.DrawCustom(); continue; }

        list_item.SetPrintPos();
        graphics.print(RouterRowLabel(current));
        graphics.setPrintPos(0, list_item.y + 2);
        list_item.DrawDefault(RouterRowValue(current), 0, CaptainSettings[0]);
    }
}

FLASHMEM void AppCaptainMIDI::DrawScreensaver() const {
    BaseScreensaver(true);
}
void AppCaptainMIDI::DrawDebugInfo() const { }

FLASHMEM void AppCaptainMIDI::HandleButtonEvent(const UI::Event &event) {
    last_mask = mask;
    mask = event.mask;

    // dual-press A+B opens the Clock Router -- see the comment above
    // ToggleClockRouter() for why this gesture, not another one.
    if (mask == (OC::CONTROL_BUTTON_A | OC::CONTROL_BUTTON_B) && mask != last_mask) {
        if (!clock_router) ToggleClockRouter();
        return;
    }
    if (clock_router) {
        // L closes it; every other button is reserved while it's open
        if (event.control == OC::CONTROL_BUTTON_L && event.type == UI::EVENT_BUTTON_PRESS)
            ToggleClockRouter();
        return;
    }

    if (event.control == OC::CONTROL_BUTTON_R && event.type == UI::EVENT_BUTTON_PRESS)
        ToggleCursor();
    if (event.control == OC::CONTROL_BUTTON_L) {
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) Panic();
        if (event.type == UI::EVENT_BUTTON_PRESS) ToggleDisplay();
    }

    if (event.control == OC::CONTROL_BUTTON_A && event.type == UI::EVENT_BUTTON_PRESS)
        SwitchSetup(1); // hOC: A is the + button
    if (event.control == OC::CONTROL_BUTTON_B) {
        if (event.type == UI::EVENT_BUTTON_PRESS) SwitchSetup(-1);
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) ToggleCopyMode();
    }
}

FLASHMEM void AppCaptainMIDI::HandleEncoderEvent(const UI::Event &event) {
    if (clock_router) {
        if (event.control == OC::CONTROL_ENCODER_L) router_cursor.Scroll(event.value);
        if (event.control == OC::CONTROL_ENCODER_R) RouterEditFocused(event.value);
        return;
    }

    if (event.control == OC::CONTROL_ENCODER_R) {
        if (detail_port < 0) {
            cursor.Scroll(event.value); // overview: pick a port
        } else if (param_cursor.editing()) {
            EditDetailParam(detail_port, param_cursor.cursor_pos(), event.value);
        } else {
            param_cursor.Scroll(event.value); // detail: pick a parameter
        }
    }
    if (event.control == OC::CONTROL_ENCODER_L) {
        LeftEncoderTurn(event.value);
    }
}
