// ---------------------------------------------------------------------------
// xeno-sim --test-app-switch: what leaving an app does to the memory it holds
// and to the config map it shares.
//
// The Scope takes a 62 KB arena from the heap when it is resumed and gives it
// back when it is suspended. So everything that moves the instrument away from
// the Scope has to suspend it, and nothing may resume it while it is not on
// screen. The real AppBase, AppSwitcher, app menu, PresetEngine and ScopeApp.h
// run here; the allocator is measured, not assumed.
//
// Quadrants and Captain cannot be built in this simulator (see
// sim_probe_apps.h); the stand-ins keep their contract with the one PhzConfig
// map, which is what the "no data lost" checks are about.
//
// One scenario per process, because some reboot from a stored image:
//   menu-handback    a background writer hands the map back while the app menu
//                    is open on the Scope
//   recall-in-menu   a preset recall resumes the Scope under the open menu
//   refused-recall   a recall that is refused as a bad preset, same place
//   old-preset       a recall refused for its schema version, same place
//   clock-handback   the clock-routing save (HSUtils) under the open menu
//   busui-handback   the preset overlay's assignment save with the app suspended
//   switch-back      Scope -> other -> Scope, repeatedly
//   quad-bank        the Quadrants stand-in's bank across two writers
//   quad-recall      a recall with Quadrants current: its bank and its unsaved
//                    edits (a preset value and a global) must come through
//   quad-dial        the same with the bank dial moved and not confirmed
//   restage-fail     a recall whose second look at the slot fails, with the
//                    app open and with it suspended under the menu
//   scan-handback    the 200e app's scan-set save while it is suspended
//   captain-config   Captain's map and live state across a writer
//   prep-scope[-slot]  writes the image the boot scenarios start from
//   sampler-menu-held  Sampler, then the app menu held open for thousands of loop
//                    passes, then the Scope: the Sampler's ~230 KB must stay released
//   sampler-switch   the same by the console switch (no re-read of the card)
//   sampler-recall   recalls into / under the menu over a Sampler
//   sampler-reinit   AppSampler::Init run again at runtime
//   boot-menu-sampler  power-up with the menu held, Sampler current
//   boot-menu        power-up with the app menu held, into the Scope
//   boot-menu-recall the same, with a preset recalled at boot
// ---------------------------------------------------------------------------

#include "sim_appswitch.h"

#include <stdio.h>
#include <string.h>

#ifdef XENO_SIM_APPSWITCH

#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include <Arduino.h>

#include "OC_apps.h"
#include "OC_app_switcher.h"
#include "OC_ui.h"
#include "Buchla200eFwVersions.h"
#include "Buchla200eModuleTable.h"
#include "HSUtils.h"
#include "PhzConfig.h"
#include "PresetBusUI.h"
#include "PresetEngine.h"
#include "sim_host.h"
#include "sim_input.h"
#include "sim_probe_apps.h"
#include "sim_runtime.h"

#include <Audio.h>
#include <TeensyVariablePlayback.h>

// The player stand-in's counters (shim/arduino/TeensyVariablePlayback.h).
long sim_sd_block_reads = 0;
long sim_open_files = 0;
long sim_reentrant_events = 0;
namespace simtvp { int g_response_disable = 0; }

// The codec graph's two ends: a stream apiece, so the apps that wire to them
// (Sampler, Tweighty) have something to connect to.
namespace {
class SimEndStream : public AudioStream {
 public:
  SimEndStream() : AudioStream(0, nullptr) {}
  void update() override {}
};
}  // namespace
namespace OC { namespace AudioIO {
AudioStream &InputStream(int) { static SimEndStream s; return s; }
AudioStream &OutputStream() { static SimEndStream s; return s; }
}}

namespace OC {
bool SimScopeArenaLive();
AppProbeQuad *SimProbeQuad();
AppProbeCaptain *SimProbeCaptain();
}

namespace {

int g_checks = 0, g_fails = 0;
#define CHECK(cond, what) do { \
    ++g_checks; \
    if (!(cond)) { ++g_fails; printf("  FAIL %s  [%s:%d]\n", what, __FILE__, __LINE__); } \
  } while (0)

// The Scope's arena is 62 KB; "taken" and "given back" are judged with a wide
// margin either way, because the simulator itself allocates (frames, log).
constexpr long kArenaTaken = 60 * 1000;
constexpr long kArenaGone = 30 * 1000;

// Bytes the allocator has handed out and not got back, as the allocator says.
long HeapInUse() {
#ifdef __APPLE__
  malloc_statistics_t st;
  malloc_zone_statistics(nullptr, &st);
  return (long)st.size_in_use;
#else
  return (long)mallinfo().uordblks;
#endif
}

void Run(uint32_t ms) { SimRuntimeAdvanceMs(ms); }

void Tap(uint16_t control) {
  SimInputSetButton(control, true);
  Run(40);
  SimInputSetButton(control, false);
  Run(60);
}

void Turn(int detents) {
  SimInputEncoder(true, detents);
  Run(4 * (detents < 0 ? -detents : detents) + 20);
}

// A + encR, the app menu. The loop suspends the app on the way in.
void OpenMenu() {
  SimInputSetButton(OC::CONTROL_BUTTON_UP, true);
  Run(60);
  SimInputSetButton(OC::CONTROL_BUTTON_R, true);
  Run(60);
  SimInputSetButton(OC::CONTROL_BUTTON_R, false);
  Run(60);
  SimInputSetButton(OC::CONTROL_BUTTON_UP, false);
  Run(200);
}

bool InMenu() { return !strcmp(SimRuntimeScreen(), "menu"); }
bool On(const char *name) { return !strcmp(SimRuntimeAppName(), name); }

size_t IndexOf(const char *name) {
  for (size_t i = 0; i < OC::SimAppCount(); ++i)
    if (!strcmp(OC::SimAppNameAt(i), name)) return i;
  return OC::SimAppCount();
}

// The console's 'a': suspend, switch, resume. Not under test here, just how
// the scenarios put the instrument on an app.
void Go(const char *name) {
  OC::SwitchToApp(IndexOf(name));
  Run(100);
}

// From the open menu, click another app. Which side the cursor has room on, or
// whether the app is alone in its folder, is not worth modelling: try the
// neighbours (encR), then the next folder (encL), and after a click that
// picked the app it started on, open the menu again for the next try.
bool PickOther(const char *avoid) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt == 2) {
      SimInputEncoder(false, 1);
      Run(60);
    } else {
      Turn(attempt == 0 ? -1 : 1);
    }
    Tap(OC::CONTROL_BUTTON_R);
    Run(400);
    if (!On(avoid)) return true;
    OpenMenu();
  }
  return false;
}

// A preset save/recall request, run to completion.
bool Save(uint8_t slot) {
  const uint32_t ops = OC::PresetEngine::OpCount();
  OC::PresetEngine::RequestSave(slot);
  for (int i = 0; i < 20 && OC::PresetEngine::OpCount() == ops; ++i) Run(250);
  Run(100);
  return OC::PresetEngine::OpCount() != ops && OC::PresetEngine::LastSaveOk();
}
bool Recall(uint8_t slot) {
  const uint32_t ops = OC::PresetEngine::OpCount();
  OC::PresetEngine::RequestRecall(slot);
  for (int i = 0; i < 20 && OC::PresetEngine::OpCount() == ops; ++i) Run(250);
  Run(100);
  return OC::PresetEngine::OpCount() != ops;
}

// Same flip corrupt_slot.py makes: one bit in the G section's payload, the
// container's own checksum left alone. The recall refuses it as a bad preset.
bool Corrupt(uint8_t slot) {
  char name[16];
  snprintf(name, sizeof(name), "PB_%02d.PBS", slot);
  auto &files = PhzConfig::myfs.volume().files;
  if (!files.count(name)) return false;
  auto &img = files[name];
  for (int i = 0; i < 6; ++i) {
    const size_t e = 16 + 12 * i;
    if (img.size() < e + 12 || img[e] != 'G') continue;
    const uint32_t off = img[e + 4] | (img[e + 5] << 8) | (img[e + 6] << 16);
    if (img.size() <= off + 15) return false;
    img[off + 15] ^= 0x40;
    return true;
  }
  return false;
}

// A slot written by a firmware with another schema: the manifest's version
// value changed in place and the container's section sum fixed up, so only the
// engine's own schema test (and not a checksum) refuses it. As corrupt_slot.py
// --fix-sum does for a bit flip.
bool Reschema(uint8_t slot) {
  char name[16];
  snprintf(name, sizeof(name), "PB_%02d.PBS", slot);
  auto &files = PhzConfig::myfs.volume().files;
  if (!files.count(name)) return false;
  auto &img = files[name];
  for (int i = 0; i < 6; ++i) {
    const size_t e = 16 + 12 * i;
    if (img.size() < e + 12 || img[e] != 'G') continue;
    const uint32_t off = img[e + 4] | (img[e + 5] << 8) | (img[e + 6] << 16);
    const uint32_t len = img[e + 8] | (img[e + 9] << 8) | (img[e + 10] << 16);
    if (img.size() < off + len) return false;
    if (!PhzConfig::poke(&img[off], len, (8 << 8) | 0 /* the schema key */, 99)) return false;
    uint16_t sum = 0;
    for (uint32_t b = 0; b < len; ++b) sum += img[off + b];
    img[e + 2] = sum & 0xFF;
    img[e + 3] = sum >> 8;
    return true;
  }
  return false;
}

// ---- scenarios ------------------------------------------------------------

// Several apps take their buffers the first time they are opened and keep
// them (Tweighty's alone is 144 KB), which would swamp a 62 KB arena in the
// allocator numbers. One visit to every app but the Scope, then the baseline.
// The Scope stays out of it on purpose: a leak must not end up in the baseline.
long WarmBase() {
  for (const char *n : {"Tweighty", "Quadrants", "Captain MIDI", "Back It Up!", "Cal Routine",
                        "Setup/About", "200e Modules"})
    Go(n);
  return HeapInUse();
}

// Common to the leak scenarios: the Scope is open, then the menu suspends it.
void ScopeThenMenu(long base) {
  Go("Scope");
  CHECK(OC::SimScopeArenaLive(), "opening the Scope takes its arena");
  CHECK(HeapInUse() - base >= kArenaTaken, "...and the allocator agrees");
  OpenMenu();
  CHECK(InMenu() && On("Scope"), "the app menu is open on the Scope");
  CHECK(!OC::SimScopeArenaLive(), "opening the app menu suspends the Scope and frees the arena");
  CHECK(HeapInUse() - base < kArenaGone, "...and the allocator agrees");
}

void LeftBehind(const char *what, long base) {
  char buf[160];
  snprintf(buf, sizeof(buf), "%s: the Scope's arena is not on the heap after the switch", what);
  CHECK(!OC::SimScopeArenaLive(), buf);
  snprintf(buf, sizeof(buf), "%s: the allocator holds no arena after the switch", what);
  CHECK(HeapInUse() - base < kArenaGone, buf);
}

void MenuHandback() {
  const long base = WarmBase();
  ScopeThenMenu(base);

  // A background writer: SaveSlot takes the config map and hands it back to
  // the active app when it is done (PresetEngine.cpp, the end of SaveSlot).
  CHECK(Save(5), "a preset save completes under the open menu");
  CHECK(InMenu() && On("Scope"), "...and the menu is still open on the Scope");
  CHECK(!OC::SimScopeArenaLive(),
        "the writer's hand-back does not resume the suspended Scope (no arena)");
  CHECK(HeapInUse() - base < kArenaGone, "...and the allocator agrees");

  CHECK(PickOther("Scope"), "the menu picks another app");
  LeftBehind("hand-back under the menu", base);
}

void RecallInMenu() {
  const long base = WarmBase();
  Go("Scope");
  CHECK(Save(3), "a preset is saved with the Scope on screen");
  OpenMenu();
  CHECK(!OC::SimScopeArenaLive(), "the menu suspended the Scope");
  CHECK(Recall(3), "the recall runs under the open menu");
  CHECK(OC::PresetEngine::LastRecallError() == nullptr, "...and is not refused");
  CHECK(On("Scope") && InMenu(), "the recall lands in the Scope, menu still open");
  CHECK(PickOther("Scope"), "the menu picks another app");
  LeftBehind("recall under the menu", base);
}

void RefusedRecall() {
  const long base = WarmBase();
  Go("Scope");
  CHECK(Save(7), "a preset is saved with the Scope on screen");
  CHECK(Corrupt(7), "its container is damaged");
  OpenMenu();
  CHECK(!OC::SimScopeArenaLive(), "the menu suspended the Scope");
  Recall(7);
  const char *why = OC::PresetEngine::LastRecallError();
  CHECK(why && !strcmp(why, "BAD PRESET"), "the recall is refused as a bad preset");
  CHECK(InMenu() && On("Scope"), "the menu is still open on the Scope");
  CHECK(!OC::SimScopeArenaLive(), "a refused recall does not resume the suspended Scope");
  CHECK(PickOther("Scope"), "the menu picks another app");
  LeftBehind("refused recall under the menu", base);
}

void OldPreset() {
  const long base = WarmBase();
  Go("Scope");
  CHECK(Save(8), "a preset is saved with the Scope on screen");
  CHECK(Reschema(8), "its schema version is changed");
  OpenMenu();
  CHECK(!OC::SimScopeArenaLive(), "the menu suspended the Scope");
  Recall(8);
  const char *why = OC::PresetEngine::LastRecallError();
  CHECK(why && !strcmp(why, "OLD PRESET"), "the recall is refused as an old preset");
  CHECK(InMenu() && On("Scope"), "the menu is still open on the Scope");
  CHECK(!OC::SimScopeArenaLive(), "a refused recall does not resume the suspended Scope");
  CHECK(PickOther("Scope"), "the menu picks another app");
  LeftBehind("old-preset recall under the menu", base);
}

// The clock-routing save: change the setting, let ClockRoutingPump see it and
// write GLOBALS 1.5 s later (HSUtils.cpp SaveClockRouting).
void ClockWrite() {
  HS::ClockRoutingPump();  // the first call only seeds its shadow
  HS::clock_sync_jack = HS::clock_sync_jack == 1 ? 2 : 1;
  for (int i = 0; i < 2500; ++i) {
    HS::ClockRoutingPump();
    Run(1);
  }
}

void ClockHandback() {
  Go("Scope");
  OpenMenu();
  CHECK(!OC::SimScopeArenaLive(), "the menu suspended the Scope");
  ClockWrite();
  CHECK(InMenu() && !OC::SimScopeArenaLive(),
        "the clock-routing save's hand-back does not resume the suspended Scope");
  CHECK(PickOther("Scope"), "the menu picks another app");
  Go("Captain MIDI");
  AppProbeCaptain *c = OC::SimProbeCaptain();
  OpenMenu();
  CHECK(InMenu(), "the app menu is open on Captain");
  const int resumed = c->n.resume, reloaded = c->n.reload;
  ClockWrite();
  CHECK(c->n.resume == resumed, "...nor a suspended Captain");
  CHECK(c->n.reload == reloaded + 1, "...which gets the reload event instead");
}

// The preset overlay saves its TR assignments when it closes. The overlay only
// opens over a running app, so the app is suspended by hand here: this checks
// the hand-back call in PresetBusUI, not a gesture a person can make.
void BusUiHandback() {
  Go("Captain MIDI");
  AppProbeCaptain *c = OC::SimProbeCaptain();
  OC::app_switcher.current_app()->DispatchAppEvent(OC::APP_EVENT_SUSPEND);
  const int resumed = c->n.resume, reloaded = c->n.reload;
  SimInputSetButton(OC::CONTROL_BUTTON_L, true);
  Run(20);
  SimInputSetButton(OC::CONTROL_BUTTON_R, true);
  Run(80);
  SimInputSetButton(OC::CONTROL_BUTTON_L, false);
  SimInputSetButton(OC::CONTROL_BUTTON_R, false);
  Run(200);
  CHECK(OC::PresetBusUI::Active(), "the preset overlay is open");
  SimInputEncoder(false, 1);  // encL: cursor to the next-slot jack row
  Run(60);
  SimInputEncoder(false, 1);
  Run(60);
  Turn(1);                    // encR: assign TR1
  Tap(OC::CONTROL_BUTTON_UP);  // A closes the overlay, and saves
  Run(300);
  CHECK(!OC::PresetBusUI::Active(), "the overlay closed");
  CHECK(c->n.resume == resumed, "the assignment save does not resume a suspended app");
  CHECK(c->n.reload == reloaded + 1, "...it gets the reload event instead");
}

static int g_hook_slot = 0;
void DamageSlotNow() { Corrupt((uint8_t)g_hook_slot); }

// The recall looks at the slot twice, before and after the outgoing app's
// Suspend. Damage it in between (from the stand-in's Suspend): the second look
// fails, and the app must come back the way it was found.
void RestageFail() {
  Go("Captain MIDI");
  AppProbeCaptain *c = OC::SimProbeCaptain();
  c->on_recall_suspend = DamageSlotNow;

  CHECK(Save(9), "a preset is saved with Captain on screen");
  g_hook_slot = 9;
  int resumed = c->n.resume;
  Recall(9);
  const char *why = OC::PresetEngine::LastRecallError();
  CHECK(why && !strcmp(why, "BAD PRESET"), "the slot goes bad between the two looks: refused");
  CHECK(c->n.resume == resumed + 1, "an app that was open is reopened");

  CHECK(Save(10), "a preset is saved again");
  OpenMenu();
  CHECK(InMenu() && !OC::app_switcher.current_app()->is_open(), "the menu suspended Captain");
  g_hook_slot = 10;
  resumed = c->n.resume;
  Recall(10);
  why = OC::PresetEngine::LastRecallError();
  CHECK(why && !strcmp(why, "BAD PRESET"), "refused again, under the open menu");
  CHECK(c->n.resume == resumed && !OC::app_switcher.current_app()->is_open(),
        "an app that was suspended stays suspended");
  c->on_recall_suspend = nullptr;
}

// The 200e app saves its scan set from its own Loop, which keeps running under
// the app menu: a firmware version frame arriving then is enough. It hands the
// map back to itself, suspended.
void ScanHandback() {
  Go("200e Modules");
  OpenMenu();
  CHECK(InMenu() && !OC::app_switcher.current_app()->is_open(), "the menu suspended the 200e app");
  auto &files = PhzConfig::myfs.volume().files;
  const std::vector<uint8_t> before = files.count("GLOBALS.CFG") ? files["GLOBALS.CFG"]
                                                                  : std::vector<uint8_t>();
  const uint8_t ver[] = {'9', '.', '9', 'Z'};
  Buchla200eFwVersionNote(Buchla200eModuleAt(0)->addr, ver, sizeof(ver));
  Run(500);
  CHECK(files.count("GLOBALS.CFG") && files["GLOBALS.CFG"] != before,
        "a version frame under the menu makes the app save its scan set");
  CHECK(InMenu() && !OC::app_switcher.current_app()->is_open(),
        "...and the hand-back does not reopen the suspended app");
}

void SwitchBack() {
  const long base = WarmBase();

  for (int round = 0; round < 3; ++round) {
    Go("Scope");
    CHECK(OC::SimScopeArenaLive() && HeapInUse() - base >= kArenaTaken,
          "switching to the Scope takes the arena");
    OpenMenu();
    CHECK(PickOther("Scope"), "the menu picks another app");
    LeftBehind("leaving the Scope by the menu", base);
    Go("Scope");
    CHECK(OC::SimScopeArenaLive(), "coming back to the Scope takes the arena again");
    Go("Tweighty");
    LeftBehind("leaving the Scope by the console switch", base);
  }
  // Cancelling the menu is a RESUME of the same app, not a switch.
  Go("Scope");
  OpenMenu();
  Tap(OC::CONTROL_BUTTON_L);
  Run(400);
  CHECK(!InMenu() && On("Scope") && OC::SimScopeArenaLive(),
        "cancelling the menu resumes the Scope with its arena");
  Go("Tweighty");
  LeftBehind("after a cancelled menu", base);
}

// Quadrants: a bank of four presets, one edited and unsaved when the menu opens.
constexpr const char *kBank = "BANK_000.DAT";
uint16_t PresetKey(int p) { return (uint16_t)((p << 11) | probe::kLiveKey); }

void SeedBank() {
  PhzConfig::clear_config();
  for (int p = 0; p < 4; ++p) PhzConfig::setValue(PresetKey(p), 1000 + p);
  PhzConfig::setValue(probe::kGlobKey, 5);
  PhzConfig::save_config(kBank);
}

bool BankHolds(const uint64_t want[4]) {
  PhzConfig::load_config(kBank);
  for (int p = 0; p < 4; ++p) {
    uint64_t v = 0;
    if (!PhzConfig::getValue(PresetKey(p), v) || v != want[p]) return false;
  }
  return true;
}

// What this guards: the bank file after two writers under the menu. Main passes
// it too (its bare RESUME re-owns the map); it fails if the hand-back is dropped
// for a suspended app instead of becoming a reload. The check that is red on
// main is quad-recall, and the reload-event counts here.
void QuadBank() {
  SeedBank();
  Go("Quadrants");
  AppProbeQuad *q = OC::SimProbeQuad();
  CHECK(q->live == 1000, "Quadrants opens on preset 0 of its bank");
  q->live = 777;  // an edit nobody has saved
  OpenMenu();
  CHECK(InMenu() && q->n.suspend == 1, "the menu suspends Quadrants (its auto-save)");
  const int resumed = q->n.resume;

  CHECK(Save(4), "a preset save runs under the open menu (the writer)");
  CHECK(q->n.resume == resumed, "the writer's hand-back does not resume Quadrants");
  CHECK(q->n.reload == 1, "...it gets the reload event, once, and takes its bank back");
  uint64_t v = 0;
  CHECK(PhzConfig::getValue(PresetKey(1), v) && v == 1001,
        "the shared map holds Quadrants' bank again");

  // The next suspend stores into whatever the map holds. A recall is one.
  // The next writer's FLUSH stores into whatever the map holds. With the map
  // left as the first writer made it, that would replace the bank file.
  CHECK(Save(5), "a second save runs under the open menu");
  CHECK(q->n.reload == 2 && q->n.resume == resumed, "...and again only reloads");
  const uint64_t want[4] = {777, 1001, 1002, 1003};
  CHECK(BankHolds(want), "the bank file keeps every preset, and the edit");

  CHECK(PickOther("Quadrants"), "the menu picks another app");
  Go("Quadrants");
  CHECK(q->live == 777, "coming back, the live preset is the edited one");
  CHECK(BankHolds(want), "the bank file is unchanged by the round trip");
}

// A recall with Quadrants current. Step 1 loads the slot's globals into the
// shared map and the Suspend stores through it: the bank file must still hold
// every preset afterwards. The edits are made AFTER the save, so they exist
// only in RAM until that Suspend: the file having them proves it stored them,
// and the global still being the edited one proves nothing put the file's
// older value back into live state on the way.
void QuadRecall() {
  SeedBank();
  Go("Quadrants");
  AppProbeQuad *q = OC::SimProbeQuad();
  CHECK(q->glob == 5, "Quadrants opens with its bank's global");
  CHECK(Save(4), "a preset is saved with Quadrants on screen");
  q->live = 888;
  q->glob = 6;
  CHECK(Recall(4), "a recall runs with Quadrants current");
  CHECK(OC::PresetEngine::LastRecallError() == nullptr, "...and is not refused");
  const uint64_t want[4] = {888, 1001, 1002, 1003};
  CHECK(BankHolds(want), "the bank file keeps every preset, and the unsaved preset edit");
  uint64_t g = 0;
  CHECK(PhzConfig::getValue(probe::kGlobKey, g) && g == 6,
        "...and the unsaved global edit: the Suspend stored it, not the file's old value");
  CHECK(q->glob == 6, "...which is also the live value");
}

// The bank dial moves while the knob turns; only a confirm loads that bank. A
// recall arriving in between must save into the bank that is loaded, and leave
// the dialled one alone.
void QuadDial() {
  SeedBank();
  PhzConfig::clear_config();
  for (int p = 0; p < 4; ++p) PhzConfig::setValue(PresetKey(p), 2000 + p);
  PhzConfig::save_config("BANK_007.DAT");
  Go("Quadrants");
  AppProbeQuad *q = OC::SimProbeQuad();
  CHECK(q->loaded_bank == 0, "bank 0 is loaded");
  CHECK(Save(4), "a preset is saved with Quadrants on screen");
  q->bank_dial = 7;  // turned, not confirmed (after the save: its hand-back is a RESUME)
  q->live = 888;
  CHECK(Recall(4), "a recall runs with the dial away from the loaded bank");
  const uint64_t loaded[4] = {888, 1001, 1002, 1003};
  CHECK(BankHolds(loaded), "the loaded bank's file holds the live preset");
  PhzConfig::load_config("BANK_007.DAT");
  bool intact = true;
  for (int p = 0; p < 4; ++p) {
    uint64_t v = 0;
    if (!PhzConfig::getValue(PresetKey(p), v) || v != (uint64_t)(2000 + p)) intact = false;
  }
  uint64_t g = 0;
  CHECK(intact && !PhzConfig::getValue(probe::kGlobKey, g), "the dialled bank's file is untouched");
}

void CaptainConfig() {
  PhzConfig::clear_config();
  PhzConfig::setValue(probe::kLiveKey, 42);
  PhzConfig::save_config("CAPTAIN.DAT");
  Go("Captain MIDI");
  AppProbeCaptain *c = OC::SimProbeCaptain();
  CHECK(c->live == 42, "Captain opens on its stored setup");
  c->live = 43;
  OpenMenu();
  CHECK(InMenu() && c->n.suspend == 1, "the menu suspends Captain (it stores)");
  const int resumed = c->n.resume;

  CHECK(Save(6), "a preset save runs under the open menu (the writer)");
  CHECK(c->n.resume == resumed, "the writer's hand-back does not resume Captain");
  CHECK(c->n.reload == 1, "...it gets the reload event, once");
  uint64_t v = 0;
  CHECK(PhzConfig::getValue(probe::kLiveKey, v) && v == 43,
        "the shared map holds Captain's file again, with the stored edit");

  CHECK(PickOther("Captain MIDI"), "the menu picks another app");
  Go("Captain MIDI");
  CHECK(c->live == 43, "coming back, Captain has the edited setup");
  PhzConfig::load_config("CAPTAIN.DAT");
  CHECK(PhzConfig::getValue(probe::kLiveKey, v) && v == 43, "CAPTAIN.DAT keeps it");
}

// ---- Sampler --------------------------------------------------------------
//
// The real AppSampler runs here against a model of the vendored player (see
// shim/arduino/TeensyVariablePlayback.h): each loaded voice holds seven 4 KB
// read-ahead blocks, eight voices about 230 KB, which is what the hardware
// held after a visit. These need the SD card (--sd-card).
constexpr long kSamplerHeld = 200 * 1000;   // eight voices loaded
constexpr int kVoices = 8;

// Warm every app but the Sampler, then take the base; Sampler visits add to it.
long SamplerBase() { return WarmBase(); }

bool SamplerHolds(long base) {
  return sim_open_files == kVoices && HeapInUse() - base >= kSamplerHeld;
}
bool SamplerReleased(long base) {
  return sim_open_files == 0 && HeapInUse() - base < kArenaGone;
}

// Scope -> Sampler -> the app menu held open on it for a long time -> Scope.
// The menu draws over the Sampler and its Loop() keeps running; SUSPEND has
// released the voices and nothing may take them back. This is the hardware
// finding: ~232 KB stayed held, and the Scope then had no room for its arena.
void SamplerMenuHeld() {
  const long base = SamplerBase();
  Go("Sampler");
  Run(300);
  CHECK(On("Sampler") && SamplerHolds(base), "the Sampler loads its eight voices (about 230 KB)");
  OpenMenu();
  CHECK(InMenu() && On("Sampler"), "the app menu is open on the Sampler");
  CHECK(sim_open_files == 0, "opening the menu suspends the Sampler: no voice is held");
  const long reads = sim_sd_block_reads;
  Run(3000);   // well over a thousand loop passes under the open menu
  CHECK(SamplerReleased(base), "...and nothing reloads them while the menu stays open");
  CHECK(sim_sd_block_reads == reads, "...and no SD block was read for them");
  CHECK(PickOther("Sampler"), "the menu picks another app");
  Run(500);
  CHECK(sim_open_files == 0 && HeapInUse() - base < kSamplerHeld / 2,
        "the heap is back near its base after leaving the Sampler (the app picked may hold its own)");
  Go("Scope");
  CHECK(OC::SimScopeArenaLive(), "the Scope gets its arena afterwards");
  CHECK(HeapInUse() - base >= kArenaTaken && HeapInUse() - base < kSamplerHeld / 2,
        "...and holds only that, not the Sampler's voices");
  Go("Sampler");
  Run(300);
  CHECK(SamplerHolds(base) && !OC::SimScopeArenaLive(),
        "coming back to the Sampler reloads the voices (and the Scope let go)");
}

// The same by the console switch, which never lets the Sampler run a Loop pass
// between SUSPEND and the switch, and the release must not re-read the card.
void SamplerSwitch() {
  const long base = SamplerBase();
  for (int round = 0; round < 3; ++round) {
    Go("Sampler");
    Run(300);
    CHECK(SamplerHolds(base), "the Sampler holds its voices while open");
    const long reads = sim_sd_block_reads;
    Go("Scope");
    CHECK(SamplerReleased(base) || (sim_open_files == 0 && OC::SimScopeArenaLive()),
          "leaving by the console switch hands the voices back");
    CHECK(sim_sd_block_reads == reads, "...without reading them from the card first");
    Go("Tweighty");
    CHECK(SamplerReleased(base), "...and the heap returns to its base");
  }
}

// A recall that lands in a Sampler that is suspended under the open menu must
// leave it suspended (nothing loads), and a recall into the Sampler from
// elsewhere loads on its RESUME.
void SamplerRecall() {
  const long base = SamplerBase();
  Go("Sampler");
  Run(300);
  CHECK(Save(3), "a preset is saved with the Sampler on screen");
  Go("Tweighty");
  CHECK(SamplerReleased(base), "the Sampler is released in the background");
  CHECK(Recall(3), "a recall runs");
  Run(300);
  CHECK(On("Sampler") && SamplerHolds(base), "the recall resumes the Sampler, which loads its voices");
  OpenMenu();
  CHECK(InMenu() && sim_open_files == 0, "the menu suspends it again");
  CHECK(Recall(3), "a recall runs under the open menu");
  Run(600);
  // A recall always resumes the app it lands in, menu or not (PresetEngine
  // step 6), so the Sampler is open again and loads; picking another app from
  // the still-open menu must then suspend it and keep it released.
  CHECK(InMenu() && On("Sampler") && sim_open_files == kVoices,
        "the recall resumes the Sampler under the menu, which loads its voices");
  CHECK(PickOther("Sampler"), "the menu picks another app");
  Run(2000);
  CHECK(sim_open_files == 0 && HeapInUse() - base < kSamplerHeld / 2,
        "...and leaving it releases the voices and nothing reloads them");
}

UI::Event ButtonA() {
  return UI::Event(UI::EVENT_BUTTON_PRESS, OC::CONTROL_BUTTON_UP, 0, 0);
}

// What the Loop() gate does not cover, and the release itself.
//  - A: a playing voice has an event queued; closing its file must not let it
//    run while the file is being closed (the old stop() masked and dropped it).
//  - B: button A reaches LoadSlotFile() through StartSlot(), not through the
//    Loop() gate. With the Sampler suspended it must load nothing: the guard
//    in LoadSlotFile is the REQUIRED one. The gate in Loop() is only an
//    optimisation, so removing it alone stays green here by design.
//  - C: Init() run on a loaded Sampler hands its files back first.
void SamplerCallers() {
  const long base = SamplerBase();
  Go("Sampler");
  Run(300);
  CHECK(SamplerHolds(base), "the Sampler holds its voices");
  OC::app_switcher.current_app()->DispatchEvent(ButtonA());   // preview slot 0
  Run(50);
  CHECK(sim_open_files == kVoices, "...a voice is playing");
  OpenMenu();
  CHECK(InMenu() && sim_open_files == 0, "the menu suspends it and the voices close");
  CHECK(sim_reentrant_events == 0, "...with no queued player event run during a close");
  OC::app_switcher.current_app()->DispatchEvent(ButtonA());   // suspended: nothing may load
  Run(500);
  CHECK(sim_open_files == 0, "button A on a suspended Sampler loads nothing (LoadSlotFile's guard)");
  CHECK(PickOther("Sampler"), "the menu picks another app");
  Go("Sampler");
  Run(300);
  CHECK(sim_open_files == kVoices, "back in the Sampler the voices load again");
  OC::app_switcher.current_app()->Init();
  CHECK(sim_open_files == 0, "Init() on a loaded Sampler hands its files back");
}

// The power-up menu: encR held through the splash opens it over the Sampler,
// which setup() has resumed; picking another app must release its voices.
void PrepSampler() {
  Go("Sampler");
  Run(5000);
}
void BootMenuSampler() {
  CHECK(On("Sampler"), "the module powers up in the Sampler");
  CHECK(InMenu(), "...with the app menu open (encR held through the splash)");
  Run(3000);
  // setup() resumes the app after the splash, and nothing suspends it for a
  // menu that was already open: it is the open app, so it holds its voices
  // until the pick (set_current_app suspends it then).
  CHECK(PickOther("Sampler"), "the menu picks another app");
  Run(2000);
  CHECK(sim_open_files == 0, "...and the Sampler's voices are released and stay so");
  Go("Sampler");
  Run(300);
  CHECK(sim_open_files == kVoices, "coming back to it loads the voices");
}

// AppSampler::Init runs again at runtime (ReinitApps): it must not stack 18
// more AudioConnections on the same streams each time, nor leave a voice open.
void SamplerReinit() {
  const long base = SamplerBase();
  Go("Sampler");
  Run(300);
  OC::ReinitApps(false);
  Run(300);
  const long after_first = HeapInUse();
  for (int i = 0; i < 10; ++i) {
    OC::ReinitApps(false);
    Run(300);
  }
  CHECK(HeapInUse() - after_first < 4000, "ten more app re-inits do not grow the heap");
  CHECK(sim_open_files == 0 || sim_open_files == kVoices, "no voice is left open twice");
  Go("Tweighty");
  CHECK(SamplerReleased(base), "leaving the Sampler after re-inits still frees every voice");
}

// ---- boot -----------------------------------------------------------------

void PrepScope(bool with_slot) {
  Go("Scope");
  if (with_slot) CHECK(Save(2), "a preset is saved with the Scope on screen");
  Run(5000);  // the engine writes its power-down record a few seconds later
}

void BootMenu(bool recall) {
  // Not a scenario of its own: the image the prep run left has to say Scope.
  CHECK(On("Scope"), "the module powers up in the Scope");
  CHECK(InMenu(), "...with the app menu open (encR held through the splash)");
  if (recall) {
    Run(1500);
    CHECK(OC::PresetEngine::LastRecallError() == nullptr, "the boot recall is not refused");
    CHECK(OC::PresetEngine::LastSlot() == 2, "...and it loaded the saved slot");
  }
  CHECK(PickOther("Scope"), "the menu picks another app");
  CHECK(!OC::SimScopeArenaLive(), "the Scope's arena is not on the heap after the switch");
  Go("Scope");
  CHECK(OC::SimScopeArenaLive(), "coming back to the Scope takes the arena again");
  Go("Tweighty");
  CHECK(!OC::SimScopeArenaLive(), "and leaving it frees it");
}

}  // namespace

void SimAppSwitchPreBoot(const std::string &scenario) {
  if (scenario == "boot-menu" || scenario == "boot-menu-recall" || scenario == "boot-menu-sampler")
    SimRuntimeBootHold(OC::CONTROL_BUTTON_R);
}

int SimAppSwitchTest(const std::string &s) {
  g_checks = g_fails = 0;
  if (s == "menu-handback") MenuHandback();
  else if (s == "recall-in-menu") RecallInMenu();
  else if (s == "refused-recall") RefusedRecall();
  else if (s == "quad-dial") QuadDial();
  else if (s == "restage-fail") RestageFail();
  else if (s == "scan-handback") ScanHandback();
  else if (s == "old-preset") OldPreset();
  else if (s == "clock-handback") ClockHandback();
  else if (s == "busui-handback") BusUiHandback();
  else if (s == "quad-recall") QuadRecall();
  else if (s == "switch-back") SwitchBack();
  else if (s == "quad-bank") QuadBank();
  else if (s == "captain-config") CaptainConfig();
  else if (s == "sampler-menu-held") SamplerMenuHeld();
  else if (s == "sampler-switch") SamplerSwitch();
  else if (s == "sampler-recall") SamplerRecall();
  else if (s == "sampler-reinit") SamplerReinit();
  else if (s == "sampler-callers") SamplerCallers();
  else if (s == "prep-sampler") PrepSampler();
  else if (s == "boot-menu-sampler") BootMenuSampler();
  else if (s == "prep-scope") PrepScope(false);
  else if (s == "prep-scope-slot") PrepScope(true);
  else if (s == "boot-menu") BootMenu(false);
  else if (s == "boot-menu-recall") BootMenu(true);
  else {
    fprintf(stderr, "xeno-sim: no app-switch scenario \"%s\"\n", s.c_str());
    return 2;
  }
  printf("app-switch %s: %d checks, %d failures\n", s.c_str(), g_checks, g_fails);
  return g_fails ? 1 : 0;
}

#else  // !XENO_SIM_APPSWITCH

void SimAppSwitchPreBoot(const std::string &) {}
int SimAppSwitchTest(const std::string &) {
  fprintf(stderr, "xeno-sim: this binary was built without the app-switch roster; "
                  "use build-switch/xeno-sim (make check builds it)\n");
  return 2;
}

#endif
