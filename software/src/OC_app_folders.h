// Folders for the app switcher.
//
// Thirty-two apps in one flat list is a scroll, not a menu: five rows visible
// at a time means the app you want is on average three screens away, and the
// list's order is registration order, which corresponds to nothing a player
// thinks about. Folders group them by WHAT THE APP DOES TO A PATCH -- the
// question you actually have when you reach for one -- and the switcher shows
// exactly one folder at a time.
//
// The model is TABS, not a tree. encL turns the folder, encR walks the apps
// inside it. There is no entering or leaving a folder, so there is no mode to
// be stuck in and no back gesture that can fail. encL's PRESS keeps meaning
// cancel, as it does everywhere else in the instrument; only its TURN is new,
// and it was previously unbound on this screen.
//
// Assignment is the player's, not ours: X and Y move the app under the cursor
// one folder forward or back, wrapping. The table below is only the starting
// arrangement.
//
// WHY 4 BITS PER APP. This mirrors HS::hidden_applets[2] exactly -- a
// uint64_t[2] indexed by position -- because that is the shipped, understood
// way this firmware stores a per-item property in bank globals. 4 bits x 32
// apps is 128 bits, the same two words, which is the whole roster with room
// for 16 folders against the 7 defined here.
//
// It is indexed by app POSITION, like hidden_applets, which means assignments
// shift if a build's app roster changes. That is the existing mechanism's
// behaviour and not a new hazard; the defaults below are keyed by app ID, so a
// module that has never been rearranged always comes up correct.

#ifndef OC_APP_FOLDERS_H_
#define OC_APP_FOLDERS_H_

#include <stdint.h>
#include <stddef.h>
#include "util/util_misc.h"

namespace OC {
namespace AppFolders {

enum Folder : uint8_t {
  FOLDER_APPLETS,
  FOLDER_PITCH,
  FOLDER_MOD,
  FOLDER_SEQ,
  FOLDER_AUDIO,
  FOLDER_SYSTEM,
  FOLDER_HIDDEN,
  FOLDER_COUNT
};

// 21 columns is the row budget and the header spends some of it on chevrons
// and a count, so these stay short enough to leave that legible.
static const char *const kFolderName[FOLDER_COUNT] = {
  "APPLETS", "PITCH", "MOD", "SEQ", "AUDIO", "SYSTEM", "HIDDEN"
};

// The starting arrangement, keyed by app ID so it survives a roster change.
//
// APPLETS is deliberately the smallest folder and deliberately first:
// Hemisphere and Quadrants are where most of this module's UI lives, and a
// two-row folder that never scrolls is the shortest possible route to them.
//
// HIDDEN is a folder like any other, not a separate mechanism -- it is simply
// last in the rotation. Nothing can be made unreachable by moving it there.
struct DefaultEntry { uint16_t app_id; uint8_t folder; };

static const DefaultEntry kDefaults[] = {
  // The applet hosts. Same UI on two pin maps; a build has one or the other.
  { TWOCCS("HS"), FOLDER_APPLETS },  // Hemisphere
  { TWOCCS("QS"), FOLDER_APPLETS },  // Quadrants

  // Anything whose job is deciding WHICH NOTE.
  { TWOCCS("QQ"), FOLDER_PITCH },    // Quantermain, 4x quantizer
  { TWOCCS("DQ"), FOLDER_PITCH },    // Meta-Q, 2x quantizer
  { TWOCCS("CQ"), FOLDER_PITCH },    // Acid Curds, chords
  { TWOCCS("HA"), FOLDER_PITCH },    // Harrington 1200, triads
  { TWOCCS("SC"), FOLDER_PITCH },    // Scale editor
  { TWOCCS("TU"), FOLDER_PITCH },    // Tuner

  // Modulation sources: envelopes, LFOs, chaos, and the stranger generators.
  { TWOCCS("EG"), FOLDER_MOD },      // Piqued, 4x EG
  { TWOCCS("PL"), FOLDER_MOD },      // Quadraturia, quadrature LFO
  { TWOCCS("LR"), FOLDER_MOD },      // Low-rents, Lorenz
  { TWOCCS("BB"), FOLDER_MOD },      // Dialectic Ping Pong, balls
  { TWOCCS("NN"), FOLDER_MOD },      // Neural Network
  { TWOCCS("AT"), FOLDER_MOD },      // Automatonnetz, vectors
  { TWOCCS("D2"), FOLDER_MOD },      // Darkest Timeline

  // Things that remember a sequence of events and play it back.
  { TWOCCS("SQ"), FOLDER_SEQ },      // Sequins, 2x sequencer
  { TWOCCS("EN"), FOLDER_SEQ },      // Enigma
  { TWOCCS("AS"), FOLDER_SEQ },      // CopierMaschine, ASR

  // Anything that handles audio rather than control voltage.
  { TWOCCS("TW"), FOLDER_AUDIO },    // Tweighty, delay loop
  { TWOCCS("SM"), FOLDER_AUDIO },    // Sampler
  { TWOCCS("BY"), FOLDER_AUDIO },    // Viznutcracker, bytebeats
  { TWOCCS("WA"), FOLDER_AUDIO },    // Wave editor
  { TWOCCS("SP"), FOLDER_AUDIO },    // Scope
  { TWOCCS("DL"), FOLDER_AUDIO },    // Delay, standalone full-screen effect
  { TWOCCS("RV"), FOLDER_AUDIO },    // Reverb, standalone full-screen effect
  { TWOCCS("BV"), FOLDER_AUDIO },    // Bungverb, standalone full-screen effect

  // The instrument looking after itself, and its doors to the outside world.
  { TWOCCS("SE"), FOLDER_SYSTEM },   // Setup/About
  { TWOCCS("C8"), FOLDER_SYSTEM },   // Calibr8or
  { TWOCCS("BU"), FOLDER_SYSTEM },   // Back It Up!
  { TWOCCS("MI"), FOLDER_SYSTEM },   // Captain MIDI
  { TWOCCS("2E"), FOLDER_SYSTEM },   // 200e Modules
  { TWOCCS("UD"), FOLDER_SYSTEM },   // USB Drive
  { TWOCCS("RF"), FOLDER_SYSTEM },   // References, voltages
  { TWOCCS("SX"), FOLDER_SYSTEM },   // Scenery, scenes

  // Starts out of the way, one turn from being brought back.
  { TWOCCS("PO"), FOLDER_HIDDEN },   // Pong
};

// An app this table does not name lands in SYSTEM rather than in HIDDEN: an
// app we failed to categorise should still be in front of the player, and a
// silent default of HIDDEN would read as the app having gone missing.
static constexpr uint8_t kFallbackFolder = FOLDER_SYSTEM;

// 4 bits per app across two words, so 32 apps. Position-indexed.
static constexpr size_t kMaxApps = 32;

struct State {
  uint64_t bits[2] = { 0, 0 };

  Folder Of(size_t index) const {
    if (index >= kMaxApps) return static_cast<Folder>(kFallbackFolder);
    const uint8_t f = (bits[index / 16] >> ((index % 16) * 4)) & 0xf;
    return f < FOLDER_COUNT ? static_cast<Folder>(f)
                            : static_cast<Folder>(kFallbackFolder);
  }

  void Set(size_t index, uint8_t folder) {
    if (index >= kMaxApps || folder >= FOLDER_COUNT) return;
    const size_t w = index / 16;
    const unsigned sh = (index % 16) * 4;
    bits[w] = (bits[w] & ~(uint64_t(0xf) << sh)) | (uint64_t(folder) << sh);
  }

  // Move one folder forward (dir=+1) or back (dir=-1), wrapping. Wrapping is
  // what makes this safe to explore: every press is one press away from being
  // undone, and no sequence of presses can reach a state that is not a folder.
  void Nudge(size_t index, int dir) {
    const int f = static_cast<int>(Of(index));
    int n = (f + dir) % FOLDER_COUNT;
    if (n < 0) n += FOLDER_COUNT;
    Set(index, static_cast<uint8_t>(n));
  }
};

// Look up an app's starting folder by its ID.
inline uint8_t DefaultFor(uint16_t app_id) {
  for (const auto &e : kDefaults)
    if (e.app_id == app_id) return e.folder;
  return kFallbackFolder;
}

}  // namespace AppFolders
}  // namespace OC

#endif  // OC_APP_FOLDERS_H_
