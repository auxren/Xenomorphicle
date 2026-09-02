// Copyright (c) 2016-2019 Patrick Dowling
//
// Author: Patrick Dowling (pld@gurkenkiste.com)
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

#include "OC_core.h"
#include "OC_gpio.h"
#include "OC_scales.h"
#include "OC_ui.h"
#include "OC_apps.h"
#include "OC_menus.h"
#include "OC_config.h"
#include "OC_digital_inputs.h"
#include "OC_storage.h"
#include "OC_app_switcher.h"
#include "OC_global_settings.h"
#include "util/util_misc.h"
#include "util/util_settings.h"
#include "util/util_integer_sequences.h"
#include "util/util_trigger_delay.h"
#include "util/util_logistic_map.h"
#include "util/util_arp.h"
#include "util/util_grid.h"
#include "util/util_ringbuffer.h"
#include "util/util_settings.h"
#include "util/util_semitone_quantizer.h"
#include "util/util_sync.h"

#include "OC_input_maps.h"
#include "OC_pitch_utils.h"
#include "OC_euclidean_mask_draw.h"
#include "OC_trigger_delays.h"

#include "OC_calibration.h"
#include "OC_patterns.h"
#include "src/drivers/FreqMeasure/OC_FreqMeasure.h"
#include "src/drivers/display.h"
#include "util/util_pagestorage.h"
#include "src/drivers/EEPROMStorage.h"
#include "PhzConfig.h"
#include "VBiasManager.h"
#include "HSClockManager.h"

#ifndef NO_HEMISPHERE
// applets
#include "applets/_config.h"
#ifdef ARDUINO_TEENSY41
#include "audio_applets/_config.h"
#endif
#endif

#include "HSApplication.h"
#include "OC_scale_edit.h"
#include "OC_visualfx.h"

// Bus200eApp.h calls into the preset engine but does not include it; in full
// builds Quadrants.h happens to pull the header in first, in reduced builds
// (the simulator's, for one) nothing does, and this file is where the app
// headers are actually compiled. Included here so app instantiation does not
// depend on which apps are enabled above it.
#include "PresetEngine.h"

// actual apps are included and instantiated here
#include "apps/_config.h"

namespace OC {

// NOTE These are slightly wasteful, in that the PageStorage implementation and
// the local data both retain a copy of the data. Removing this would in theory
// reclaim some memory, although RAM isn't currently an issue.
/*extern*/ DMAMEM GlobalSettings global_settings;
/*extern*/ AppSwitcher app_switcher;
static DMAMEM AppData app_data;
#ifndef __IMXRT1062__
static GlobalSettingsStorage global_settings_storage;
#endif
static DMAMEM AppDataStorage app_data_storage;

#ifdef __IMXRT1062__
enum GlobalSettingsDataKeys : uint16_t {
  // upper 8 bits of key, non-zero
  METADATA_KEY        = 1 << 8, // selected app id, etc.
  USER_SCALES_KEY     = 2 << 8,
  SEQUENCES_KEY       = 3 << 8,
  CHORDS_KEY          = 4 << 8,
  TURING_MACHINES_KEY = 5 << 8,
  WAVEFORMS_KEY       = 6 << 8,
  AUTOCAL_KEY         = 7 << 8,
  PRESETBUS_KEY       = 8 << 8, // preset-bus module addr + slot manifests

  // lower 8 bits of key
  SCALE_METADATA = 0xff,
  SCALE_NOTEDATA = 0,
};
#endif

#ifdef __IMXRT1062__
// Write the global-settings key/values into the *currently loaded* PhzConfig
// map (no file I/O, no SD export). Shared by SaveGlobalSettings and the
// preset engine's slot capture so the two can never diverge.
FLASHMEM
void BuildGlobalSettingsValues() {
  // Metadata
  uint64_t data = 0;
  // TODO:
  //global_settings.DAC_scaling = OC::DAC::store_scaling();
  Pack(data, PackLocation{0, 16}, global_settings.current_app_id);
  Pack(data, PackLocation{16, 1}, global_settings.encoders_enable_acceleration);
  Pack(data, PackLocation{17, 1}, 1); // v2.0 first-run validation
  Pack(data, PackLocation{18, 1}, global_settings.invert_display);
  PhzConfig::setValue(METADATA_KEY, data);

  // User Scales
  for (size_t i = 0; i < Scales::SCALE_USER_COUNT; ++i) {
    PhzConfig::setValue(USER_SCALES_KEY | (i << 4) | SCALE_METADATA, uint64_t(user_scales[i].span) << 16 | user_scales[i].num_notes);
    data = 0;
    for (size_t nn = 0; nn < user_scales[i].num_notes; ++nn) {
      Pack(data, PackLocation{(nn & 0x3)*16, 16}, (uint16_t)user_scales[i].notes[nn]);

      // after every 4th value (64 bits), store and reset
      if ((nn & 0x3) == 0x3) {
        PhzConfig::setValue(USER_SCALES_KEY | (i << 4) | (SCALE_NOTEDATA + (nn >> 2)), data);
        data = 0;
      }
    }
  }

  // User Patterns aka Sequences
  for (size_t i = 0; i < Patterns::PATTERN_USER_COUNT; ++i) {
    data = 0;
    for (size_t step = 0; step < ARRAY_SIZE(Pattern::notes); ++step) {
      Pack(data, PackLocation{(step & 0x3)*16, 16}, (uint16_t)user_patterns[i].notes[step]);
      if ((step & 0x3) == 0x3) {
        PhzConfig::setValue(SEQUENCES_KEY | (i << 3) | (step >> 2), data);
        data = 0;
      }
    }
  }

  // User Chords (progression sequences from Acid Curds)
  for (size_t i = 0; i < Chords::CHORDS_USER_COUNT; ++i) {
    data = 0;
    Pack(data, PackLocation{0, 8}, (uint8_t)user_chords[i].quality);
    Pack(data, PackLocation{8, 8}, (uint8_t)user_chords[i].inversion);
    Pack(data, PackLocation{16,8}, (uint8_t)user_chords[i].voicing);
    Pack(data, PackLocation{24,8}, (uint8_t)user_chords[i].base_note);
    Pack(data, PackLocation{32,8}, (uint8_t)user_chords[i].octave);
    PhzConfig::setValue(CHORDS_KEY | i, data);
  }

  // User Turing Machines (for Enigma and friends)
  for (size_t i = 0; i < HS::TURING_MACHINE_COUNT; ++i) {
    data = 0;
    Pack(data, PackLocation{0, 16}, HS::user_turing_machines[i].reg);
    Pack(data, PackLocation{16, 8}, HS::user_turing_machines[i].len);
    Pack(data, PackLocation{24, 1}, HS::user_turing_machines[i].favorite);
    PhzConfig::setValue(TURING_MACHINES_KEY | i, data);
  }

#ifndef NO_HEMISPHERE
  data = 0;
  // User Waveform (custom VectorOsc shapes)
  for (size_t i = 0; i < HS::VO_SEGMENT_COUNT; ++i) {
    Pack(data, PackLocation{(i & 0x3) * 16, 16}, uint16_t(HS::user_waveforms[i].level) << 8 | HS::user_waveforms[i].time);

    if ((i & 0x3) == 0x3) {
      PhzConfig::setValue(WAVEFORMS_KEY | (i >> 2), data);
      data = 0;
    }
  }
#endif

  // Auto Calibration Data
  /*
  for (size_t i = 0; i < DAC_CHANNEL_COUNT; ++i) {
    data = 0;
    PhzConfig::setValue(AUTOCAL_KEY | (0xff - i), auto_calibration_data[i].use_auto_calibration_);

    for (size_t oct = 0; oct < OCTAVES + 1; ++oct) {
      Pack(data, PackLocation{(oct & 0x3) * 16, 16}, auto_calibration_data[i].auto_calibrated_octaves[oct]);
      if ((oct & 0x3) == 0x3) {
        PhzConfig::setValue(AUTOCAL_KEY | (i << 4) | (oct >> 2), data);
        data = 0;
      }
    }
  }
  */
}
#endif // __IMXRT1062__

FLASHMEM
static void SaveGlobalSettings() {
  APPS_SERIAL_PRINTLN("Save global settings");

#ifdef __IMXRT1062__
  //PhzConfig::clear_config();
  PhzConfig::load_config(); // use default config file

  BuildGlobalSettingsValues();

  // export user scales as Scala files alongside the config
  if (SDcard_Ready) {
    char filename[] = "000.SCL";
    for (size_t i = 0; i < Scales::SCALE_USER_COUNT; ++i) {
      filename[2] = char('0' + i);
      SD.remove(filename);
      File file = SD.open(filename, FILE_WRITE_BEGIN);
      if (file) {
        Scales::SaveToScala(user_scales[i], file);
      }
      file.close();
    }
  }

  PhzConfig::save_config(); // save to default config file
#else // --- Teensy 3.2
  memcpy(global_settings.user_scales, OC::user_scales, sizeof(OC::user_scales));
  memcpy(global_settings.user_patterns, OC::user_patterns, sizeof(OC::user_patterns));
#ifdef ENABLE_APP_CHORDS
  memcpy(global_settings.user_chords, OC::user_chords, sizeof(OC::user_chords));
#else
  memcpy(global_settings.user_turing_machines, HS::user_turing_machines, sizeof(HS::user_turing_machines));
#endif
#ifndef NO_HEMISPHERE
  memcpy(global_settings.user_waveforms, HS::user_waveforms, sizeof(HS::user_waveforms));
#endif

  for (int i = 0; i < QUANT_CHANNEL_COUNT; ++i) {
    global_settings.q_engines[i].scale = HS::q_engine[i].scale;
    global_settings.q_engines[i].mask = HS::q_engine[i].mask;
    global_settings.q_engines[i].octave = HS::q_engine[i].octave;
    global_settings.q_engines[i].root_note = HS::q_engine[i].root_note;
  }
  for (int i = 0; i < MIDIMAP_MAX; ++i) {
    global_settings.midi_maps[i] = HS::frame.MIDIState.mapping[i].settings();
  }

  global_settings_storage.Save(global_settings);
  APPS_SERIAL_PRINTLN("Saved global settings: page_index %d", global_settings_storage.page_index());
#endif
}

/* old eeprom space checking logic
static constexpr size_t total_storage_size() {
    size_t used = 0;
    for (size_t i = 0; i < app_container.num_apps(); ++i) {
        used += app_container[i]->storage_size() + sizeof(AppChunkHeader);
        if (used & 1) ++used; // align on 2-byte boundaries
    }
    return used;
}
static constexpr size_t totalsize = total_storage_size();
static_assert(totalsize < OC::AppData::kAppDataSize, "EEPROM Allocation Exceeded");
*/

// Serialize every app's chunk into `out` (RAM only, no storage write).
FLASHMEM void BuildAppData(AppData &out) {
  APPS_SERIAL_PRINTLN("Build app data... (%u bytes available)", OC::AppData::kAppDataSize);

  out.used = 0;
  uint8_t *data = out.data;
  uint8_t *data_end = data + OC::AppData::kAppDataSize;

  size_t start_app = random(app_container.num_apps());
  for (size_t i = 0; i < app_container.num_apps(); ++i) {
    const AppBase* app = (AppBase*)app_container[(start_app + i) % app_container.num_apps()].instance;
    if (!app) continue;
    size_t storage_size = app->storage_size() + sizeof(AppChunkHeader);
    if (storage_size & 1) ++storage_size; // Align chunks on 2-byte boundaries
    if (storage_size > sizeof(AppChunkHeader)) {
      if (data + storage_size > data_end) {
        APPS_SERIAL_PRINTLN("%s: ERROR: %u BYTES NEEDED, %u BYTES AVAILABLE OF %u BYTES TOTAL", app->name(), storage_size, data_end - data, AppData::kAppDataSize);
        continue;
      }

      AppChunkHeader *chunk = reinterpret_cast<AppChunkHeader *>(data);
      chunk->id = app->id();
      chunk->length = storage_size;

      util::StreamBufferWriter stream_buffer{chunk + 1, chunk->length};
      auto result = app->Save(stream_buffer);
      if (stream_buffer.overflow()) {
        APPS_SERIAL_PRINTLN("* %s (%02x) : Save overflowed, result=%u, skipping app...", 
                            app->name(), app->id(), result);
      } else {
        APPS_SERIAL_PRINTLN("* %s (%02x) : Saved %u bytes... (%u)",
                            app->name(), app->id(), result, storage_size);
        out.used += chunk->length;
        data += chunk->length;
      }
      (void)result;
    }
  }
  APPS_SERIAL_PRINTLN("App settings used: %u/%u", out.used, EEPROM_APPDATA_BINARY_SIZE);
}

FLASHMEM void SaveAppData() {
  SaveGlobalSettings(); // yeah, why not
  BuildAppData(app_data);
  app_data_storage.Save(app_data);
  APPS_SERIAL_PRINTLN("Saved app settings in page_index %d", app_data_storage.page_index());
}

// Apply a serialized chunk stream to the live apps (per-chunk validated).
FLASHMEM void ApplyAppData(const AppData &in) {
  APPS_SERIAL_PRINTLN("Restoring app data, used=%u", in.used);

  const uint8_t *data = in.data;
  const uint8_t *data_end = data + in.used;
  size_t restored_bytes = 0;

  while (data < data_end) {
    const AppChunkHeader *chunk = reinterpret_cast<const AppChunkHeader *>(data);
    if (data + chunk->length > data_end) {
      APPS_SERIAL_PRINTLN("App chunk length %u exceeds available space (%u)", chunk->length, data_end - data);
      break;
    }
    if (!chunk->id || !chunk->length) {
      APPS_SERIAL_PRINTLN("Invalid app chunk id=%02x, length=%d, stopping restore", chunk->id, chunk->length);
      break;
    }

    auto app = app_container.FindAppByID(chunk->id);
    if (!app) {
      APPS_SERIAL_PRINTLN("App %02x not found, ignoring chunk... skipping %u", chunk->id, chunk->length);
      if (!chunk->length)
        break;
      data += chunk->length;
      continue;
    }
    size_t expected_length = app->storage_size() + sizeof(AppChunkHeader);
    if (expected_length & 0x1) ++expected_length;
    if (chunk->length != expected_length) {
      APPS_SERIAL_PRINTLN("* %s (%02x): chunk length %u != %u (storage_size=%u), skipping...", app->name(), chunk->id, chunk->length, expected_length, app->storage_size());
      data += chunk->length;
      continue;
    }

    util::StreamBufferReader stream_buffer{chunk + 1, chunk->length};
    auto result = app->Restore(stream_buffer);
    if (stream_buffer.underflow()) {
      APPS_SERIAL_PRINTLN("* %s (%02x): Restore underflow, result=%u, re-init",
                          app->name(), chunk->id, result);
      app->InitDefaults();
    } else {
      APPS_SERIAL_PRINTLN("* %s (%02x): Restored %u from %u (chunk length %u)...",
                          app->name(), chunk->id, result, chunk->length - sizeof(AppChunkHeader), chunk->length);
      restored_bytes += chunk->length;
    }
    (void)result;

    data += chunk->length;
  }

  APPS_SERIAL_PRINTLN("App data restored: %u, expected %u", restored_bytes, in.used);
}

FLASHMEM
static void RestoreAppData() {
  APPS_SERIAL_PRINTLN("Restore from page_index %d", app_data_storage.page_index());
  ApplyAppData(app_data);
}

#ifdef __IMXRT1062__
// Restore the global-settings values from the *currently loaded* PhzConfig
// map. Shared by boot (AppSwitcher::Init) and the preset engine's runtime
// recall so the two can never diverge. scala_loaded_mask marks user-scale
// indices already populated from SD Scala files (those win over config).
FLASHMEM
void RestoreGlobalSettingsFromConfig(uint8_t scala_loaded_mask) {
  uint64_t data = 0;

  // User Scales
  for (size_t i = 0; i < Scales::SCALE_USER_COUNT; ++i) {
    if ((scala_loaded_mask & (1 << i)) ||
        !PhzConfig::getValue(USER_SCALES_KEY | (i << 4) | SCALE_METADATA, data))
        continue;

    user_scales[i].span = (data >> 16) & 0xffff;
    user_scales[i].num_notes = data & 0x00ff;

    for (size_t nn = 0; nn < user_scales[i].num_notes; ++nn) {
      // the first of every 4 values needs a new config chunk
      if ((nn & 0x3) == 0x0) {
        data = 0;
        if (!PhzConfig::getValue(USER_SCALES_KEY | (i << 4) | (SCALE_NOTEDATA + (nn >> 2)), data))
          break;
      }
      user_scales[i].notes[nn] = Unpack(data, PackLocation{(nn & 0x3)*16, 16});
    }
  }

  // User Patterns aka Sequences
  for (size_t i = 0; i < Patterns::PATTERN_USER_COUNT; ++i) {
    for (size_t step = 0; step < ARRAY_SIZE(Pattern::notes); ++step) {
      if ((step & 0x3) == 0x0) {
        data = 0;
        if (!PhzConfig::getValue(SEQUENCES_KEY | (i << 3) | (step >> 2), data))
          break;
      }
      user_patterns[i].notes[step] = Unpack(data, PackLocation{(step & 0x3)*16, 16});
    }
  }

  // User Chords (progression sequences from Acid Curds)
  for (size_t i = 0; i < Chords::CHORDS_USER_COUNT; ++i) {
    data = 0;
    if (!PhzConfig::getValue(CHORDS_KEY | i, data))
      break;
    user_chords[i].quality = Unpack(data, PackLocation{0, 8});
    user_chords[i].inversion = Unpack(data, PackLocation{8, 8});
    user_chords[i].voicing = Unpack(data, PackLocation{16,8});
    user_chords[i].base_note = Unpack(data, PackLocation{24,8});
    user_chords[i].octave = Unpack(data, PackLocation{32,8});
  }

  // -- User Turing Machines (for Enigma and friends)
  for (size_t i = 0; i < HS::TURING_MACHINE_COUNT; ++i) {
    data = 0;
    if (!PhzConfig::getValue(TURING_MACHINES_KEY | i, data))
      break;
    HS::user_turing_machines[i].reg = Unpack(data, PackLocation{0, 16});
    HS::user_turing_machines[i].len = Unpack(data, PackLocation{16, 8});
    HS::user_turing_machines[i].favorite = Unpack(data, PackLocation{24, 1});
  }

#ifndef NO_HEMISPHERE
  // -- User Waveform (custom VectorOsc shapes)
  for (size_t i = 0; i < HS::VO_SEGMENT_COUNT; ++i) {
    if ((i & 0x3) == 0x0) {
      data = 0;
      if (!PhzConfig::getValue(WAVEFORMS_KEY | (i >> 2), data))
        break;
    }
    uint16_t wavedata = Unpack(data, PackLocation{(i & 0x3) * 16, 16});
    HS::user_waveforms[i].level = (wavedata >> 8) & 0xff;
    HS::user_waveforms[i].time = wavedata & 0xff;
  }
#endif
}
#endif // __IMXRT1062__

#ifdef __IMXRT1062__
FLASHMEM
size_t ResolveAppIndexByID(uint16_t app_id) {
  size_t idx = app_container.IndexOfAppByID(app_id);
  if (idx >= app_container.num_apps())
    idx = app_container.IndexOfAppByID(global_settings.current_app_id);
  return idx;
}
#endif

FLASHMEM
void AppSwitcher::set_current_app(size_t index)
{
  current_app_ = app_container[index];
  global_settings.current_app_id = current_app_.id();
  #ifdef VOR
  VBiasManager *vbias_m = vbias_m->get();
  vbias_m->SetStateForApp(current_app_);
  #endif
}

// Factory defaults for the global settings themselves. One definition shared
// by the reset branch and the nothing-in-storage branch of Init() so the two
// cannot drift apart.
FLASHMEM
static void SeedGlobalSettingsDefaults() {
  global_settings.Init();
  global_settings.encoders_enable_acceleration = OC_ENCODERS_ENABLE_ACCELERATION_DEFAULT;
  global_settings.invert_display = false;
  global_settings.reserved1 = false;
  global_settings.reserved2 = 0U;
  global_settings.current_app_id = DEFAULT_APP_ID;
}

// True once Init() has completed. It tells the boot call (nothing is live
// yet, so ConfirmReset is the user's only say in the matter) apart from
// runtime re-inits, where live state exists and Setup's FactoryReset()
// arrives already confirmed by its own arm+confirm screen.
static bool s_init_has_run = false;

FLASHMEM
bool AppSwitcher::Init(bool reset_settings) {

  APPS_SERIAL_PRINTLN("Init");

  // -------------------------------------------------------------------------
  // Phase 1: DECIDE. Nothing is erased or overwritten until the outcome --
  // reset or restore -- is settled. This function used to run InitDefaults()
  // on every app and re-default global_settings *before* asking ConfirmReset,
  // so a "no" at the prompt could win back no more than whatever the last
  // SaveAppData() had written; the question has to be asked while the state
  // it protects still exists.
  // -------------------------------------------------------------------------

  const bool runtime_call = s_init_has_run;

  // reset_settings=true at runtime comes only from Setup's FactoryReset(),
  // which fronts its own arm+confirm screen -- asking ConfirmReset() again
  // here would be a second prompt for one decision, and "no" the second time
  // after "ERASE" the first is not a flow anyone designed. At boot the same
  // flag comes from the A+B splash gesture, which is a request, not a
  // confirmation: that path keeps the prompt below.
  const bool caller_confirmed = reset_settings && runtime_call;

  // Find out whether storage holds valid settings, touching no live state.
  // Skipped once a reset is already confirmed: the answer couldn't change it.
  bool stored_valid = false;
#ifdef __IMXRT1062__
  // Peeking the PhzConfig map is a pure read -- but only of whatever file is
  // IN the map. At boot that is GLOBALS.CFG. At runtime it is whichever file
  // was touched last: CAPTAIN.DAT after Captain's Suspend, a bank after
  // Quadrants, a slot section after a bus recall. Peeking one of those finds
  // no METADATA_KEY, which read as "no stored settings", which went down the
  // defaults branch, whose SaveGlobalSettings() then loaded the REAL
  // GLOBALS.CFG and overwrote it with factory values. A Backup restore, run
  // in the wrong app order, silently reset the user's scales and MIDI maps.
  // Loading the globals file first costs one file read and makes the answer
  // about storage, not about what happened to be in RAM.
  uint64_t metadata = 0;
  bool have_metadata = false;
  if (!caller_confirmed) {
    if (runtime_call) PhzConfig::load_config();
    have_metadata = PhzConfig::getValue(METADATA_KEY, metadata);
    if (have_metadata)
      stored_valid = Unpack(metadata, PackLocation{17, 1}); // v2.0 first-run bit
  }
#else
  // Teensy 3.x has no cheap validity peek: only PageStorage::Load can tell,
  // and it fills global_settings as a side effect. That is still observation,
  // not destruction: Load leaves the struct untouched on failure, and on
  // success writes exactly what the restore branch below would apply anyway.
  bool gs_loaded = false;
  if (!caller_confirmed) {
    APPS_SERIAL_PRINTLN("Load global settings: size: %u, PAGESIZE=%u, PAGES=%u, LENGTH=%u",
                  sizeof(GlobalSettings),
                  GlobalSettingsStorage::PAGESIZE,
                  GlobalSettingsStorage::PAGES,
                  GlobalSettingsStorage::LENGTH);
    gs_loaded = global_settings_storage.Load(global_settings);
    if (!gs_loaded)
      APPS_SERIAL_PRINTLN("Settings invalid, using defaults!");
    // .valid rides inside the stored struct; gs_loaded alone is not enough
    // because a failed load leaves whatever the field already held.
    stored_valid = gs_loaded && global_settings.valid;
  }
#endif

  // Two different decisions that used to be one flag:
  //
  //   do_reset  -- start from factory defaults rather than stored settings
  //   do_erase  -- WIPE storage: blank the EEPROM and format the filesystem
  //
  // Conflating them meant that a module which simply had no config to load --
  // a first run, or storage lost to an interrupted save -- reformatted its
  // filesystem and took all thirty presets with it. "I have nothing to load"
  // and "destroy what is there" are not the same statement. Only a user who
  // explicitly asked for a reset gets the erase.
  bool do_reset;
  bool do_erase = false;
  if (caller_confirmed) {
    do_reset = do_erase = true;
  } else if (!stored_valid) {
    // Checked BEFORE reset_settings, deliberately.
    //
    // The caller passes `reset_settings || firstrun` (Main.cpp), so a module
    // with no valid config arrives here with the flag already true and cannot
    // be told apart from a user who asked for a reset. stored_valid is the
    // honest discriminator, and when it is false there is nothing to protect:
    // asking "may I discard your settings?" when there are demonstrably none
    // strands anyone whose storage failed behind a prompt only a physical
    // button can answer. That is not hypothetical -- a reset landing inside
    // save_config's rename window took GLOBALS.CFG and GLOBALS.BAK together,
    // and the module came up unreachable on a bench with no hands near it.
    //
    // Defaults, and NO erase: there is nothing to wipe, and eraseFiles() on
    // T4.1 is a full format that would take every preset with it.
    SERIAL_PRINTLN("No stored settings; starting from defaults");
    do_reset = true;
  } else if (reset_settings) {
    // A real request, with real settings to lose. This one still asks.
    do_reset = do_erase = ui.ConfirmReset();
  } else {
    do_reset = false;
  }

  if (!do_reset && runtime_call && !stored_valid) {
    // A reset declined while the instrument is running, with nothing valid in
    // storage to load (a botched Backup restore is how you get here): the only
    // state worth anything is what is already live, so leave all of it alone.
    // Falling through would re-default every app and then find nothing to
    // restore -- the wipe the user just refused, minus the EEPROM write.
    return true;
  }

  // -------------------------------------------------------------------------
  // Phase 2: ACT. Exactly one of two things happens below: reset to defaults
  // and erase storage, or seed defaults and restore from storage. Both
  // branches share the seeding pass because apps expect their fields
  // initialised before Restore() runs, and ApplyAppData() re-runs
  // InitDefaults() only on a chunk *underflow* -- an app whose chunk is
  // absent from storage must arrive there already at defaults.
  // -------------------------------------------------------------------------

  app_container.for_each([](RuntimeSlot app) {
    APPS_SERIAL_PRINTLN("> %s", static_cast<AppBase *>(app.instance)->name());
    app.InitDefaults(app.instance);
  });

  current_app_ = app_container[DEFAULT_APP_INDEX];

  SERIAL_PRINTLN("[App Initializations]");

  Scales::Init();
  HS::Init();
#ifndef NO_HEMISPHERE
  HS::showhide_cursor.Init(0, HEMISPHERE_AVAILABLE_APPLETS - 1);
#endif
  HS::frame.Init();
  memset(HS::user_turing_machines, 0, sizeof(HS::user_turing_machines));

  bool gs_restored = false;
  if (do_reset) {
    SeedGlobalSettingsDefaults();
    if (do_erase) {
      // Only ever on an explicit request. This blanks ~3.9 KB of emulated
      // EEPROM a byte at a time and then formats the filesystem, which takes
      // long enough that the module looks dead while it runs -- and on T4.1
      // eraseFiles() is fs.format(), so it destroys every preset too.
      APPS_SERIAL_PRINTLN("Erase EEPROM ...");
      EEPtr d = EEPROM_GLOBALSETTINGS_START;
      size_t len = EEPROMStorage::LENGTH - EEPROM_GLOBALSETTINGS_START;
      while (len--)
        *d++ = 0;
      APPS_SERIAL_PRINTLN("...done");
#ifdef __IMXRT1062__
      PhzConfig::eraseFiles();
#else
      global_settings_storage.Init();
#endif
    }
    // ALWAYS, erase or not: this initialises the page-storage structure, it
    // does not wipe user data. Gating it on do_erase left the defaults path
    // with an uninitialised app_data_storage for the restore that follows.
    app_data_storage.Init();
    APPS_SERIAL_PRINTLN("Using defaults...");
    global_settings.valid = true;
    SaveGlobalSettings();
    // Defaults means the default app too; the engine's power-down record
    // would otherwise bring back the app (and slot) from before the reset.
    PresetEngine::ForgetCurrent();
    // gs_restored stays false: a confirmed reset reports firstrun so the
    // caller shows the welcome splash over factory defaults.
  } else {
#ifdef __IMXRT1062__
    SeedGlobalSettingsDefaults();
    if (have_metadata) {
      global_settings.current_app_id = Unpack(metadata, PackLocation{0, 16});
      global_settings.encoders_enable_acceleration = Unpack(metadata, PackLocation{16, 1});
      global_settings.valid = Unpack(metadata, PackLocation{17, 1});
      global_settings.invert_display = Unpack(metadata, PackLocation{18, 1});
      gs_restored = global_settings.valid;
      // 15 bits empty...
      // TODO:
      //global_settings.DAC_scaling = Unpack(metadata, PackLocation{32, 32});
      //OC::DAC::restore_scaling(global_settings.DAC_scaling);
    }
    // The app on screen at power-down beats the one GLOBALS.CFG names: the
    // file is only rewritten by the app menu's long press (and Setup), the
    // engine's record by every switch. Before set_current_app below, so the
    // first RESUME is already the right app and nothing is resumed twice.
    PresetEngine::BootAppChoice(&global_settings.current_app_id);

    // User Scales from SD Scala files take precedence over config values
    char filename[] = "000.SCL";
    uint8_t scala_loaded_mask = 0;
    for (size_t i = 0; i < Scales::SCALE_USER_COUNT; ++i) {
      if (SDcard_Ready && SD.exists(filename)) {
        filename[2] = char('0' + i);
        File file = SD.open(filename);
        if (file) {
          Scales::LoadScala(user_scales[i], file);
          scala_loaded_mask |= (1 << i);
        }
        file.close();
      }
    }

    if (global_settings.valid) {
      RestoreGlobalSettingsFromConfig(scala_loaded_mask);
    }


#else // Teensy 3.2
    // global_settings already holds the stored struct: the decide phase ran
    // PageStorage::Load to learn validity. global_settings.Init() must NOT
    // run over it here -- it would reset the autotune calibration member the
    // load just filled (the old ordering got away with Init-then-Load; this
    // ordering would be Load-then-clobber). Chords::Init() is the one piece
    // of live-array seeding Init() also did that nothing below repeats on
    // every build, so it runs by itself.
    if (gs_loaded) {
      Chords::Init();
      gs_restored = true;
      APPS_SERIAL_PRINTLN("Loaded settings from page_index %d, current_app_id is %02x",
                    global_settings_storage.page_index(),global_settings.current_app_id);
      memcpy(user_scales, global_settings.user_scales, sizeof(user_scales));
      memcpy(user_patterns, global_settings.user_patterns, sizeof(user_patterns));
#ifdef ENABLE_APP_CHORDS
      memcpy(user_chords, global_settings.user_chords, sizeof(user_chords));
#else
      memcpy(HS::user_turing_machines, global_settings.user_turing_machines, sizeof(HS::user_turing_machines));
#endif
#ifndef NO_HEMISPHERE
      memcpy(HS::user_waveforms, global_settings.user_waveforms, sizeof(HS::user_waveforms));
#endif

      // restore q_engines and midi_maps
      for (int i = 0; i < QUANT_CHANNEL_COUNT; ++i) {
        HS::q_engine[i].scale     = global_settings.q_engines[i].scale;
        HS::q_engine[i].mask      = global_settings.q_engines[i].mask;
        HS::q_engine[i].octave    = global_settings.q_engines[i].octave;
        HS::q_engine[i].root_note = global_settings.q_engines[i].root_note;
        HS::q_engine[i].Reconfig();
      }
      for (int i = 0; i < MIDIMAP_MAX; ++i) {
        HS::frame.MIDIState.mapping[i].apply_settings(global_settings.midi_maps[i]);
      }
      HS::frame.MIDIState.UpdateMidiChannelFilter();
      HS::frame.MIDIState.UpdateMaxPolyphony();
    } else {
      // Load failed (and the prompt was declined, or this is a quiet boot
      // with blank EEPROM): nothing stored, so factory-default the globals.
      SeedGlobalSettingsDefaults();
    }
#endif

    // old school EEPROM storage for legacy apps
    APPS_SERIAL_PRINTLN("Load app data: size is %u, PAGESIZE=%u, PAGES=%u, LENGTH=%u",
                  sizeof(AppData),
                  AppDataStorage::PAGESIZE,
                  AppDataStorage::PAGES,
                  AppDataStorage::LENGTH);

    if (!app_data_storage.Load(app_data)) {
      APPS_SERIAL_PRINTLN("Data not loaded, using defaults!");
    } else {
      RestoreAppData();
    }
  }

  // Validation to guard against junk data
  Chords::Validate();
  Scales::Validate();
#ifndef NO_HEMISPHERE
  WaveformManager::Validate();
#endif
  for (int i = 0; i < HS::TURING_MACHINE_COUNT; ++i) {
    HS::user_turing_machines[i].Validate();
  }

  size_t current_app_index = app_container.IndexOfAppByID(global_settings.current_app_id);
  if (current_app_index >= app_container.num_apps()) {
    APPS_SERIAL_PRINTLN("App id %02x not found, using default!", global_settings.current_app_id);
    current_app_index = DEFAULT_APP_INDEX;
  }

  APPS_SERIAL_PRINTLN("Encoder acceleration: %s", global_settings.encoders_enable_acceleration ? "enabled" : "disabled");
  ui.encoders_enable_acceleration(global_settings.encoders_enable_acceleration);
  display::SetInverted(global_settings.invert_display);

  set_current_app(current_app_index);

  delay(100);

  s_init_has_run = true;
  return gs_restored;
}

FLASHMEM
void draw_save_message(uint8_t c) {
  GRAPHICS_BEGIN_FRAME(true);
  uint8_t _size = c % 120;
  graphics.setPrintPos(37, 18);
  graphics.print("Saving...");
  graphics.drawRect(0, 28, _size, 8);
  GRAPHICS_END_FRAME();
}

FLASHMEM
bool Ui::AppSettings(bool drawmenu) {
  static menu::ScreenCursor<5> cursor;
  static bool change_app = false;
  static bool save = false;
  static bool opened = false;
  static bool cancel = false;
  static bool encoder_r_held = false;
  static bool accel_notice = false;
  static elapsedMillis accel_notice_time;

  // Long enough to read the word after looking down at the panel, short enough
  // that the legend it covers is back before the next thing you try.
  static constexpr uint32_t kAccelNoticeMs = 2000;

  // --- state change: entering App Menu
  if (!opened) {
    cursor.Init(0, app_container.num_apps() - 1);
    cursor.Scroll(app_container.IndexOfAppByID(global_settings.current_app_id));
    opened = true;
  }

  // View - graphics
  if (drawmenu) {
    // The list runs on a 10px pitch rather than menu::kMenuLineH's 12. Five
    // apps then end at y=49 and the bottom row is free for the legend below --
    // which is the only thing on this screen that tells a newcomer what any of
    // these unlabelled controls do. (The 12px pitch filled the screen and left
    // four behaviours with zero labels between them.)
    static constexpr weegfx::coord_t kAppLineH = 10;
    static constexpr weegfx::coord_t kBarX = menu::kIndentDx + 8;
    static constexpr weegfx::coord_t kNameX = kBarX + menu::kIndentDx + weegfx::Graphics::kFixedFontW;

    // assumes this is called from within a graphics frame context
    if (global_settings.encoders_enable_acceleration)
      graphics.drawBitmap8(120, 1, 4, bitmap_indicator_4x8);

    weegfx::coord_t y = 0;
    for (int current = max(cursor.first_visible(), 0);
         current <= cursor.last_visible() && current < (int)app_container.num_apps();
         ++current, y += kAppLineH) {
      // todo: make a secret button combo to switch to boring names
      // if (your_mom_is_boring)
      //   graphics.print(app_container[current]->boring_name());
      // else
      const char *name = app_container[current].name();
      graphics.setPrintPos(kNameX, y + 1);
      graphics.print(name);
      if (global_settings.current_app_id == app_container[current].id())
        graphics.drawBitmap8(0, y + 1, 8, ZAP_ICON);

      if (current == cursor.cursor_pos()) {
        // The selection bar used to run to x=127 regardless: 118px of the
        // screen's brightest object for a name that needs 66, so nearly half of
        // it was blank. Hug the text instead -- the bar means "this one", and a
        // bar the width of the name says that without shouting.
        weegfx::coord_t w = (kNameX - kBarX)
                          + weegfx::Graphics::kFixedFontW * (weegfx::coord_t)strlen(name)
                          + menu::kIndentDx;
        if (kBarX + w > menu::kDisplayWidth) w = menu::kDisplayWidth - kBarX;
        graphics.invertRect(kBarX, y, w, kAppLineH - 1);
      }
    }

    // The legend. The two things nobody can guess are that encR picks and that
    // HOLDING encR picks *and writes app data to EEPROM* -- the main way state
    // is persisted here, and previously undocumented anywhere on the module.
    // The hold hint takes the line over while encR is down, which is the one
    // moment it is actionable.
    graphics.drawHLine(0, 51, menu::kDisplayWidth);
    graphics.setPrintPos(2, 54);
    if (accel_notice && accel_notice_time < kAccelNoticeMs)
      graphics.print(global_settings.encoders_enable_acceleration
                     ? "Encoder accel: ON" : "Encoder accel: OFF");
    else if (encoder_r_held)
      graphics.print("keep holding to save");
    else
      graphics.print("encR:pick  encL:back");

#ifdef VOR
    VBiasManager *vbias_m = vbias_m->get();
    vbias_m->DrawPopupPerhaps();
#endif

    return true;
  }

  // UI - event handling
  if (!change_app && !cancel && idle_time() < APP_SELECTION_TIMEOUT_MS) {
    while (event_queue_.available()) {
      UI::Event event = event_queue_.PullEvent();
      if (IgnoreEvent(event))
        continue;

      switch (event.control) {
      case CONTROL_ENCODER_R:
        if (UI::EVENT_ENCODER == event.type)
          cursor.Scroll(event.value);
        break;

      case CONTROL_BUTTON_R:
        save = event.type == UI::EVENT_BUTTON_LONG_PRESS;
        change_app = event.type != UI::EVENT_BUTTON_DOWN; // true on button release
        encoder_r_held = UI::EVENT_BUTTON_DOWN == event.type; // legend: hold = save
        break;
      case CONTROL_BUTTON_L:
        // encL is cancel everywhere else in this instrument -- the 200e app, the
        // boot menus, the IO menus -- so it is what you reach for to back out of
        // a switcher you opened by accident. It used to drop into DebugStats,
        // a blocking loop full of numbers whose only exit (encR) it never
        // stated: indistinguishable from a crash. Cancel here, and DebugStats
        // moves to a deliberate hold, since it is a service tool.
        if (UI::EVENT_BUTTON_PRESS == event.type)
            cancel = true;
        else if (UI::EVENT_BUTTON_LONG_PRESS == event.type)
            ui.DebugStats();
        break;
      case CONTROL_BUTTON_UP:
#ifdef VOR
        // VBias menu for units without Range button
        if (UI::EVENT_BUTTON_LONG_PRESS == event.type || UI::EVENT_BUTTON_DOWN == event.type) {
          VBiasManager *vbias_m = vbias_m->get();
          vbias_m->AdvanceBias();
        }
#endif
        break;
      case CONTROL_BUTTON_DOWN:
        // B is the button directly below the A you are holding to be in this
        // screen, and this toggle changes how every encoder in the instrument
        // responds, permanently, for every app. On a bare press its entire
        // feedback was a 4x8 dot in the corner -- four pixels out of 8192, with
        // no legend anywhere -- so a newcomer pressing buttons to work out how
        // to choose an app could not possibly connect cause to effect. Now it
        // wants a deliberate hold, and it answers in words on the legend line.
        if (UI::EVENT_BUTTON_LONG_PRESS == event.type) {
            bool enabled = !global_settings.encoders_enable_acceleration;
            APPS_SERIAL_PRINTLN("Encoder acceleration: %s", enabled ? "enabled" : "disabled");
            ui.encoders_enable_acceleration(enabled);
            global_settings.encoders_enable_acceleration = enabled;
            accel_notice = true;
            accel_notice_time = 0;
        }
        break;

        default: break;
      }
    }

    return true;
  }
  // else... idle time expired, or an app was selected or the menu cancelled
  // cleanup and exit
  cancel = false;
  encoder_r_held = false;
  accel_notice = false;
  event_queue_.Flush();
  event_queue_.Poke();

  // --- state change: exiting App menu
  CORE::app_isr_enabled = false;
  delay(1);

  if (change_app) {
    app_switcher.set_current_app(cursor.cursor_pos());
    FreqMeasure.end();
    OC::DigitalInputs::reInit();
    if (save) {
      SaveAppData();
      // draw message:
      int cnt = 0;
      while(idle_time() < SETTINGS_SAVE_TIMEOUT_MS)
        draw_save_message((cnt++) >> 4);
      save = false;
    }
    // The plain press still leaves every app's data unsaved, as it always
    // has; the choice of app itself is remembered either way, through the
    // engine's power-down record (a cheap EEPROM write, ~3 s later). The
    // long press wrote it to GLOBALS.CFG too, as SaveGlobalSettings always
    // has; the record wins at boot.
    PresetEngine::NoteAppOnScreen();
    change_app = false;
  }

  OC::ui.encoders_enable_acceleration(global_settings.encoders_enable_acceleration);

  // Restore state
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
  CORE::app_isr_enabled = true;

  opened = false;
  return false; // close menu
}

FLASHMEM
bool Ui::ConfirmReset() {

  SetButtonIgnoreMask();

  bool done = false;
  bool confirm = false;

  do {
    while (event_queue_.available()) {
      UI::Event event = event_queue_.PullEvent();
      if (IgnoreEvent(event))
        continue;
      if (CONTROL_BUTTON_R == event.control && UI::EVENT_BUTTON_PRESS == event.type) {
        confirm = true;
        done = true;
      } else if (CONTROL_BUTTON_L == event.control && UI::EVENT_BUTTON_PRESS == event.type) {
        confirm = false;
        done = true;
      }
    }

    GRAPHICS_BEGIN_FRAME(true);
    graphics.setPrintPos(1, 2);
    graphics.print("Setup: Reset");
    graphics.drawLine(0, 10, 127, 10);
    graphics.drawLine(0, 12, 127, 12);

    graphics.setPrintPos(1, 15);
    graphics.print("Reset application");
    graphics.setPrintPos(1, 25);
    graphics.print("settings on EEPROM?");

    graphics.setPrintPos(0, 55);
    graphics.print("[CANCEL]         [OK]");

    GRAPHICS_END_FRAME();

  } while (!done);

  return confirm;
}

FLASHMEM
void start_calibration() {
  OC::calibration_data.set_calstart();
  OC::app_switcher.set_current_app(0);
}

// Remote bench control (console 'a'): activate an app by index with the
// same suspend/switch/resume choreography the app menu and preset recall
// use. Loop context only.
FLASHMEM void SwitchToApp(size_t index) {
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SUSPEND);
  CORE::app_isr_enabled = false;
  delay(1);
  FreqMeasure.end();
  DigitalInputs::reInit();
#ifdef AUDIO_INTERFACE
  AudioNoInterrupts();
#endif
  app_switcher.set_current_app(index);
  // Same rule as the panel: the app on screen is what the next power-up
  // shows. The Orin's 'a' (Captain) has to stick across a power cycle the
  // way a menu pick does, or the bench flow depends on which preset was
  // last recalled.
  PresetEngine::NoteAppOnScreen();
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
#ifdef AUDIO_INTERFACE
  AudioInterrupts();
#endif
  CORE::app_isr_enabled = true;
  CORE::app_loop_enabled = true;
  ::MENU_REDRAW = 1;
  Serial.printf("app: %s\n", app_switcher.current_app()->name());
}

FLASHMEM void SwitchToDefaultApp() { SwitchToApp(DEFAULT_APP_INDEX); }

// Re-run AppSwitcher::Init while the instrument is running: a Backup restore
// (reload what the SysEx just wrote to EEPROM) or Setup's factory reset.
//
// Both used to call app_switcher.Init() bare, and Backup's call came from
// inside the app ISR -- ListenForSysEx runs in Process(). Init then spent
// 100+ ms in that ISR (its closing delay, plus file reads and, on the
// defaults branch, a flash write) with USB starved underneath it, ran
// InitDefaults() over the very app whose Process() it was executing inside,
// and swapped current_app_ out from under it. Neither caller sent the new app
// APP_EVENT_RESUME: Init at boot leaves that to setup(), so a runtime Init
// landed in Captain with no CAPTAIN.DAT loaded and, with audio, no output
// path built. This is the same suspend / stop ISR / switch / resume
// choreography SwitchToApp uses, with Init in the middle. Loop context only.
FLASHMEM void ReinitApps(bool reset_settings) {
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_SUSPEND);
  CORE::app_isr_enabled = false;
  delay(1);
  FreqMeasure.end();
  DigitalInputs::reInit();
#ifdef AUDIO_INTERFACE
  AudioNoInterrupts();
#endif
  app_switcher.Init(reset_settings);
  // A reset formatted the filesystem: the engine's RAM copy of the slot
  // names now describes presets that no longer exist.
  if (reset_settings) PresetEngine::Init();
  app_switcher.current_app()->DispatchAppEvent(APP_EVENT_RESUME);
#ifdef AUDIO_INTERFACE
  AudioInterrupts();
#endif
  CORE::app_isr_enabled = true;
  CORE::app_loop_enabled = true;
  ::MENU_REDRAW = 1;
}

// The container is file-static (apps/_config.h), so the console's app table
// has to be printed from here. Index is what SwitchToApp() takes; id is what
// a preset slot records, so the two can be matched against 'b' and a
// container's manifest.
size_t NumApps() { return app_container.num_apps(); }
FLASHMEM void ListApps() {
  for (size_t i = 0; i < app_container.num_apps(); ++i) {
    const RuntimeSlot &slot = app_container[i];
    Serial.printf("  %2u  %04x  %s%s\n", (unsigned)i, slot.id(), slot.name(),
                  slot.instance == app_switcher.current_app() ? "  <- current" : "");
  }
}

}; // namespace OC
