// workaround
namespace menu = OC::menu;

// For the kMaxApps assertion below the container. OC_apps.cpp includes this
// file before OC_app_folders.h, so the assert cannot wait for that one.
#include "../OC_app_folders.h"

// Quadrants is the applet host on this hardware. The old 2-up host, which
// drew two applets side by side on the four-button panel, was deleted in the
// 2026-09-13 hard fork: it only ever built for non-T4.1 targets this fork no
// longer has.
#ifndef NO_HEMISPHERE
#include "Quadrants.h"
#endif

#include "Calibr8or.h"
#include "ASR.h"
#ifdef ENABLE_APP_H1200
#include "H1200.h"
#endif
#ifdef ENABLE_APP_AUTOMATONNETZ
#include "Automatonnetz.h"
#endif
#include "Sequins.h"
#include "QQ.h"
#include "DQ.h"
#include "Quadraturia.h"
#include "Lorenz.h"
#include "Piqued.h"
#include "BBGEN.h"
#include "Viznutcracker.h"
#include "Chords.h"
#ifdef ENABLE_APP_REFERENCES
#include "References.h"
#endif
// #include "Passencore.h"
#include "CaptainMIDI.h"
#include "TheDarkestTimeline.h"
#include "Enigma.h"
#ifdef ENABLE_APP_NEURAL_NETWORK
#include "NeuralNetwork.h"
#endif
#include "ScaleEditor.h"
#ifdef ENABLE_APP_TUNER
#include "TunerApp.h"
#endif
#ifdef ENABLE_APP_BUS200E
#include "Bus200eApp.h"
#endif
#ifdef ENABLE_APP_TWEIGHTY
#include "TweightyApp.h"
#endif
#ifdef ENABLE_APP_SCOPE
#include "ScopeApp.h"
#endif
#ifdef ENABLE_APP_SAMPLER
#include "SamplerApp.h"
#endif
#ifdef ENABLE_APP_ARP
#include "ArpApp.h"
#endif

#ifdef ENABLE_APP_DELAY
#include "DelayApp.h"
#endif
#ifdef ENABLE_APP_REVERB
#include "ReverbApp.h"
#endif
#ifdef ENABLE_APP_BUNGVERB
#include "BungverbApp.h"
#endif
#include "Backup.h"
#include "SETTINGS.h"


namespace OC {

/*
// The order in the AppContainer is not inconsequential.
// Each app's Start() method is called in sequence.
// For example, the default quantizer settings from Hemisphere
// are overwritten when Calibr8or loads its settings
*/

// Instantiate the available apps below.
// Any type not listed here should not exist, i.e. the linker should be able to
// triage all code (minus any dangling static parts).

// RAM2 (needs the startup .bss.dma zeroing hook): 5.9KB of app instances,
// CPU-only access (no DMA), buys DTCM stack headroom so the USB host MIDI
// objects can live in non-cacheable DTCM where EHCI DMA needs them.
static DMAMEM AppContainer<void // this space intentionally left blank
  , AppSettings
#ifndef NO_HEMISPHERE
  , AppQuadrants
#endif
#ifdef ENABLE_APP_CALIBR8OR
  , AppCalibr8or
#endif
#ifdef ENABLE_APP_MIDI
  , AppCaptainMIDI
#endif
#ifdef ENABLE_APP_DARKEST_TIMELINE
  , TheDarkestTimeline
#endif
#ifdef ENABLE_APP_ENIGMA
  , AppEnigma
#endif
#ifdef ENABLE_APP_NEURAL_NETWORK
  , AppNeuralNetwork
#endif
#ifdef ENABLE_APP_PASSENCORE
  // , AppPassencore
#endif
#ifdef ENABLE_APP_ASR
  , AppASR
#endif
#ifdef ENABLE_APP_H1200
  , AppH1200
#endif
#ifdef ENABLE_APP_AUTOMATONNETZ
  , AppAutomatonnetz
#endif
#ifdef ENABLE_APP_QUANTERMAIN
  , AppQuadQuantizer
#endif
#ifdef ENABLE_APP_METAQ
  , AppDualQuantizer
#endif
#ifdef ENABLE_APP_POLYLFO
  , AppPolyLfo
#endif
#ifdef ENABLE_APP_LORENZ
  , AppLorenzGenerator
#endif
#ifdef ENABLE_APP_PIQUED
  , AppQuadEnvelopeGenerator
#endif
#ifdef ENABLE_APP_SEQUINS
  , AppDualSequencer
#endif
#ifdef ENABLE_APP_BBGEN
  , AppQuadBouncingBalls
#endif
#ifdef ENABLE_APP_BYTEBEATGEN
  , AppQuadByteBeats
#endif
#ifdef ENABLE_APP_CHORDS
  , AppChordQuantizer
#endif
#ifdef ENABLE_APP_REFERENCES
  , AppReferences
#endif
#ifdef ENABLE_APP_TUNER
  , AppTuner
#endif
#ifdef ENABLE_APP_BUS200E
  , AppBus200e
#endif
#ifdef ENABLE_APP_TWEIGHTY
  , AppTweighty
#endif
#ifdef ENABLE_APP_SCOPE
  , AppScope
#endif
#ifdef ENABLE_APP_SAMPLER
  , AppSampler
#endif
#ifdef ENABLE_APP_ARP
  , AppArp
#endif
#ifdef ENABLE_APP_DELAY
  , AppDelay
#endif
#ifdef ENABLE_APP_REVERB
  , AppReverb
#endif
#ifdef ENABLE_APP_BUNGVERB
  , AppBungverb
#endif
  , AppScaleEditor
  , AppBackup
> app_container;

static_assert(decltype(app_container)::TotalAppDataStorageSize() < AppData::kAppDataSize,
              "Apps use too much EEPROM space!");

// The app switcher files apps by position, four bits each across two words, so
// it can only see the first kMaxApps of them. An app past that never enters the
// switcher's visible list at all and would be PERMANENTLY UNREACHABLE -- and
// since the switcher is the only route between apps, a build that booted into
// one would open on SYSTEM with the running app simply not listed.
//
// The maximal roster is at 31 of 32 today; uncommenting Passencore makes it
// exactly 32. Fail the build rather than ship a silently invisible app.
static_assert(decltype(app_container)::kNumApps <= OC::AppFolders::kMaxApps,
              "More apps than the app switcher can file: raise "
              "OC::AppFolders::kMaxApps (and widen State::bits) first.");

#if   defined(DEFAULT_APP_MIDI) && defined(ENABLE_APP_MIDI) && defined(ARDUINO_TEENSY41) && !defined(NO_HEMISPHERE)
// Boots into Captain MIDI:
// [0]=AppSettings, [1]=Quadrants, [2]=Calibr8or, [3]=CaptainMIDI
// (was 4 until Scenery was deleted; the static_assert below is what
// catches this the moment the roster above the boot app changes)
static constexpr int DEFAULT_APP_INDEX = 3;
#else
static constexpr int DEFAULT_APP_INDEX = 1;
#endif
static constexpr uint16_t DEFAULT_APP_ID = decltype(app_container)::GetAppIDAtIndex<DEFAULT_APP_INDEX>();
#if defined(ENABLE_APP_MIDI) && defined(DEFAULT_APP_MIDI)
static_assert(DEFAULT_APP_ID == AppCaptainMIDI::kAppId, "DEFAULT_APP_INDEX must select Captain MIDI");
#endif

}
