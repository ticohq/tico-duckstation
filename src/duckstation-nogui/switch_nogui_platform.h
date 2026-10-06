#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include "nogui_host.h"
#include "nogui_platform.h"

#include <switch.h>

class SwitchNoGUIPlatform : public NoGUIPlatform
{
public:
  SwitchNoGUIPlatform();
  ~SwitchNoGUIPlatform();

  bool Initialize();

  void ReportError(std::string_view title, std::string_view message) override;
  bool ConfirmMessage(std::string_view title, std::string_view message) override;

  void SetDefaultConfig(SettingsInterface& si) override;

  bool CreatePlatformWindow(std::string title) override;
  bool HasPlatformWindow() const override;
  void DestroyPlatformWindow() override;
  std::optional<WindowInfo> GetPlatformWindowInfo() override;
  void SetPlatformWindowTitle(std::string title) override;

  std::optional<u32> ConvertHostKeyboardStringToCode(std::string_view str) override;
  std::optional<std::string> ConvertHostKeyboardCodeToString(u32 code) override;

  void RunMessageLoop() override;
  void ExecuteInMessageLoop(std::function<void()> func) override;
  void QuitMessageLoop() override;


  void SetFullscreen(bool enabled) override;

  bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height) override;

  bool OpenURL(std::string_view url) override;
  bool CopyTextToClipboard(std::string_view text) override;

  void AppletModeChange(AppletHookType type);

private:
  AppletHookCookie m_applet_cookie;

  std::deque<std::function<void()>> m_callback_queue;
  std::mutex m_callback_queue_mutex;
  std::condition_variable m_callback_queue_cv;
  std::atomic_bool m_message_loop_running{false};
};
