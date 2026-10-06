#include "switch_nogui_platform.h"
#include "common/switch_thread_report.h"

#include "core/host.h"

#include "common/log.h"

#include "util/page_fault_handler.h"
#include "util/switch_exception_frame.h"

#include <switch.h>

#include <chrono>
#include <cstdio>

extern "C" {


void HandleFault(uint64_t pc, uint64_t lr, uint64_t fp, uint64_t fault_addr, Result desc)
{
  // into the log too (tico's debug log), not only the nxlink console; offsets
  // from the module's load address (__start__ is an absolute 0 in the PIE NRO)
  MemoryInfo text_info = {};
  u32 page_info = 0;
  svcQueryMemory(&text_info, &page_info, reinterpret_cast<u64>(&HandleFault));
  const uint64_t base = text_info.addr;
  const bool pc_in_text = (pc >= base && pc < text_info.addr + text_info.size);
  char line[256];
  if (pc_in_text)
  {
    std::snprintf(line, sizeof(line), "Crash: fault in .text at +0x%llx (type 0x%x), accessing %p",
                  static_cast<unsigned long long>(pc - base), desc, reinterpret_cast<void*>(fault_addr));
  }
  else
  {
    std::snprintf(line, sizeof(line), "Crash: fault at %p outside .text (JIT code?) (type 0x%x), accessing %p, LR +0x%llx",
                  reinterpret_cast<void*>(pc), desc, reinterpret_cast<void*>(fault_addr),
                  static_cast<unsigned long long>(lr - base));
  }
  std::puts(line);
  Log::Write("Crash", LOGLEVEL_ERROR, line);

  for (int frame_num = 0; frame_num <= 16 && fp != 0 && (fp & 0x7) == 0; frame_num++)
  {
    std::snprintf(line, sizeof(line), "Crash: stack frame %d +0x%llx", frame_num,
                  static_cast<unsigned long long>(lr - base));
    std::puts(line);
    Log::Write("Crash", LOGLEVEL_ERROR, line);
    lr = *reinterpret_cast<uint64_t*>(fp + 8);
    fp = *reinterpret_cast<uint64_t*>(fp);
  }

  svcBreak(BreakReason_Panic, 0, 0);
}

static_assert(sizeof(ExceptionFrameA64) == 0x78);

alignas(16) uint8_t switch_exception_stack_top[0x8000];

void switch_exception_handler(Result reason, ExceptionFrameA64* frame, u64 fp)
{
  if (PageFaultHandler::HandleSwitchException(frame))
    return;
  HandleFault(frame->pc, frame->lr, fp, frame->far, reason);
}
}

std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateSwitchPlatform()
{
  std::unique_ptr<SwitchNoGUIPlatform> platform(std::make_unique<SwitchNoGUIPlatform>());
  if (!platform->Initialize())
    platform.reset();
  return platform;
}

SwitchNoGUIPlatform::SwitchNoGUIPlatform()
{
  m_message_loop_running.store(true, std::memory_order_release);
}

SwitchNoGUIPlatform::~SwitchNoGUIPlatform() = default;

void SwitchNoGUIPlatform::AppletModeChange(AppletHookType type)
{
  switch (type)
  {
    case AppletHookType_OnOperationMode:
    {
      std::optional<WindowInfo> wi = GetPlatformWindowInfo();
      NoGUIHost::ProcessPlatformWindowResize(wi->surface_width, wi->surface_height, wi->surface_scale);
      break;
    }
    default:
      break;
  }
}

static void AppletModeChange(AppletHookType type, void* host_interface)
{
  static_cast<SwitchNoGUIPlatform*>(host_interface)->AppletModeChange(type);
}

bool SwitchNoGUIPlatform::Initialize()
{
  appletHook(&m_applet_cookie, ::AppletModeChange, this);

  return true;
}

void SwitchNoGUIPlatform::ReportError(std::string_view title, std::string_view message)
{
  // The title is usually just error which is not that informative
  // so we append the first line of the message
  std::string shortError(title);
  shortError += ": ";
  shortError += message.substr(0, message.find('\n'));
  // small hack to make the error messages nicer to look at.
  if (shortError[shortError.size() - 1] == ':')
    shortError[shortError.size() - 1] = '.';
  std::string longError(message);

  ErrorApplicationConfig errapp;
  errorApplicationCreate(&errapp, shortError.c_str(), longError.c_str());

  errorApplicationShow(&errapp);
}

bool SwitchNoGUIPlatform::ConfirmMessage(std::string_view title, std::string_view message)
{
  return true;
}

void SwitchNoGUIPlatform::SetDefaultConfig(SettingsInterface& si)
{
}

bool SwitchNoGUIPlatform::CreatePlatformWindow(std::string title)
{
  return true;
}

bool SwitchNoGUIPlatform::HasPlatformWindow() const
{
  return true;
}

void SwitchNoGUIPlatform::DestroyPlatformWindow()
{
}

std::optional<WindowInfo> SwitchNoGUIPlatform::GetPlatformWindowInfo()
{
  WindowInfo wi;
  AppletOperationMode mode = appletGetOperationMode();
  if (mode == AppletOperationMode_Handheld)
  {
    wi.surface_width = 1280;
    wi.surface_height = 720;
  }
  else
  {
    wi.surface_width = 1920;
    wi.surface_height = 1080;
  }
  wi.surface_scale = 1.2f;
  wi.surface_refresh_rate = 60.f;
  wi.surface_format = GPUTexture::Format::RGBA8;
  wi.type = WindowInfo::Type::Switch;
  wi.window_handle = nwindowGetDefault();
  return wi;
}

void SwitchNoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
}

std::optional<u32> SwitchNoGUIPlatform::ConvertHostKeyboardStringToCode(std::string_view str)
{
  return std::nullopt;
}

std::optional<std::string> SwitchNoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
  return std::nullopt;
}

void SwitchNoGUIPlatform::RunMessageLoop()
{
  // appletMainLoop() does not block: without a wait here this thread spun at
  // 100% of a core beside the emulation thread. Queued work wakes it at once;
  // applet messages (HOME, suspend) are checked every 10 ms.
  static constexpr auto APPLET_POLL_INTERVAL = std::chrono::milliseconds(10);
  while (m_message_loop_running.load(std::memory_order_acquire))
  {
    SwitchThreadReport::Tick("host loop");
    if (!appletMainLoop())
      NoGUIHost::StopRunning();

    std::unique_lock lock(m_callback_queue_mutex);
    m_callback_queue_cv.wait_for(lock, APPLET_POLL_INTERVAL, [this]() {
      return !m_callback_queue.empty() || !m_message_loop_running.load(std::memory_order_acquire);
    });
    while (!m_callback_queue.empty())
    {
      std::function<void()> func = std::move(m_callback_queue.front());
      m_callback_queue.pop_front();
      lock.unlock();
      func();
      lock.lock();
    }
  }
}

void SwitchNoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
  {
    std::unique_lock lock(m_callback_queue_mutex);
    m_callback_queue.push_back(std::move(func));
  }
  m_callback_queue_cv.notify_one();
}

void SwitchNoGUIPlatform::QuitMessageLoop()
{
  m_message_loop_running.store(false, std::memory_order_release);
  m_callback_queue_cv.notify_one();
}

void SwitchNoGUIPlatform::SetFullscreen(bool enabled)
{
}

bool SwitchNoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  return false;
}

bool SwitchNoGUIPlatform::OpenURL(std::string_view url)
{
  return false;
}

bool SwitchNoGUIPlatform::CopyTextToClipboard(std::string_view text)
{
  return false;
}
