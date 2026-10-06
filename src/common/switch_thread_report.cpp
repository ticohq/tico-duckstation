#include "switch_thread_report.h"

#if defined(__SWITCH__) && defined(TICO_DEBUG_LOG)

#include "log.h"

#include <switch.h>

Log_SetChannel(ThreadReport);

namespace SwitchThreadReport {

static constexpr u64 WINDOW_NS = 5000000000ull;

void Tick(const char* name)
{
  static thread_local u64 s_window_start = 0;
  static thread_local u64 s_core_ticks[4] = {};

  const u64 now = armGetSystemTick();
  if (s_window_start != 0 && armTicksToNs(now - s_window_start) < WINDOW_NS)
    return;

  u64 ticks[4] = {};
  for (u64 core = 0; core < 4; core++)
    svcGetInfo(&ticks[core], InfoType_ThreadTickCount, CUR_THREAD_HANDLE, core);

  if (s_window_start != 0)
  {
    const double window = static_cast<double>(now - s_window_start);
    INFO_LOG("thread {}: core0 {:.1f}% core1 {:.1f}% core2 {:.1f}% core3 {:.1f}% (now on core {})", name,
                100.0 * (ticks[0] - s_core_ticks[0]) / window, 100.0 * (ticks[1] - s_core_ticks[1]) / window,
                100.0 * (ticks[2] - s_core_ticks[2]) / window, 100.0 * (ticks[3] - s_core_ticks[3]) / window,
                svcGetCurrentProcessorNumber());
  }
  else
  {
    u64 core_mask = 0;
    svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    INFO_LOG("thread {}: started on core {} (process core mask 0x{:X})", name, svcGetCurrentProcessorNumber(),
                core_mask);
  }

  for (int core = 0; core < 4; core++)
    s_core_ticks[core] = ticks[core];
  s_window_start = now;
}

} // namespace SwitchThreadReport

#else

void SwitchThreadReport::Tick(const char*)
{
}

#endif
