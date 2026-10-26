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
//
// The frequency counter on the Scope (ScopeFreqService.h): a synthetic tone
// goes in through the CV frame hook the firmware installs
// (OC::ADC::scope_frame_hook(), what Scan_DMA calls), and the screen is read
// back from the framebuffer.
//   freq-cv-tone     a 1 kHz tone on CV1: the reading, the measurement line,
//                    the calibrated / unconfirmed marks, the clock correction
//   freq-fast        the 0.1 s update rate, and the 1 s gate
//   freq-status      NO SIGNAL, TOO QUIET, OK, UNSTABLE and SIGNAL LOST on screen
//   freq-lfo         a 1 Hz CV LFO: the low range takes over, the line says Hz
//   freq-no-arena    the arena allocation fails: the counter and its line stay
//   freq-switch      leaving the Scope stops the counter, coming back restarts
//                    it, and the clock correction survives both
//   freq-layout      the line never touches the plot: single, overlaid, stacked,
//                    CV+TR lanes and the spectrum
//   freq-fft-header  the spectrum header stays put as the peak's digits change, and
//                    only the source field is selectable there
//   freq-settings    a saved Scope from the 16-byte and 3-byte layouts is
//                    restored, the new layout carries the clock, and a save
//                    from before the calibration does not wipe a live one
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

#include <math.h>

#include <string>

#include "OC_ADC.h"
#include "OC_apps.h"
#include "OC_app_switcher.h"
#include "OC_ui.h"
#include "Buchla200eFwVersions.h"
#include "Buchla200eModuleTable.h"
#include "HSUtils.h"
#include "PhzConfig.h"
#include "PresetBusUI.h"
#include "PresetEngine.h"
#include "ScopeFreqLine.h"
#include "ScopeFreqService.h"
#include "ScopeSettings.h"
#include "ScopeSources.h"
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

#include "src/extern/gfx_font_6x8.h"

namespace OC {
void BuildAppData(AppData &data);
void ApplyAppData(const AppData &data);
void ApplyAppDataFromPreset(const AppData &data);
void ReinitApps(bool reset_settings);
}  // namespace OC

namespace OC {
bool SimScopeArenaLive();
void SimScopeAllocFails(bool fails);
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


// ---- the frequency counter ----------------------------------------------------

const double kCvFs = ScopeFreqSvc::kCvFsHz;   // 37,350.6 Hz, derived in ScopeFreqService.h

// What the framebuffer says on the measurement line (rows 56-63, one page).
// Cells are matched against the firmware's own font; a cell that is not a
// glyph comes back as '\x01'. Trailing spaces trimmed.
std::string BottomLine() {
  uint8_t fb[1024];
  SimPanelVisible(fb);
  std::string out;
  for (int cell = 0; cell < 21; ++cell) {
    const int x = 1 + cell * 6;
    char found = '\x01';
    bool blank = true;
    for (int c = 0; c < 6; ++c) if (fb[7 * 128 + x + c]) blank = false;
    if (blank) { out += ' '; continue; }
    for (int g = 0; g < 92; ++g) {
      bool same = true;
      for (int c = 0; c < 6 && same; ++c) same = fb[7 * 128 + x + c] == ssd1306xled_font6x8[g * 6 + c];
      if (same) { found = (char)(' ' + g); break; }
    }
    out += found;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Ink outside the text on the bottom page, or on row 55 (the gap): the plot
// reaching into the measurement line.
int InkOutsideLine(size_t text_len) {
  uint8_t fb[1024];
  SimPanelVisible(fb);
  int n = 0;
  for (int x = 0; x < 128; ++x) {
    if (fb[6 * 128 + x] & 0x80) ++n;                                  // y = 55
    if (x >= 1 + (int)text_len * 6 && fb[7 * 128 + x]) ++n;           // y 56-63, right of the text
    if (x < 1 && fb[7 * 128 + x]) ++n;                                // and left of it
  }
  return n;
}

int PlotInk() {
  uint8_t fb[1024];
  SimPanelVisible(fb);
  int n = 0;
  for (int y = 11; y < 55; ++y)
    for (int x = 0; x < 128; ++x) n += (fb[(y >> 3) * 128 + x] >> (y & 7)) & 1;
  return n;
}

// Advances `ms` simulated milliseconds while CV input `cv` (0-based) carries
// word(n) on frame n, the others parked. Frames come at kCvFsHz of SIMULATED
// time (a simulated millisecond is 17 core passes of 60 us, so not exactly
// 1 ms of SimNowUs), delivered the way Scan_DMA delivers them: through the
// hook the firmware installs.
template <typename F>
void RunCv(int cv, uint32_t ms, F word) {
  static uint64_t n = 0;
  static uint32_t base_us = 0;
  static bool started = false;
  if (!started) { base_us = SimNowUs(); started = true; }
  const int j = cv < 4 ? cv + 4 : cv - 4;
  for (uint32_t t = 0; t < ms; ++t) {
    Run(1);
    const uint64_t due = (uint64_t)((double)(uint32_t)(SimNowUs() - base_us) * kCvFs * 1e-6);
    OC::ADC::ScopeFrameHook hook = OC::ADC::scope_frame_hook();
    for (; n < due; ++n) {
      int16_t f[8];
      for (int k = 0; k < 8; ++k) f[k] = (int16_t)(3000 + 1000 * k);
      f[j] = (int16_t)word(n);
      if (hook) hook(f);
    }
  }
}

auto Sine(double hz, double amp, double mid = 16000.0) {
  return [=](uint64_t n) { return mid + amp * sin(2 * M_PI * (0.1 + hz * n / kCvFs)); };
}

double CentsOff(double hz, double ref) { return 1200.0 * log2(hz / ref); }

// The Scope on CV1, counting. Default settings: the quick list starts at IN L,
// which a build without the codec masks, and CV1 is the third entry on.
// One detent at a time, far enough apart that the encoder's acceleration does
// not turn them into several.
void TurnN(int detents) {
  for (int i = 0; i < detents; ++i) { Turn(1); Run(250); }
}

void ScopeOnCv1() {
  Go("Scope");
  TurnN(3);
  Run(50);
}

bool StartsWith(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }
bool Has(const std::string &s, const char *p) { return s.find(p) != std::string::npos; }

void FreqCvTone() {
  CHECK(ScopeFreqSvc::ActiveSource() == -1, "nothing is counted before the Scope is opened");
  CHECK(OC::ADC::scope_frame_hook() == nullptr, "...and no ADC frame hook is installed");
  ScopeOnCv1();
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN1, "the Scope on CV1 counts CV1");
  CHECK(ScopeFreqSvc::ActiveProfile() == ScopeFreqSvc::Profile::CV, "...in the CV profile");
  CHECK(OC::ADC::scope_frame_hook() != nullptr, "...through the ADC frame hook");
  CHECK(ScopeFreqSvc::GetGate() == ScopeFreqSvc::Gate::FAST, "the screen uses the fast gate");
  RunCv(0, 400, Sine(1000.0, 6000.0));
  const ScopeFreqLine::Reading &r = ScopeFreqSvc::CurrentReading();
  CHECK(r.have && r.status == ScopeFreq::Status::OK, "a 1 kHz tone reads OK after 0.4 s");
  CHECK(fabs(CentsOff(r.hz, 1000.0)) < 0.05, "...within 0.05 cent");
  CHECK(r.sigma_cents > 0 && r.sigma_cents < 0.05, "...with a sigma of the same order");
  CHECK(r.gate_s >= 0.1 && r.gate_s < 0.12, "...from a 0.1 s gate");
  RunCv(0, 300, Sine(1000.0, 6000.0));
  CHECK(r.confirmed, "two gates that agree confirm it");
  std::string line = BottomLine();
  printf("  line: \"%s\"\n", line.c_str());
  CHECK(StartsWith(line, "#1000.0"), "a CV rate not yet confirmed on hardware: the line starts with the # mark");
  CHECK(Has(line, "Hz B5 +21.3"), "...then the note and the cents against it");
  CHECK(line.size() <= 21, "...and fits the 21 columns");
  CHECK(InkOutsideLine(line.size()) == 0, "...and nothing else is drawn on that page");

  // the clock correction: +20.0 ppm raises the reading by 20 ppm; the mark goes
  ScopeFreqSvc::SetClock(true, 200);
  CHECK(!ScopeFreqSvc::CurrentReading().have, "a clock change starts the reading over");
  RunCv(0, 240, Sine(1000.0, 6000.0));
  line = BottomLine();
  CHECK(StartsWith(line, "?1000.0"), "the first gate after it is unconfirmed: ? mark");
  RunCv(0, 300, Sine(1000.0, 6000.0));
  line = BottomLine();
  printf("  line: \"%s\"\n", line.c_str());
  CHECK(StartsWith(line, "#1000.0"), "confirmed and calibrated, the CV rate still unconfirmed: # (the * is gone)");
  CHECK(Has(line, "Hz B5 +21."), "...");
  const double hz_cal = ScopeFreqSvc::CurrentReading().hz;
  ScopeFreqSvc::SetClock(false, 0);
  RunCv(0, 450, Sine(1000.0, 6000.0));
  const double hz_raw = ScopeFreqSvc::CurrentReading().hz;
  CHECK(fabs(hz_cal / hz_raw - 1.00002) < 5e-6, "the +20.0 ppm correction moved the reading by 20 ppm");
}

void FreqFast() {
  ScopeOnCv1();
  uint32_t last_seq = 0;
  int updates = 0;
  uint32_t first_ms = 0, prev_ms = 0, worst_gap = 0, best_gap = 100000;
  const uint32_t t0 = SimNowMs();
  for (int ms = 0; ms < 1500; ++ms) {
    RunCv(0, 1, Sine(440.0, 6000.0));
    const ScopeFreqLine::Reading &r = ScopeFreqSvc::CurrentReading();
    if (r.have && r.seq != last_seq) {
      last_seq = r.seq;
      const uint32_t now = SimNowMs() - t0;
      if (updates == 0) first_ms = now;
      else {
        const uint32_t gap = now - prev_ms;
        if (gap > worst_gap) worst_gap = gap;
        if (gap < best_gap) best_gap = gap;
      }
      prev_ms = now;
      ++updates;
    }
  }
  printf("  %d updates in 1.5 s, first at %u ms, gaps %u..%u ms\n", updates, first_ms, best_gap, worst_gap);
  CHECK(first_ms > 150 && first_ms < 300, "the first reading arrives within 0.3 s (0.1 s learning + 0.1 s gate)");
  CHECK(updates >= 11, "then one reading every ~0.1 s");
  CHECK(best_gap >= 95 && worst_gap <= 115, "...each between 95 and 115 ms apart");

  // the slow (calibration) gate: ten times coarser in time
  ScopeFreqSvc::SetGate(ScopeFreqSvc::Gate::SLOW);
  last_seq = 0; updates = 0; worst_gap = 0; best_gap = 100000;
  const uint32_t t1 = SimNowMs();
  for (int ms = 0; ms < 3400; ++ms) {
    RunCv(0, 1, Sine(440.0, 6000.0));
    const ScopeFreqLine::Reading &r = ScopeFreqSvc::CurrentReading();
    if (r.have && r.seq != last_seq) {
      last_seq = r.seq;
      const uint32_t now = SimNowMs() - t1;
      if (updates > 0) { const uint32_t gap = now - prev_ms; if (gap > worst_gap) worst_gap = gap; if (gap < best_gap) best_gap = gap; }
      prev_ms = now;
      ++updates;
    }
  }
  printf("  slow gate: %d updates in 3.4 s, gaps %u..%u ms\n", updates, best_gap, worst_gap);
  CHECK(updates >= 2 && updates <= 3, "the 1 s gate gives a reading about every second");
  CHECK(best_gap >= 995 && worst_gap <= 1015, "...a second apart");
  CHECK(ScopeFreqSvc::CurrentReading().gate_s >= 1.0, "...and the reading says so");
  ScopeFreqSvc::SetGate(ScopeFreqSvc::Gate::FAST);
}

void FreqStatus() {
  ScopeOnCv1();
  RunCv(0, 250, [](uint64_t) { return 16000.0; });
  CHECK(BottomLine() == "NO SIGNAL", "a flat input says NO SIGNAL");
  RunCv(0, 300, Sine(440.0, 30.0));
  CHECK(BottomLine() == "TOO QUIET", "a tone under the threshold says TOO QUIET");
  CHECK(ScopeFreqSvc::ActiveProfile() == ScopeFreqSvc::Profile::CV, "(a flat or tiny input does not leave the CV range)");
  RunCv(0, 450, Sine(440.0, 6000.0));
  std::string line = BottomLine();
  printf("  line: \"%s\"\n", line.c_str());
  CHECK(Has(line, "Hz A4 "), "a tone of 6000 counts reads as A4");
  CHECK(line[0] == '#' || line[0] == '?', "...marked, the CV rate being unconfirmed");
  // heavy noise on the same tone: counted wrong, so refused
  uint32_t seed = 12345;
  auto noisy = [&](uint64_t n) {
    seed = seed * 1664525u + 1013904223u;
    return 16000.0 + 3000.0 * sin(2 * M_PI * (0.1 + 440.0 * n / kCvFs)) + ((seed >> 8) % 6000) - 3000.0;
  };
  RunCv(0, 700, noisy);
  line = BottomLine();
  printf("  line: \"%s\"\n", line.c_str());
  CHECK(!(line[0] >= '0' && line[0] <= '9') && line[0] != '#',
        "noise as big as the signal is never shown as a good reading");
  // the tone stops
  RunCv(0, 350, [](uint64_t) { return 16000.0; });
  line = BottomLine();
  CHECK(line == "SIGNAL LOST" || line == "NO SIGNAL", "a tone that stops says SIGNAL LOST or NO SIGNAL");
  // and comes back, at another pitch
  RunCv(0, 800, Sine(523.25, 6000.0));
  line = BottomLine();
  CHECK(Has(line, "Hz C5 "), "...and the next tone is read at its own pitch");
}

void FreqLfo() {
  ScopeOnCv1();
  auto tri = [](uint64_t n) {
    const double ph = 0.1 + 1.0 * n / kCvFs;
    return 16000.0 + 5000.0 * (4.0 * fabs(ph - floor(ph + 0.5)) - 1.0);
  };
  int ms = 0;
  while (ms < 12000 && !(ScopeFreqSvc::ActiveProfile() == ScopeFreqSvc::Profile::CV_LOW &&
                         ScopeFreqSvc::CurrentReading().status == ScopeFreq::Status::OK)) {
    RunCv(0, 100, tri);
    ms += 100;
  }
  printf("  1 Hz LFO read after %d ms\n", ms);
  CHECK(ScopeFreqSvc::ActiveProfile() == ScopeFreqSvc::Profile::CV_LOW, "a 1 Hz LFO moves the counter to the low range");
  const ScopeFreqLine::Reading &r = ScopeFreqSvc::CurrentReading();
  CHECK(r.status == ScopeFreq::Status::OK && fabs(r.hz - 1.0) < 0.002, "...and reads 1.000 Hz");
  CHECK(r.gate_s >= 2.0, "...from a 2 s gate");
  const std::string line = BottomLine();
  printf("  line: \"%s\"\n", line.c_str());
  CHECK(Has(line, "1.0") && Has(line, "Hz") && !Has(line, "A") && !Has(line, "B"), "an LFO is shown in Hz, with no note name");
}

void FreqNoArena() {
  OC::SimScopeAllocFails(true);
  Go("Scope");
  TurnN(3);
  Run(50);
  CHECK(!OC::SimScopeArenaLive(), "the arena allocation fails");
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN1, "...and the counter still follows the view");
  CHECK(OC::ADC::scope_frame_hook() != nullptr, "...with the frame hook installed for it alone");
  RunCv(0, 700, Sine(1000.0, 6000.0));
  const ScopeFreqLine::Reading &r = ScopeFreqSvc::CurrentReading();
  CHECK(r.have && r.status == ScopeFreq::Status::OK && r.confirmed, "it reads the tone");
  const std::string line = BottomLine();
  printf("  line: \"%s\"\n", line.c_str());
  CHECK(Has(line, "Hz B5 "), "and the line is on screen beside \"not enough memory\"");
  // a freeze keeps counting
  OC::SimScopeAllocFails(false);
  Tap(OC::CONTROL_BUTTON_A);
  const uint32_t seq = ScopeFreqSvc::CurrentReading().seq;
  RunCv(0, 300, Sine(1000.0, 6000.0));
  CHECK(ScopeFreqSvc::CurrentReading().seq != seq, "a frozen trace does not freeze the counter");
  Tap(OC::CONTROL_BUTTON_A);
  Run(100);
  CHECK(OC::SimScopeArenaLive(), "unfreezing re-arms and the arena comes back (the failure was transient)");
  RunCv(0, 300, Sine(1000.0, 6000.0));
  CHECK(Has(BottomLine(), "Hz B5 "), "...and the counter did not notice");
  OC::SimScopeAllocFails(false);
}

void FreqSwitch() {
  ScopeOnCv1();
  ScopeFreqSvc::SetClock(true, 150);
  RunCv(0, 300, Sine(1000.0, 6000.0));
  CHECK(ScopeFreqSvc::CurrentReading().have, "counting on CV1");
  Go("Tweighty");
  CHECK(ScopeFreqSvc::ActiveSource() == -1, "leaving the Scope stops the counter");
  CHECK(OC::ADC::scope_frame_hook() == nullptr, "...and takes the frame hook off");
  CHECK(!OC::SimScopeArenaLive(), "...and frees the arena");
  CHECK(ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 150,
        "the clock correction is still there (the counter is not in the arena)");
  RunCv(0, 200, Sine(1000.0, 6000.0));
  CHECK(!ScopeFreqSvc::CurrentReading().have, "frames that arrive meanwhile are ignored");
  Go("Scope");
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN1, "coming back, the counter listens to CV1 again");
  CHECK(OC::ADC::scope_frame_hook() != nullptr, "...with the hook");
  RunCv(0, 600, Sine(1000.0, 6000.0));
  CHECK(ScopeFreqSvc::CurrentReading().status == ScopeFreq::Status::OK, "...and reads");
  CHECK(ScopeFreqSvc::ClockPpm10() == 150, "the correction survived the round trip");
  // the app menu suspends the Scope too
  OpenMenu();
  CHECK(InMenu() && ScopeFreqSvc::ActiveSource() == -1 && OC::ADC::scope_frame_hook() == nullptr,
        "opening the app menu stops it as well");
  CHECK(PickOther("Scope"), "the menu picks another app");
  for (int i = 0; i < 4; ++i) {
    Go("Scope");
    CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN1, "switch-back: counting");
    Go("Backup");
    CHECK(ScopeFreqSvc::ActiveSource() == -1 && OC::ADC::scope_frame_hook() == nullptr, "switch-away: stopped");
  }
  CHECK(ScopeFreqSvc::ClockPpm10() == 150, "the correction is still 15.0 ppm after all of it");
}

void FreqLayout() {
  ScopeOnCv1();
  auto check_view = [&](const char *what) {
    RunCv(0, 350, Sine(1000.0, 6000.0));
    printf("  (%s: source %d)\n", what, ScopeFreqSvc::ActiveSource());
    const std::string line = BottomLine();
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: the measurement line reads (\"%s\")", what, line.c_str());
    CHECK(Has(line, "Hz B5 "), msg);
    snprintf(msg, sizeof(msg), "%s: nothing but the line on rows 55-63", what);
    CHECK(InkOutsideLine(line.size()) == 0, msg);
  };
  check_view("one trace");
  CHECK(PlotInk() > 0, "...the trace itself is drawn above it");
  TurnN(8);   // CV1-4, overlaid
  Run(50);
  check_view("overlaid CV1-4");
  CHECK(PlotInk() > 0, "...with ink in the plot");
  Tap(OC::CONTROL_BUTTON_Y);
  check_view("stacked CV1-4");
  Tap(OC::CONTROL_BUTTON_Y);
  TurnN(2);   // CV+TR, with its 1-bit lanes at the bottom of the plot
  Run(50);
  check_view("CV1-4 + TR lanes");
  Tap(OC::CONTROL_BUTTON_X);
  Run(100);
  check_view("spectrum");
  CHECK(PlotInk() > 0, "...with ink in the plot");
  Tap(OC::CONTROL_BUTTON_X);
}


// ---- the spectrum header ------------------------------------------------------

// The top line's 21 character cells, decoded from the framebuffer (text at y = 1,
// so not page aligned; a cell may be inverted, as the selected field is).
std::string HeaderCells() {
  uint8_t fb[1024];
  SimPanelVisible(fb);
  std::string out;
  for (int cell = 0; cell < 21; ++cell) {
    const int x = 1 + cell * 6;
    uint8_t col[6];
    bool blank = true;
    for (int c = 0; c < 6; ++c) {
      uint8_t b = 0;
      for (int cy = 0; cy < 8; ++cy) b |= (uint8_t)(((fb[((1 + cy) >> 3) * 128 + x + c] >> ((1 + cy) & 7)) & 1) << cy);
      col[c] = b;
      if (b) blank = false;
    }
    if (blank) { out += ' '; continue; }
    char found = '\x01';
    for (int inv = 0; inv < 2 && found == '\x01'; ++inv)
      for (int g = 0; g < 92; ++g) {
        bool same = true;
        for (int c = 0; c < 6 && same; ++c) same = (uint8_t)(inv ? ~col[c] : col[c]) == ssd1306xled_font6x8[g * 6 + c];
        if (same) { found = (char)(' ' + g); break; }
      }
    out += found;
  }
  return out;
}

void FreqFftHeader() {
  ScopeOnCv1();
  Tap(OC::CONTROL_BUTTON_X);   // spectrum
  Run(50);
  struct { double hz; } tones[] = {{79.0}, {98.0}, {808.0}, {1100.0}, {12300.0}};
  for (auto &t : tones) {
    RunCv(0, 500, Sine(t.hz, 6000.0));
    const std::string h = HeaderCells();
    char msg[200];
    printf("  %6.0f Hz: \"%s\"\n", t.hz, h.c_str());
    snprintf(msg, sizeof(msg), "%.0f Hz: FFT sits in columns 18-20", t.hz);
    CHECK(h.compare(18, 3, "FFT") == 0, msg);
    snprintf(msg, sizeof(msg), "%.0f Hz: the peak text is right-aligned, ending at column 16", t.hz);
    CHECK(h[16] != ' ' && h[17] == ' ', msg);
    // the field (columns 9-16): spaces, then "pk <number><unit>"
    const size_t p = h.find("pk ", 9);
    snprintf(msg, sizeof(msg), "%.0f Hz: the peak text starts with pk inside the field", t.hz);
    CHECK(p != std::string::npos && p >= 9 && p <= 12, msg);
    snprintf(msg, sizeof(msg), "%.0f Hz: nothing else between the channel and the peak", t.hz);
    CHECK(h.compare(0, 4, "CV1 ") == 0 || h.compare(0, 3, "CV1") == 0, msg);
    // the number is the tone, within one bin
    double v = atof(h.c_str() + p + 3);
    if (h[16] == 'k') v *= 1000.0;
    snprintf(msg, sizeof(msg), "%.0f Hz: the reading %.1f is the tone", t.hz, v);
    CHECK(fabs(v - t.hz) < 0.1 * t.hz + 40.0, msg);
  }
  // the digits changed width four times above and FFT never moved: compare two
  // extremes directly
  RunCv(0, 500, Sine(79.0, 6000.0));
  const std::string a = HeaderCells();
  RunCv(0, 500, Sine(12300.0, 6000.0));
  const std::string b = HeaderCells();
  CHECK(a.compare(17, 4, b.substr(17, 4)) == 0, "the columns right of the field are identical for 79 Hz and 12.3k");
  CHECK(a.size() == b.size() && a.find("pk") != b.find("pk"), "(the text did change width: it starts in different columns)");

  // only the source is selectable here: encL stays on it, so encR still changes the source
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN1, "(on CV1)");
  SimInputEncoder(false, 1);
  Run(250);
  SimInputEncoder(false, 1);
  Run(250);
  TurnN(1);
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN2,
        "in the spectrum encL leaves the source field alone: encR still changes the source");
  // leaving the spectrum, the other fields are back
  Tap(OC::CONTROL_BUTTON_X);
  Run(100);
  SimInputEncoder(false, 1);   // to the time field
  Run(250);
  TurnN(1);
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN2, "(in the scope view encL moves to the time field: encR no longer changes the source)");
}

// ---- saved settings -----------------------------------------------------------

constexpr uint16_t kScopeId = TWOCCS("SP");

// Rewrites the Scope's chunk in `src` to hold `payload` (n bytes) as its app
// data, other chunks untouched, the way an older firmware would have written it.
void RewriteScopeChunk(const OC::AppData &src, OC::AppData *dst, size_t io, const uint8_t *payload, size_t n) {
  memset(dst, 0, sizeof(*dst));
  size_t in = 0, out = 0;
  while (in < src.used) {
    const OC::AppChunkHeader *h = reinterpret_cast<const OC::AppChunkHeader *>(src.data + in);
    if (h->id != kScopeId) {
      memcpy(dst->data + out, src.data + in, h->length);
      out += h->length;
    } else {
      size_t len = sizeof(OC::AppChunkHeader) + io + n;
      if (len & 1) ++len;
      memcpy(dst->data + out, src.data + in, sizeof(OC::AppChunkHeader) + io);
      memcpy(dst->data + out + sizeof(OC::AppChunkHeader) + io, payload, n);
      reinterpret_cast<OC::AppChunkHeader *>(dst->data + out)->length = (uint16_t)len;
      out += len;
    }
    in += h->length;
  }
  dst->used = (uint32_t)out;
}

// Rewrites the chunk of app `id` so its payload is `extra` bytes longer than
// the app declares (zero filled), other chunks untouched.
void LengthenChunk(const OC::AppData &src, OC::AppData *dst, uint16_t id, size_t extra) {
  memset(dst, 0, sizeof(*dst));
  size_t in = 0, out = 0;
  while (in < src.used) {
    const OC::AppChunkHeader *h = reinterpret_cast<const OC::AppChunkHeader *>(src.data + in);
    memcpy(dst->data + out, src.data + in, h->length);
    if (h->id == id) {
      memset(dst->data + out + h->length, 0, extra);
      reinterpret_cast<OC::AppChunkHeader *>(dst->data + out)->length = (uint16_t)(h->length + extra);
      out += h->length + extra;
    } else {
      out += h->length;
    }
    in += h->length;
  }
  dst->used = (uint32_t)out;
}

void FreqSettings() {
  static OC::AppData cur, old;
  Go("Scope");
  TurnN(5);                             // quick entry 5 = CV3
  ScopeFreqSvc::SetClock(true, 321);
  OC::BuildAppData(cur);
  // find the Scope's saved bytes
  const uint8_t *payload = nullptr;
  size_t plen = 0;
  const size_t io = OC::app_switcher.current_app()->storage_size() - kScopeSettingsBytes;
  for (size_t at = 0; at < cur.used;) {
    const OC::AppChunkHeader *h = reinterpret_cast<const OC::AppChunkHeader *>(cur.data + at);
    if (h->id == kScopeId) {
      payload = cur.data + at + sizeof(OC::AppChunkHeader) + io;
      plen = h->length - sizeof(OC::AppChunkHeader) - io;
    }
    at += h->length;
  }
  CHECK(payload && plen >= kScopeSettingsBytes, "the save holds a Scope chunk of the new size");
  if (!payload) return;
  uint8_t cur_bytes[kScopeSettingsBytes];
  memcpy(cur_bytes, payload, kScopeSettingsBytes);
  printf("  (saved: quick %d, bytes 16-19: %02X %02X %02X %02X, io %zu, plen %zu)\n", cur_bytes[0], cur_bytes[16], cur_bytes[17], cur_bytes[18], cur_bytes[19], io, plen);
  CHECK(cur_bytes[0] == 5 && cur_bytes[16] == kScopeSettingsVersion && cur_bytes[17] == 1, "...quick entry 5, version 2, calibrated");
  CHECK(cur_bytes[18] == (321 & 0xFF) && cur_bytes[19] == (321 >> 8), "...and the clock, 32.1 ppm");

  // 1. the new layout restores the clock
  Go("Tweighty");
  ScopeFreqSvc::SetClock(false, 0);
  OC::ApplyAppData(cur);
  CHECK(ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 321, "a version-2 save restores the clock correction");
  Go("Scope");
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN3, "...and the view (CV3)");

  // 2. the 16-byte layout of the shipped firmware: restored, not dropped
  TurnN(2);                               // CV5 now
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN5, "(the view moved off CV3)");
  Go("Tweighty");
  RewriteScopeChunk(cur, &old, io, cur_bytes, kScopeSettingsBytesV1);
  CHECK(old.used < cur.used, "(the old-layout save is shorter)");
  ScopeFreqSvc::SetClock(false, 0);
  OC::ApplyAppData(old);
  Go("Scope");
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN3, "a 16-byte save is restored: CV3 is back");
  CHECK(!ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 0, "...with the clock uncalibrated");

  // 3. an old save must not wipe a calibration made since
  Go("Tweighty");
  ScopeFreqSvc::SetClock(true, 999);
  OC::ApplyAppData(old);
  CHECK(ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 999, "an older save does not wipe a live calibration");

  // 4. the 3-byte layout of the first Scope: channel 3 = CV IN 4
  const uint8_t legacy[3] = {3, 3, 1};
  OC::AppData leg;
  RewriteScopeChunk(cur, &leg, io, legacy, 3);
  Go("Scope");
  TurnN(2);
  Go("Tweighty");
  OC::ApplyAppData(leg);
  Go("Scope");
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN4, "a 3-byte save is restored: CV4");

  // 5. any other length is still dropped
  uint8_t odd[18];
  memcpy(odd, cur_bytes, 16);
  odd[16] = odd[17] = 0;
  OC::AppData bad;
  RewriteScopeChunk(cur, &bad, io, odd, 18);
  TurnN(1);
  const int before = ScopeFreqSvc::ActiveSource();
  Go("Tweighty");
  OC::ApplyAppData(bad);
  Go("Scope");
  CHECK(ScopeFreqSvc::ActiveSource() == before, "a chunk of any other length is still dropped");

  // 6. only the Scope accepts an older layout: another app's chunk of the wrong
  //    length is dropped as before (the stand-in counts the restores it is given)
  AppProbeQuad *quad = OC::SimProbeQuad();
  static OC::AppData longer;
  const int r0 = quad->n.restore;
  OC::ApplyAppData(cur);
  CHECK(quad->n.restore == r0 + 1, "(a chunk of the right length reaches the other app's restore)");
  LengthenChunk(cur, &longer, quad->id(), 2);
  OC::ApplyAppData(longer);
  CHECK(quad->n.restore == r0 + 1, "another app's chunk of the wrong length is still dropped");
  LengthenChunk(cur, &longer, quad->id(), 1);
  OC::ApplyAppData(longer);
  CHECK(quad->n.restore == r0 + 1, "...by one byte too");

  // 7. the correction belongs to the module: a preset recall neither overwrites nor undoes it
  Go("Scope");
  ScopeFreqSvc::SetClock(true, 888);
  OC::BuildAppData(cur);                            // a save that carries 88.8 ppm
  ScopeFreqSvc::SetClock(true, 500);
  OC::ApplyAppDataFromPreset(cur);
  CHECK(ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 500,
        "a preset recall does not overwrite a live correction with its own");
  ScopeFreqSvc::SetClock(false, 0);
  OC::ApplyAppDataFromPreset(cur);
  CHECK(!ScopeFreqSvc::ClockCalibrated(), "...nor bring back one that was cleared (%cx)");
  OC::ApplyAppData(cur);
  CHECK(ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 888,
        "while the settings load path still applies it");
  CHECK(!OC::AppDataIsPresetRecall(), "(the recall flag is down again)");

  // 7b. through the real recall: save a preset slot, change the correction, recall
  //     the slot with PresetEngine's own RecallSlot path -- the live value stays
  Go("Scope");
  ScopeFreqSvc::SetClock(true, 100);
  CHECK(Save(5), "(a preset is saved with a 10.0 ppm correction in it)");
  ScopeFreqSvc::SetClock(true, 200);
  CHECK(Recall(5), "(the slot is recalled)");
  CHECK(ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 200,
        "a recall through the preset engine leaves the live correction alone");
  ScopeFreqSvc::SetClock(false, 0);
  CHECK(Recall(5), "(and again)");
  CHECK(!ScopeFreqSvc::ClockCalibrated(), "...and does not bring back one that was cleared");

  // 8. a factory reset forgets the calibration, and the next save does not write it back
  ScopeFreqSvc::SetClock(true, 777);
  OC::ReinitApps(true);
  Run(100);
  CHECK(!ScopeFreqSvc::ClockCalibrated() && ScopeFreqSvc::ClockPpm10() == 0, "a factory reset clears the correction");
  OC::BuildAppData(cur);
  bool saved_zero = false;
  for (size_t at = 0; at < cur.used;) {
    const OC::AppChunkHeader *h = reinterpret_cast<const OC::AppChunkHeader *>(cur.data + at);
    if (h->id == kScopeId) saved_zero = cur.data[at + sizeof(OC::AppChunkHeader) + io + 17] == 0;
    at += h->length;
  }
  CHECK(saved_zero, "...and the next save writes none");
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
  else if (s == "freq-cv-tone") FreqCvTone();
  else if (s == "freq-fast") FreqFast();
  else if (s == "freq-status") FreqStatus();
  else if (s == "freq-lfo") FreqLfo();
  else if (s == "freq-no-arena") FreqNoArena();
  else if (s == "freq-switch") FreqSwitch();
  else if (s == "freq-layout") FreqLayout();
  else if (s == "freq-settings") FreqSettings();
  else if (s == "freq-fft-header") FreqFftHeader();
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
