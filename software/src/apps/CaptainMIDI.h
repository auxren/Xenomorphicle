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

// per channel
const settings::ValueAttributes CaptainSettings[] = {
  // Assigned function
  // MIDI-to-CV
  { 0, 0, HEM_MIDI_MAX_FUNCTION, "", midi_fn_name, settings::STORAGE_TYPE_U8 },
  // CV-to-MIDI
  { 0, 0, HS::CVFN_COUNT-1, "", cv_out_fn_names, settings::STORAGE_TYPE_U8 },
  // Trigger-to-MIDI
  { 0, 0, HS::TRFN_COUNT-1, "", trig_out_fn_names, settings::STORAGE_TYPE_U8 },

  // Channel
  { 0, 0, 16, "", midi_channels, settings::STORAGE_TYPE_U8 },
  // Transpose
  { 0, -48, 48, "", NULL, settings::STORAGE_TYPE_I8 },
  // Range Low
  { 0, 0, 127, "", midi_note_numbers, settings::STORAGE_TYPE_U8 },
  // Range High
  { 0, 0, 127, "", midi_note_numbers, settings::STORAGE_TYPE_U8 },
};

enum CaptainsKeys : uint16_t {
  SETUP_KEY = 0,

  // upper 7 bits of mapping key
  INPUT_MAP_KEY = 1 << 9,
  OUTPUT_MAP_KEY = 2 << 9,
};

const char* const midi_messages[7] = {
    "Note", "Off", "CC#", "Aft", "Bend", "SysEx", "Diag"
};

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

    OC::menu::ScreenCursor<OC::menu::kScreenLines> cursor;

    void Start() {
        screen = 0;
        display = 0;
        cursor.Init(0, DAC_CHANNEL_COUNT + HS::MIDIFrame::kOutPorts - 1);
        log_index = 0;
        log_view = 0;
        active_setup = 0;
        // seed all setups from the freshly-initialized live state
        for (int s = 0; s < kNumSetups; ++s) PackSetup(s);
        Reset();
    }

    // pack the live mapping/outports state into setups[s]
    void PackSetup(int s) {
        for (int i = 0; i < MIDIMAP_MAX; ++i)
            setups[s].inmaps[i] = frame.MIDIState.mapping[i].Pack();
        for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
            setups[s].outports[i] = frame.MIDIState.outports[i].Pack();
    }
    // apply setups[s] to the live state
    void UnpackSetup(int s) {
        for (int i = 0; i < MIDIMAP_MAX; ++i)
            frame.MIDIState.mapping[i].Unpack(setups[s].inmaps[i]);
        for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i) {
            frame.MIDIState.outports[i].Unpack(setups[s].outports[i]);
            frame.MIDIState.outports[i].ResetRuntime();
        }
        frame.MIDIState.UpdateMidiChannelFilter();
        frame.MIDIState.UpdateMaxPolyphony();
    }

    void Suspend() {
        PackSetup(active_setup);
#ifdef __IMXRT1062__
        StoreData();
#endif
        OnSendSysEx();
    }

#ifdef __IMXRT1062__
    void StoreData() {
        PhzConfig::setValue(SETUP_KEY, active_setup);
        for (int s = 0; s < kNumSetups; ++s) {
          for (int i = 0; i < MIDIMAP_MAX; ++i)
            PhzConfig::setValue(INPUT_MAP_KEY + i + s*MIDIMAP_MAX, setups[s].inmaps[i]);
          for (int i = 0; i < HS::MIDIFrame::kOutPorts; ++i)
            PhzConfig::setValue(OUTPUT_MAP_KEY + i + s*HS::MIDIFrame::kOutPorts, setups[s].outports[i]);
        }
        PhzConfig::save_config("CAPTAIN.DAT");
    }
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
        UnpackSetup(active_setup);
        screen = 0;
    }
#else
    // T3.x: setups[] persist via SaveAppData/RestoreAppData (EEPROM pool)
    void Resume() {
        UnpackSetup(active_setup);
        screen = 0;
    }
#endif

    void Controller() {
        // Drain incoming MIDI traffic; cap per-tick work to bound ISR time
        int budget = 4;
        while (budget-- > 0 && poll_midi(usbMIDI)) {}
#ifdef ARDUINO_TEENSY41
        budget = 4;
        while (budget-- > 0 && poll_midi(usbHostMIDI[0])) {}
        budget = 4;
        while (budget-- > 0 && poll_midi(usbHostMIDI[1])) {}
        budget = 4;
        while (budget-- > 0 && poll_midi(MIDI1)) {}
#endif

        // Convert CV/trigger inputs to outgoing MIDI messages
        frame.MIDIState.ProcessOutputs(frame);

        // set CV outputs from MIDI mappings
        for (int ch = 0; ch < DAC_CHANNEL_COUNT; ch++)
        {
            // Handle clock timing
            if (indicator_in[ch] > 0) --indicator_in[ch];

            MIDIMapping &map = frame.MIDIState.mapping[ch];

            Out(ch, map.output);
        }
    }

    void View() const;
    void MainView() const {
        if (copy_mode) DrawCopyScreen();
        else if (display == 0) DrawSetupScreens();
        else DrawLogScreen();
    }

    void EncoderEdit(int dir) {
      int pos = cursor.cursor_pos();
      bool input = pos < DAC_CHANNEL_COUNT;

      if (input) {
        MIDIMapping &m = frame.MIDIState.mapping[pos];
        switch (screen) {
          case 0: m.AdjustFunction(dir); break;
          case 1: m.AdjustChannel(dir); break;
          case 2: m.AdjustTranspose(dir); break;
          case 3: m.AdjustRangeLow(dir); break;
          case 4: m.AdjustRangeHigh(dir); break;
          default: break;
        }
        return;
      }

      const int p = pos - DAC_CHANNEL_COUNT;
      HS::MIDIOutPort &o = frame.MIDIState.outports[p];
      const bool is_trig = p >= HS::MIDIFrame::kCVOutPorts;
      const int fn_count = is_trig ? HS::TRFN_COUNT : HS::CVFN_COUNT;
      switch (screen) {
        case 0:
          o.function = constrain(o.function + dir, 0, fn_count - 1);
          o.ResetRuntime();
          break;
        case 1: o.channel = constrain(o.channel + dir, 0, 15); break;
        case 2: o.transpose = constrain(o.transpose + dir, -48, 48); break;
        case 3: o.range_low = constrain(o.range_low + dir, 0, o.range_high); break;
        case 4: o.range_high = constrain(o.range_high + dir, o.range_low, 127); break;
        default: break;
      }
    }
    void MoveCursor(int dir) {
        cursor.Scroll(dir);
    }

    void SelectSetup(int setup_number, int new_screen = -1) {
        // moving to another setup?
        if (setup_number != get_setup_number()) {
          PackSetup(active_setup); // store current settings
          active_setup = setup_number;
          UnpackSetup(active_setup);
          Reset();
        }

        // Screen switching, default to same
        if (new_screen == -1) new_screen = screen;

        screen = new_screen;
    }

    void SwitchScreenOrLogView(int dir) {
        if (display == 0) {
            // Switch screen
            int new_screen = constrain(screen + dir, 0, 4);
            SelectSetup(get_setup_number(), new_screen);
        } else {
            // Scroll Log view
            if (log_index > 6) log_view = constrain(log_view + dir, 0, log_index - 6);
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
        else display = 1 - display;
    }

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

    // runs in the main loop (not the ISR): drains deferred work
    void DoLoop() {
        auto &hMIDI = frame.MIDIState;
        if (hMIDI.panic_request) {
            hMIDI.panic_request = false;
            hMIDI.PanicOutputs(); // targeted note-offs for engine notes
            for (int ch = 0; ch < 16; ++ch) {
                hMIDI.SendCC(ch, 120, 0); // All Sound Off
                hMIDI.SendCC(ch, 123, 0); // All Notes Off
            }
        }
    }

    /* When the app is suspended, it sends out a system exclusive dump, generated here */
    void OnSendSysEx() {
        // Teensy will receive 60-byte sysex files, so there's room for one and only one
        // Setup. The currently-selected Setup will be the one we're sending. That's 40
        // bytes.
        uint8_t V[MIDI_PARAMETER_COUNT];
        for (int i = 0; i < MIDI_PARAMETER_COUNT; i++)
        {
          // TODO
            int p = 0;
            // uint8_t offset = MIDI_PARAMETER_COUNT * get_setup_number();
            // int p = values_[i + offset];
            if (i > 15 && i < 24) p += 24; // These are signed, so they need to be converted
            V[i] = static_cast<uint8_t>(p);
        }

        // Pack the data and send it out
        UnpackedData unpacked;
        unpacked.set_data(40, V);
        PackedData packed = unpacked.pack();
        SendSysEx(packed, 'M');
    }

    void OnReceiveSysEx() {
        // Since only one Setup is coming, use the currently-selected setup to determine
        // where to stash it.
        uint8_t V[MIDI_PARAMETER_COUNT];
        if (ExtractSysExData(V, 'M')) {
            for (int i = 0; i < MIDI_PARAMETER_COUNT; i++)
            {
                int p = (int)V[i];
                if (i > 15 && i < 24) p -= 24; // Restore the sign removed in OnSendSysEx()
                // TODO
                // uint8_t offset = MIDI_PARAMETER_COUNT * get_setup_number();
                // apply_value(i + offset, p);
            }
            UpdateLog(1, 0, 5, 0, 'M', 0);
        } else {
            char app_code = LastSysExApplicationCode();
            UpdateLog(1, 0, 5, 0, app_code, 0);
        }
        Resume();
    }

   void ToggleCopyMode() {
       copy_mode = 1 - copy_mode;
       copy_setup_source = get_setup_number();
       copy_setup_target = copy_setup_source + 1;
       if (copy_setup_target > 3) copy_setup_target = 0;
   }

   void ToggleCursor() {
       if (copy_mode) CopySetup(copy_setup_target, copy_setup_source);
       else cursor.toggle_editing();
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
    int screen; // 0=Assign, 1=Channel, 2=Transpose, 3=Range Low, 4=Range High
    bool display; // 0=Setup Edit 1=Log
    bool copy_mode; // Copy mode on/off
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

    void DrawSetupScreens() {
        // Create the header, showing the current Setup and Screen name
        gfxHeader("");
        switch (screen) {
          case 0: graphics.print("MIDI Assign"); break;
          case 1: graphics.print("MIDI Channel"); break;
          case 2: graphics.print("Transpose"); break;
          case 3: graphics.print("Range Low"); break;
          case 4: graphics.print("Range High"); break;
          default: break;
        }
        gfxPrint(128 - 42, 1, "Setup ");
        gfxPrint(get_setup_number() + 1);

        // Iterate through the current range of settings
        OC::menu::SettingsList<OC::menu::kScreenLines, 0, OC::menu::kDefaultValueX - 1> settings_list(cursor);
        OC::menu::SettingsListItem list_item;
        while (settings_list.available())
        {
            bool suppress = 0; // Don't show the setting if it's not relevant
            const int current = settings_list.Next(list_item);
            int p = current % (DAC_CHANNEL_COUNT + HS::MIDIFrame::kOutPorts);
            const bool is_input = p < DAC_CHANNEL_COUNT;

            // MIDI In and Out indicators for all screens
            if (is_input) { // It's a MIDI In assignment
                int in_fn = get_in_assign(p);
                if (in_fn == MIDI_IN_OFF && screen > 0) suppress = 1;

                if (indicator_in[p] > 0 || note_in[p] > -1) {
                    if (in_fn == MIDI_IN_NOTE) {
                        if (note_in[p] > -1) {
                            graphics.setPrintPos(70, list_item.y + 2);
                            graphics.print(midi_note_numbers[note_in[p]]);
                        }
                    } else graphics.drawBitmap8(70, list_item.y + 2, 8, MIDI_ICON);
                }

                // Indicate if the assignment is a note type
                if (in_fn == MIDI_IN_NOTE)
                    graphics.drawBitmap8(56, list_item.y + 1, 8, MIDI_note_icon);
                else if (screen > 1) suppress = 1;

                // Indicate if the assignment is a clock
                if (in_fn >= MIDI_IN_CLOCK_4TH) {
                    uint8_t o_x = (frame.MIDIState.clock_count < 12) ? 2 : 0;
                    graphics.drawBitmap8(80 + o_x, list_item.y + 1, 8, MIDI_clock_icon);
                    if (screen > 0) suppress = 1;
                }

            } else { // It's a MIDI Out assignment (CV or trigger port)
                p -= DAC_CHANNEL_COUNT;
                const HS::MIDIOutPort &port = frame.MIDIState.outports[p];
                const bool is_trig = p >= HS::MIDIFrame::kCVOutPorts;
                const bool note_fn = is_trig
                    ? (port.function >= HS::TRFN_NOTE && port.function <= HS::TRFN_NOTE_LATCH)
                    : (port.function == HS::CVFN_PITCH || port.function == HS::CVFN_PITCH_FREE
                       || port.function == HS::CVFN_GATE_NOTE);

                if (!port.function && screen > 0) suppress = 1;

                if (port.indicator > 0 || port.gated) {
                    if (note_fn && port.gated) {
                        graphics.setPrintPos(70, list_item.y + 2);
                        graphics.print(midi_note_numbers[port.last_note]);
                    } else graphics.drawBitmap8(70, list_item.y + 2, 8, MIDI_ICON);
                }

                // Indicate if the assignment is a note type
                if (note_fn)
                    graphics.drawBitmap8(56, list_item.y + 1, 8, MIDI_note_icon);
                else if (screen > 1) suppress = 1;
            }

            // Draw the item last so that if it's selected, the icons are reversed, too
            if (!suppress) {
              int idx;
              if (screen == 0) {
                if (is_input) idx = 0;
                else idx = (p >= HS::MIDIFrame::kCVOutPorts) ? 2 : 1;
              } else {
                idx = screen + 2;
              }
              list_item.SetPrintPos();
              graphics.print(RowLabel(current));
              list_item.DrawDefault(GetLabel(current), GetValue(current), CaptainSettings[idx]);
            } else {
                list_item.SetPrintPos();
                graphics.print("                   --");
                list_item.DrawCustom();
            }
        }
    }

    const char* RowLabel(int pos) const {
        if (pos < DAC_CHANNEL_COUNT) return midi2cv_label[pos];
        const int p = pos - DAC_CHANNEL_COUNT;
        if (p < HS::MIDIFrame::kCVOutPorts) return cv2midi_label[p];
        return trig2midi_label[p - HS::MIDIFrame::kCVOutPorts];
    }

    // The value of the parameter at the given cursor position
    int GetValue(int pos) const {
      if (pos < DAC_CHANNEL_COUNT) {
        MIDIMapping &m = frame.MIDIState.mapping[pos];
        switch (screen) {
          case 0: return (m.get_type() | m.get_subtype()); // idk man
          case 1: return m.get_channel();
          case 2: return m.get_transpose();
          case 3: return m.get_low();
          case 4: return m.get_high();
          default: return 0;
        }
      }
      const HS::MIDIOutPort &o = frame.MIDIState.outports[pos - DAC_CHANNEL_COUNT];
      switch (screen) {
        case 0: return o.function;
        case 1: return o.channel;
        case 2: return o.transpose;
        case 3: return o.range_low;
        case 4: return o.range_high;
        default: return 0;
      }
    }
    const char* const GetLabel(int pos) const {
      if (pos < DAC_CHANNEL_COUNT)
        return frame.MIDIState.mapping[pos].get_label();
      const int p = pos - DAC_CHANNEL_COUNT;
      const HS::MIDIOutPort &o = frame.MIDIState.outports[p];
      if (p >= HS::MIDIFrame::kCVOutPorts) return trig_out_fn_names[o.function];
      return cv_out_fn_names[o.function];
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
    bool poll_midi(T1 &device) {
        if (!device.read()) return false;

        uint8_t message = device.getType();
        uint8_t channel = device.getChannel();
        uint8_t data1 = device.getData1();
        uint8_t data2 = device.getData2();

        // Handle system exclusive dump for Setup data
        if (message == HEM_MIDI_SYSEX) OnReceiveSysEx();

        HS::frame.MIDIState.ProcessMIDIMsg({channel, message, data1, data2});
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
void AppCaptainMIDI::Init() { BaseStart(); }

size_t AppCaptainMIDI::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
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
size_t AppCaptainMIDI::RestoreAppData(util::StreamBufferReader &stream_buffer) {
#ifdef __IMXRT1062__
  return 0;
#else
  const uint8_t version = stream_buffer.Read<uint8_t>();
  if (version != 1) return stream_buffer.read();
  active_setup = constrain(stream_buffer.Read<uint8_t>(), 0, kNumSetups - 1);
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

  ioconfig.cv[0].set("Ch1 CV");
  ioconfig.cv[1].set("Ch2 CV");
  ioconfig.cv[2].set("Ch3 CV");
  ioconfig.cv[3].set("Ch4 CV");
  ioconfig.cv[4].set("Ch5 CV");
  ioconfig.cv[5].set("Ch6 CV");
  ioconfig.cv[6].set("Ch7 CV");
  ioconfig.cv[7].set("Ch8 CV");

  ioconfig.outputs[0].set("Ch1", OUTPUT_MODE_PITCH);
  ioconfig.outputs[1].set("Ch2", OUTPUT_MODE_PITCH);
  ioconfig.outputs[2].set("Ch3", OUTPUT_MODE_PITCH);
  ioconfig.outputs[3].set("Ch4", OUTPUT_MODE_PITCH);
  ioconfig.outputs[4].set("Ch5", OUTPUT_MODE_PITCH);
  ioconfig.outputs[5].set("Ch6", OUTPUT_MODE_PITCH);
  ioconfig.outputs[6].set("Ch7", OUTPUT_MODE_PITCH);
  ioconfig.outputs[7].set("Ch8", OUTPUT_MODE_PITCH);
}

FLASHMEM
void AppCaptainMIDI::HandleAppEvent(OC::AppEvent event) {
  if (event == OC::APP_EVENT_SUSPEND) {
    Suspend();
  }
  if (event == OC::APP_EVENT_RESUME) {
    Resume();
  }
}

void AppCaptainMIDI::Loop() { DoLoop(); } // deferred (non-ISR) work

FLASHMEM
void AppCaptainMIDI::DrawMenu() const { BaseView(); }

FLASHMEM
void AppCaptainMIDI::View() const { MainView(); }

void AppCaptainMIDI::DrawScreensaver() const {
    BaseScreensaver(true);
}
void AppCaptainMIDI::DrawDebugInfo() const { }

void AppCaptainMIDI::HandleButtonEvent(const UI::Event &event) {
    if (event.control == OC::CONTROL_BUTTON_R && event.type == UI::EVENT_BUTTON_PRESS)
        ToggleCursor();
    if (event.control == OC::CONTROL_BUTTON_L) {
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) Panic();
        if (event.type == UI::EVENT_BUTTON_PRESS) ToggleDisplay();
    }

    if (event.control == OC::CONTROL_BUTTON_A && event.type == UI::EVENT_BUTTON_PRESS)
        SwitchSetup(-1);
    if (event.control == OC::CONTROL_BUTTON_B) {
        if (event.type == UI::EVENT_BUTTON_PRESS) SwitchSetup(1);
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) ToggleCopyMode();
    }
}

void AppCaptainMIDI::HandleEncoderEvent(const UI::Event &event) {
    if (event.control == OC::CONTROL_ENCODER_R) {
        if (cursor.editing()) {
            EncoderEdit(event.value);
        } else {
            MoveCursor(event.value);
        }
    }
    if (event.control == OC::CONTROL_ENCODER_L) {
        SwitchScreenOrLogView(event.value);
    }
}
