// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TicoOverlay.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

namespace Tico
{
namespace
{
constexpr float kOverlayAnimationSeconds = 0.22f;
constexpr float kResetHoldSeconds = 0.75f;
constexpr int kSettingsItems = 2;
constexpr int kResetConfirmItems = 2;
constexpr int kSaveQuickIndex = 0;
constexpr int kLoadQuickIndex = 1;

bool ReadJsonStringValue(const std::string& text, const char* key, std::string& value);

bool ContainsValue(const std::string& text, const char* key, const char* value)
{
  std::string current_value;
  return ReadJsonStringValue(text, key, current_value) && current_value == value;
}

bool ReadJsonStringValue(const std::string& text, const char* key, std::string& value)
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

std::string EscapeJsonString(const std::string& value)
{
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value)
  {
    if (ch == '"' || ch == '\\')
      escaped.push_back('\\');
    escaped.push_back(ch);
  }
  return escaped;
}

bool FindJsonStringBounds(const std::string& text, const char* key, size_t& value_begin, size_t& value_end)
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

  bool escape = false;
  for (size_t i = quote_pos + 1; i < text.size(); i++)
  {
    const char ch = text[i];
    if (escape)
    {
      escape = false;
      continue;
    }
    if (ch == '\\')
    {
      escape = true;
      continue;
    }
    if (ch == '"')
    {
      value_begin = quote_pos + 1;
      value_end = i;
      return true;
    }
  }

  return false;
}

void UpsertJsonString(std::string& text, const char* key, const std::string& value)
{
  if (text.find('{') == std::string::npos || text.find('}') == std::string::npos)
    text = "{\n}\n";

  size_t value_begin = 0;
  size_t value_end = 0;
  const std::string escaped_value = EscapeJsonString(value);
  if (FindJsonStringBounds(text, key, value_begin, value_end))
  {
    text.replace(value_begin, value_end - value_begin, escaped_value);
    return;
  }

  size_t object_end = text.rfind('}');
  if (object_end == std::string::npos)
  {
    text = "{\n}\n";
    object_end = text.rfind('}');
  }

  const bool has_existing_entry = text.find(':') != std::string::npos && text.find(':') < object_end;
  std::string insertion = has_existing_entry ? ",\n" : "\n";
  insertion += "    \"";
  insertion += key;
  insertion += "\": \"";
  insertion += escaped_value;
  insertion += "\"\n";
  text.insert(object_end, insertion);
}
}

TicoOverlay::TicoOverlay()
{
  LoadSettings();
}

void TicoOverlay::SetGameTitle(std::string title)
{
  if (!title.empty())
    title_ = std::move(title);
}

void TicoOverlay::SetDiscEntries(std::vector<std::string> labels, int current_index)
{
  disc_labels_.clear();
  disc_labels_.reserve(std::min(labels.size(), MaxOverlayItems));
  for (std::string& label : labels)
  {
    if (disc_labels_.size() >= MaxOverlayItems)
      break;
    if (label.empty())
      label = "Disc " + std::to_string(disc_labels_.size() + 1);
    disc_labels_.push_back(std::move(label));
  }

  if (disc_labels_.empty())
  {
    disc_selected_ = 0;
    if (menu_ == OverlayMenu::DiscSelect)
      menu_ = OverlayMenu::QuickMenu;
  }
  else
  {
    disc_selected_ = std::clamp(current_index, 0, static_cast<int>(disc_labels_.size()) - 1);
  }

  if (selected_ >= QuickMenuItemCount())
    selected_ = QuickMenuItemCount() - 1;
}

void TicoOverlay::SetStateSlotUsage(const std::array<bool, StateSlotCount>& used)
{
  state_slots_used_ = used;
}

void TicoOverlay::SetRANotifications(std::vector<RANotificationSnapshot> notifications)
{
  ra_notifications_.clear();
  ra_notifications_.reserve(std::min(notifications.size(), MaxRANotifications));
  for (RANotificationSnapshot& notification : notifications)
  {
    if (ra_notifications_.size() >= MaxRANotifications)
      break;
    if (!notification.title.empty() || !notification.description.empty())
      ra_notifications_.push_back(std::move(notification));
  }
}

void TicoOverlay::Update(float delta_time)
{
  if (visible_)
  {
    anim_timer_ += delta_time;
    if (reset_shortcut_down_ && menu_ != OverlayMenu::ResetConfirm)
    {
      reset_hold_timer_ += delta_time;
      if (reset_hold_timer_ >= kResetHoldSeconds)
      {
        menu_ = OverlayMenu::ResetConfirm;
        reset_confirm_selected_ = 0;
        HoldResetShortcut(true);
      }
    }
  }
  else
  {
    anim_timer_ = 0.0f;
    reset_shortcut_down_ = false;
    reset_hold_timer_ = 0.0f;
  }
}

bool TicoOverlay::HandleInput(const FrameInput& input)
{
  if (input.pressed & Pad_Guide)
  {
    if (input.buttons & Pad_Select)
      HoldResetShortcut(true);

    if (!visible_)
      Show();
    else if (menu_ == OverlayMenu::Settings)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = SettingsQuickIndex();
    }
    else if (menu_ == OverlayMenu::DiscSelect)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = DiscQuickIndex();
    }
    else if (menu_ == OverlayMenu::StateSlots)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = state_save_mode_ ? kSaveQuickIndex : kLoadQuickIndex;
    }
    else if (menu_ == OverlayMenu::ResetConfirm)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = 0;
    }
    else
      Hide();
    return true;
  }

  if (!visible_)
  {
    reset_shortcut_down_ = false;
    reset_hold_timer_ = 0.0f;
    if (!(input.buttons & Pad_Select))
      reset_shortcut_needs_release_ = false;
    return false;
  }

  UpdateResetShortcut(input);

  if (input.pressed & Pad_Up)
  {
    if (menu_ == OverlayMenu::Settings)
      settings_selected_ = (settings_selected_ + kSettingsItems - 1) % kSettingsItems;
    else if (menu_ == OverlayMenu::DiscSelect && HasDiscEntries())
    {
      const int count = static_cast<int>(disc_labels_.size());
      disc_selected_ = (disc_selected_ + count - 1) % count;
    }
    else if (menu_ == OverlayMenu::StateSlots)
      state_selected_ = (state_selected_ + static_cast<int>(StateSlotCount) - 1) %
                        static_cast<int>(StateSlotCount);
    else if (menu_ == OverlayMenu::ResetConfirm)
      reset_confirm_selected_ = (reset_confirm_selected_ + kResetConfirmItems - 1) %
                                kResetConfirmItems;
    else
      selected_ = (selected_ + CurrentItemCount() - 1) % CurrentItemCount();
    return true;
  }
  if (input.pressed & Pad_Down)
  {
    if (menu_ == OverlayMenu::Settings)
      settings_selected_ = (settings_selected_ + 1) % kSettingsItems;
    else if (menu_ == OverlayMenu::DiscSelect && HasDiscEntries())
    {
      const int count = static_cast<int>(disc_labels_.size());
      disc_selected_ = (disc_selected_ + 1) % count;
    }
    else if (menu_ == OverlayMenu::StateSlots)
      state_selected_ = (state_selected_ + 1) % static_cast<int>(StateSlotCount);
    else if (menu_ == OverlayMenu::ResetConfirm)
      reset_confirm_selected_ = (reset_confirm_selected_ + 1) % kResetConfirmItems;
    else
      selected_ = (selected_ + 1) % CurrentItemCount();
    return true;
  }
  if ((input.pressed & Pad_Left) && menu_ == OverlayMenu::ResetConfirm)
  {
    reset_confirm_selected_ = (reset_confirm_selected_ + kResetConfirmItems - 1) %
                              kResetConfirmItems;
    return true;
  }
  if ((input.pressed & Pad_Right) && menu_ == OverlayMenu::ResetConfirm)
  {
    reset_confirm_selected_ = (reset_confirm_selected_ + 1) % kResetConfirmItems;
    return true;
  }
  if ((input.pressed & Pad_Left) && menu_ == OverlayMenu::Settings)
  {
    CycleSelectedSetting(-1);
    return true;
  }
  if ((input.pressed & Pad_Right) && menu_ == OverlayMenu::Settings)
  {
    CycleSelectedSetting(1);
    return true;
  }
  if ((input.pressed & Pad_X) && menu_ == OverlayMenu::QuickMenu && HasDiscEntries())
  {
    menu_ = OverlayMenu::DiscSelect;
    disc_selected_ = std::clamp(disc_selected_, 0, static_cast<int>(disc_labels_.size()) - 1);
    return true;
  }
  if (input.pressed & Pad_B)
  {
    ActivateSelection();
    return true;
  }
  if (input.pressed & Pad_A)
  {
    if (menu_ == OverlayMenu::Settings)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = SettingsQuickIndex();
    }
    else if (menu_ == OverlayMenu::DiscSelect)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = DiscQuickIndex();
    }
    else if (menu_ == OverlayMenu::StateSlots)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = state_save_mode_ ? kSaveQuickIndex : kLoadQuickIndex;
    }
    else if (menu_ == OverlayMenu::ResetConfirm)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = 0;
    }
    else
      Hide();
    return true;
  }

  return true;
}

OverlaySnapshot TicoOverlay::Snapshot() const
{
  OverlaySnapshot snapshot = {};
  snapshot.visible = visible_;
  snapshot.menu = menu_;
  if (menu_ == OverlayMenu::Settings)
    snapshot.selected = settings_selected_;
  else if (menu_ == OverlayMenu::DiscSelect)
    snapshot.selected = disc_selected_;
  else if (menu_ == OverlayMenu::StateSlots)
    snapshot.selected = state_selected_;
  else if (menu_ == OverlayMenu::ResetConfirm)
    snapshot.selected = reset_confirm_selected_;
  else
    snapshot.selected = selected_;
  snapshot.progress =
    anim_timer_ >= kOverlayAnimationSeconds ? 1.0f : anim_timer_ / kOverlayAnimationSeconds;
  snapshot.has_disc_menu = HasDiscEntries();
  snapshot.display_mode = display_mode_;
  snapshot.display_size = display_size_;
  snapshot.ra_notification_count =
    static_cast<int>(std::min(ra_notifications_.size(), MaxRANotifications));
  for (int i = 0; i < snapshot.ra_notification_count; i++)
    snapshot.ra_notifications[i] = ra_notifications_[i];

  if (menu_ == OverlayMenu::Settings)
  {
    snapshot.title = "Settings";
    snapshot.item_count = kSettingsItems;
    snapshot.show_values = true;
    snapshot.items[0] = "Display Mode";
    snapshot.values[0] = DisplayModeText();
    snapshot.items[1] = "Size";
    snapshot.values[1] = DisplaySizeText();
  }
  else if (menu_ == OverlayMenu::DiscSelect)
  {
    snapshot.title = "Select Disc";
    snapshot.item_count = static_cast<int>(std::min(disc_labels_.size(), MaxOverlayItems));
    for (int i = 0; i < snapshot.item_count; i++)
      snapshot.items[i] = disc_labels_[i];
  }
  else if (menu_ == OverlayMenu::StateSlots)
  {
    snapshot.title = state_save_mode_ ? "Save State" : "Load State";
    snapshot.item_count = static_cast<int>(StateSlotCount);
    snapshot.show_values = true;
    for (int i = 0; i < snapshot.item_count; i++)
    {
      snapshot.items[i] = "Slot " + std::to_string(i + 1);
      snapshot.values[i] = state_slots_used_[i] ? "In Use" : "Empty";
    }
  }
  else if (menu_ == OverlayMenu::ResetConfirm)
  {
    snapshot.title = "Reset Game";
    snapshot.item_count = kResetConfirmItems;
    snapshot.items[0] = "Cancel";
    snapshot.items[1] = "Reset";
  }
  else
  {
    snapshot.title = title_;
    snapshot.item_count = QuickMenuItemCount();
    snapshot.items[kSaveQuickIndex] = "Save State";
    snapshot.items[kLoadQuickIndex] = "Load State";
    snapshot.items[SettingsQuickIndex()] = "Settings";
    if (HasDiscEntries())
      snapshot.items[DiscQuickIndex()] = "Select Disc";
    snapshot.items[ExitQuickIndex()] = "Exit Game";
  }

  return snapshot;
}

void TicoOverlay::Show()
{
  visible_ = true;
  menu_ = OverlayMenu::QuickMenu;
  if (selected_ >= QuickMenuItemCount())
    selected_ = QuickMenuItemCount() - 1;
  anim_timer_ = 0.0f;
  reset_shortcut_down_ = false;
  reset_hold_timer_ = 0.0f;
}

void TicoOverlay::Hide()
{
  visible_ = false;
  menu_ = OverlayMenu::QuickMenu;
  reset_shortcut_down_ = false;
  reset_hold_timer_ = 0.0f;
}

void TicoOverlay::ForceHide()
{
  Hide();
}

void TicoOverlay::ActivateSelection()
{
  if (menu_ == OverlayMenu::Settings)
  {
    CycleSelectedSetting(1);
    return;
  }
  if (menu_ == OverlayMenu::DiscSelect)
  {
    if (HasDiscEntries())
      requested_disc_ = disc_selected_;
    Hide();
    return;
  }
  if (menu_ == OverlayMenu::StateSlots)
  {
    if (!state_save_mode_ && !state_slots_used_[state_selected_])
      return;

    requested_state_slot_ = state_selected_;
    requested_state_action_ =
      state_save_mode_ ? OverlayStateAction::Save : OverlayStateAction::Load;
    if (state_save_mode_)
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = kSaveQuickIndex;
    }
    else
    {
      Hide();
    }
    return;
  }
  if (menu_ == OverlayMenu::ResetConfirm)
  {
    if (reset_confirm_selected_ == 1)
    {
      should_reset_ = true;
      Hide();
    }
    else
    {
      menu_ = OverlayMenu::QuickMenu;
      selected_ = 0;
    }
    return;
  }

  if (selected_ == kSaveQuickIndex)
  {
    state_save_mode_ = true;
    state_selected_ = 0;
    menu_ = OverlayMenu::StateSlots;
    return;
  }
  if (selected_ == kLoadQuickIndex)
  {
    state_save_mode_ = false;
    state_selected_ = 0;
    menu_ = OverlayMenu::StateSlots;
    return;
  }
  if (selected_ == SettingsQuickIndex())
  {
    menu_ = OverlayMenu::Settings;
    settings_selected_ = 0;
    return;
  }
  if (HasDiscEntries() && selected_ == DiscQuickIndex())
  {
    menu_ = OverlayMenu::DiscSelect;
    disc_selected_ = std::clamp(disc_selected_, 0, static_cast<int>(disc_labels_.size()) - 1);
    return;
  }

  should_exit_ = true;
}

int TicoOverlay::TakeRequestedDiscIndex()
{
  const int index = requested_disc_;
  requested_disc_ = -1;
  return index;
}

OverlayStateAction TicoOverlay::TakeRequestedStateAction(int& slot)
{
  const OverlayStateAction action = requested_state_action_;
  slot = requested_state_slot_;
  requested_state_action_ = OverlayStateAction::None;
  requested_state_slot_ = -1;
  return action;
}

int TicoOverlay::QuickMenuItemCount() const
{
  return HasDiscEntries() ? 5 : 4;
}

int TicoOverlay::CurrentItemCount() const
{
  if (menu_ == OverlayMenu::Settings)
    return kSettingsItems;
  if (menu_ == OverlayMenu::DiscSelect)
    return std::max(1, static_cast<int>(disc_labels_.size()));
  if (menu_ == OverlayMenu::StateSlots)
    return static_cast<int>(StateSlotCount);
  if (menu_ == OverlayMenu::ResetConfirm)
    return kResetConfirmItems;
  return QuickMenuItemCount();
}

int TicoOverlay::SettingsQuickIndex() const
{
  return 2;
}

int TicoOverlay::DiscQuickIndex() const
{
  return HasDiscEntries() ? 3 : SettingsQuickIndex();
}

int TicoOverlay::ExitQuickIndex() const
{
  return HasDiscEntries() ? 4 : 3;
}

void TicoOverlay::HoldResetShortcut(bool wait_for_release)
{
  reset_shortcut_down_ = false;
  reset_hold_timer_ = 0.0f;
  if (wait_for_release)
    reset_shortcut_needs_release_ = true;
}

void TicoOverlay::UpdateResetShortcut(const FrameInput& input)
{
  const bool select_down = (input.buttons & Pad_Select) != 0;
  if (!select_down)
  {
    reset_shortcut_down_ = false;
    reset_hold_timer_ = 0.0f;
    reset_shortcut_needs_release_ = false;
    return;
  }

  const bool guide_chord_down = (input.buttons & Pad_Guide) != 0 || (input.buttons & Pad_Start) != 0;
  if (guide_chord_down || reset_shortcut_needs_release_ || menu_ == OverlayMenu::ResetConfirm)
  {
    HoldResetShortcut(guide_chord_down || reset_shortcut_needs_release_);
    return;
  }

  reset_shortcut_down_ = true;
}

void TicoOverlay::CycleSelectedSetting(int direction)
{
  if (settings_selected_ == 0)
  {
    display_mode_ =
      display_mode_ == DisplayMode::Display ? DisplayMode::Integer : DisplayMode::Display;
    display_size_ = display_mode_ == DisplayMode::Integer ? DisplaySize::Auto : DisplaySize::_4_3;
    SaveSettings();
    return;
  }

  if (display_mode_ == DisplayMode::Integer)
  {
    int value = static_cast<int>(display_size_) + direction;
    if (value < static_cast<int>(DisplaySize::_1x))
      value = static_cast<int>(DisplaySize::Auto);
    if (value > static_cast<int>(DisplaySize::Auto))
      value = static_cast<int>(DisplaySize::_1x);
    display_size_ = static_cast<DisplaySize>(value);
  }
  else
  {
    int value = static_cast<int>(display_size_) + direction;
    if (value < static_cast<int>(DisplaySize::Stretch))
      value = static_cast<int>(DisplaySize::Original);
    if (value > static_cast<int>(DisplaySize::Original))
      value = static_cast<int>(DisplaySize::Stretch);
    display_size_ = static_cast<DisplaySize>(value);
  }

  SaveSettings();
}

void TicoOverlay::LoadSettings()
{
  std::ifstream input(SettingsPath());
  if (!input.is_open())
    return;

  std::ostringstream ss;
  ss << input.rdbuf();
  const std::string text = ss.str();

  display_mode_ = ContainsValue(text, "display_mode", "Integer") ? DisplayMode::Integer :
                                                              DisplayMode::Display;

  const char* size_key = display_mode_ == DisplayMode::Integer ? "integer_scale" : "display_size";
  std::string size_value;
  if (!ReadJsonStringValue(text, size_key, size_value))
    ReadJsonStringValue(text, "display_size", size_value);

  if (size_value == "Stretch")
    display_size_ = DisplaySize::Stretch;
  else if (size_value == "16:9")
    display_size_ = DisplaySize::_16_9;
  else if (size_value == "Original")
    display_size_ = DisplaySize::Original;
  else if (size_value == "1x")
    display_size_ = DisplaySize::_1x;
  else if (size_value == "2x")
    display_size_ = DisplaySize::_2x;
  else if (size_value == "Auto")
    display_size_ = DisplaySize::Auto;
  else
    display_size_ = DisplaySize::_4_3;

  if (display_mode_ == DisplayMode::Integer && static_cast<int>(display_size_) < 4)
    display_size_ = DisplaySize::Auto;
  if (display_mode_ == DisplayMode::Display && static_cast<int>(display_size_) > 3)
    display_size_ = DisplaySize::_4_3;
}

void TicoOverlay::SaveSettings() const
{
  std::ifstream input(SettingsPath());
  std::ostringstream ss;
  if (input.is_open())
    ss << input.rdbuf();

  std::string text = ss.str();
  UpsertJsonString(text, "display_mode", DisplayModeText());
  UpsertJsonString(text, display_mode_ == DisplayMode::Integer ? "integer_scale" : "display_size", DisplaySizeText());

  std::ofstream output(SettingsPath());
  if (!output.is_open())
    return;

  output << text;
}

std::string TicoOverlay::DisplayModeText() const
{
  return display_mode_ == DisplayMode::Integer ? "Integer" : "Display";
}

std::string TicoOverlay::DisplaySizeText() const
{
  if (display_mode_ == DisplayMode::Integer)
  {
    switch (display_size_)
    {
      case DisplaySize::_1x:
        return "1x";
      case DisplaySize::_2x:
        return "2x";
      case DisplaySize::Auto:
      default:
        return "Auto";
    }
  }

  switch (display_size_)
  {
    case DisplaySize::Stretch:
      return "Stretch";
    case DisplaySize::_16_9:
      return "16:9";
    case DisplaySize::Original:
      return "Original";
    case DisplaySize::_4_3:
    default:
      return "4:3";
  }
}

const char* TicoOverlay::SettingsPath() const
{
  return "sdmc:/tico/config/cores/duckstation.jsonc";
}

} // namespace Tico
