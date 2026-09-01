// The fake environment behind the real PresetBusUI.cpp. See sim_preset.h.

#include "sim_preset.h"

#include <cstring>

#include "shim/oc_shim.h"

#include "PresetEngine.h"          // real declarations; definitions below are ours
#include "shim/fw/HSUtils.h"
#include "shim/fw/OC_app_switcher.h"
#include "shim/fw/OC_digital_inputs.h"
#include "sim_bus.h"

// PresetBusUI.cpp kicks this to force a redraw. The simulator redraws every
// frame anyway, so it is a flag nothing reads -- but it has to exist, and a
// front end could show it.
uint_fast8_t MENU_REDRAW = 1;

namespace {

constexpr int kSlots = OC::PresetEngine::kNumSlots;   // 30
constexpr size_t kNameLen = OC::PresetEngine::kNameLen;

bool g_used[kSlots];
char g_name[kSlots][kNameLen + 1];

int8_t g_last_slot = -1;
uint32_t g_opcount = 0;
bool g_last_save_ok = true;
const char *g_last_recall_error = nullptr;

// The op in flight. The overlay watches OpCount for completion and gives up
// after 4000ms, so both the confirmation and the timeout path are reachable.
int g_pending_slot = -1;
bool g_pending_save = false;
uint32_t g_pending_done_ms = 0;

SimAppBase *g_app = nullptr;

// A save is a whole-module snapshot (config + app chunks + banks); a recall
// re-applies one. Neither is instant on the module. These are the SIMULATOR's
// guesses, not measurements -- what is real is that they are slower than the
// overlay's redraw and faster than its 4-second give-up.
constexpr uint32_t kSaveMs = 900;
constexpr uint32_t kRecallMs = 700;
constexpr uint32_t kRefusalMs = 60;   // a refusal is decided, not performed

void SetName(int slot, const char *n) {
  memset(g_name[slot], 0, sizeof(g_name[slot]));
  for (size_t i = 0; i < kNameLen && n[i]; ++i) g_name[slot][i] = n[i];
}

}  // namespace

void SimPresetInit(SimAppBase *app) {
  g_app = app;
  memset(g_used, 0, sizeof(g_used));
  for (int i = 0; i < kSlots; ++i) memset(g_name[i], 0, sizeof(g_name[i]));

  // Four slots pre-stored so the overlay has something to browse: a named one,
  // an unnamed one, and empties either side. Entirely invented.
  g_used[0] = true;  SetName(0, "OPENING");
  g_used[1] = true;  SetName(1, "DRONE BED");
  g_used[2] = true;                       // stored but unnamed
  g_used[6] = true;  SetName(6, "TUTTI");
  SimLog("preset slots 1,2,3,7 pre-stored with invented names -- the "
         "simulator has no SD card, so nothing here came off one");
}

void SimPresetTask() {
  if (g_pending_slot < 0) return;
  if (SimNowMs() < g_pending_done_ms) return;

  const int slot = g_pending_slot;
  g_pending_slot = -1;
  if (g_pending_save) {
    g_used[slot] = true;
    g_last_save_ok = true;
    g_last_slot = (int8_t)slot;
    SimLog("STORE preset %d completed -- and went NOWHERE: no SD card, no "
           "LittleFS, nothing written", slot + 1);
  } else if (g_last_recall_error) {
    SimLog("RECALL preset %d refused: %s", slot + 1, g_last_recall_error);
  } else {
    g_last_slot = (int8_t)slot;
    SimLog("RECALL preset %d completed (simulated; no module state changed)",
           slot + 1);
  }
  ++g_opcount;
}

void SimPresetRequestSave(uint8_t slot) {
  if (slot >= kSlots) return;
  g_pending_slot = slot;
  g_pending_save = true;
  g_last_recall_error = nullptr;
  g_pending_done_ms = SimNowMs() + kSaveMs;
  SimLog("bus broadcast SAVE %d (general call) -- writes are discarded",
         slot + 1);
}

void SimPresetRequestRecall(uint8_t slot) {
  if (slot >= kSlots) return;
  g_pending_slot = slot;
  g_pending_save = false;
  // The refusal the firmware reports immediately rather than as a timeout.
  g_last_recall_error = g_used[slot] ? nullptr : "EMPTY SLOT";
  g_pending_done_ms =
      SimNowMs() + (g_last_recall_error ? kRefusalMs : kRecallMs);
  SimLog("bus broadcast RECALL %d (general call)", slot + 1);
}

// --- OC::PresetEngine -------------------------------------------------------

namespace OC {
namespace PresetEngine {

int8_t LastSlot() { return g_last_slot; }
uint32_t OpCount() { return g_opcount; }
bool LastSaveOk() { return g_last_save_ok; }
const char *LastRecallError() { return g_last_recall_error; }

bool SlotUsed(uint8_t slot) {
  return slot < kSlots && g_used[slot];
}

const char *SlotName(uint8_t slot) {
  return slot < kSlots ? g_name[slot] : "";
}

void SetSlotName(uint8_t slot, const char *name) {
  if (slot >= kSlots) return;
  SetName(slot, name);
  SimLog("preset %d renamed to \"%s\" -- in RAM only; PBNAMES.BIN is not "
         "written here", slot + 1, g_name[slot]);
}

}  // namespace PresetEngine

// --- OC::DigitalInputs ------------------------------------------------------

namespace DigitalInputs {
bool read_immediate(DigitalInput) { return false; }
}  // namespace DigitalInputs

// --- OC::app_switcher -------------------------------------------------------

SimAppSwitcher app_switcher;

void SimCurrentApp::DispatchAppEvent(AppEvent e) {
  if (g_app) g_app->HandleAppEvent(e);
}

SimCurrentApp *SimAppSwitcher::current_app() {
  static SimCurrentApp one;
  return &one;
}

}  // namespace OC

// --- HS ---------------------------------------------------------------------

namespace HS {
void PokePopup(PopupType, const char *msg) {
  SimLog("popup: %s", msg ? msg : "");
}
}  // namespace HS
