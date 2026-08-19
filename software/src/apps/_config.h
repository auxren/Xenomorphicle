// workaround
namespace menu = OC::menu;

#ifndef NO_HEMISPHERE

#ifdef ARDUINO_TEENSY41
#include "Quadrants.h"
#else
#include "Hemisphere.h"
#endif

#endif

#include "Calibr8or.h"
#include "Scenery.h"
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
#include "WaveformEditor.h"
#include "PongGame.h"
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
  #ifdef ARDUINO_TEENSY41
  , AppQuadrants
  #else
  , AppHemisphere
  #endif
#endif
#ifdef ENABLE_APP_CALIBR8OR
  , AppCalibr8or
#endif
#ifdef ENABLE_APP_SCENES
  , AppScenery
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
#ifdef ENABLE_APP_PONG
  , AppPong
#endif
  , AppScaleEditor
#ifndef NO_HEMISPHERE
  , AppWaveformEditor
#endif
  , AppBackup
> app_container;

static_assert(decltype(app_container)::TotalAppDataStorageSize() < AppData::kAppDataSize,
              "Apps use too much EEPROM space!");

#if defined(NLM_hOC) && defined(ENABLE_APP_MIDI)
// hOC MIDI build boots into Captain MIDI: [0]=AppSettings, [1]=Calibr8or, [2]=CaptainMIDI
static constexpr int DEFAULT_APP_INDEX = 2;
#elif defined(DEFAULT_APP_MIDI) && defined(ENABLE_APP_MIDI) && defined(ARDUINO_TEENSY41) && !defined(NO_HEMISPHERE)
// T41 boots into Captain MIDI:
// [0]=AppSettings, [1]=Quadrants, [2]=Calibr8or, [3]=Scenery, [4]=CaptainMIDI
static constexpr int DEFAULT_APP_INDEX = 4;
#else
static constexpr int DEFAULT_APP_INDEX = 1;
#endif
static constexpr uint16_t DEFAULT_APP_ID = decltype(app_container)::GetAppIDAtIndex<DEFAULT_APP_INDEX>();
#if defined(ENABLE_APP_MIDI) && (defined(NLM_hOC) || defined(DEFAULT_APP_MIDI))
static_assert(DEFAULT_APP_ID == AppCaptainMIDI::kAppId, "DEFAULT_APP_INDEX must select Captain MIDI");
#endif

}
