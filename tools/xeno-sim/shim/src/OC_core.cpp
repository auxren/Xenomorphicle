// ---------------------------------------------------------------------------
// The core timers, replaced.
//
// On hardware this file also owns the two IntervalTimers that drive the 16.6
// kHz app ISR and the 1 kHz UI poll. The simulator has neither: sim_runtime.cpp
// calls the same two functions from one loop, on the virtual clock, at the same
// nominal rates. So the ORDER of ISR work against loop work is right and the
// RATES are right, but nothing here can overrun, preempt, or be late, and CPU
// load is not a thing the simulator has. See README.md.
//
// The deferred-task queue and the tick counter below are the real ones, copied
// only because the file they live in is otherwise all peripheral setup.
// ---------------------------------------------------------------------------
#include "OC_core.h"

std::queue<Task> fn_queue;

namespace OC {
namespace CORE {
volatile uint32_t ticks = 0;
volatile bool app_isr_enabled = false;
volatile bool display_update_enabled = false;
volatile bool app_loop_enabled = false;
}  // namespace CORE
}  // namespace OC

void OC::CORE::DeferTask(Task func) { fn_queue.emplace(func); }

void OC::CORE::FlushTasks() {
  if (fn_queue.empty()) return;
  while (!fn_queue.empty()) {
    fn_queue.front()();
    fn_queue.pop();
  }
}

// A fixed, plausible number. A host process has no RAM1/RAM2 split, and the
// simulator says nothing about the firmware's memory pressure.
int OC::CORE::FreeRam() { return 128 * 1024; }
