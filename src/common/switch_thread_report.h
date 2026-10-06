#pragma once

// Debug builds on the Switch (TICO_DEBUG_LOG): call from a thread's loop;
// every 5 seconds it logs how busy the thread was on each core, from the
// kernel's per-thread tick counts. Nothing elsewhere.
namespace SwitchThreadReport {
void Tick(const char* name);
}
