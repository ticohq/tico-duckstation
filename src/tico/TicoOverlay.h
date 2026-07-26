// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "TicoMain.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace Tico
{

static constexpr size_t MaxOverlayItems = 10;
static constexpr size_t MaxRANotifications = 4;
static constexpr size_t StateSlotCount = 4;

enum class OverlayMenu
{
  QuickMenu,
  Settings,
  DiscSelect,
  StateSlots,
  ResetConfirm,
};

enum class OverlayStateAction
{
  None,
  Save,
  Load,
};

enum class DisplayMode
{
  Integer,
  Display,
};

enum class DisplaySize
{
  Stretch = 0,
  _4_3 = 1,
  _16_9 = 2,
  Original = 3,
  _1x = 4,
  _2x = 5,
  Auto = 6,
};

struct RANotificationSnapshot
{
  std::string title;
  std::string description;
  std::string badge_name;
  void* badge_texture = nullptr;
  float timer = 0.0f;
  float duration = 4.0f;
  float slide_in = 0.32f;
  float slide_out = 0.28f;
};

struct OverlaySnapshot
{
  bool visible = false;
  OverlayMenu menu = OverlayMenu::QuickMenu;
  int selected = 0;
  int item_count = 0;
  bool show_values = false;
  bool has_disc_menu = false;
  float progress = 0.0f;
  std::string title;
  std::array<std::string, MaxOverlayItems> items = {};
  std::array<std::string, MaxOverlayItems> values = {};
  int ra_notification_count = 0;
  std::array<RANotificationSnapshot, MaxRANotifications> ra_notifications = {};
  DisplayMode display_mode = DisplayMode::Display;
  DisplaySize display_size = DisplaySize::_4_3;
};

class TicoOverlay
{
public:
  TicoOverlay();

  void SetGameTitle(std::string title);
  void SetDiscEntries(std::vector<std::string> labels, int current_index);
  void SetStateSlotUsage(const std::array<bool, StateSlotCount>& used);
  void SetRANotifications(std::vector<RANotificationSnapshot> notifications);
  void Update(float delta_time);
  bool HandleInput(const FrameInput& input);
  void ForceHide();
  OverlaySnapshot Snapshot() const;

  bool IsVisible() const { return visible_; }
  bool ShouldExit() const { return should_exit_; }
  bool ShouldReset() const { return should_reset_; }
  int TakeRequestedDiscIndex();
  OverlayStateAction TakeRequestedStateAction(int& slot);
  void ClearExit() { should_exit_ = false; }
  void ClearReset() { should_reset_ = false; }

private:
  void Show();
  void Hide();
  void ActivateSelection();
  void HoldResetShortcut(bool wait_for_release);
  void UpdateResetShortcut(const FrameInput& input);
  void CycleSelectedSetting(int direction);
  bool HasDiscEntries() const { return !disc_labels_.empty(); }
  int QuickMenuItemCount() const;
  int CurrentItemCount() const;
  int SettingsQuickIndex() const;
  int DiscQuickIndex() const;
  int ExitQuickIndex() const;
  void LoadSettings();
  void SaveSettings() const;
  std::string DisplayModeText() const;
  std::string DisplaySizeText() const;
  const char* SettingsPath() const;

  std::string title_ = "GooseStation";
  bool visible_ = false;
  OverlayMenu menu_ = OverlayMenu::QuickMenu;
  bool should_exit_ = false;
  bool should_reset_ = false;
  int selected_ = 0;
  int settings_selected_ = 0;
  int disc_selected_ = 0;
  int state_selected_ = 0;
  int reset_confirm_selected_ = 0;
  int requested_disc_ = -1;
  int requested_state_slot_ = -1;
  bool state_save_mode_ = true;
  OverlayStateAction requested_state_action_ = OverlayStateAction::None;
  bool reset_shortcut_down_ = false;
  bool reset_shortcut_needs_release_ = false;
  std::vector<std::string> disc_labels_;
  std::array<bool, StateSlotCount> state_slots_used_ = {};
  std::vector<RANotificationSnapshot> ra_notifications_;
  DisplayMode display_mode_ = DisplayMode::Display;
  DisplaySize display_size_ = DisplaySize::_4_3;
  float anim_timer_ = 0.0f;
  float reset_hold_timer_ = 0.0f;
};

} // namespace Tico
