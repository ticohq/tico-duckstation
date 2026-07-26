// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace TicoDuck
{

using ExitApplicationCallback = void (*)();

void SetExitApplicationCallback(ExitApplicationCallback callback);
void Initialize();
void Shutdown();
void RenderOverlay();
bool ShouldChainloadLauncher();
void ChainloadLauncherIfRequested();
void PushRANotification(std::string title, std::string description, std::string badge_path, float duration);
void PlayRATrophySound();

#ifdef __SWITCH__
bool HandleSwitchInput(unsigned controller_index, uint64_t buttons, const HidAnalogStickState& left,
                       const HidAnalogStickState& right);
#endif

} // namespace TicoDuck
