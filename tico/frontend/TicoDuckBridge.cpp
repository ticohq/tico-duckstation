// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

// The tico overlay inside DuckStation. DuckStation's CPU thread emulates,
// presents (System::PresentDisplay) and polls the controllers, so everything
// here runs on it; what the menu chooses is carried out between frames
// (Host::RunOnCPUThread), never in the middle of one.

#include "tico/TicoDuckBridge.h"

#include "DuckDiscs.h"
#include "TicoChainload.h"
#include "TicoConfig.h"
#include "TicoLogger.h"
#include "TicoOverlayHost.h"
#include "UsbStorage.h"
#include "overlay/imgui_compat.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include "core/achievements.h"
#include "core/host.h"
#include "core/settings.h"
#include "core/system.h"
#include "util/gpu_device.h"
#include "util/platform_misc.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/timer.h"
#include "util/imgui_manager.h"

#define STB_IMAGE_IMPLEMENTATION
#include "deps/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "deps/stb/stb_image_write.h"
#include "imgui.h"
#include <json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

Log_SetChannel(TicoDuck);

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayConfig = SwitchFrontend::TicoConfig;
using SwitchFrontend::OverlayTranslation::tr;

namespace TicoDuck
{
namespace
{

constexpr unsigned kPorts = 2;
// directional hold-repeat in the menu, in polls (one a frame)
constexpr int kNavInitialDelay = 14;
constexpr int kNavRepeat = 6;
// the overlay's layout is designed for a 720p display
constexpr float kDesignHeight = 720.0f;

std::string FileStem(const std::string& path)
{
  std::string name = path;
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos)
    name.erase(0, slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos)
    name.erase(dot);
  return name;
}

std::string ReadTextFile(const char* path)
{
  std::ifstream input(path);
  if (!input.is_open())
    return {};
  std::ostringstream ss;
  ss << input.rdbuf();
  return ss.str();
}

// The launch: what tico started, and what the session goes on to.
int s_argc = 0;
char** s_argv = nullptr;
std::string s_rom_path; // as launched (an .m3u stays the .m3u), names the states
std::string s_resolved_rom; // a USB game's mount path, which argv[1] then points at
std::string s_title;
bool s_chainload_launcher = false;
bool s_relaunch = false;
ExitApplicationCallback s_exit_callback = nullptr;
SettingsReloadCallback s_settings_reload_callback = nullptr;

// The overlay
class DuckOverlayHost;
std::unique_ptr<DuckOverlayHost> s_host;
bool s_ready = false;
bool s_enabled = false; // between Initialize and Shutdown: the overlay follows the GPU device
bool s_menu_open = false;
bool s_paused_by_menu = false;
bool s_offer_resume = false;
bool s_auto_saved = false; // the session's auto save is written
ImDrawData* s_draw_data = nullptr;
Common::Timer::Value s_last_frame = 0;

// Menu input
uint64_t s_previous_buttons = 0;
uint64_t s_nav_held = 0;
int s_nav_repeat = 0;

// HUD
int s_hud_frames = 0;
float s_hud_seconds = 0.0f;
float s_hud_fps = 0.0f;

// Change Disc rows, in menu order: a path to insert, or a playlist entry
struct DiscChoice
{
  std::string path;
  int sub_image = -1;
};
std::vector<DiscChoice> s_discs;
std::array<ImTextureID, 6> s_slot_pictures{};

#ifdef __SWITCH__
bool s_touch_ready = false;
#endif

std::string StatePath(int index)
{
  const std::string game = s_rom_path.empty() ? System::GetMediaFileName() : s_rom_path;
  return fmt::format("{}/{}.state{}", TicoConfig::StatesPath, FileStem(game), index);
}

std::string RestartMarkerPath()
{
  return StatePath(OverlayUI::kAutoStateSlot - 1) + ".restart";
}

// Adapts DuckStation and its GPU device to the overlay's host interfaces.
class DuckOverlayHost final : public IOverlayHost, public IOverlayRAHost
{
public:
  ~DuckOverlayHost() override = default;

  std::string GetGamePath() override { return s_rom_path; }
  bool IsGameLoaded() override { return System::IsValid(); }

  bool StateSlotExists(int index) override { return FileSystem::FileExists(StatePath(index).c_str()); }
  void SaveStateSlot(int index) override
  {
    if (!System::IsValid())
      return;
    FileSystem::EnsureDirectoryExists(TicoConfig::StatesPath, true);
    Error error;
    if (!System::SaveState(StatePath(index).c_str(), &error, false))
      LOG_ERROR("OVERLAY", "Save state %d failed: %s", index, error.GetDescription().c_str());
  }
  void LoadStateSlot(int index) override
  {
    if (!System::IsValid())
      return;
    Error error;
    if (!System::LoadState(StatePath(index).c_str(), &error, false))
      LOG_ERROR("OVERLAY", "Load state %d failed: %s", index, error.GetDescription().c_str());
  }
  void SwapDisc(const std::string& path) override
  {
    if (System::IsValid())
      System::InsertMedia(path.c_str());
  }

  ImTextureID CreateTextureRGBA(const unsigned char* rgba, int width, int height) override
  {
    if (!g_gpu_device || !rgba || width <= 0 || height <= 0)
      return nullptr;
    std::unique_ptr<GPUTexture> texture =
      g_gpu_device->CreateTexture(static_cast<u32>(width), static_cast<u32>(height), 1, 1, 1,
                                  GPUTexture::Type::Texture, GPUTexture::Format::RGBA8, rgba,
                                  static_cast<u32>(width) * 4);
    if (!texture)
      return nullptr;
    GPUTexture* id = texture.get();
    m_textures.emplace(id, std::move(texture));
    return id;
  }
  void DestroyTexture(ImTextureID tex) override
  {
    if (tex)
      m_textures.erase(static_cast<GPUTexture*>(tex));
  }

  IOverlayRAHost* RA() override { return this; }
  std::mutex& Mutex() override { return m_ra_mutex; }
  std::vector<RANotification>& Notifications() override { return m_notifications; }
  RAAlertPosition AlertPosition() const override { return m_alert_position; }
  ImTextureID IconTexture() const override { return m_ra_icon; }
  void SetIconTexture(ImTextureID tex) override { m_ra_icon = tex; }
  ImTextureID BadgeTexture(const std::string& badge) const override
  {
    const auto it = m_badges.find(badge);
    return it != m_badges.end() ? it->second : nullptr;
  }

  void SetAlertPosition(RAAlertPosition position) { m_alert_position = position; }

  // Uploads the badges of queued toasts. A badge may still be downloading:
  // its file is looked for again every half second, not every frame.
  void LoadBadges()
  {
    std::vector<std::string> wanted;
    {
      std::lock_guard lock(m_ra_mutex);
      for (const RANotification& notification : m_notifications)
      {
        if (notification.textureId == nullptr && !notification.badge_name.empty() &&
            notification.badge_name != "ra_icon" && !m_badges.count(notification.badge_name))
          wanted.push_back(notification.badge_name);
      }
    }
    for (const std::string& badge : wanted)
    {
      int& wait = m_badge_retry[badge];
      if (wait > 0)
      {
        wait--;
        continue;
      }
      int width = 0, height = 0, channels = 0;
      unsigned char* rgba = stbi_load(badge.c_str(), &width, &height, &channels, 4);
      if (!rgba)
      {
        wait = 30;
        continue;
      }
      if (ImTextureID texture = CreateTextureRGBA(rgba, width, height))
        m_badges.emplace(badge, texture);
      stbi_image_free(rgba);
      m_badge_retry.erase(badge);
    }
  }

private:
  std::unordered_map<GPUTexture*, std::unique_ptr<GPUTexture>> m_textures;
  std::mutex m_ra_mutex;
  std::vector<RANotification> m_notifications;
  RAAlertPosition m_alert_position = RAAlertPosition::TopRight;
  ImTextureID m_ra_icon = nullptr;
  std::map<std::string, ImTextureID> m_badges;
  std::map<std::string, int> m_badge_retry;
};

// tico's RetroAchievements toast corner (accounts.jsonc).
RAAlertPosition ConfiguredAlertPosition()
{
  const nlohmann::json root =
    nlohmann::json::parse(ReadTextFile("sdmc:/tico/config/accounts.jsonc"), nullptr, false, true);
  const std::string position =
    root.is_object() ? root.value("ra_alert_position", std::string("top_right")) : std::string("top_right");
  if (position == "top_left")
    return RAAlertPosition::TopLeft;
  if (position == "bottom_left")
    return RAAlertPosition::BottomLeft;
  if (position == "bottom_right")
    return RAAlertPosition::BottomRight;
  return RAAlertPosition::TopRight;
}

std::string TrFormat(const char* key, int value)
{
  const std::string format = tr(key);
  char text[256];
  std::snprintf(text, sizeof(text), format.c_str(), value);
  return text;
}

std::vector<std::string> ControllerNames()
{
  std::vector<std::string> names(kPorts);
#ifdef __SWITCH__
  auto name = [](u32 style) -> std::string {
    const char* key = nullptr;
    if (style & HidNpadStyleTag_NpadFullKey)
      key = "emulator_pad_pro";
    else if (style & HidNpadStyleTag_NpadHandheld)
      key = "emulator_pad_handheld";
    else if (style & HidNpadStyleTag_NpadJoyDual)
      key = "emulator_pad_joycon_pair";
    else if (style & HidNpadStyleTag_NpadJoyLeft)
      key = "emulator_pad_joycon_left";
    else if (style & HidNpadStyleTag_NpadJoyRight)
      key = "emulator_pad_joycon_right";
    else if (style & HidNpadStyleTag_NpadGc)
      key = "emulator_pad_gamecube";
    else if (style)
      key = "emulator_pad_other";
    return key ? tr(key) : std::string();
  };
  for (unsigned player = 0; player < kPorts; ++player)
  {
    u32 style = hidGetNpadStyleSet(static_cast<HidNpadIdType>(HidNpadIdType_No1 + player));
    // player 1 also reads the handheld Joy-Con
    if (player == 0 && !style)
      style = hidGetNpadStyleSet(HidNpadIdType_Handheld);
    names[player] = name(style);
  }
#endif
  return names;
}

bool ShowControllerOrder()
{
#ifdef __SWITCH__
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 0;
  arg.hdr.player_count_max = kPorts;
  HidLaControllerSupportResultInfo info{};
  return R_SUCCEEDED(hidLaShowControllerSupport(&info, &arg));
#else
  return false;
#endif
}

#ifdef __SWITCH__
bool PromptKeyboard(const char* header, const std::string& initial, size_t max_length, std::string& output)
{
  SwkbdConfig keyboard;
  if (R_FAILED(swkbdCreate(&keyboard, 0)))
    return false;
  swkbdConfigMakePresetDefault(&keyboard);
  swkbdConfigSetHeaderText(&keyboard, header);
  if (!initial.empty())
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
  swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(max_length));
  std::vector<char> text(max_length + 1);
  const Result result = swkbdShow(&keyboard, text.data(), text.size());
  swkbdClose(&keyboard);
  if (R_FAILED(result))
    return false;
  output = text.data();
  return true;
}
#endif

// The game is paused while the menu is open, and resumed when it closes if
// the menu paused it.
void SyncPause()
{
  if (s_menu_open && !s_paused_by_menu && System::IsValid() && !System::IsPaused())
  {
    s_paused_by_menu = true;
    Host::RunOnCPUThread([]() {
      if (System::IsValid() && !System::IsPaused())
        System::PauseSystem(true);
    });
  }
  else if (!s_menu_open && s_paused_by_menu)
  {
    s_paused_by_menu = false;
    if (s_chainload_launcher || s_relaunch)
      return;
    Host::RunOnCPUThread([]() {
      if (System::IsValid() && System::IsPaused())
        System::PauseSystem(false);
    });
  }
}

void OpenMenu()
{
  if (!s_ready || s_menu_open)
    return;
  s_menu_open = true;
  s_nav_held = 0;
  s_nav_repeat = 0;
  OverlayUI::SetHardcoreMode(Achievements::IsHardcoreModeActive());
  ImGuiOverlay::SetVisible(true);
  SyncPause();
}

void CloseMenu()
{
  if (!s_menu_open)
    return;
  s_menu_open = false;
  ImGuiOverlay::SetVisible(false);
  SyncPause();
}

// The game's state goes to the auto slot (listed first in Load State),
// whatever ends the session: Exit, Restart or HOME.
void WriteAutoSave()
{
  if (s_auto_saved || !s_host || !System::IsValid())
    return;
  s_auto_saved = true;
  s_host->SaveStateSlot(OverlayUI::kAutoStateSlot - 1);
}

void EndSession()
{
  CloseMenu();
  // after this frame: saved before the session's end shuts the system down
  Host::RunOnCPUThread([]() {
    WriteAutoSave();
    if (s_exit_callback)
      s_exit_callback();
    else
      Host::RequestSystemShutdown(false, false);
  });
}

// Once the game's first frame has run, the menu asks whether to continue from
// the auto save, if there is one (not after Restart, nor in hardcore).
void OfferResume()
{
  s_offer_resume = false;
  if (std::remove(RestartMarkerPath().c_str()) == 0)
    return;
  if (Achievements::IsHardcoreModeActive() || !s_host->StateSlotExists(OverlayUI::kAutoStateSlot - 1))
    return;
  // tico's General > Continue Last Game
  const std::string mode = OverlayConfig::ResumeOnLaunch();
  if (mode == "never")
    return;
  if (mode == "always")
  {
    Host::RunOnCPUThread([]() {
      s_host->LoadStateSlot(OverlayUI::kAutoStateSlot - 1);
      OverlayUI::ShowToast(tr("emulator_auto_loaded"));
    });
    return;
  }
  OpenMenu();
  if (s_menu_open)
    OverlayUI::ShowResumePrompt();
}

// Carries out what the menu chose while building the last frame.
void RunMenuAction()
{
  using OverlayUI::Action;
  const Action action = ImGuiOverlay::ConsumeAction();
  if (OverlayUI::ConsumeSettingsChanged() && s_settings_reload_callback)
    Host::RunOnCPUThread([]() { s_settings_reload_callback(); });

  switch (action)
  {
    case Action::None:
      return;
    case Action::Resume:
      CloseMenu();
      return;
    case Action::Exit:
      INFO_LOG("{}", "Exit requested");
      s_chainload_launcher = true;
      EndSession();
      return;
    case Action::Restart:
      // this NRO starts again with the same arguments (a renderer change
      // takes effect then); the relaunch finds the marker and skips the
      // resume prompt, Restart meaning from the start
      INFO_LOG("{}", "Restart requested");
      if (System::IsValid())
      {
        if (std::FILE* marker = std::fopen(RestartMarkerPath().c_str(), "wb"))
          std::fclose(marker);
      }
      s_relaunch = true;
      EndSession();
      return;
    case Action::Reset:
      Host::RunOnCPUThread([]() {
        if (System::IsValid())
          System::ResetSystem();
      });
      CloseMenu();
      return;
    case Action::ControllerOrder:
      Host::RunOnCPUThread([]() {
        if (!ShowControllerOrder())
          OverlayUI::ShowToast(tr("emulator_controllers_failed"), OverlayUI::ToastCorner::TopRight);
      });
      return;
    case Action::SwapDisc:
    {
      const int index = OverlayUI::ConsumeDiscIndex();
      if (index >= 0 && index < static_cast<int>(s_discs.size()))
      {
        const DiscChoice disc = s_discs[static_cast<size_t>(index)];
        Host::RunOnCPUThread([disc]() {
          if (!System::IsValid())
            return;
          if (disc.sub_image >= 0)
            System::SwitchMediaSubImage(static_cast<u32>(disc.sub_image));
          else
            System::InsertMedia(disc.path.c_str());
        });
      }
      CloseMenu();
      return;
    }
    case Action::EditText:
    {
      const OverlayConfig::OptionDef* option = OverlayUI::ConsumeTextEditOption();
#ifdef __SWITCH__
      if (option)
      {
        Host::RunOnCPUThread([option]() {
          std::string value;
          const size_t length = option->max_length > 0 ? option->max_length : 64;
          if (PromptKeyboard(tr(option->label_key).c_str(), OverlayConfig::GetOptionValue(*option), length, value))
          {
            OverlayConfig::SetOptionValue(*option, value);
            OverlayUI::NotifyOptionEdited(*option);
          }
        });
      }
#else
      (void)option;
#endif
      return;
    }
    default:
      break;
  }

  if (!System::IsValid())
    return;
  if (OverlayUI::IsSaveStateAction(action))
  {
    const int slot = OverlayUI::GetStateSlotForAction(action);
    Host::RunOnCPUThread([slot]() {
      s_host->SaveStateSlot(slot - 1);
      OverlayUI::ShowToast(TrFormat("emulator_state_saved", slot));
    });
    CloseMenu();
  }
  else if (OverlayUI::IsLoadStateAction(action))
  {
    const int slot = OverlayUI::GetStateSlotForAction(action);
    Host::RunOnCPUThread([slot]() {
      s_host->LoadStateSlot(slot - 1);
      if (slot == OverlayUI::kAutoStateSlot)
        OverlayUI::ShowToast(tr("emulator_auto_loaded"));
      else
        OverlayUI::ShowToast(TrFormat("emulator_state_loaded", slot));
    });
    CloseMenu();
  }
}

void UpdateHud(float delta_time)
{
  s_hud_frames++;
  s_hud_seconds += delta_time;
  if (s_hud_seconds >= 0.5f)
  {
    s_hud_fps = static_cast<float>(s_hud_frames) / s_hud_seconds;
    s_hud_frames = 0;
    s_hud_seconds = 0.0f;
  }
  OverlayUI::HudStats stats;
  stats.fps = System::IsValid() ? System::GetFPS() : s_hud_fps;
  OverlayUI::SetHudStats(stats);
}

OverlayUI::SlotPreview SlotPreview(int slot)
{
  OverlayUI::SlotPreview preview;
  if (slot < 1 || slot > static_cast<int>(s_slot_pictures.size()))
    return preview;
  ImTextureID& picture = s_slot_pictures[static_cast<size_t>(slot - 1)];
  s_host->DestroyTexture(picture); // the slot may have been saved again
  picture = nullptr;
  const std::string path = StatePath(slot - 1);
  if (!FileSystem::FileExists(path.c_str()))
    return preview;
  // DuckStation keeps a picture of the game in each state
  std::optional<ExtendedSaveStateInfo> info = System::GetExtendedSaveStateInfo(path.c_str());
  if (!info)
    return preview;
  char when[32];
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&info->timestamp));
  preview.saved_at = when;
  if (info->screenshot.IsValid())
  {
    picture = s_host->CreateTextureRGBA(reinterpret_cast<const unsigned char*>(info->screenshot.GetPixels()),
                                        static_cast<int>(info->screenshot.GetWidth()),
                                        static_cast<int>(info->screenshot.GetHeight()));
  }
  preview.texture = SwitchFrontend::FromTextureId(picture);
  preview.aspect = g_settings.gpu_widescreen_hack ? 16.0f / 9.0f : 4.0f / 3.0f;
  return preview;
}

std::vector<OverlayUI::DiscMenuEntry> ListDiscs()
{
  std::vector<OverlayUI::DiscMenuEntry> entries;
  s_discs.clear();
  if (!System::IsValid())
    return entries;
  // a playlist (.m3u) or multi-disc image: DuckStation knows its discs
  if (System::HasMediaSubImages())
  {
    const u32 count = System::GetMediaSubImageCount();
    const u32 current = System::GetMediaSubImageIndex();
    for (u32 i = 0; i < count; i++)
    {
      std::string title = System::GetMediaSubImageTitle(i);
      if (title.empty())
        title = fmt::format("Disc {}", i + 1);
      entries.push_back({std::move(title), i == current});
      s_discs.push_back({std::string(), static_cast<int>(i)});
    }
    return entries;
  }
  // otherwise the other discs are found beside the launched one
  const std::string current = NormalizeDiscPath(System::GetMediaFileName());
  for (const DiscEntry& disc : ScanDiscs(NormalizeDiscPath(s_rom_path)))
  {
    entries.push_back({disc.displayName, disc.romPath == current});
    s_discs.push_back({disc.romPath, -1});
  }
  return entries;
}

bool InitOverlay()
{
  if (!g_gpu_device)
    return false;
  s_host = std::make_unique<DuckOverlayHost>();
  s_host->SetAlertPosition(ConfiguredAlertPosition());
  OverlayConfig::ReloadConfig();
  OverlayConfig::SetGame(s_rom_path);
  ImGuiOverlay::SetExtraGlyphText(s_title);
  const float font_scale = std::max(1.0f, static_cast<float>(g_gpu_device->GetWindowHeight()) / kDesignHeight);
  if (!ImGuiOverlay::Init(s_host.get(), font_scale))
  {
    s_host.reset();
    return false;
  }
  OverlayUI::SetGameTitle(s_title);
  // the menu's slots 1..6 are the state files .state0 .. .state5
  OverlayUI::SetSlotOccupiedCallback([](int slot) { return slot >= 1 && s_host->StateSlotExists(slot - 1); });
  s_slot_pictures = {};
  OverlayUI::SetSlotPreviewCallback(SlotPreview);
  OverlayUI::SetDiscCallback(ListDiscs);
  OverlayUI::PlayerCallbacks players;
  players.ports = [] { return ControllerNames(); };
  OverlayUI::SetPlayerCallbacks(std::move(players));
  OverlayUI::ReloadSettings();
  return true;
}

void ShutdownOverlay()
{
  if (!s_ready)
    return;
  OverlayUI::SetSlotOccupiedCallback(nullptr);
  OverlayUI::SetSlotPreviewCallback(nullptr);
  OverlayUI::SetDiscCallback(nullptr);
  OverlayUI::SetPlayerCallbacks({});
  for (ImTextureID& picture : s_slot_pictures)
  {
    s_host->DestroyTexture(picture);
    picture = nullptr;
  }
  ImGuiOverlay::Shutdown(); // frees its textures through the host
  s_host.reset();           // and the rest
  s_draw_data = nullptr;
  s_ready = false;
  s_menu_open = false;
}

bool IsTicoSoundEnabled()
{
  const nlohmann::json root =
    nlohmann::json::parse(ReadTextFile("sdmc:/tico/config/audio.jsonc"), nullptr, false, true);
  if (!root.is_object())
    return false;
  const auto it = root.find("sound_enabled");
  if (it == root.end())
    return false;
  if (it->is_boolean())
    return it->get<bool>();
  return it->is_string() && it->get<std::string>() == "true";
}

} // namespace

void PrepareLaunch(int argc, char* argv[])
{
  s_argc = argc;
  s_argv = argv;
  // DuckStation's own messages (OSD) in tico's font
  ImGuiManager::SetFontPathAndRange("romfs:/fonts/font.ttf", {});
  UsbStorage::Init(); // drives mount in the background
  if (argc > 1 && argv[1] && argv[1][0] != '-')
  {
    s_rom_path = argv[1];
    // tico names a game on a USB drive by the drive's id: find where it is mounted
    if (UsbStorage::IsToken(s_rom_path))
    {
      s_resolved_rom = UsbStorage::Resolve(s_rom_path);
      if (s_resolved_rom.empty())
        ERROR_LOG("USB drive for {} is not connected", s_rom_path);
      else
        argv[1] = s_resolved_rom.data();
      s_rom_path = argv[1];
    }
  }
  if (argc > 2 && argv[2] && argv[2][0] != '\0' && argv[2][0] != '-')
    s_title = argv[2];
  else if (!s_rom_path.empty())
    s_title = FileStem(s_rom_path);
  else
    s_title = "DuckStation";
}

void SetExitApplicationCallback(ExitApplicationCallback callback)
{
  s_exit_callback = callback;
}

void SetSettingsReloadCallback(SettingsReloadCallback callback)
{
  s_settings_reload_callback = callback;
}

void Initialize()
{
  FileSystem::EnsureDirectoryExists(TicoConfig::StatesPath, true);
#ifdef __SWITCH__
  if (!s_touch_ready)
  {
    hidInitializeTouchScreen();
    s_touch_ready = true;
  }
#endif
  s_enabled = true;
  s_ready = InitOverlay();
  if (!s_ready)
  {
    ERROR_LOG("{}", "tico overlay unavailable");
    return;
  }
  s_last_frame = Common::Timer::GetCurrentValue();
  s_offer_resume = !s_rom_path.empty();
}

void OnGPUDeviceCreated()
{
  if (!s_enabled || s_ready)
    return;
  s_ready = InitOverlay();
  if (!s_ready)
    ERROR_LOG("{}", "tico overlay unavailable on the new GPU device");
  s_last_frame = Common::Timer::GetCurrentValue();
}

void OnGPUDeviceReleasing()
{
  // the menu closes with it; the session (auto save, resume prompt) goes on
  ShutdownOverlay();
}

void Shutdown()
{
  WriteAutoSave(); // HOME: the session ends without the menu
  s_enabled = false;
  ShutdownOverlay();
  s_previous_buttons = 0;
}

void RenderOverlay()
{
  if (!s_ready || !g_gpu_device)
    return;

  RunMenuAction();
  if (s_offer_resume && System::IsRunning())
    OfferResume();

  const Common::Timer::Value now = Common::Timer::GetCurrentValue();
  float delta_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(now - s_last_frame));
  s_last_frame = now;
  if (delta_time <= 0.0f || delta_time > 0.25f)
    delta_time = 1.0f / 60.0f;

  UpdateHud(delta_time);
  s_host->LoadBadges();
  s_draw_data = ImGuiOverlay::BuildFrame(static_cast<float>(g_gpu_device->GetWindowWidth()),
                                         static_cast<float>(g_gpu_device->GetWindowHeight()), delta_time);
}

void DrawOverlay()
{
  if (s_ready && s_draw_data && g_gpu_device)
    g_gpu_device->RenderImGuiDrawData(s_draw_data);
}

bool ShouldChainloadLauncher()
{
  return s_chainload_launcher;
}

void ExitApplication()
{
  UsbStorage::Shutdown(); // flush and unmount before tico takes over again
  const Tico::LogCallback log = [](const std::string& message) { INFO_LOG("{}", message.c_str()); };
  if (s_relaunch)
    Tico::RelaunchSelf(s_argc, s_argv, log);
  else if (s_chainload_launcher)
    Tico::ChainloadLauncher(log);
}

void PushRANotification(std::string title, std::string description, std::string badge_path, float duration)
{
  if (!s_host || (title.empty() && description.empty()))
    return;

  RANotification notification;
  notification.title = std::move(title);
  notification.description = std::move(description);
  notification.badge_name = badge_path.empty() ? "ra_icon" : std::move(badge_path);
  notification.duration = std::max(duration, 1.0f);

  std::lock_guard lock(s_host->Mutex());
  std::vector<RANotification>& notifications = s_host->Notifications();
  if (notifications.size() >= 8)
    notifications.erase(notifications.begin());
  notifications.push_back(std::move(notification));
}

void PlayRATrophySound()
{
  if (!IsTicoSoundEnabled())
    return;

  if (!PlatformMisc::PlaySoundAsync("romfs:/assets/trophy.mp3"))
    PlatformMisc::PlaySoundAsync("romfs:/assets/trophy.wav");
}

#ifdef __SWITCH__
bool HandleSwitchInput(unsigned controller_index, uint64_t buttons, const HidAnalogStickState& left,
                       const HidAnalogStickState& right)
{
  (void)right;
  if (!s_ready)
    return false;
  if (controller_index != 0)
    return s_menu_open;

  const uint64_t pressed = buttons & ~s_previous_buttons;
  s_previous_buttons = buttons;

  // Plus+Minus only ever opens the menu; while the combo is held it is kept
  // from the game.
  const bool combo = (buttons & HidNpadButton_Plus) && (buttons & HidNpadButton_Minus);
  if (combo && !s_menu_open)
    OpenMenu();
  if (!s_menu_open)
    return combo;

  // directions: D-pad and left stick, firing on press and then repeating
  uint64_t held = 0;
  if ((buttons & HidNpadButton_Up) || left.y > 16000)
    held |= HidNpadButton_Up;
  if ((buttons & HidNpadButton_Down) || left.y < -16000)
    held |= HidNpadButton_Down;
  if ((buttons & HidNpadButton_Left) || left.x < -16000)
    held |= HidNpadButton_Left;
  if ((buttons & HidNpadButton_Right) || left.x > 16000)
    held |= HidNpadButton_Right;
  uint64_t fire = held & ~s_nav_held;
  if (held != 0 && held == s_nav_held)
  {
    if (--s_nav_repeat <= 0)
    {
      fire |= held;
      s_nav_repeat = kNavRepeat;
    }
  }
  else if (fire != 0)
  {
    s_nav_repeat = kNavInitialDelay;
  }
  s_nav_held = held;

  ImGuiOverlay::FeedNav({
    .up = (fire & HidNpadButton_Up) != 0,
    .down = (fire & HidNpadButton_Down) != 0,
    .left = (fire & HidNpadButton_Left) != 0,
    .right = (fire & HidNpadButton_Right) != 0,
    .accept = (pressed & HidNpadButton_A) != 0,
    .cancel = (pressed & HidNpadButton_B) != 0,
  });

  HidTouchScreenState touch = {};
  const bool touched = hidGetTouchScreenStates(&touch, 1) > 0 && touch.count > 0;
  ImGuiOverlay::FeedTouch({touched, touched ? static_cast<float>(touch.touches[0].x) : 0.0f,
                           touched ? static_cast<float>(touch.touches[0].y) : 0.0f});
  return true;
}
#endif

} // namespace TicoDuck
