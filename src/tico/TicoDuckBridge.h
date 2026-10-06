// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#endif

// The tico overlay (tico/frontend) inside DuckStation: the quick menu, its
// settings, save states, disc changes and the RetroAchievements toasts. It runs
// on the CPU thread, which emulates, presents and polls the controllers.
namespace TicoDuck
{

using ExitApplicationCallback = void (*)();
using SettingsReloadCallback = void (*)();

/// From main(), before the command line is parsed: a game on a USB drive is
/// named by the drive's id (usb://<id>/...), which becomes its mount path in
/// argv; the game's title from tico (argv[2]) is kept for the menu.
void PrepareLaunch(int argc, char* argv[]);

/// Ends the session (the CPU thread's loop) after Exit Game or Restart.
void SetExitApplicationCallback(ExitApplicationCallback callback);
/// Reads tico's settings into DuckStation's again and applies them.
void SetSettingsReloadCallback(SettingsReloadCallback callback);

/// On the CPU thread, once the GPU device exists / before it is released.
void Initialize();
void Shutdown();

/// Builds the overlay's frame (System::PresentDisplay, before presenting).
void RenderOverlay();
/// Draws the frame RenderOverlay built, over the game and DuckStation's OSD.
void DrawOverlay();

bool ShouldChainloadLauncher();
/// From main(), last: unmounts USB drives and starts what the session chose
/// next (tico, or this NRO again for Restart).
void ExitApplication();

void PushRANotification(std::string title, std::string description, std::string badge_path, float duration);
void PlayRATrophySound();

#ifdef __SWITCH__
/// Returns true when the input goes to the menu (it is open, or being opened)
/// instead of the game.
bool HandleSwitchInput(unsigned controller_index, uint64_t buttons, const HidAnalogStickState& left,
                       const HidAnalogStickState& right);
#endif

} // namespace TicoDuck
