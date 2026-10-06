// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tico/TicoDuckBridge.h"

#include "tico/TicoChainload.h"
#include "tico/TicoConfig.h"
#include "tico/TicoOverlay.h"
#include "tico/TicoUtils.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "core/host.h"
#include "core/settings.h"
#include "core/system.h"
#include "util/gpu_device.h"
#include "util/image.h"
#include "util/imgui_manager.h"
#include "util/platform_misc.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#define NANOSVG_IMPLEMENTATION
#include "tico/deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "tico/deps/nanosvg/nanosvgrast.h"

Log_SetChannel(TicoDuck);

namespace TicoDuck
{
namespace
{
uint64_t s_previous_buttons = 0;
bool s_chainload_launcher = false;
ExitApplicationCallback s_exit_application_callback = nullptr;
std::unique_ptr<GPUTexture> s_avatar_texture;
std::unique_ptr<GPUTexture> s_bolt_texture;
std::unique_ptr<GPUTexture> s_ra_icon_texture;
bool s_avatar_load_attempted = false;
bool s_bolt_load_attempted = false;
bool s_ra_icon_load_attempted = false;
bool s_overlay_pause_active = false;
bool s_resume_after_overlay = false;
std::vector<std::string> s_disc_paths;
std::mutex s_ra_notifications_mutex;
std::vector<Tico::RANotificationSnapshot> s_ra_notifications;
std::map<std::string, std::unique_ptr<GPUTexture>> s_ra_badge_textures;
#ifdef __SWITCH__
bool s_psm_initialized = false;
#endif

Tico::TicoOverlay& Overlay()
{
  static Tico::TicoOverlay overlay;
  return overlay;
}

bool ReadTicoJsonStringValue(const std::string& text, const char* key, std::string& value)
{
  const std::string pattern = std::string("\"") + key + "\"";
  const size_t key_pos = text.find(pattern);
  if (key_pos == std::string::npos)
    return false;

  const size_t colon_pos = text.find(':', key_pos + pattern.size());
  if (colon_pos == std::string::npos)
    return false;

  const size_t quote_pos = text.find('"', colon_pos + 1);
  if (quote_pos == std::string::npos)
    return false;

  value.clear();
  bool escape = false;
  for (size_t i = quote_pos + 1; i < text.size(); i++)
  {
    const char ch = text[i];
    if (escape)
    {
      value.push_back(ch);
      escape = false;
      continue;
    }
    if (ch == '\\')
    {
      escape = true;
      continue;
    }
    if (ch == '"')
      return true;
    value.push_back(ch);
  }

  return false;
}

bool ReadTicoJsonBoolValue(const std::string& text, const char* key, bool& value)
{
  const std::string pattern = std::string("\"") + key + "\"";
  const size_t key_pos = text.find(pattern);
  if (key_pos == std::string::npos)
    return false;

  const size_t colon_pos = text.find(':', key_pos + pattern.size());
  if (colon_pos == std::string::npos)
    return false;

  const size_t value_pos = text.find_first_not_of(" \n\r\t", colon_pos + 1);
  if (value_pos == std::string::npos)
    return false;

  if (text[value_pos] == '"')
  {
    std::string string_value;
    if (!ReadTicoJsonStringValue(text, key, string_value))
      return false;
    std::transform(string_value.begin(), string_value.end(), string_value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (string_value == "true")
      value = true;
    else if (string_value == "false")
      value = false;
    else
      return false;
    return true;
  }

  if (text.compare(value_pos, 4, "true") == 0)
  {
    value = true;
    return true;
  }
  if (text.compare(value_pos, 5, "false") == 0)
  {
    value = false;
    return true;
  }

  return false;
}

bool IsTicoSoundEnabled()
{
  std::ifstream input("sdmc:/tico/config/audio.jsonc");
  if (!input.is_open())
    return false;

  std::ostringstream ss;
  ss << input.rdbuf();
  bool enabled = false;
  return ReadTicoJsonBoolValue(ss.str(), "sound_enabled", enabled) && enabled;
}

void EnsureTicoFolders()
{
  FileSystem::EnsureDirectoryExists(Tico::Paths::Root, false);
  FileSystem::EnsureDirectoryExists(Tico::Paths::Cores, false);
  FileSystem::EnsureDirectoryExists(Tico::Paths::Assets, false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/assets/ra", false);
  FileSystem::EnsureDirectoryExists(Tico::Paths::SystemRoot, false);
  FileSystem::EnsureDirectoryExists(TicoConfig::SYSTEM_PATH, false);
  FileSystem::EnsureDirectoryExists(Tico::Paths::SavesRoot, false);
  FileSystem::EnsureDirectoryExists(TicoConfig::SAVES_PATH, false);
  FileSystem::EnsureDirectoryExists(Tico::Paths::StatesRoot, false);
  FileSystem::EnsureDirectoryExists(TicoConfig::STATES_PATH, false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/config", false);
  FileSystem::EnsureDirectoryExists(Tico::Paths::CoreConfigDir, false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/cache", false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/cache/duckstation", false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/screenshots", false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/screenshots/psx", false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/textures", false);
  FileSystem::EnsureDirectoryExists("sdmc:/tico/textures/psx", false);
}

std::string GetLegacyTicoStatePath(int slot)
{
  std::string media_path = System::GetMediaFileName();
  if (media_path.empty())
    media_path = System::GetGameSerial();

  const std::string state_name = fmt::format("{}.state{}", Path::GetFileTitle(media_path), slot);
  return Path::Combine(TicoConfig::STATES_PATH, state_name);
}

uint64_t MapSwitchButtons(uint64_t hid)
{
  uint64_t buttons = 0;
  if (hid & HidNpadButton_B)
    buttons |= Tico::Pad_A;
  if (hid & HidNpadButton_A)
    buttons |= Tico::Pad_B;
  if (hid & HidNpadButton_Y)
    buttons |= Tico::Pad_X;
  if (hid & HidNpadButton_X)
    buttons |= Tico::Pad_Y;
  if (hid & HidNpadButton_Up)
    buttons |= Tico::Pad_Up;
  if (hid & HidNpadButton_Down)
    buttons |= Tico::Pad_Down;
  if (hid & HidNpadButton_Left)
    buttons |= Tico::Pad_Left;
  if (hid & HidNpadButton_Right)
    buttons |= Tico::Pad_Right;
  if (hid & HidNpadButton_L)
    buttons |= Tico::Pad_L;
  if (hid & HidNpadButton_R)
    buttons |= Tico::Pad_R;
  if (hid & HidNpadButton_ZL)
    buttons |= Tico::Pad_L2;
  if (hid & HidNpadButton_ZR)
    buttons |= Tico::Pad_R2;
  if (hid & HidNpadButton_StickL)
    buttons |= Tico::Pad_L3;
  if (hid & HidNpadButton_StickR)
    buttons |= Tico::Pad_R3;
  if (hid & HidNpadButton_Plus)
    buttons |= Tico::Pad_Start;
  if (hid & HidNpadButton_Minus)
    buttons |= Tico::Pad_Select;
  if ((buttons & Tico::Pad_Start) && (buttons & Tico::Pad_Select))
    buttons |= Tico::Pad_Guide;
  return buttons;
}

std::string BaseName(std::string path)
{
  if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
    path = path.substr(1, path.size() - 2);

  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos)
    path.erase(0, slash + 1);
  return path;
}

std::string Trim(std::string text)
{
  const size_t start = text.find_first_not_of(" \n\r\t");
  if (start == std::string::npos)
    return {};
  const size_t end = text.find_last_not_of(" \n\r\t");
  return text.substr(start, end - start + 1);
}

std::string StripQuotes(std::string path)
{
  path = Trim(std::move(path));
  if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
    path = path.substr(1, path.size() - 2);
  return path;
}

std::string NormalizePath(std::string path)
{
  path = StripQuotes(std::move(path));
  std::replace(path.begin(), path.end(), '\\', '/');
  return Path::Canonicalize(path);
}

std::string StripExtension(std::string name)
{
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos)
    name.erase(dot);
  return name;
}

std::string ToLower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

std::string ExtractDiscLabelFromText(const std::string& text)
{
  const std::string lower = ToLower(text);
  static constexpr const char* keywords[] = {"disc", "disk", "cd"};

  for (const char* keyword : keywords)
  {
    size_t pos = 0;
    while ((pos = lower.find(keyword, pos)) != std::string::npos)
    {
      size_t number_pos = pos + std::strlen(keyword);
      while (number_pos < lower.size() && (lower[number_pos] == ' ' || lower[number_pos] == '_' ||
                                           lower[number_pos] == '-' || lower[number_pos] == '#'))
      {
        number_pos++;
      }

      size_t number_end = number_pos;
      while (number_end < lower.size() && std::isdigit(static_cast<unsigned char>(lower[number_end])))
        number_end++;

      if (number_end > number_pos)
        return "Disc " + lower.substr(number_pos, number_end - number_pos);

      pos += std::strlen(keyword);
    }
  }

  return {};
}

bool HasDiscSuffix(const std::string& title)
{
  return ToLower(title).find(" - disc ") != std::string::npos;
}

int GetExtensionPriority(const std::string& lower_extension)
{
  if (lower_extension == ".m3u")
    return 1;
  if (lower_extension == ".chd")
    return 2;
  if (lower_extension == ".cue")
    return 3;
  if (lower_extension == ".pbp")
    return 4;
  if (lower_extension == ".iso")
    return 5;
  if (lower_extension == ".bin")
    return 6;
  return 100;
}

struct DiscScanEntry
{
  std::string label;
  std::string path;
  int number = 0;
  int priority = 100;
};

bool ExtractDiscPattern(const std::string& lower_text, int* disc_number, size_t* pattern_pos = nullptr)
{
  static constexpr const char* keywords[] = {"disc", "disk", "cd"};
  for (const char* keyword : keywords)
  {
    for (int number = 1; number <= 10; number++)
    {
      const std::string pattern = std::string("(") + keyword + " " + std::to_string(number) + ")";
      const size_t pos = lower_text.find(pattern);
      if (pos != std::string::npos)
      {
        if (disc_number)
          *disc_number = number;
        if (pattern_pos)
          *pattern_pos = pos;
        return true;
      }
    }
  }

  return false;
}

bool DirectoryNameLooksLikeDiscFolder(const std::string& directory)
{
  const std::string lower = ToLower(directory);
  return lower.find("disc") != std::string::npos || lower.find("disk") != std::string::npos ||
         lower.find("cd") != std::string::npos;
}

std::string DirectoryName(const std::string& path)
{
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::vector<DiscScanEntry> ParseM3UDiscs(const std::string& playlist_path, const std::string& directory)
{
  std::vector<DiscScanEntry> discs;
  auto fp = FileSystem::OpenManagedCFile(playlist_path.c_str(), "r");
  if (!fp)
    return discs;

  char line[1024];
  int disc_index = 1;
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    std::string entry = Trim(line);
    if (entry.empty() || entry.front() == '#')
      continue;

    DiscScanEntry disc;
    disc.label = "Disc " + std::to_string(disc_index);
    disc.path = NormalizePath(Path::Combine(directory, entry));
    disc.number = disc_index;
    disc.priority = GetExtensionPriority(ToLower(std::string(Path::GetExtension(disc.path))));
    discs.push_back(std::move(disc));
    disc_index++;
  }

  return discs;
}

void ConsiderDiscFile(const std::string& file_directory, const std::string& filename, const std::string& prefix,
                      std::map<int, DiscScanEntry>* best_discs)
{
  std::string lower_filename = ToLower(filename);
  const size_t dot = lower_filename.find_last_of('.');
  const std::string lower_extension = dot == std::string::npos ? std::string() : lower_filename.substr(dot);
  const int priority = GetExtensionPriority(lower_extension);
  if (priority > 6)
    return;

  const std::string lower_name_no_ext = dot == std::string::npos ? lower_filename : lower_filename.substr(0, dot);
  if (lower_name_no_ext.rfind(prefix, 0) != 0)
    return;

  std::string before_paren;
  const std::string after_prefix = lower_name_no_ext.substr(prefix.size());
  const size_t paren_pos = after_prefix.find('(');
  before_paren = paren_pos == std::string::npos ? after_prefix : after_prefix.substr(0, paren_pos);
  if (!Trim(before_paren).empty())
    return;

  int disc_number = 0;
  if (!ExtractDiscPattern(lower_filename, &disc_number))
    return;

  DiscScanEntry entry;
  entry.label = "Disc " + std::to_string(disc_number);
  entry.path = NormalizePath(Path::Combine(file_directory, filename));
  entry.number = disc_number;
  entry.priority = priority;

  auto it = best_discs->find(disc_number);
  if (it == best_discs->end() || priority < it->second.priority)
    (*best_discs)[disc_number] = std::move(entry);
}

void ScanDirectoryForDiscFiles(const std::string& directory, const std::string& prefix,
                               std::map<int, DiscScanEntry>* best_discs)
{
  DIR* dir = opendir(directory.c_str());
  if (!dir)
    return;

  while (dirent* ent = readdir(dir))
  {
    const std::string filename = ent->d_name;
    if (filename == "." || filename == "..")
      continue;

    const std::string path = Path::Combine(directory, filename);
    if (FileSystem::DirectoryExists(path.c_str()))
    {
      DIR* subdir = opendir(path.c_str());
      if (!subdir)
        continue;

      while (dirent* sub_ent = readdir(subdir))
      {
        const std::string sub_filename = sub_ent->d_name;
        if (sub_filename != "." && sub_filename != "..")
          ConsiderDiscFile(path, sub_filename, prefix, best_discs);
      }
      closedir(subdir);
    }
    else
    {
      ConsiderDiscFile(directory, filename, prefix, best_discs);
    }
  }

  closedir(dir);
}

std::vector<DiscScanEntry> ScanFilesystemDiscs(const std::string& media_path, int* current_disc)
{
  std::vector<DiscScanEntry> discs;
  if (current_disc)
    *current_disc = 0;

  const std::string current_path = NormalizePath(media_path);
  if (current_path.empty())
    return discs;

  const std::string directory = DirectoryName(current_path);
  const std::string basename = BaseName(current_path);
  const std::string lower_basename = ToLower(basename);

  if (lower_basename.size() >= 4 && lower_basename.ends_with(".m3u"))
  {
    discs = ParseM3UDiscs(current_path, directory);
    if (current_disc && System::HasMediaSubImages())
      *current_disc = std::clamp(static_cast<int>(System::GetMediaSubImageIndex()), 0,
                                 std::max(0, static_cast<int>(discs.size()) - 1));
    return discs;
  }

  int current_disc_number = 0;
  if (!ExtractDiscPattern(lower_basename, &current_disc_number))
    return discs;

  const size_t first_paren = lower_basename.find('(');
  std::string prefix = first_paren == std::string::npos ? StripExtension(lower_basename) : lower_basename.substr(0, first_paren);
  prefix = Trim(std::move(prefix));
  if (prefix.empty())
    return discs;

  std::vector<std::string> scan_dirs;
  scan_dirs.push_back(directory);
  if (DirectoryNameLooksLikeDiscFolder(directory))
    scan_dirs.push_back(Path::Combine(directory, ".."));

  std::map<int, DiscScanEntry> best_discs;
  for (const std::string& scan_dir : scan_dirs)
    ScanDirectoryForDiscFiles(NormalizePath(scan_dir), prefix, &best_discs);

  discs.reserve(best_discs.size());
  for (auto& pair : best_discs)
    discs.push_back(std::move(pair.second));

  std::sort(discs.begin(), discs.end(),
            [](const DiscScanEntry& lhs, const DiscScanEntry& rhs) { return lhs.number < rhs.number; });

  const std::string normalized_current = NormalizePath(current_path);
  for (int i = 0; i < static_cast<int>(discs.size()); i++)
  {
    if (NormalizePath(discs[static_cast<size_t>(i)].path) == normalized_current ||
        discs[static_cast<size_t>(i)].number == current_disc_number)
    {
      if (current_disc)
        *current_disc = i;
      break;
    }
  }

  return discs;
}

std::string FormattedGameTitle()
{
  const std::string media_path = System::GetMediaFileName();
  const std::string media_name = BaseName(media_path);
  const std::string media_stem = StripExtension(media_name);
  const std::string disc_label = ExtractDiscLabelFromText(media_stem);

  std::string title = TicoUtils::GetCleanTitle(media_name);
  if (title.empty() && !System::GetGameTitle().empty())
    title = TicoUtils::GetCleanTitle(System::GetGameTitle());
  if (title.empty() && !media_path.empty())
    title = media_stem;
  if (title.empty())
    title = "DuckStation";

  if (!disc_label.empty() && !HasDiscSuffix(title))
    title += " - " + disc_label;

  return title;
}

void SyncEmulationPause(bool overlay_visible)
{
  if (!System::IsValid())
  {
    s_overlay_pause_active = false;
    s_resume_after_overlay = false;
    return;
  }

  if (overlay_visible)
  {
    if (!s_overlay_pause_active)
    {
      s_resume_after_overlay = !System::IsPaused();
      if (s_resume_after_overlay)
      {
        Host::RunOnCPUThread([]() {
          if (System::IsValid() && !System::IsPaused())
            System::PauseSystem(true);
        });
      }
      s_overlay_pause_active = true;
    }
  }
  else if (s_overlay_pause_active)
  {
    const bool should_resume = s_resume_after_overlay && !s_chainload_launcher;
    s_overlay_pause_active = false;
    s_resume_after_overlay = false;
    if (should_resume)
    {
      Host::RunOnCPUThread([]() {
        if (System::IsValid() && System::IsPaused())
          System::PauseSystem(false);
      });
    }
  }
}

void RefreshOverlayModel()
{
  Tico::TicoOverlay& overlay = Overlay();

  std::array<bool, Tico::StateSlotCount> used = {};
  for (int i = 0; i < static_cast<int>(Tico::StateSlotCount); i++)
    used[i] = FileSystem::FileExists(GetLegacyTicoStatePath(i).c_str());
  overlay.SetStateSlotUsage(used);

  std::vector<std::string> discs;
  s_disc_paths.clear();
  int current_disc = 0;
  std::vector<DiscScanEntry> scanned_discs = ScanFilesystemDiscs(System::GetMediaFileName(), &current_disc);
  if (scanned_discs.size() >= 2)
  {
    discs.reserve(std::min(scanned_discs.size(), Tico::MaxOverlayItems));
    s_disc_paths.reserve(std::min(scanned_discs.size(), Tico::MaxOverlayItems));
    for (DiscScanEntry& disc : scanned_discs)
    {
      if (discs.size() >= Tico::MaxOverlayItems)
        break;
      discs.push_back(std::move(disc.label));
      s_disc_paths.push_back(std::move(disc.path));
    }
    current_disc = std::clamp(current_disc, 0, std::max(0, static_cast<int>(discs.size()) - 1));
  }
  else if (System::HasMediaSubImages())
  {
    const u32 count = System::GetMediaSubImageCount();
    current_disc = static_cast<int>(System::GetMediaSubImageIndex());
    discs.reserve(std::min<u32>(count, static_cast<u32>(Tico::MaxOverlayItems)));
    s_disc_paths.reserve(std::min<u32>(count, static_cast<u32>(Tico::MaxOverlayItems)));
    for (u32 i = 0; i < count && discs.size() < Tico::MaxOverlayItems; i++)
    {
      std::string title = System::GetMediaSubImageTitle(i);
      if (title.empty())
        title = "Disc " + std::to_string(i + 1);
      discs.push_back(std::move(title));
      s_disc_paths.emplace_back();
    }
  }

  std::string title = FormattedGameTitle();
  if (!HasDiscSuffix(title) && !discs.empty() && current_disc >= 0 && current_disc < static_cast<int>(discs.size()))
  {
    const std::string& disc_title = discs[static_cast<size_t>(current_disc)];
    const std::string disc_label = ExtractDiscLabelFromText(disc_title);
    title += " - " + (disc_label.empty() ? disc_title : disc_label);
  }
  overlay.SetGameTitle(std::move(title));
  overlay.SetDiscEntries(std::move(discs), current_disc);
}

void ApplyDisplaySettings(const Tico::OverlaySnapshot& snapshot)
{
  if (!snapshot.visible)
    return;

  const DisplayScalingMode scaling = snapshot.display_mode == Tico::DisplayMode::Integer ?
                                       DisplayScalingMode::NearestInteger :
                                       DisplayScalingMode::BilinearSmooth;

  DisplayAspectRatio aspect_ratio = DisplayAspectRatio::R4_3;
  if (snapshot.display_mode == Tico::DisplayMode::Display)
  {
    switch (snapshot.display_size)
    {
      case Tico::DisplaySize::Stretch:
        aspect_ratio = DisplayAspectRatio::MatchWindow;
        break;
      case Tico::DisplaySize::_16_9:
        aspect_ratio = DisplayAspectRatio::R16_9;
        break;
      case Tico::DisplaySize::Original:
        aspect_ratio = DisplayAspectRatio::PAR1_1;
        break;
      case Tico::DisplaySize::_4_3:
      default:
        aspect_ratio = DisplayAspectRatio::R4_3;
        break;
    }
  }

  if (g_settings.display_scaling == scaling && g_settings.display_aspect_ratio == aspect_ratio)
    return;

  Host::SetBaseStringSettingValue("Display", "Scaling", Settings::GetDisplayScalingName(scaling));
  Host::SetBaseStringSettingValue("Display", "AspectRatio", Settings::GetDisplayAspectRatioName(aspect_ratio));
  Host::CommitBaseSettingChanges();
  System::ApplySettings(false);
}

void ProcessActions()
{
  Tico::TicoOverlay& overlay = Overlay();
  const int requested_disc = overlay.TakeRequestedDiscIndex();
  if (requested_disc >= 0)
  {
    if (requested_disc < static_cast<int>(s_disc_paths.size()) && !s_disc_paths[static_cast<size_t>(requested_disc)].empty())
      System::InsertMedia(s_disc_paths[static_cast<size_t>(requested_disc)].c_str());
    else
      System::SwitchMediaSubImage(static_cast<u32>(requested_disc));
  }

  int state_slot = -1;
  const Tico::OverlayStateAction state_action = overlay.TakeRequestedStateAction(state_slot);
  if (state_action != Tico::OverlayStateAction::None && state_slot >= 0)
  {
    const std::string path = GetLegacyTicoStatePath(state_slot);
    if (state_action == Tico::OverlayStateAction::Save)
      System::SaveState(path.c_str(), nullptr, g_settings.create_save_state_backups);
    else
      System::LoadState(path.c_str(), nullptr);
  }

  if (overlay.ShouldReset())
  {
    overlay.ClearReset();
    System::ResetSystem();
  }

  if (overlay.ShouldExit())
  {
    overlay.ClearExit();
    s_chainload_launcher = true;
    s_overlay_pause_active = false;
    s_resume_after_overlay = false;
    if (s_exit_application_callback)
      s_exit_application_callback();
    else
      Host::RequestSystemShutdown(false, g_settings.save_state_on_exit);
  }
}

ImU32 Color(float r, float g, float b, float a)
{
  return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

std::unique_ptr<GPUTexture> UploadRGBA8Texture(const char* tag, u32 width, u32 height, const void* pixels, u32 pitch)
{
  if (!g_gpu_device || width == 0 || height == 0 || !pixels)
    return {};

  std::unique_ptr<GPUTexture> texture = g_gpu_device->FetchTexture(
    width, height, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8, pixels, pitch);
  if (!texture)
    Log_ErrorFmt("Failed to upload Tico texture '{}'", tag ? tag : "texture");
  return texture;
}

GPUTexture* GetAvatarTexture()
{
  if (s_avatar_load_attempted)
    return s_avatar_texture.get();

  s_avatar_load_attempted = true;
#ifdef __SWITCH__
  Result rc = accountInitialize(AccountServiceType_Application);
  if (R_FAILED(rc))
  {
    Log_WarningFmt("accountInitialize failed: 0x{:08X}", rc);
    return nullptr;
  }

  AccountUid uid = {};
  bool found = false;
  if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid))
    found = true;
  if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid))
    found = true;
  if (!found)
  {
    s32 user_count = 0;
    if (R_SUCCEEDED(accountGetUserCount(&user_count)) && user_count > 0)
    {
      AccountUid uids[ACC_USER_LIST_SIZE] = {};
      s32 actual_total = 0;
      if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actual_total)) && actual_total > 0)
      {
        uid = uids[0];
        found = accountUidIsValid(&uid);
      }
    }
  }

  if (found)
  {
    AccountProfile profile;
    if (R_SUCCEEDED(accountGetProfile(&profile, uid)))
    {
      u32 image_size = 0;
      if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &image_size)) && image_size > 0)
      {
        std::vector<u8> jpeg_data(image_size);
        u32 actual_size = 0;
        if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg_data.data(), image_size, &actual_size)) &&
            actual_size > 0)
        {
          RGBA8Image image;
          if (image.LoadFromBuffer("switch-account-avatar.jpg", jpeg_data.data(), actual_size))
          {
            s_avatar_texture = UploadRGBA8Texture("switch-account-avatar", image.GetWidth(), image.GetHeight(),
                                                  image.GetPixels(), image.GetPitch());
            if (s_avatar_texture)
              Log_InfoFmt("Loaded Switch account avatar ({}x{})", image.GetWidth(), image.GetHeight());
          }
        }
      }
      accountProfileClose(&profile);
    }
  }

  accountExit();
#endif
  return s_avatar_texture.get();
}

GPUTexture* GetBoltTexture()
{
  if (s_bolt_load_attempted)
    return s_bolt_texture.get();

  s_bolt_load_attempted = true;

  constexpr int width = 18;
  constexpr int height = 22;
  std::vector<u8> rgba(static_cast<size_t>(width) * height * 4);

  NSVGimage* image = nsvgParseFromFile("romfs:/assets/bolt.svg", "px", 96.0f);
  if (!image || image->width <= 0.0f || image->height <= 0.0f)
  {
    Log_WarningPrint("Failed to parse romfs:/assets/bolt.svg");
    if (image)
      nsvgDelete(image);
    return nullptr;
  }

  NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
  if (!rasterizer)
  {
    nsvgDelete(image);
    return nullptr;
  }

  const float scale = std::min(static_cast<float>(width) / image->width, static_cast<float>(height) / image->height);
  const float tx = (static_cast<float>(width) - image->width * scale) * 0.5f;
  const float ty = (static_cast<float>(height) - image->height * scale) * 0.5f;
  nsvgRasterize(rasterizer, image, tx, ty, scale, rgba.data(), width, height, width * 4);
  nsvgDeleteRasterizer(rasterizer);
  nsvgDelete(image);

  s_bolt_texture = UploadRGBA8Texture("bolt.svg", width, height, rgba.data(), width * 4);
  return s_bolt_texture.get();
}

std::unique_ptr<GPUTexture> LoadSVGTexture(const char* path, int width, int height)
{
  std::vector<u8> rgba(static_cast<size_t>(width) * height * 4);

  NSVGimage* image = nsvgParseFromFile(path, "px", 96.0f);
  if (!image || image->width <= 0.0f || image->height <= 0.0f)
  {
    Log_WarningFmt("Failed to parse '{}'", path);
    if (image)
      nsvgDelete(image);
    return {};
  }

  NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
  if (!rasterizer)
  {
    nsvgDelete(image);
    return {};
  }

  const float scale = std::min(static_cast<float>(width) / image->width, static_cast<float>(height) / image->height);
  const float tx = (static_cast<float>(width) - image->width * scale) * 0.5f;
  const float ty = (static_cast<float>(height) - image->height * scale) * 0.5f;
  nsvgRasterize(rasterizer, image, tx, ty, scale, rgba.data(), width, height, width * 4);
  nsvgDeleteRasterizer(rasterizer);
  nsvgDelete(image);

  return UploadRGBA8Texture(path, width, height, rgba.data(), width * 4);
}

GPUTexture* GetRAIconTexture()
{
  if (!s_ra_icon_load_attempted)
  {
    s_ra_icon_load_attempted = true;
    s_ra_icon_texture = LoadSVGTexture("romfs:/assets/ra.svg", 96, 96);
  }

  return s_ra_icon_texture.get();
}

GPUTexture* GetRABadgeTexture(const std::string& path)
{
  if (path.empty() || path == "ra_icon")
    return GetRAIconTexture();

  auto it = s_ra_badge_textures.find(path);
  if (it != s_ra_badge_textures.end())
    return it->second.get();

  if (!FileSystem::FileExists(path.c_str()))
    return nullptr;

  RGBA8Image image;
  if (!image.LoadFromFile(path.c_str()))
    return nullptr;

  std::unique_ptr<GPUTexture> texture =
    UploadRGBA8Texture(path.c_str(), image.GetWidth(), image.GetHeight(), image.GetPixels(), image.GetPitch());
  GPUTexture* texture_ptr = texture.get();
  if (texture_ptr)
    s_ra_badge_textures.emplace(path, std::move(texture));
  return texture_ptr;
}

float OverlayScale()
{
#ifdef __SWITCH__
  return appletGetOperationMode() == AppletOperationMode_Handheld ? 1.0f : 1.5f;
#else
  return 1.0f;
#endif
}

float TextWidth(const char* text, float size)
{
  ImFont* font = ImGui::GetFont();
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text ? text : "").x;
}

float TextHeight(float size)
{
  return ImGui::GetFont()->FontSize * (size / ImGui::GetFontSize());
}

void DrawText(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text, float size)
{
  ImFont* font = ImGui::GetFont();
  dl->AddText(font, size, pos, color, text);
}

void DrawCenteredText(ImDrawList* dl, float x, float y, float w, const char* text, float size,
                      ImU32 color)
{
  const float tw = TextWidth(text, size);
  DrawText(dl, ImVec2(x + (w - tw) * 0.5f, y), color, text, size);
}

void DrawButtonPrompt(ImDrawList* dl, float x, float y, float size, const char* symbol, float font_size)
{
  const ImU32 bg = Color(215.0f / 255.0f, 215.0f / 255.0f, 215.0f / 255.0f, 1.0f);
  const ImU32 fg = Color(40.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f);
  dl->AddCircleFilled(ImVec2(x + size * 0.5f, y + size * 0.5f), size * 0.5f, bg, 24);

  const float tw = TextWidth(symbol, font_size);
  const float th = TextHeight(font_size);
  DrawText(dl, ImVec2(x + (size - tw) * 0.5f, y + (size - th) * 0.5f), fg, symbol, font_size);
}

void DrawOverlayBackground(ImDrawList* dl, const ImVec2& display, float ease)
{
  const ImU32 top = Color(0.0f, 0.0f, 0.0f, (230.0f / 255.0f) * ease);
  const ImU32 mid = Color(0.0f, 0.0f, 0.0f, (170.0f / 255.0f) * ease);
  const float top_h = display.y * 0.20f;
  const float bottom_y = display.y - top_h;
  dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(display.x, top_h), top, top, mid, mid);
  dl->AddRectFilled(ImVec2(0.0f, top_h), ImVec2(display.x, bottom_y), mid);
  dl->AddRectFilledMultiColor(ImVec2(0.0f, bottom_y), display, mid, mid, top, top);
}

void DrawSocialArea(ImDrawList* dl, float scale, float ease)
{
  const float avatar_size = 72.0f * scale;
  const float side_margin = 32.0f * scale;
  const float top_margin = 32.0f * scale;
  const float bar_h = 50.0f * scale;
  const float start_offset = 200.0f * scale * (1.0f - ease);
  const float x = side_margin - start_offset;
  const float y = top_margin + (bar_h - avatar_size) * 0.5f;
  const float inset = 4.0f * scale;

  dl->AddCircleFilled(ImVec2(x + avatar_size * 0.5f, y + avatar_size * 0.5f), avatar_size * 0.5f,
                      Color(45.0f / 255.0f, 45.0f / 255.0f, 45.0f / 255.0f, ease), 48);
  const ImVec2 image_min(x + inset, y + inset);
  const ImVec2 image_max(x + avatar_size - inset, y + avatar_size - inset);
  if (GPUTexture* avatar = GetAvatarTexture())
  {
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(avatar), image_min, image_max, ImVec2(0.0f, 0.0f),
                        ImVec2(1.0f, 1.0f), Color(1.0f, 1.0f, 1.0f, ease),
                        (avatar_size - inset * 2.0f) * 0.5f);
  }
  else
  {
    dl->AddCircleFilled(ImVec2(x + avatar_size * 0.5f, y + avatar_size * 0.5f),
                        (avatar_size - inset * 2.0f) * 0.5f,
                        Color(200.0f / 255.0f, 200.0f / 255.0f, 210.0f / 255.0f, ease), 48);
  }
}

void DrawBatteryIcon(ImDrawList* dl, float x, float y, float w, float h, u32 percent, ImU32 color)
{
  dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), color, h * 0.2f, 0, 1.5f);
  dl->AddRectFilled(ImVec2(x + w + 3.0f, y + h * 0.3f), ImVec2(x + w + 6.0f, y + h * 0.7f),
                    color, 1.5f);
  const float fill_w = std::max(0.0f, (w - 5.0f) * (static_cast<float>(percent) / 100.0f));
  dl->AddRectFilled(ImVec2(x + 2.5f, y + 2.5f), ImVec2(x + 2.5f + fill_w, y + h - 2.5f),
                    color, h * 0.12f);
}

void DrawStatusBar(ImDrawList* dl, const ImVec2& display, float scale, float ease)
{
  char time_buf[16] = "--:--";
  const std::time_t now = std::time(nullptr);
  if (const std::tm* local = std::localtime(&now))
    std::strftime(time_buf, sizeof(time_buf), "%H:%M", local);

  u32 battery_percent = 100;
  bool battery_charging = false;
#ifdef __SWITCH__
  psmGetBatteryChargePercentage(&battery_percent);
  PsmChargerType charger_type = PsmChargerType_Unconnected;
  if (R_SUCCEEDED(psmGetChargerType(&charger_type)))
  {
    battery_charging = charger_type == PsmChargerType_EnoughPower ||
                       charger_type == PsmChargerType_LowPower;
  }
#endif
  battery_percent = std::clamp(battery_percent, 0u, 100u);

  const float font = 26.0f * scale;
  const float bar_h = 50.0f * scale;
  const float side_margin = 32.0f * scale;
  const float top_margin = 32.0f * scale;
  const float item_spacing = 20.0f * scale;
  const float padding = 20.0f * scale;
  const float battery_w = 32.0f * scale;
  const float battery_h = 20.0f * scale;
  const float bolt_w = 18.0f * scale;
  const float bolt_h = 22.0f * scale;
  const float time_w = TextWidth(time_buf, font);
  const float total_w = padding * 2.0f + time_w + item_spacing + battery_w + 6.0f * scale +
                        (battery_charging ? item_spacing * 0.5f + bolt_w : 0.0f);
  const float start_x = display.x - side_margin - total_w + 90.0f * scale * (1.0f - ease) + padding;
  const float center_y = top_margin + bar_h * 0.5f - 20.0f * scale * (1.0f - ease);
  const ImU32 color = Color(200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, ease);

  DrawText(dl, ImVec2(start_x, center_y - TextHeight(font) * 0.5f), color, time_buf, font);
  const float battery_x = start_x + time_w + item_spacing;
  DrawBatteryIcon(dl, battery_x, center_y - battery_h * 0.5f, battery_w, battery_h, battery_percent, color);
  if (battery_charging)
  {
    const float bolt_x = battery_x + battery_w + item_spacing * 0.5f + 6.0f * scale;
    const ImVec2 bolt_min(bolt_x, center_y - bolt_h * 0.5f);
    const ImVec2 bolt_max(bolt_x + bolt_w, center_y + bolt_h * 0.5f);
    if (GPUTexture* bolt = GetBoltTexture())
    {
      dl->AddImage(reinterpret_cast<ImTextureID>(bolt), bolt_min, bolt_max, ImVec2(0.0f, 0.0f),
                   ImVec2(1.0f, 1.0f), color);
    }
    else
    {
      dl->AddTriangleFilled(ImVec2(bolt_x + bolt_w * 0.62f, bolt_min.y),
                            ImVec2(bolt_x + bolt_w * 0.35f, center_y),
                            ImVec2(bolt_x + bolt_w * 0.7f, center_y), color);
      dl->AddTriangleFilled(ImVec2(bolt_x + bolt_w * 0.3f, center_y),
                            ImVec2(bolt_x + bolt_w * 0.65f, center_y),
                            ImVec2(bolt_x + bolt_w * 0.25f, bolt_max.y), color);
    }
  }
}

void UpdateRANotifications(float delta_time)
{
  std::lock_guard lock(s_ra_notifications_mutex);
  for (Tico::RANotificationSnapshot& notification : s_ra_notifications)
    notification.timer += delta_time;

  s_ra_notifications.erase(
    std::remove_if(s_ra_notifications.begin(), s_ra_notifications.end(),
                   [](const Tico::RANotificationSnapshot& notification) {
                     return notification.timer >= notification.duration;
                   }),
    s_ra_notifications.end());

  std::vector<Tico::RANotificationSnapshot> snapshots;
  snapshots.reserve(std::min(s_ra_notifications.size(), Tico::MaxRANotifications));
  const size_t first =
    s_ra_notifications.size() > Tico::MaxRANotifications ? s_ra_notifications.size() - Tico::MaxRANotifications : 0;
  for (size_t i = first; i < s_ra_notifications.size(); i++)
  {
    Tico::RANotificationSnapshot snapshot = s_ra_notifications[i];
    snapshot.badge_texture = GetRABadgeTexture(snapshot.badge_name);
    snapshots.push_back(std::move(snapshot));
  }

  Overlay().SetRANotifications(std::move(snapshots));
}

void DrawRANotifications(ImDrawList* dl, const Tico::OverlaySnapshot& snapshot, const ImVec2& display, float scale)
{
  const int count = std::clamp(snapshot.ra_notification_count, 0, static_cast<int>(Tico::MaxRANotifications));
  if (count == 0)
    return;

  const float alert_w = std::min(520.0f * scale, display.x - 48.0f * scale);
  const float alert_h = 100.0f * scale;
  const float gap = 12.0f * scale;
  const float margin = 24.0f * scale;
  const float padding = 12.0f * scale;
  const float badge_size = 76.0f * scale;
  const float badge_radius = 4.0f * scale;
  const float title_font = 21.0f * scale;
  const float desc_font = 17.0f * scale;
  const ImU32 bg_base = Color(23.0f / 255.0f, 23.0f / 255.0f, 26.0f / 255.0f, 0.94f);
  const ImU32 border_base = Color(82.0f / 255.0f, 82.0f / 255.0f, 90.0f / 255.0f, 0.82f);

  for (int i = 0; i < count; i++)
  {
    const Tico::RANotificationSnapshot& notification = snapshot.ra_notifications[i];
    if (notification.title.empty() && notification.description.empty())
      continue;

    float slide_progress = 1.0f;
    if (notification.timer < notification.slide_in)
    {
      const float t = notification.slide_in > 0.0f ? notification.timer / notification.slide_in : 1.0f;
      slide_progress = 1.0f - std::pow(1.0f - std::clamp(t, 0.0f, 1.0f), 3.0f);
    }
    else if (notification.timer > notification.duration - notification.slide_out)
    {
      const float t = notification.slide_out > 0.0f ?
                        (notification.duration - notification.timer) / notification.slide_out :
                        0.0f;
      slide_progress = 1.0f - std::pow(1.0f - std::clamp(t, 0.0f, 1.0f), 3.0f);
    }

    const float alpha = std::clamp(slide_progress, 0.0f, 1.0f);
    if (alpha <= 0.0f)
      continue;

    const float stack_offset = static_cast<float>(i) * (alert_h + gap);
    const float x = display.x - margin - alert_w;
    const float anchor_y = margin + stack_offset;
    const float y = anchor_y - (alert_h + margin + stack_offset) * (1.0f - alpha);
    const ImVec2 rect_min(x, y);
    const ImVec2 rect_max(x + alert_w, y + alert_h);

    dl->AddRectFilled(rect_min, rect_max, (bg_base & 0x00ffffffu) | (static_cast<ImU32>(alpha * 240.0f) << 24),
                      8.0f * scale);
    dl->AddRect(rect_min, rect_max,
                (border_base & 0x00ffffffu) | (static_cast<ImU32>(alpha * 210.0f) << 24), 8.0f * scale, 0,
                1.0f * scale);

    const ImVec2 badge_min(x + padding, y + (alert_h - badge_size) * 0.5f);
    const ImVec2 badge_max(badge_min.x + badge_size, badge_min.y + badge_size);
    dl->AddRectFilled(badge_min, badge_max, Color(8.0f / 255.0f, 8.0f / 255.0f, 10.0f / 255.0f, alpha),
                      badge_radius);

    if (notification.badge_texture)
    {
      ImVec2 image_min = badge_min;
      ImVec2 image_max = badge_max;
      if (notification.badge_name == "ra_icon")
      {
        const float inset = badge_size * 0.15f;
        image_min = ImVec2(badge_min.x + inset, badge_min.y + inset);
        image_max = ImVec2(badge_max.x - inset, badge_max.y - inset);
      }
      dl->AddImageRounded(reinterpret_cast<ImTextureID>(notification.badge_texture), image_min, image_max,
                          ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), Color(1.0f, 1.0f, 1.0f, alpha),
                          badge_radius);
    }

    const float text_x = badge_max.x + padding;
    const float text_w = rect_max.x - padding - text_x;
    const char* title = notification.title.c_str();
    const char* description = notification.description.c_str();
    std::string clipped_title = notification.title;
    std::string clipped_desc = notification.description;
    while (!clipped_title.empty() && TextWidth(clipped_title.c_str(), title_font) > text_w)
      clipped_title.pop_back();
    while (!clipped_desc.empty() && TextWidth(clipped_desc.c_str(), desc_font) > text_w)
      clipped_desc.pop_back();
    title = clipped_title.c_str();
    description = clipped_desc.c_str();

    DrawText(dl, ImVec2(text_x, y + 20.0f * scale), Color(1.0f, 1.0f, 1.0f, alpha), title, title_font);
    DrawText(dl, ImVec2(text_x, y + 54.0f * scale), Color(0.78f, 0.78f, 0.82f, alpha), description, desc_font);
  }
}

float OverlayUIScale(const ImVec2& display)
{
  return OverlayScale() * (display.x >= 1600.0f || display.y >= 1000.0f ? 1.0f : display.y / 720.0f);
}

void DrawOverlaySnapshot(const Tico::OverlaySnapshot& snapshot)
{
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 display = io.DisplaySize;
  const float scale = OverlayUIScale(display);
  DrawRANotifications(dl, snapshot, display, scale);

  if (!snapshot.visible)
    return;

  const float ease = std::clamp(snapshot.progress, 0.0f, 1.0f);
  DrawOverlayBackground(dl, display, ease);

  std::string title = snapshot.title.empty() ? "DuckStation" : snapshot.title;
  const size_t last = title.find_last_not_of(" \n\r\t");
  if (last != std::string::npos)
    title.erase(last + 1);
  if (title.length() > 50)
  {
    title.resize(47);
    title += "...";
  }

  const float title_font = 26.0f * scale;
  const float title_h = 72.0f * scale;
  const float top_space = 110.0f * scale;
  const float card_w = std::min(display.x * 0.8f, display.x * 0.4f * scale);
  const float card_x = (display.x - card_w) * 0.5f;
  const float card_y = (top_space - title_h) * 0.5f;
  const float start_title_y = -150.0f * scale;
  const float title_y = start_title_y + (card_y - start_title_y) * ease;
  DrawCenteredText(dl, card_x, title_y + (title_h - TextHeight(title_font)) * 0.5f, card_w,
                   title.c_str(), title_font,
                   Color(200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, ease));

  const float menu_w = 400.0f * scale;
  const float item_h = 64.0f * scale;
  const int items = std::clamp(snapshot.item_count, 1, static_cast<int>(Tico::MaxOverlayItems));
  const float menu_h = item_h * static_cast<float>(items);
  const float menu_x = (display.x - menu_w) * 0.5f;
  const float target_y = (display.y - menu_h) * 0.5f;
  const float start_y = display.y + 100.0f * scale;
  const float menu_y = start_y + (target_y - start_y) * ease;
  const float radius = 16.0f * scale;
  const float container = 45.0f / 255.0f;
  const float selected_bg = 60.0f / 255.0f;
  const float menu_font = 22.0f * scale;

  dl->AddRectFilled(ImVec2(menu_x, menu_y), ImVec2(menu_x + menu_w, menu_y + menu_h),
                    Color(container, container, container, 1.0f), radius);

  for (int i = 0; i < items; i++)
  {
    const float item_y = menu_y + static_cast<float>(i) * item_h;
    const bool selected = i == snapshot.selected;
    if (selected)
    {
      ImDrawFlags flags = 0;
      if (i == 0)
        flags = ImDrawFlags_RoundCornersTop;
      else if (i == items - 1)
        flags = ImDrawFlags_RoundCornersBottom;
      dl->AddRectFilled(ImVec2(menu_x, item_y), ImVec2(menu_x + menu_w, item_y + item_h),
                        Color(selected_bg, selected_bg, selected_bg, 1.0f), radius, flags);
    }

    const float text = selected ? 1.0f : 200.0f / 255.0f;
    const float text_y = item_y + (item_h - TextHeight(menu_font)) * 0.5f;
    DrawText(dl, ImVec2(menu_x + 20.0f * scale, text_y), Color(text, text, text, ease),
             snapshot.items[i].c_str(), menu_font);

    if (snapshot.show_values && !snapshot.values[i].empty())
    {
      const float value_text = selected ? 1.0f : 170.0f / 255.0f;
      const float value_w = TextWidth(snapshot.values[i].c_str(), menu_font);
      const float value_x = menu_x + menu_w - (snapshot.menu == Tico::OverlayMenu::Settings ? 40.0f : 20.0f) * scale - value_w;
      DrawText(dl, ImVec2(value_x, text_y),
               Color(value_text, value_text, value_text, ease), snapshot.values[i].c_str(), menu_font);

      if (snapshot.menu == Tico::OverlayMenu::Settings && selected)
      {
        const ImU32 arrow_color = Color(value_text, value_text, value_text, ease);
        const float arrow_size = 12.0f * scale;
        const float arrow_y = item_y + (item_h - arrow_size) * 0.5f;
        const float left_x = value_x - arrow_size - 12.0f * scale;
        dl->AddTriangleFilled(ImVec2(left_x, arrow_y + arrow_size * 0.5f),
                              ImVec2(left_x + arrow_size, arrow_y),
                              ImVec2(left_x + arrow_size, arrow_y + arrow_size), arrow_color);
        const float right_x = value_x + value_w + 12.0f * scale;
        dl->AddTriangleFilled(ImVec2(right_x + arrow_size, arrow_y + arrow_size * 0.5f),
                              ImVec2(right_x, arrow_y),
                              ImVec2(right_x, arrow_y + arrow_size), arrow_color);
      }
    }
  }

  const float helper_font = 18.0f * scale;
  const float bar_h = 42.0f * scale;
  const float margin_bottom = 24.0f * scale;
  const float padding = 14.0f * scale;
  const float button_size = 20.0f * scale;
  const float item_spacing = 12.0f * scale;
  const float text_gap = 8.0f * scale;
  const bool settings = snapshot.menu == Tico::OverlayMenu::Settings;
  const bool disc_shortcut = snapshot.menu == Tico::OverlayMenu::QuickMenu && snapshot.has_disc_menu;
  const bool reset_shortcut = snapshot.menu != Tico::OverlayMenu::ResetConfirm;
  const float change_w = settings ? TextWidth("Change", helper_font) : 0.0f;
  const float disc_w = disc_shortcut ? TextWidth("Disc", helper_font) : 0.0f;
  const float reset_w = reset_shortcut ? TextWidth("Hold Reset", helper_font) : 0.0f;
  const float back_w = TextWidth("Back", helper_font);
  const float select_w = TextWidth("Select", helper_font);
  const float total_w = padding * 2.0f +
                        (settings ? button_size + text_gap + change_w + item_spacing : 0.0f) +
                        (disc_shortcut ? button_size + text_gap + disc_w + item_spacing : 0.0f) +
                        (reset_shortcut ? button_size + text_gap + reset_w + item_spacing : 0.0f) +
                        button_size + text_gap + back_w + item_spacing + button_size + text_gap + select_w;
  float cursor_x = display.x - total_w - 20.0f * scale + 400.0f * scale * (1.0f - ease) + padding;
  cursor_x = std::max(padding, cursor_x);
  const float center_y = display.y - margin_bottom - bar_h * 0.5f;
  const float button_y = center_y - button_size * 0.5f;
  const float helper_text_y = center_y - TextHeight(helper_font) * 0.5f;
  const ImU32 helper_color = Color(200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, ease);

  auto prompt = [&](const char* symbol, const char* label, float symbol_scale) {
    DrawButtonPrompt(dl, cursor_x, button_y, button_size, symbol, helper_font * symbol_scale);
    cursor_x += button_size + text_gap;
    DrawText(dl, ImVec2(cursor_x, helper_text_y), helper_color, label, helper_font);
    cursor_x += TextWidth(label, helper_font) + item_spacing;
  };

  if (settings)
    prompt("DP", "Change", 0.55f);
  if (disc_shortcut)
    prompt("X", "Disc", 0.75f);
  if (reset_shortcut)
    prompt("-", "Hold Reset", 0.75f);
  prompt("B", "Back", 0.75f);
  prompt("A", "Select", 0.75f);

  DrawSocialArea(dl, scale, ease);
  DrawStatusBar(dl, display, scale, ease);
}
} // namespace

void SetExitApplicationCallback(ExitApplicationCallback callback)
{
  s_exit_application_callback = callback;
}

void Initialize()
{
  EnsureTicoFolders();
  ImGuiManager::SetFontPathAndRange("romfs:/fonts/font.ttf", {});
#ifdef __SWITCH__
  if (!s_psm_initialized)
    s_psm_initialized = R_SUCCEEDED(psmInitialize());
#endif
  RefreshOverlayModel();
}

void Shutdown()
{
  s_previous_buttons = 0;
  Overlay().ForceHide();
  SyncEmulationPause(false);
  s_avatar_texture.reset();
  s_bolt_texture.reset();
  s_ra_icon_texture.reset();
  s_ra_badge_textures.clear();
  s_avatar_load_attempted = false;
  s_bolt_load_attempted = false;
  s_ra_icon_load_attempted = false;
  {
    std::lock_guard lock(s_ra_notifications_mutex);
    s_ra_notifications.clear();
  }
#ifdef __SWITCH__
  if (s_psm_initialized)
  {
    psmExit();
    s_psm_initialized = false;
  }
#endif
}

bool ShouldChainloadLauncher()
{
  return s_chainload_launcher;
}

void ChainloadLauncherIfRequested()
{
  if (!s_chainload_launcher)
    return;

  Tico::ChainloadLauncher([](const std::string& message) { Log_InfoPrint(message.c_str()); });
}

void RenderOverlay()
{
  // This runs on every presented frame, paused or not. The folders are made
  // once (Initialize), and the menu's contents (state slots, the game's
  // other discs: SD card lookups) are read when it opens and then once a
  // second while it stays open: done every frame, they kept the system's
  // file service busy (core 3 at 90%) all game long.
  static bool s_menu_was_visible = false;
  static Common::Timer::Value s_last_refresh = 0;
  Tico::TicoOverlay& overlay = Overlay();
  const bool menu_visible = overlay.IsVisible();
  const Common::Timer::Value now = Common::Timer::GetCurrentValue();
  if (menu_visible &&
      (!s_menu_was_visible || Common::Timer::ConvertValueToSeconds(now - s_last_refresh) >= 1.0))
  {
    RefreshOverlayModel();
    s_last_refresh = now;
  }
  s_menu_was_visible = menu_visible;
  const float delta_time = ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : (1.0f / 60.0f);
  overlay.Update(delta_time);
  UpdateRANotifications(delta_time);
  ProcessActions();
  SyncEmulationPause(overlay.IsVisible());
  const Tico::OverlaySnapshot snapshot = overlay.Snapshot();
  ApplyDisplaySettings(snapshot);
  DrawOverlaySnapshot(snapshot);
}

void PushRANotification(std::string title, std::string description, std::string badge_path, float duration)
{
  if (title.empty() && description.empty())
    return;

  Tico::RANotificationSnapshot notification;
  notification.title = std::move(title);
  notification.description = std::move(description);
  notification.badge_name = badge_path.empty() ? "ra_icon" : std::move(badge_path);
  notification.duration = std::max(duration, 1.0f);

  std::lock_guard lock(s_ra_notifications_mutex);
  if (s_ra_notifications.size() >= 8)
    s_ra_notifications.erase(s_ra_notifications.begin());
  s_ra_notifications.push_back(std::move(notification));
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
  EnsureTicoFolders();
  Tico::TicoOverlay& overlay = Overlay();
  if (controller_index != 0)
    return overlay.IsVisible();

  Tico::FrameInput input = {};
  input.buttons = MapSwitchButtons(buttons);
  input.pressed = input.buttons & ~s_previous_buttons;
  input.released = s_previous_buttons & ~input.buttons;
  input.leftStickX = left.x;
  input.leftStickY = -left.y;
  input.rightStickX = right.x;
  input.rightStickY = -right.y;
  s_previous_buttons = input.buttons;

  const bool consumed = overlay.HandleInput(input) || overlay.IsVisible();
  SyncEmulationPause(overlay.IsVisible());
  return consumed;
}
#endif

} // namespace TicoDuck
