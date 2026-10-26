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

#include <algorithm>
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
#include "ScopeCapture.h"
#include "ScopeFft.h"
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
void SimScopeReadFails(int draws);
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

// The same for the audio jacks: samples at 48 kHz of simulated time, through the real
// capture entry (ScopeCapture::OnAudio, what AudioScopeCapture::update calls), tap 0 = IN L.
template <typename F>
void RunAudio(int tap, uint32_t ms, F sample) {
  static uint64_t n = 0;
  static uint32_t base_us = 0;
  static bool started = false;
  if (!started) { base_us = SimNowUs(); started = true; }
  for (uint32_t t = 0; t < ms; ++t) {
    Run(1);
    const uint64_t due = (uint64_t)((double)(uint32_t)(SimNowUs() - base_us) * 48000.0 * 1e-6);
    while (n < due) {
      int16_t blk[128];
      int k = 0;
      for (; n < due && k < 128; ++n, ++k) blk[k] = (int16_t)lround(sample(n));
      ScopeCapture::OnAudio(tap, blk, k);
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
  CHECK(StartsWith(line, "*1000.0"), "uncalibrated crystal: the line starts with the * mark (the CV rate is verified: no #)");
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
  CHECK(!line.empty() && line[0] >= '0' && line[0] <= '9', "confirmed and calibrated: no mark at all");
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
  CHECK(line[0] == '*' || line[0] == '?', "...marked, the crystal being uncalibrated");
  // heavy noise on the same tone: counted wrong, so refused
  uint32_t seed = 12345;
  auto noisy = [&](uint64_t n) {
    seed = seed * 1664525u + 1013904223u;
    return 16000.0 + 3000.0 * sin(2 * M_PI * (0.1 + 440.0 * n / kCvFs)) + ((seed >> 8) % 6000) - 3000.0;
  };
  RunCv(0, 700, noisy);
  line = BottomLine();
  printf("  line: \"%s\"\n", line.c_str());
  CHECK(!(line[0] >= '0' && line[0] <= '9') && line[0] != '*',
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
std::string RowCells(int y0) {
  uint8_t fb[1024];
  SimPanelVisible(fb);
  std::string out;
  for (int cell = 0; cell < 21; ++cell) {
    const int x = 1 + cell * 6;
    uint8_t col[6];
    bool blank = true;
    for (int c = 0; c < 6; ++c) {
      uint8_t b = 0;
      for (int cy = 0; cy < 8; ++cy) b |= (uint8_t)(((fb[((y0 + cy) >> 3) * 128 + x + c] >> ((y0 + cy) & 7)) & 1) << cy);
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

std::string HeaderCells() { return RowCells(1); }

// Height in pixels of the bar in each plot column: the run of set pixels up from the bottom row.
void Bars(int out[128]) {
  uint8_t fb[1024];
  SimPanelVisible(fb);
  for (int x = 0; x < 128; ++x) {
    int h = 0;
    for (int y = 54; y >= 11; --y) {
      if ((fb[(y >> 3) * 128 + x] >> (y & 7)) & 1) ++h;
      else break;
    }
    out[x] = h;
  }
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

  // three live fields, in order: source, pk (span), FFT (window/averaging)
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN1, "(on CV1)");
  SimInputEncoder(false, 1);   // to pk
  Run(250);
  TurnN(1);                    // the span, not the source
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN1, "with pk selected encR changes the span, not the source");
  CHECK(RowCells(12).compare(0, 6, "0-3.7k") == 0, "...and the span (for CV1: 0-3.7k) shows at the top left of the plot");
  SimInputEncoder(false, 1);   // to FFT
  Run(250);
  TurnN(1);
  CHECK(HeaderCells().compare(19, 2, "H4") == 0, "with FFT selected encR steps the preset: H4, right-aligned");
  SimInputEncoder(false, 1);   // around to the source
  Run(250);
  TurnN(1);
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN2, "a third encL is back on the source: encR changes it");
  // leaving the spectrum, the scope view's own fields are back
  Tap(OC::CONTROL_BUTTON_X);
  Run(100);
  SimInputEncoder(false, 1);   // to the time field
  Run(250);
  TurnN(1);
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN2, "(in the scope view encL moves to the time field: encR no longer changes the source)");
}

// ---- the spectrum zoom, windows and averaging ---------------------------------

// To the spectrum with the span and preset asked for. Focus order: source, pk, FFT.
int g_focus = 0;   // 0 source, 1 pk, 2 FFT: where the spectrum's selection is
void FocusTo(int f) {
  while (g_focus != f) {
    SimInputEncoder(false, 1);
    Run(250);
    g_focus = (g_focus + 1) % 3;
  }
}
void SpectrumSet(int span, int mode) {
  Tap(OC::CONTROL_BUTTON_X);
  Run(50);
  g_focus = 0;   // entering the spectrum selects the source
  FocusTo(1);
  TurnN(span);
  FocusTo(2);
  TurnN(mode);
}

// The zoom's source: CV1 or IN L, with the span the Scope really shows for it.
struct ZSrc {
  bool audio;
  double Rate() const { return audio ? 48000.0 : ScopeFreqSvc::kCvFsHz; }
  // written out here, not taken from the code under test: 0.8 of the decimated Nyquist
  double Span(int span) const { return 0.8 * (Rate() / (span == 1 ? 4.0 : span == 2 ? 16.0 : 64.0)) / 2.0; }
  void Feed(uint32_t ms, double hz, double amp) const {
    if (audio) RunAudio(0, ms, [=](uint64_t n) { return amp * sin(2 * M_PI * (0.1 + hz * n / 48000.0)); });
    else RunCv(0, ms, Sine(hz, amp));
  }
};

// One tone at one span: the header reading, the tallest bar's column, the label.
void ZoomCase(const ZSrc &z, int span, double hz, double tol, const char *label, bool fine) {
  z.Feed(span == 3 ? 2700 : span == 2 ? 1000 : 450, hz, z.audio ? 9000.0 : 6000.0);
  const std::string h = HeaderCells();
  char msg[220];
  const size_t p = h.find("pk ");
  const double v = p == std::string::npos ? 0 : atof(h.c_str() + p + 3) * (h[16] == 'k' ? 1000.0 : 1.0);
  printf("  %s span %d, %.1f Hz: \"%s\" -> %.2f\n", z.audio ? "IN L" : "CV1", span, hz, h.c_str(), v);
  if (v >= 999.5) tol = std::max(tol, 50.0);   // the kHz form has one decimal: 100 Hz steps
  snprintf(msg, sizeof(msg), "%s span %d, %.1f Hz: pk reads %.2f, within %.2f Hz", z.audio ? "IN L" : "CV1", span, hz, v, tol);
  CHECK(p != std::string::npos && fabs(v - hz) <= tol, msg);
  if (p != std::string::npos) {   // the fine reading (one decimal, no unit) only on the 0-300 span
    const std::string num = h.substr(p + 3, 8);
    const bool has_dot = num.find('.') != std::string::npos && num.find('k') == std::string::npos;
    const bool has_hz = num.find("Hz") != std::string::npos;
    snprintf(msg, sizeof(msg), "span %d: the peak text is in the %s form", span, fine ? "fine (98.4)" : "whole-hertz (98Hz)");
    if (hz < 999) CHECK(fine ? (has_dot && !has_hz) : (!has_dot && has_hz), msg);
  }
  int bars[128];
  Bars(bars);
  int best = 0;
  for (int x = 1; x < 128; ++x) if (bars[x] > bars[best]) best = x;
  const int want = (int)lround(hz / z.Span(span) * 127.0);
  snprintf(msg, sizeof(msg), "%s span %d, %.1f Hz: the tallest bar is in column %d (expected %d)", z.audio ? "IN L" : "CV1", span, hz, best, want);
  CHECK(abs(best - want) <= 1 && bars[best] >= 30, msg);
  snprintf(msg, sizeof(msg), "span %d: the span label %s is shown", span, label);
  CHECK(RowCells(12).compare(0, strlen(label), label) == 0, msg);
}

void FreqFftZoom() {
  ScopeOnCv1();
  SpectrumSet(0, 0);
  const ZSrc cv{false};
  const double s1 = cv.Span(1), s2 = cv.Span(2), s3 = cv.Span(3);
  printf("  CV spans: %.2f %.2f %.2f Hz\n", s1, s2, s3);
  CHECK(fabs(s1 - 3735.06) < 0.01 && fabs(s2 - 933.76) < 0.01 && fabs(s3 - 233.44) < 0.01, "the CV spans are 0.8 of the decimated Nyquist at the verified frame rate");
  FocusTo(1);
  TurnN(1);   // 0-3.7k
  ZoomCase(cv, 1, 1000.0, 2.0, "0-3.7k", false);
  ZoomCase(cv, 1, 440.0, 2.0, "0-3.7k", false);
  ZoomCase(cv, 1, 50.0, 3.0, "0-3.7k", false);                     // mains hum is read
  ZoomCase(cv, 1, 30.0, 4.0, "0-3.7k", false);                     // and so is a tone between 3 and 5 bins (the floor is 20 Hz)
  ZoomCase(cv, 1, 0.95 * s1, 4.0, "0-3.7k", false);                // near the top of the span
  TurnN(1);   // 0-934
  ZoomCase(cv, 2, 440.0, 0.8, "0-934", false);
  ZoomCase(cv, 2, 100.0, 0.8, "0-934", false);
  ZoomCase(cv, 2, 0.95 * s2, 1.5, "0-934", false);
  TurnN(1);   // 0-233
  ZoomCase(cv, 3, 100.0, 0.2, "0-233", true);
  ZoomCase(cv, 3, 40.0, 0.2, "0-233", true);
  ZoomCase(cv, 3, 0.95 * s3, 0.3, "0-233", true);
  // quarter marks: dotted lines at 1/4, 1/2 and 3/4 of the span, nowhere else (the tone is at 0.95)
  {
    uint8_t fb[1024];
    SimPanelVisible(fb);
    auto ink_in_column = [&](int x) { int n = 0; for (int y = 11; y < 55; ++y) n += (fb[(y >> 3) * 128 + x] >> (y & 7)) & 1; return n; };
    for (int q = 1; q <= 3; ++q) {
      const int col = q == 1 ? 32 : q == 2 ? 64 : 95;   // 127 * q / 4 = 31.75, 63.5, 95.25, rounded
      printf("  quarter mark %d at column %d: %d px\n", q, col, ink_in_column(col));
      CHECK(ink_in_column(col) >= 15, "a quarter mark is a dotted line in its column");
      CHECK(ink_in_column(col + 6) < 8 && ink_in_column(col - 6) < 8, "...and nothing is drawn six columns either side");
    }
  }
  // a tone just above the span edge and a tone well above the decimated Nyquist are filtered
  // out, not folded into the shown band: no false peak, no tall bar
  FocusTo(1);
  TurnN(-2 + 4);   // 0-3.7k again (wraps 3 -> 0 -> 1)
  for (double hz : {1.15 * s1, 4300.0, 5000.0}) {
    cv.Feed(600, hz, 6000.0);
    int bars[128];
    Bars(bars);
    int tall = 0;
    for (int x = 0; x < 128; ++x) tall = std::max(tall, bars[x]);
    const std::string h = HeaderCells();
    const size_t p = h.find("pk ");
    const double v = p == std::string::npos ? 0 : atof(h.c_str() + p + 3) * (h[16] == 'k' ? 1000.0 : 1.0);
    printf("  %.0f Hz above the 0-3.7k span: tallest bar %d, \"%s\"\n", hz, tall, h.c_str());
    char msg[160];
    snprintf(msg, sizeof(msg), "%.0f Hz is not drawn inside the 0-3.7k span (tallest bar %d)", hz, tall);
    CHECK(tall < 12, msg);
    snprintf(msg, sizeof(msg), "%.0f Hz does not read as a peak inside the span (pk %.0f)", hz, v);
    CHECK(fabs(v - hz) > 150.0 && fabs(v - (2 * 4668.8 - hz)) > 150.0, msg);
  }
  // back to the full span: no label, the log axis (1 kHz sits at its log column)
  FocusTo(1);
  TurnN(3);   // wraps round to 0
  cv.Feed(500, 1000.0, 6000.0);
  CHECK(RowCells(12).compare(0, 2, "0-") != 0, "the full span has no label");
  int bars[128];
  Bars(bars);
  int best = 0;
  for (int x = 1; x < 128; ++x) if (bars[x] > bars[best]) best = x;
  // log axis 20 Hz .. Nyquist (18,675 Hz) over 128 columns, worked out here
  const int want_log = (int)lround(log10(1000.0 / 20.0) / log10(ScopeFreqSvc::kCvFsHz / 2.0 / 20.0) * 127.0);
  printf("  full span: loudest column %d (log axis puts 1 kHz at %d)\n", best, want_log);
  CHECK(abs(best - want_log) <= 1, "...and 1 kHz is where the log axis puts it");
  // the full-span reading is right to the bin: 100.0 Hz, not 100.4 (the nominal-rate error)
}

// The audio jacks: the spans are the round ones (4800 / 1200 / 300), through ScopeCapture::OnAudio.
void FreqFftZoomAudio() {
  Go("Scope");   // opens on IN L
  Run(50);
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_IN_L, "(the Scope opens on IN L)");
  SpectrumSet(0, 0);
  const ZSrc au{true};
  CHECK(fabs(au.Span(1) - 4800.0) < 1e-6 && fabs(au.Span(2) - 1200.0) < 1e-6 && fabs(au.Span(3) - 300.0) < 1e-6, "the audio spans are the round 4800 / 1200 / 300 Hz");
  FocusTo(1);
  TurnN(1);
  ZoomCase(au, 1, 1000.0, 2.0, "0-4.8k", false);
  ZoomCase(au, 1, 0.95 * 4800.0, 4.0, "0-4.8k", false);
  TurnN(1);
  ZoomCase(au, 2, 440.0, 0.8, "0-1.2k", false);
  ZoomCase(au, 2, 0.95 * 1200.0, 1.5, "0-1.2k", false);
  TurnN(1);
  ZoomCase(au, 3, 100.0, 0.2, "0-300", true);
  ZoomCase(au, 3, 0.95 * 300.0, 0.3, "0-300", true);
}

// Freezing a zoomed span keeps what is on screen (the ring is not reset); unfreezing goes live.
void FreqFftFreeze() {
  ScopeOnCv1();
  SpectrumSet(1, 0);
  const ZSrc cv{false};
  cv.Feed(600, 1000.0, 6000.0);
  const std::string live = HeaderCells();
  int b0[128];
  Bars(b0);
  long ink0 = 0;
  for (int x = 0; x < 128; ++x) ink0 += b0[x];
  printf("  live:   \"%s\" ink %ld\n", live.c_str(), ink0);
  CHECK(live.find("pk 1.0k") != std::string::npos && ink0 > 30, "(live: pk 1.0k and a plot)");
  Tap(OC::CONTROL_BUTTON_A);   // freeze
  Run(400);
  const std::string frozen = HeaderCells();
  int b1[128];
  Bars(b1);
  long ink1 = 0;
  for (int x = 0; x < 128; ++x) ink1 += b1[x];
  printf("  frozen: \"%s\" ink %ld\n", frozen.c_str(), ink1);
  CHECK(frozen.find("pk 1.0k") != std::string::npos, "frozen: the same peak reading");
  CHECK(ink1 > 30 && labs(ink1 - ink0) <= ink0 / 10, "frozen: the same plot");
  // frames that arrive while frozen change nothing
  cv.Feed(300, 440.0, 6000.0);
  int b2[128];
  Bars(b2);
  long ink2 = 0;
  for (int x = 0; x < 128; ++x) ink2 += b2[x];
  CHECK(ink2 == ink1 && HeaderCells() == frozen, "frozen: later frames change nothing");
  Tap(OC::CONTROL_BUTTON_A);   // run
  cv.Feed(600, 440.0, 6000.0);
  CHECK(HeaderCells().find("pk 440Hz") != std::string::npos || HeaderCells().find("pk 44") != std::string::npos,
        "unfrozen: live again at the new tone");
}

// CV out and TR are not fed to the decimator: a zoom there says so.
void FreqFftNoZoomOnOutputs() {
  ScopeOnCv1();
  TurnN(12);   // quick entry 15: OUT1-4
  Run(50);
  SpectrumSet(1, 0);
  Run(600);
  const std::string label = RowCells(12);
  printf("  label row: \"%s\"\n", label.c_str());
  CHECK(label.compare(0, 7, "no zoom") == 0, "a zoomed span on a CV out says \"no zoom\"");
  CHECK(HeaderCells().find("pk --") != std::string::npos, "...and has no peak reading");
  FocusTo(1);
  TurnN(3);   // back to the full span: the spectrum of the output works as before
  Run(300);
  CHECK(RowCells(12).compare(0, 7, "no zoom") != 0, "the full span has no such label");
}

// A snapshot that never comes out clean (an ISR pushing all the time) redraws the last good
// frame: no blank plot, no "pk --".
void FreqFftReadFailure() {
  ScopeOnCv1();
  SpectrumSet(1, 0);
  const ZSrc cv{false};
  cv.Feed(700, 1000.0, 6000.0);
  const std::string live = HeaderCells();
  int b0[128];
  Bars(b0);
  long ink0 = 0;
  for (int x = 0; x < 128; ++x) ink0 += b0[x];
  CHECK(live.find("pk 1.0k") != std::string::npos && ink0 > 30, "(live: pk 1.0k and a plot)");
  OC::SimScopeReadFails(1000000);   // every draw for the whole stretch
  cv.Feed(300, 1000.0, 6000.0);   // many draws, all refused
  const std::string held = HeaderCells();
  int b1[128];
  Bars(b1);
  long ink1 = 0;
  for (int x = 0; x < 128; ++x) ink1 += b1[x];
  printf("  live \"%s\" ink %ld, with every snapshot refused \"%s\" ink %ld\n", live.c_str(), ink0, held.c_str(), ink1);
  CHECK(held.find("pk 1.0k") != std::string::npos, "with every snapshot refused the peak reading stays");
  CHECK(ink1 > 30 && labs(ink1 - ink0) <= ink0 / 10, "...and so does the plot (no blank frame)");
  OC::SimScopeReadFails(0);
  cv.Feed(300, 440.0, 6000.0);
  CHECK(HeaderCells().find("pk 440Hz") != std::string::npos || HeaderCells().find("pk 44") != std::string::npos, "and it is live again afterwards");
}

// Averaging is reset by a live re-arm: unfreeze (or button B) must not blend the new feed into
// the pre-freeze average.
void FreqFftAveragingRearm() {
  ScopeOnCv1();
  SpectrumSet(1, 2);   // 0-3.7k, H16
  const ZSrc cv{false};
  cv.Feed(1200, 1000.0, 6000.0);
  const int col_1k = (int)lround(1000.0 / cv.Span(1) * 127.0);
  int bars[128];
  Bars(bars);
  int before = 0;
  for (int x = col_1k - 2; x <= col_1k + 2; ++x) before = std::max(before, bars[x]);
  CHECK(before >= 30, "(the averaged 1 kHz tone is on screen)");
  Tap(OC::CONTROL_BUTTON_A);          // freeze on it
  Run(200);
  Tap(OC::CONTROL_BUTTON_A);          // run again, with another tone on the input
  cv.Feed(500, 300.0, 6000.0);
  Bars(bars);
  int ghost = 0;
  for (int x = col_1k - 2; x <= col_1k + 2; ++x) ghost = std::max(ghost, bars[x]);
  printf("  1 kHz column: %d px before the freeze, %d px after the unfreeze\n", before, ghost);
  CHECK(ghost < 14, "after an unfreeze the old average is gone");
}

// The average takes a step per half frame of FRESH samples, not per redraw: on the narrowest zoom
// (0.7 s per half frame) a noisy picture changes about once in that time, not at every draw.
void FreqFftAveragingRate() {
  ScopeOnCv1();
  SpectrumSet(3, 2);   // 0-233, H16
  uint32_t seed = 31;
  auto noisy = [&](uint64_t n) {
    seed = seed * 1664525u + 1013904223u;
    return 16000.0 + 3000.0 * sin(2 * M_PI * (0.1 + 100.0 * n / kCvFs)) + ((seed >> 8) % 3000) - 1500.0;
  };
  RunCv(0, 2600, noisy);   // fill
  int prev[128], cur[128];
  Bars(prev);
  int changes = 0, frames = 0;
  for (int f = 0; f < 30; ++f) {
    RunCv(0, 40, noisy);
    Bars(cur);
    bool diff = false;
    for (int x = 0; x < 128; ++x) if (cur[x] != prev[x]) diff = true;
    if (diff) ++changes;
    for (int x = 0; x < 128; ++x) prev[x] = cur[x];
    ++frames;
  }
  printf("  %d of %d frames (40 ms apart, 1.2 s) differ from the one before\n", changes, frames);
  CHECK(changes <= 4, "averaging steps only when a half frame of new samples has arrived (about every 0.7 s)");
}

// A near-full-scale tone far above the shown span folds through the first decimator stage: it
// must not appear in the plot.
void FreqFftFolding() {
  Go("Scope");
  Run(50);
  SpectrumSet(1, 0);
  const ZSrc au{true};
  for (double hz : {19500.0, 19800.0, 21000.0}) {
    au.Feed(500, hz, 30000.0);
    int bars[128];
    Bars(bars);
    int tall = 0;
    for (int x = 0; x < 128; ++x) tall = std::max(tall, bars[x]);
    const std::string h = HeaderCells();
    printf("  IN L 0-4.8k, %.0f Hz at -1 dBFS: tallest bar %d, \"%s\"\n", hz, tall, h.c_str());
    char msg[160];
    snprintf(msg, sizeof(msg), "a %.0f Hz tone at -1 dBFS does not fold into the 0-4.8k plot (tallest bar %d)", hz, tall);
    CHECK(tall < 12, msg);
    snprintf(msg, sizeof(msg), "...and the header reads no peak, not a ghost frequency (\"%s\")", h.c_str());
    CHECK(h.find("pk --") != std::string::npos, msg);
  }
}

// Push resets the fields; span and preset are plain every time the Scope opens; averaging restarts
// when the source changes; the preset text.
void FreqFftResets() {
  ScopeOnCv1();
  SpectrumSet(2, 3);
  CHECK(RowCells(12).compare(0, 6, "0-934") == 0 || RowCells(12).compare(0, 5, "0-934") == 0, "(span 2 set)");
  CHECK(HeaderCells().compare(19, 2, "BH") == 0, "(preset BH set)");
  FocusTo(1);
  Tap(OC::CONTROL_BUTTON_R);
  Run(150);
  CHECK(RowCells(12).compare(0, 2, "0-") != 0, "encR push on pk puts the span back to full");
  CHECK(HeaderCells().compare(19, 2, "BH") == 0, "...and leaves the preset alone");
  TurnN(1);
  FocusTo(2);
  Tap(OC::CONTROL_BUTTON_R);
  Run(150);
  CHECK(HeaderCells().compare(18, 3, "FFT") == 0, "encR push on FFT puts the preset back to plain Hann");
  // reopen: a zoomed span and a preset do not survive
  FocusTo(1);
  TurnN(1);
  FocusTo(2);
  TurnN(4);
  Go("Tweighty");
  Go("Scope");
  Run(300);
  CHECK(RowCells(12).compare(0, 2, "0-") != 0, "reopening the Scope: the span is full again");
  CHECK(HeaderCells().compare(18, 3, "FFT") == 0, "...and the preset is plain Hann again");
}

// Averaging restarts when the source changes: the old source's tone leaves no ghost.
void FreqFftAveragingSource() {
  ScopeOnCv1();
  SpectrumSet(0, 2);   // H16
  CHECK(HeaderCells().compare(18, 3, "H16") == 0, "(H16 set)");
  const ZSrc cv{false};
  cv.Feed(1500, 1000.0, 6000.0);
  const int col_1k = (int)lround(log10(1000.0 / 20.0) / log10(ScopeFreqSvc::kCvFsHz / 2.0 / 20.0) * 127.0);
  int bars[128];
  Bars(bars);
  int before = 0;
  for (int x = col_1k - 2; x <= col_1k + 2; ++x) before = std::max(before, bars[x]);
  printf("  before the source change: the 1 kHz column %d px\n", before);
  CHECK(before >= 30, "(the averaged 1 kHz tone is on screen)");
  FocusTo(0);
  TurnN(1);   // CV2
  CHECK(ScopeFreqSvc::ActiveSource() == ScopeSources::SRC_CV_IN2, "(on CV2)");
  RunCv(1, 200, Sine(440.0, 6000.0));
  Bars(bars);
  int ghost = 0;
  for (int x = col_1k - 2; x <= col_1k + 2; ++x) ghost = std::max(ghost, bars[x]);
  printf("  after the source change: 1 kHz column %d px\n", ghost);
  CHECK(ghost < 14, "the old source's tone is gone at once (the average restarted)");
}

void FreqFftWindowAndAveraging() {
  ScopeOnCv1();
  SpectrumSet(0, 0);
  uint32_t seed = 99;
  auto noisy = [&](uint64_t n) {
    seed = seed * 1664525u + 1013904223u;
    return 16000.0 + 3000.0 * sin(2 * M_PI * (0.1 + 1000.0 * n / kCvFs)) + ((seed >> 8) % 4000) - 2000.0;
  };
  // frame-to-frame change of the bars (mean absolute, in pixels) over a stretch of frames
  auto jitter = [&]() {
    int prev[128], cur[128];
    RunCv(0, 300, noisy);
    Bars(prev);
    double sum = 0;
    int frames = 0;
    for (int f = 0; f < 24; ++f) {
      RunCv(0, 40, noisy);
      Bars(cur);
      for (int x = 20; x < 120; ++x) sum += abs(cur[x] - prev[x]);
      for (int x = 0; x < 128; ++x) prev[x] = cur[x];
      ++frames;
    }
    return sum / (frames * 100.0);
  };
  const double plain = jitter();
  // the FFT field to H16 (two steps), then measure again
  FocusTo(2);
  TurnN(2);
  CHECK(HeaderCells().compare(18, 3, "H16") == 0, "the preset H16 is shown");
  const double avg = jitter();
  printf("  mean column change per frame: plain %.3f px, H16 %.3f px\n", plain, avg);
  CHECK(plain > 0.15, "(the noisy tone does jitter without averaging)");
  CHECK(avg < 0.6 * plain, "averaging 16 frames cuts the column jitter to under 60%");
  // every preset finds the tone; their skirts differ (total bar ink with a clean tone:
  // Rect leaks the most, Blackman-Harris and flat-top the least, outside the main lobe). Presets in order:
  // FFT(Hann) H4 H16 BH BH4 FT REC. Now on H16.
  const int tone_col = (int)lround(log10(1000.0 / 20.0) / log10(ScopeFreqSvc::kCvFsHz / 2.0 / 20.0) * 127.0);
  // ink well away from the tone (the skirts), not its main lobe
  auto ink = [&]() { int b[128]; Bars(b); long t = 0; for (int x = 0; x < 128; ++x) if (abs(x - tone_col) > 14) t += b[x]; return t; };
  long ink_of[7] = {0};
  TurnN(5);   // H16 wraps round to FFT
  for (int idx = 0; idx < 7; ++idx) {
    RunCv(0, 300, Sine(1000.0, 6000.0));
    ink_of[idx] = ink();
    const std::string h = HeaderCells();
    const size_t p = h.find("pk ");
    const double v = p == std::string::npos ? 0 : atof(h.c_str() + p + 3) * (h[16] == 'k' ? 1000.0 : 1.0);
    printf("  preset %d: \"%s\", ink %ld\n", idx, h.c_str(), ink_of[idx]);
    CHECK(fabs(v - 1000.0) < 60.0, "each window finds the 1 kHz tone");
    if (idx < 6) TurnN(1);
  }
  CHECK(ink_of[6] > ink_of[0] + 100, "Rect leaks more than Hann (more ink in the plot)");
  // (on a 12-bit CV input the floor, not leakage, is what the wide-noise-bandwidth windows
  // show more of: Blackman-Harris and flat-top sit higher than Hann, flat-top most)
  CHECK(ink_of[0] < ink_of[3] && ink_of[3] < ink_of[5] && ink_of[5] < ink_of[6], "the four windows draw four different skirts");
  CHECK(ink_of[1] != ink_of[2] || ink_of[0] != ink_of[2], "(averaging changes the picture too)");
  // wraps back to the default, and encR push on the field restores it at once
  CHECK(HeaderCells().compare(18, 3, "REC") == 0, "(the last preset is Rect)");
  Tap(OC::CONTROL_BUTTON_R);
  Run(100);
  CHECK(HeaderCells().compare(18, 3, "FFT") == 0, "encR push puts the FFT field back to the default");
}

// 1 kHz sine, an offset and mains hum: the low columns are a slope, not blocks.
void FreqFftLowBand() {
  ScopeOnCv1();
  SpectrumSet(0, 0);
  uint32_t seed = 7;
  auto word = [&](uint64_t n) {
    seed = seed * 1664525u + 1013904223u;
    return 14000.0 + 5000.0 * sin(2 * M_PI * (0.1 + 1000.0 * n / kCvFs)) + 100.0 * sin(2 * M_PI * 50.0 * n / kCvFs) +
           ((seed >> 8) % 8) - 4.0;
  };
  RunCv(0, 600, word);
  int bars[128];
  Bars(bars);
  printf("  columns 0-39:");
  for (int x = 0; x < 40; ++x) printf(" %d", bars[x]);
  printf("\n");
  int step = 0, run = 0, longest = 0;
  for (int x = 1; x < 40; ++x) {
    step = std::max(step, abs(bars[x] - bars[x - 1]));
    run = bars[x] == bars[x - 1] ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  CHECK(longest <= 12, "no plateau of more than 12 equal columns in the low band (it was 23)");
  CHECK(bars[0] < 30, "the offset (bin 0) does not fill the first columns");
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
  else if (s == "freq-fft-zoom") FreqFftZoom();
  else if (s == "freq-fft-zoom-audio") FreqFftZoomAudio();
  else if (s == "freq-fft-freeze") FreqFftFreeze();
  else if (s == "freq-fft-nozoom") FreqFftNoZoomOnOutputs();
  else if (s == "freq-fft-resets") FreqFftResets();
  else if (s == "freq-fft-readfail") FreqFftReadFailure();
  else if (s == "freq-fft-avgrearm") FreqFftAveragingRearm();
  else if (s == "freq-fft-avgrate") FreqFftAveragingRate();
  else if (s == "freq-fft-folding") FreqFftFolding();
  else if (s == "freq-fft-avgsource") FreqFftAveragingSource();
  else if (s == "freq-fft-window") FreqFftWindowAndAveraging();
  else if (s == "freq-fft-lowband") FreqFftLowBand();
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
