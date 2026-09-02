// Copyright (c) 2018, Jason Justian
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

#pragma once

#ifdef PEWPEWPEW
#include "../util/pewpewsplash.h"
#endif
#include "../PresetBus.h"
#include "../PhzConfig.h"

extern "C" void _reboot_Teensyduino_();
using namespace OC;

OC_APP_CLASS(AppSettings, TWOCCS("SE"), "Setup/About", "Settings"),
  public HSApplication {
public:
  OC_APP_INTERFACE_DECLARE(AppSettings, 0);

  // The two irreversible things this screen can do. Neither happens on the
  // press that asks for it: the press arms one of these, a screen of its own
  // states the consequence, and only then does a DIFFERENT button commit.
  //
  // Factory reset used to be a bare encR press -- the most-practised press in
  // the instrument (it picks an app in the switcher, enters a module in the
  // 200e app, confirms a write) applied on the app whose name promises it is
  // where you go to READ about the module. AppSwitcher::Init(true) then ran
  // InitDefaults() on every app, reset global_settings and zeroed the user
  // Turing machines BEFORE its own ConfirmReset() prompt was drawn, so every
  // app's live state was already gone by the time anything asked -- and that
  // prompt's "OK" is encR as well, which made two presses of one button a
  // full EEPROM erase. A two-step confirmation whose two steps are the same
  // button is a one-step confirmation.
  //
  // Reflash used to latch on `reflash = (event.value > 0)` from any encL
  // turn, which swapped the row to "[Reflash]" and re-bound the encL press
  // from calibrate to reboot-into-the-bootloader. [RESET] then vanished from
  // the screen while encR still reset, i.e. a stray nudge in a rack left the
  // factory reset unlabelled. It is a held gesture now (encL long-press), and
  // it latches nothing.
  enum PendingAction : uint8_t { PENDING_NONE, PENDING_RESET, PENDING_REFLASH };
  PendingAction pending_ = PENDING_NONE;
  uint32_t armed_ms_ = 0;    // millis() the confirm screen appeared
  bool b_down_solo_ = false; // B went down alone, after this screen appeared
  bool a_down_solo_ = false; // A went down alone, i.e. not as the A+B chord
  bool encl_held_ = false;   // encL is down: the footer offers the long-press

  // How long a confirm screen ignores its "yes" after appearing. Same value
  // and same reasoning as the 200e write confirm's kConfirmDeadMs: long
  // enough that no fumble or double-tap crosses it, short enough that a
  // deliberate press never feels refused.
  static constexpr uint32_t kConfirmDeadMs = 350;

  bool bus_addr_dirty = false;
  // Pending bus address, 0 = untouched. The module's address is its identity on
  // the bus: change it by accident and it stops answering where a preset
  // manager expects it, or collides with another module. It used to move on a
  // bare right-encoder turn -- the only thing that encoder did on this screen --
  // and each detent was applied to the live bus immediately, so dialling 3C->40
  // made this module briefly answer at 3D, 3E and 3F while a WPM was polling.
  // Now it takes X held down, and nothing reaches the bus until app exit.
  uint8_t bus_addr_edit = 0;
  bool invert_display_dirty = false;
  bool calibration_mode = false;
  bool calibration_complete = true;
  bool cal_save_q = false;
  int current_octave = 0; // for fine-tuning DAC points
  OC::DigitalInputDisplay digital_input_displays[4];
  OC::TickCount tick_count;

  OC::CalibrationState calstate = {
    OC::HELLO,
    &OC::calibration_steps[OC::HELLO],
    0, // "use defaults: no"
  };

  void Start() {
  }

  void Resume() {
    Disarm();
    if (OC::calibration_data.get_calstart()) {
      StartCalibration();
      OC::calibration_data.set_calstart(false);
    } else if (calibration_mode && !calibration_complete) {
      // restart calibration if you exit and come back
      StartCalibration();
    }
  }
  void Suspend() {
    Disarm();
    if (cal_save_q) {
      OC::calibration_save();
      cal_save_q = false;
    }
    if (bus_addr_dirty && bus_addr_edit) {
      // Apply the pending address ONCE, here, rather than per detent while the
      // user was still dialling -- the bus sees one change, not every value
      // swept through on the way. SetModuleAddress sets it live and writes it
      // to the globals map (load first: the shared PhzConfig map may hold
      // another app's file).
      PhzConfig::load_config();
      OC::PresetBus::SetModuleAddress(bus_addr_edit);
      PhzConfig::save_config();
    }
    bus_addr_dirty = false;
    bus_addr_edit = 0;
    if (invert_display_dirty) {
#ifdef __IMXRT1062__
      // invert_display lives in the same METADATA_KEY-packed globals struct
      // as current_app_id/encoders_enable_acceleration -- BuildGlobalSettingsValues()
      // (declared in OC_apps.h) is the shared writer both SaveGlobalSettings()
      // and this app use, so the two can never diverge (same reasoning as
      // the bus_addr_dirty block above).
      PhzConfig::load_config();
      BuildGlobalSettingsValues();
      PhzConfig::save_config();
#endif
      // T3.2 has no PhzConfig map and no BuildGlobalSettingsValues(): its
      // GlobalSettings (invert_display included) persist through the EEPROM
      // PageStorage path in SaveGlobalSettings() instead, so there is nothing
      // to flush here -- just clear the flag so it can't latch.
      invert_display_dirty = false;
    }
  }

  void SwitchToStep(int direction) {

      if (calstate.current_step->calibration_type == CALIBRATE_OCTAVE && !calstate.used_defaults) {
        // fine-tuning for CALIBRATE_DAC
        int octave = current_octave + direction;
        if ( !(octave < 0 || octave > min(OCTAVES, calstate.current_step->index + 7)) ) {
          current_octave = octave;
          calstate.encoder_value =
              OC::calibration_data.dac.calibrated_octaves[step_to_channel(calstate.step)][current_octave];
          return;
        }
      }

      CALIBRATION_STEP index = static_cast<CALIBRATION_STEP>(calstate.step + direction);
      CONSTRAIN(index, CENTER_DISPLAY, CALIBRATION_EXIT);
      const CalibrationStep *next_step = &calibration_steps[index];
      if (next_step != calstate.current_step)
      {
        calstate.step = index;
        const DAC_CHANNEL chan = step_to_channel(next_step->step);
        SERIAL_PRINTLN("%s (%d)", next_step->title, chan);

        // Special cases on exit current step
        if (calstate.used_defaults
          && CALIBRATE_OCTAVE == calstate.current_step->calibration_type
          && calstate.current_step->index > 5) {
          // always apply interpolation when leaving a high DAC point
          InterpolateChannel(step_to_channel(calstate.current_step->step));
        }
        switch (calstate.current_step->step) {
          case HELLO:
            if (calstate.encoder_value) {
              SERIAL_PRINTLN("Reset to defaults...");
              uint32_t flags = OC::calibration_data.flags & CALIBRATION_FLAG_ENCODER_MASK;
              OC::calibration_reset();
              OC::calibration_data.flags |= flags; // preserve encoder config
              calstate.used_defaults = true;
            }
            break;
          case DAC_A_VOLT_HIGH:
            if (calstate.used_defaults) {
              // copy DAC A to the rest of them, to make life easier
              const DAC_CHANNEL chan[DAC_CHANNEL_COUNT] = {
                DAC_CHANNEL_A, DAC_CHANNEL_B, DAC_CHANNEL_C, DAC_CHANNEL_D,
#ifdef ARDUINO_TEENSY41
                DAC_CHANNEL_E, DAC_CHANNEL_F, DAC_CHANNEL_G, DAC_CHANNEL_H,
#endif
              };
              for (int ch = 1; ch < DAC_CHANNEL_COUNT; ++ch) {
                for (int i = 0; i < OCTAVES; ++i) {
                  OC::calibration_data.dac.calibrated_octaves[chan[ch]][i]
                    = OC::calibration_data.dac.calibrated_octaves[chan[0]][i];
                }
              }
            }
            break;
          case ADC_PITCH_C4:
            if (calstate.adc_1v && calstate.adc_3v) {
              OC::ADC::CalibratePitch(calstate.adc_1v, calstate.adc_3v);
              SERIAL_PRINTLN("ADC SCALE 1V=%d, 3V=%d -> %d",
                             calstate.adc_1v, calstate.adc_3v,
                             OC::calibration_data.adc.pitch_cv_scale);
            }
            break;

          default: break;
        }

        // Setup next step
        switch (next_step->calibration_type) {
        case CALIBRATE_OCTAVE:
          current_octave = next_step->index;
          calstate.encoder_value =
              OC::calibration_data.dac.calibrated_octaves[chan][current_octave];
            #ifdef VOR
            /* set 0V @ unipolar range */
            DAC::set_Vbias(DAC::VBiasUnipolar);
            #endif
          break;

        #ifdef VOR
        case CALIBRATE_VBIAS_BIPOLAR:
          calstate.encoder_value = (0xFFFF & OC::calibration_data.v_bias); // bipolar = lower 2 bytes
        break;
        case CALIBRATE_VBIAS_ASYMMETRIC:
          calstate.encoder_value = (OC::calibration_data.v_bias >> 16);  // asymmetric = upper 2 bytes
        break;
        #endif

        case CALIBRATE_ADC_OFFSET: // set ADC zero-point offset
          if (calstate.used_defaults) { // start fresh? auto-cal
            for (int i = 0; i < ADC_CHANNEL_COUNT; ++i) {
              OC::calibration_data.adc.offset[i] = OC::ADC::smoothed_raw_value(static_cast<ADC_CHANNEL>(i));
            }
          }

          #ifdef VOR
          DAC::set_Vbias(DAC::VBiasUnipolar);
          #endif
          break;
        case CALIBRATE_DISPLAY:
          calstate.encoder_value = OC::calibration_data.display_offset;
          break;

        case CALIBRATE_ADC_1V:
        case CALIBRATE_ADC_3V:
          SERIAL_PRINTLN("offset=%d", OC::calibration_data.adc.offset[ADC_CHANNEL_1]);
          break;

        case CALIBRATE_SCREENSAVER:
          calstate.encoder_value = OC::calibration_data.screensaver_timeout;
          SERIAL_PRINTLN("timeout=%d", calstate.encoder_value);
          break;

        case CALIBRATE_NONE:
        default:
          if (CALIBRATION_EXIT != next_step->step) {
            calstate.encoder_value = 0;
          } else {
            // Make the default "Save: no" if the calibration data was reset
            // manually, but only if calibration data was actually loaded from
            // EEPROM
            if (calstate.used_defaults && OC::calibration_data_loaded)
              calstate.encoder_value = 0;
            else
              calstate.encoder_value = 1;
          }
        }
        calstate.current_step = next_step;
      }
  }

  void InterpolateChannel(int ch) {
    const int idxlow = 0;
    const int idxhigh = current_octave;
    uint32_t value = OC::calibration_data.dac.calibrated_octaves[ch][idxlow];
    const uint16_t second = OC::calibration_data.dac.calibrated_octaves[ch][idxhigh];
    int interval = (second - value) / (idxhigh - idxlow);

    for (int i = idxlow+1; i < OCTAVES + 1; ++i) {
      value += interval;
      if (value > 0xFFFF) value = 0xFFFF;
      OC::calibration_data.dac.calibrated_octaves[ch][i] = value;
    }

    calstate.auto_scale_set[ch] = true;
  }

  void set_all_octave(int oct) {
    for (int i = 0; i < DAC_CHANNEL_COUNT; ++i) {
      Out(i, oct * ONE_OCTAVE);
    }
  }

  void Controller()
  {
    using namespace OC;
    if (calibration_mode && !calibration_complete)
    {
      uint32_t ticks = tick_count.Update();
      digital_input_displays[0].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_1>());
      digital_input_displays[1].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_2>());
      digital_input_displays[2].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_3>());
      digital_input_displays[3].Update(ticks, DigitalInputs::read_immediate<DIGITAL_INPUT_4>());

      // moved from calibration_update()
      const CalibrationStep *step = calstate.current_step;

      switch (step->calibration_type) {
        case CALIBRATE_NONE:
          set_all_octave(0);
          break;
        case CALIBRATE_OCTAVE:
          OC::calibration_data.dac.calibrated_octaves[step_to_channel(step->step)][current_octave] =
            calstate.encoder_value;
          set_all_octave((current_octave - DAC::kOctaveZero)*(1+DAC_20Vpp));
          break;
        #ifdef VOR
        case CALIBRATE_VBIAS_BIPOLAR:
          /* set 0V @ bipolar range */
          set_all_octave(5);
          OC::calibration_data.v_bias = (OC::calibration_data.v_bias & 0xFFFF0000) | calstate.encoder_value;
          DAC::set_Vbias(0xFFFF & OC::calibration_data.v_bias);
          break;
        case CALIBRATE_VBIAS_ASYMMETRIC:
          /* set 0V @ asym. range */
          set_all_octave(3);
          OC::calibration_data.v_bias = (OC::calibration_data.v_bias & 0xFFFF) | (calstate.encoder_value << 16);
          DAC::set_Vbias(OC::calibration_data.v_bias >> 16);
        break;
        #endif
        case CALIBRATE_ADC_OFFSET:
          set_all_octave(0);
          break;
        case CALIBRATE_ADC_1V:
          set_all_octave(1);
          break;
        case CALIBRATE_ADC_3V:
          set_all_octave(3);
          break;
        case CALIBRATE_DISPLAY:
          OC::calibration_data.display_offset = calstate.encoder_value;
          display::AdjustOffset(OC::calibration_data.display_offset);
          break;
        case CALIBRATE_SCREENSAVER:
          set_all_octave(0);
          OC::calibration_data.screensaver_timeout = calstate.encoder_value;
          break;
      }

      return;
    }

    if (CORE::ticks % 3200 == 0) {
      pick_left = random(8);
      pick_right = random(8);
    }

  }

  const uint8_t *iconography[8] = {
    PhzIcons::voltage, ZAP_ICON,
    PhzIcons::pigeons, PhzIcons::camels,
    PhzIcons::legoFace, PhzIcons::tb3P0,
    PhzIcons::drLoFi, PhzIcons::umbrella,
  };
  int pick_left = 0, pick_right = 0;

    void View() const;
    void DrawConfirm() const;

  void DrawCalibration() const {
    const OC::CalibrationStep *step = calstate.current_step;

    menu::DefaultTitleBar::Draw();
    graphics.print(step->title);

    weegfx::coord_t y = menu::CalcLineY(0);

    static constexpr weegfx::coord_t kValueX = menu::kDisplayWidth - 30;

    gfxPos(menu::kIndentDx, y + 2);
    switch (step->calibration_type) {
      case CALIBRATE_OCTAVE:
      {
        if (calstate.auto_scale_set[step_to_channel(step->step)]) {
          graphics.drawBitmap8(menu::kDisplayWidth - 10, y + 13, 8, CHECK_ICON);
          gfxPos(menu::kIndentDx, y + 2);
        }
        int voltage = (current_octave - DAC::kOctaveZero) * (NorthernLightModular? 12: 10) * (1+DAC_20Vpp);
        graphics.printf("-> %d.%d00V", voltage / 10, voltage % 10);
        gfxPos(kValueX, y + 2);
        graphics.print((int)calstate.encoder_value, 5);
        menu::DrawEditIcon(kValueX, y, calstate.encoder_value, step->min, step->max);
        break;
      }

      case CALIBRATE_SCREENSAVER:
      #ifdef VOR
      case CALIBRATE_VBIAS_BIPOLAR:
      case CALIBRATE_VBIAS_ASYMMETRIC:
      #endif
        graphics.print(step->message);
        gfxPos(kValueX, y + 2);
        graphics.print((int)calstate.encoder_value, 5);
        menu::DrawEditIcon(kValueX, y, calstate.encoder_value, step->min, step->max);
        break;

      case CALIBRATE_ADC_OFFSET:
        for (int i = 0; i < ADC_CHANNEL_COUNT; ++i) {
          gfxPos(1 + i%4*32, y + 10*(i/4));
          graphics.printf("%3d", (int)OC::ADC::value(static_cast<ADC_CHANNEL>(i)));
        }
        y += 10;
        break;

      case CALIBRATE_DISPLAY:
        graphics.print(step->message);
        gfxPos(kValueX, y + 2);
        graphics.pretty_print((int)calstate.encoder_value, 2);
        menu::DrawEditIcon(kValueX, y, calstate.encoder_value, step->min, step->max);
        graphics.drawFrame(0, 0, 128, 64);
        break;

      case CALIBRATE_ADC_1V:
      case CALIBRATE_ADC_3V:
        gfxPos(menu::kIndentDx, y + 2);
        graphics.printf(step->message, (NorthernLightModular*2*step->index));
        y += menu::kMenuLineH;
        gfxPos(menu::kIndentDx, y + 2);
        graphics.print((int)OC::ADC::value(ADC_CHANNEL_1), 2);
        if ( (calstate.adc_1v && step->calibration_type == CALIBRATE_ADC_1V) ||
             (calstate.adc_3v && step->calibration_type == CALIBRATE_ADC_3V) )
        {
          graphics.print("  (set)");
        }
        break;

      case CALIBRATE_NONE:
      default:
        if (CALIBRATION_EXIT != step->step) {
          gfxPos(menu::kIndentDx, y + 2);
          graphics.print(step->message);
          if (step->value_str)
            graphics.print(step->value_str[calstate.encoder_value]);
        } else {
          gfxPos(menu::kIndentDx, y + 2);

          if (calibration_data_loaded && calstate.used_defaults)
            graphics.print("Overwrite? ");
          else
            graphics.print("Save? ");

          if (step->value_str)
            graphics.print(step->value_str[calstate.encoder_value]);

        }
        break;
    }

    y += menu::kMenuLineH;
    gfxPos(menu::kIndentDx, y + 2);
    if (step->help)
      graphics.print(step->help);

    // NJM: display encoder direction config on first and last screens
    if (step->step == HELLO || step->step == CALIBRATION_EXIT) {
        y += menu::kMenuLineH;
        gfxPos(menu::kIndentDx, y + 2);
        graphics.print("Encoders: ");
        graphics.print(OC::Strings::encoder_config_strings[ OC::calibration_data.encoder_config() ]);
    }

    weegfx::coord_t x = menu::kDisplayWidth - 22;
    y = 2;
    for (int input = OC::DIGITAL_INPUT_1; input < OC::DIGITAL_INPUT_LAST; ++input) {
      uint8_t state = (digital_input_displays[input].getState() + 3) >> 2;
      if (state)
        graphics.drawBitmap8(x, y, 4, OC::bitmap_gate_indicators_8 + (state << 2));
      x += 5;
    }

    graphics.drawStr(1, menu::kDisplayHeight - menu::kFontHeight - 3, step->footer);

    static constexpr uint16_t step_width = (menu::kDisplayWidth << 8 ) / (CALIBRATION_STEP_LAST - 1);
    graphics.drawRect(0, menu::kDisplayHeight - 2, (calstate.step * step_width) >> 8, 2);

  }

    /////////////////////////////////////////////////////////////////
    // Control handlers
    /////////////////////////////////////////////////////////////////

    void HandleUiEvent(const UI::Event &event) {
      if (!calibration_mode) {
        // An armed screen owns all input: see HandleConfirmEvent.
        if (pending_ != PENDING_NONE) {
          HandleConfirmEvent(event);
          return;
        }

        // encL: a press calibrates, holding it past kLongPressTicks offers
        // the bootloader. Two different gestures on one button, neither of
        // which can be reached by turning the encoder, and the footer says
        // what continuing to hold will do while you are holding it.
        if (event.control == OC::CONTROL_BUTTON_L) {
          switch (event.type) {
            case UI::EVENT_BUTTON_DOWN:
              encl_held_ = true;
              break;
            case UI::EVENT_BUTTON_PRESS:
              encl_held_ = false;
              StartCalibration();
              break;
            case UI::EVENT_BUTTON_LONG_PRESS:
              encl_held_ = false;
              Arm(PENDING_REFLASH);
              break;
            default:            // LONG_RELEASE: the arm already happened
              encl_held_ = false;
              break;
          }
        }
        // encR arms the factory reset. Deliberately the harmless half of the
        // gesture: this is the press a newcomer arrives with, and all it can
        // do is put a screen up that says what the other half would erase.
        if (event.control == OC::CONTROL_BUTTON_R && event.type == UI::EVENT_BUTTON_PRESS)
          Arm(PENDING_RESET);

        // The 200e module address moves only while X is held -- see the note on
        // bus_addr_edit. A bare turn is ignored on purpose: this encoder does
        // nothing else here, so every stray nudge used to land on the module's
        // bus identity. Nothing is applied to the bus until Suspend().
        if (event.control == OC::CONTROL_ENCODER_R && OC::PresetBus::Enabled()) {
          if (!(event.mask & OC::CONTROL_BUTTON_X)) return;
          int a = (int)(bus_addr_edit ? bus_addr_edit
                                      : OC::PresetBus::ModuleAddress())
                  + event.value;
          CONSTRAIN(a, 0x01, 0x77);
          bus_addr_edit = (uint8_t)a;
          bus_addr_dirty = (bus_addr_edit != OC::PresetBus::ModuleAddress());
        }

        // dual-press UP+DOWN / A+B to flip screen
        if ( event.type == UI::EVENT_BUTTON_DOWN &&
            (event.mask == (OC::CONTROL_BUTTON_A | OC::CONTROL_BUTTON_B)) ) {
          OC::calibration_data.toggle_flipmode();
          display::SetFlipMode(OC::calibration_data.flipscreen());
          cal_save_q = true;
        }

        // solo UP press (not the UP+DOWN flip-screen chord above) toggles
        // pixel invert -- the inversion itself, visible instantly, is the
        // confirmation; persisted to GLOBALS.CFG on app exit like bus_addr
        //
        // "Solo" has to be decided on the DOWN edge, for the same reason the
        // confirm screens do it there: event.mask is the raw pin state when
        // the event was queued and a release is only reported seven ticks
        // after the pin rises, so whichever of A/B is let go of first carries
        // an empty mask by the time its press event exists. Testing the mask
        // at the release let the flip-screen chord invert the display as
        // well, which this comment already said it must not.
        if (event.control == OC::CONTROL_BUTTON_UP && event.type == UI::EVENT_BUTTON_DOWN)
          a_down_solo_ = (event.mask == OC::CONTROL_BUTTON_UP);
        if (event.control == OC::CONTROL_BUTTON_UP && event.type == UI::EVENT_BUTTON_PRESS
            && a_down_solo_) {
          a_down_solo_ = false;
          bool inverted = !OC::global_settings.invert_display;
          display::SetInverted(inverted);
          OC::global_settings.invert_display = inverted;
          invert_display_dirty = true;
        }

        return;
      }

      // event handling from Ui::Calibrate()
      if (event.type == UI::EVENT_BUTTON_DOWN) {
        // act-on-press for right encoder
        if (event.control == CONTROL_BUTTON_R) {
          if (calstate.step < CALIBRATION_EXIT) {
            SwitchToStep(1); // step forward
          } else
            calibration_complete = true;

          // ignore release and long-press during calibration
          OC::ui.SetButtonIgnoreMask();
        }
      } else {
        // press, long-press, or encoder movements
        switch (event.control) {
          case CONTROL_BUTTON_L:
            if (calstate.step == HELLO) calibration_complete = 1; // Way out --jj
            if (calstate.step > CENTER_DISPLAY)
              SwitchToStep(-1); // step backward
            break;
          case CONTROL_BUTTON_R:
            break;

          case CONTROL_ENCODER_L:
            if (calstate.step > HELLO) {
              SwitchToStep(event.value);
            }
            break;
          case CONTROL_ENCODER_R: {
            int delta = event.value;
            if (event.mask & OC::CONTROL_BUTTON_X) delta *= 16;
            else if (event.mask & OC::CONTROL_BUTTON_Y) delta *= 32;
            calstate.encoder_value = constrain(
              calstate.encoder_value + delta,
              calstate.current_step->min,
              calstate.current_step->max
            );
            break;
          }

          case CONTROL_BUTTON_UP:
          case CONTROL_BUTTON_DOWN:
            if (UI::EVENT_BUTTON_LONG_PRESS == event.type) {
              const CalibrationStep *step = calstate.current_step;

              // long-press B/DOWN to measure ADC points
              switch (step->step) {
                case ADC_PITCH_C2:
                  calstate.adc_1v = OC::ADC::value(ADC_CHANNEL_1);
                  break;
                case ADC_PITCH_C4:
                  calstate.adc_3v = OC::ADC::value(ADC_CHANNEL_1);
                  break;
                default: break;
              }

              // long-press to re-set ADC zero-point offset
              if (CALIBRATE_ADC_OFFSET == step->calibration_type) {
                for (int i = 0; i < ADC_CHANNEL_COUNT; ++i) {
                  OC::calibration_data.adc.offset[i] = OC::ADC::smoothed_raw_value(static_cast<ADC_CHANNEL>(i));
                }
              }

              // long-press DOWN to auto-scale DAC values on current channel
              if (step->calibration_type == CALIBRATE_OCTAVE && current_octave > 0) {
                InterpolateChannel(step_to_channel(step->step));
              }
              break;
            }

            // regular press cycles thru encoder orientations on first/last screen
            if (calstate.step == HELLO || calstate.step == CALIBRATION_EXIT)
            {
              OC::ui.configure_encoders(calibration_data.next_encoder_config());
              cal_save_q = true;
            }

            break;
          default:
            break;
        }
      }

      if (calibration_complete) {
        if (calstate.encoder_value) {
          SERIAL_PRINTLN("Calibration complete");
          OC::calibration_save();
          cal_save_q = false;
        } else {
          SERIAL_PRINTLN("Calibration complete (but don't save)");
        }

        OC::ui.set_screensaver_timeout(OC::calibration_data.screensaver_timeout);
        calibration_mode = false;
      }
    }
    void StartCalibration() {
        // migrated from OC::ui.Calibrate();

        calstate = {
          OC::HELLO,
          &OC::calibration_steps[OC::HELLO],
          OC::calibration_data_loaded ? 0 : 1, // "use defaults: no" if data loaded
        };
        calstate.used_defaults = false;

        for (auto &did : digital_input_displays)
          did.Init();

        tick_count.Init();

        OC::ui.encoder_enable_acceleration(OC::CONTROL_ENCODER_R, true);
        #ifdef VOR
        {
          VBiasManager *vb = vb->get();
          vb->SetState(VBiasManager::UNI);
        }
        #endif

        calibration_complete = false;
        calibration_mode = true;
    }
    void Arm(PendingAction what) {
      pending_ = what;
      armed_ms_ = millis();
      // A B that was already held when the screen appeared cannot commit:
      // only a press that BEGINS here counts, so "hold B, press encR" is one
      // gesture short of a reset rather than one gesture past it.
      b_down_solo_ = false;
    }
    void Disarm() {
      // Never leave an action armed across a suspend, a screensaver or a
      // return to this app: the confirm screen would be gone but B would
      // still commit whatever it was.
      pending_ = PENDING_NONE;
      encl_held_ = false;
    }

    // The armed screen answers to exactly two things: B commits, encL says
    // no. The encoder buttons (including encR, which armed the reset), the
    // other face buttons and both encoder turns are all inert here, so no
    // chord and no habitual press can finish what a stray press started.
    void HandleConfirmEvent(const UI::Event &event) {
      if (event.control == OC::CONTROL_BUTTON_L      // encL = no, everywhere
          && event.type == UI::EVENT_BUTTON_PRESS) {
        Disarm();
        return;
      }
      if (event.control != OC::CONTROL_BUTTON_B) return;

      // The chord test has to happen on the DOWN edge. event.mask is the raw
      // pin state at the tick the event was queued, and a release is only
      // reported seven ticks after the pin rises -- so let go of A a few
      // milliseconds before B, as anyone releasing the A+B flip-screen chord
      // does, and B's press event carries an empty mask and looks solo. At
      // the DOWN edge the other button is still down and the chord is plain.
      if (event.type == UI::EVENT_BUTTON_DOWN) {
        b_down_solo_ = (event.mask == OC::CONTROL_BUTTON_B);
        return;
      }
      if (event.type != UI::EVENT_BUTTON_PRESS) return;

      const bool solo = b_down_solo_;
      b_down_solo_ = false;
      if (!solo || event.mask) return;   // ...and nothing else still held

      // The screen must have been readable for kConfirmDeadMs before it will
      // take a yes. Deliberately silent: a slip inside the window should feel
      // like nothing happened, and the user reads the screen and presses again.
      if (millis() - armed_ms_ < kConfirmDeadMs) return;

      const PendingAction go = pending_;
      Disarm();
      if (go == PENDING_RESET) FactoryReset();
      else if (go == PENDING_REFLASH) Reflash();
    }

    void Reflash() {
      uint32_t start = millis();
      while(millis() < start + SETTINGS_SAVE_TIMEOUT_MS) {
        GRAPHICS_BEGIN_FRAME(true);
        gfxPos(5, 10);
        graphics.print("Flash Upgrade Mode");
        gfxPos(5, 19);
        graphics.print("(use Teensy Loader)");
        GRAPHICS_END_FRAME();
      }

      // special teensy_reboot command
      _reboot_Teensyduino_();
    }

    void FactoryReset() {
      // Not a bare Init(): that switched to the default app with the ISR
      // still running and never sent it RESUME. ReinitApps does the same
      // stop/switch/resume dance the app menu does, around the reset.
      OC::ReinitApps(true);

      // Then start over. The erase rewrote storage, but a reset is not only
      // storage: the preset-bus module address, the overlay's trigger
      // assignments, the clock routing, the engine's current-slot mirror
      // all live in RAM copies seeded once at boot, and every one of them
      // would keep the pre-reset value until the next power cycle -- and
      // the first save from any of them would write it straight back into
      // the fresh GLOBALS.CFG. A restart makes "factory" true everywhere at
      // once, and brings up the first-run splash the way a new module does.
      // SCB_AIRCR is the ARM SYSRESETREQ register the Teensy core's own fault
      // handler reboots through; the simulator has no such register, and
      // there the reset simply continues into the default app.
#ifdef SCB_AIRCR
      uint32_t start = millis();
      while(millis() < start + SETTINGS_SAVE_TIMEOUT_MS) {
        GRAPHICS_BEGIN_FRAME(true);
        gfxPos(5, 10);
        graphics.print("Factory reset done");
        gfxPos(5, 19);
        graphics.print("restarting...");
        GRAPHICS_END_FRAME();
      }
      Serial.flush();
      SCB_AIRCR = 0x05FA0004;
      while (true) ;
#endif
    }
};

// The confirm screens. Nothing has been erased or rebooted when these are
// drawn -- they exist only to state, in words, what B is about to do.
FLASHMEM void AppSettings::DrawConfirm() const {
  const bool reset = (pending_ == PENDING_RESET);

  graphics.setPrintPos(0, 13);
  graphics.print(reset ? "FACTORY RESET" : "REFLASH: BOOTLOADER");
  graphics.invertRect(0, 12, 128, 10);   // inversion is the only emphasis here

  graphics.setPrintPos(0, 26);
  if (reset) {
    // What AppSwitcher::Init(true) actually does: InitDefaults() on every
    // app, global_settings back to defaults, the user Turing machines
    // zeroed, EEPROM erased from EEPROM_GLOBALSETTINGS_START up, plus
    // PhzConfig::eraseFiles() on T4.1.
    //
    // No second prompt: Init() now resolves the decision BEFORE it mutates
    // anything, and treats a runtime reset as already-confirmed because THIS
    // screen is the confirmation. It used to ask again afterwards, over state
    // it had already discarded -- a prompt performed on a corpse.
    // Calibration lives below that mark and survives, which is worth saying:
    // it is the one thing a user cannot recreate without a voltmeter.
    // "+ all 30 bus presets" is not padding. The reset calls
    // PhzConfig::eraseFiles(), which is a full myfs format -- and since the
    // preset store moved to internal flash it now takes every PB_NN.PBS,
    // PBNAMES.BIN and PBSNAP.BIN with it. Before that move, with a card
    // seated, a format left the presets alone. The blast radius grew and this
    // screen did not, so it was understating what the press destroys by
    // thirty presets. Export to the card first if they matter.
    graphics.print("Erases settings, apps");
    graphics.setPrintPos(0, 36);
    graphics.print("+ all 30 bus presets");
    graphics.setPrintPos(0, 46);
    graphics.print("Calibration is kept.");
  } else {
    graphics.print("Reboots to bootloader");
    graphics.setPrintPos(0, 36);
    graphics.print("for Teensy Loader.");
    graphics.setPrintPos(0, 46);
    graphics.print("Module stops running.");
  }

  // encL is the harmless answer at x=0 on every screen this app draws; the
  // committing button sits where nothing else does.
  graphics.setPrintPos(0, 56);
  graphics.print("encL:no");
  graphics.setPrintPos(78, 56);
  graphics.print(reset ? "B:ERASE" : "B:BOOT");
}

FLASHMEM void AppSettings::View() const {
      if (calibration_mode) {
        DrawCalibration();
        return;
      }
      if (pending_ != PENDING_NONE) {
        DrawConfirm();
        return;
      }

      gfxHeader("Setup/About");
      gfxIcon(80, 0, OC::calibration_data.flipscreen() ? DOWN_ICON : UP_ICON);
      gfxIcon(90, 0, OC::calibration_data.flipcontrols() ? LEFT_ICON : RIGHT_ICON);

      #if defined(ARDUINO_TEENSY40)
      gfxPrint(100, 0, "T4.0");
      //gfxPrint(0, 45, "E2END="); gfxPrint(E2END);
      #elif defined(ARDUINO_TEENSY41)
      gfxPrint(100, 0, "T4.1");
      #else
      gfxPrint(100, 0, "T3.2");
      #endif

      gfxIcon(0, 15, iconography[pick_left]);
      gfxIcon(120, 15, iconography[pick_right]);
      #ifdef PEWPEWPEW
      gfxPrint(21, 15, "PEW! PEW! PEW!");
      #else
      gfxPrint(12, 15, OC::Strings::RELEASE_NAME);
      #endif
      gfxIcon(0, 25, PhzIcons::full_book);
      gfxPrint(10, 25, OC::Strings::VERSION);
      gfxIcon(0, 35, PhzIcons::runglBook);
      gfxPrint(10, 35, OC::Strings::BUILD_TAG);
      gfxIcon(0, 45, PhzIcons::frontBack);
      if (OC::PresetBus::Enabled()) {
        // 200e preset bus. The inverted address is THE right-encoder target
        // on this screen (inversion = focus); presence dots per the system
        // idiom; caps = active, lowercase = inactive.
        gfxPrint(10, 45, "Bus");
        graphics.setPrintPos(34, 45);
        // Show the pending value while it is being dialled, and invert it only
        // then -- inversion is this UI's "the right encoder changes this", and
        // it was previously on permanently, advertising an edit that a stray
        // turn would make. Unedited, the address just reads as a fact.
        graphics.printf("%02X", bus_addr_edit ? bus_addr_edit
                                              : OC::PresetBus::ModuleAddress());
        if (bus_addr_edit) graphics.invertRect(32, 44, 16, 10);
        graphics.setPrintPos(58, 45);
        gfxPrint(OC::PresetBus::RemoteEnabled() ? "REM" : "rem");
        graphics.drawCircle(94, 49, 2);
        if (OC::PresetBus::WpmPresent()) graphics.drawRect(93, 48, 3, 3);
        graphics.setPrintPos(100, 45);
        gfxPrint(OC::PresetBus::WpmPresent() ? "WPM" : "wpm");
      } else {
        gfxPrint(10, 45, "github.com/djphazer");
      }
      // 21 columns exactly, and it states its bindings rather than relying on
      // the unwritten "left label = left encoder" convention the bracketed
      // labels used to. Same legend grammar as the 200e app's "encR:CONFIRM
      // encL:no". While encL is held the row offers the other half of that
      // button, so the long-press is discoverable without being reachable by
      // accident -- you are told what continuing to hold does while you hold.
      gfxPrint(0, 55, encl_held_ ? "keep holding: Reflash"
                                 : "encL:Cal   encR:Reset");
}

FLASHMEM void AppSettings::Init() {
    BaseStart();
}

// Not using O_C Storage
size_t AppSettings::SaveAppData(util::StreamBufferWriter &) const { return 0; }
size_t AppSettings::RestoreAppData(util::StreamBufferReader &) { return 0; }

void AppSettings::Process(OC::IOFrame *ioframe) {
  BaseController(ioframe);
}

FLASHMEM void AppSettings::HandleAppEvent(OC::AppEvent event) {
  if (event == OC::APP_EVENT_RESUME) {
    Resume();
  }
  if (event == OC::APP_EVENT_SUSPEND) {
    Suspend();
  }
  // The screensaver is not a suspend, but it hides the confirm screen just as
  // completely -- an armed action must not outlive the words that explain it.
  if (event == OC::APP_EVENT_SCREENSAVER_ON) {
    Disarm();
  }
}

void AppSettings::Loop() {} // Deprecated

void AppSettings::GetIOConfig(OC::IOConfig &ioconfig) const
{
  ioconfig.outputs[0].set("CH1", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[1].set("CH2", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[2].set("CH3", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[3].set("CH4", OC::OUTPUT_MODE_UNI);
}
void AppSettings::DrawDebugInfo() const { }

FLASHMEM void AppSettings::DrawMenu() const {
    BaseView();
}

FLASHMEM void AppSettings::DrawScreensaver() const {
#ifdef PEWPEWPEW
    for (int i = 0; i < (pewpew_width * pewpew_height / 64); ++i) {
      // TODO: the problem here is that one byte in XBM is a row of 8 pixels,
      //       while one byte in the framebuffer is a column of 8 pixels
      gfxBitmap((i & 0x1)*64, (i>>1)*8, 64, pewpew_bits + i*64);
    }
#endif
  ZapScreensaver();
}

FLASHMEM void AppSettings::HandleButtonEvent(const UI::Event &event) {
  HandleUiEvent(event);
}

FLASHMEM void AppSettings::HandleEncoderEvent(const UI::Event &event) {
  HandleUiEvent(event);
}
