// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan_loader.h"

#include <span>

// Frame generation for the Switch's Vulkan renderer (tico/deps/LSFG-VK):
// Lossless Scaling's shaders, read from the user's own Lossless.dll, put a
// generated frame between two of the game's, so a 30 fps game shows 60. Only
// presenting changes; a game that already shows 50/60 fps is left alone.
// Everything runs on the thread that presents (the CPU thread: presentation is
// not threaded on the Switch).
namespace VulkanLSFG {

/// Where the user puts Lossless.dll (never shipped), and Dolphin's folder for it.
static constexpr const char* DLL_PATH = "sdmc:/tico/system/lsfg/Lossless.dll";
static constexpr const char* DOLPHIN_DLL_PATH = "sdmc:/switch/dolphin/lsfg/Lossless.dll";

/// tico's settings: on/off, the motion estimation scale (0.25 or 0.5 of the
/// screen) and the lighter "performance" shaders. Takes effect at the next
/// present.
void SetOptions(bool enabled, float flow_scale, bool performance_mode);
bool IsRequested();

/// At device creation: true when Lossless.dll is installed, and the device and
/// its swapchain are then made ready for it (timeline semaphores, transfer
/// usage, two more images, FIFO), so it can be switched on in game.
bool Prepare();
bool IsPrepared();
/// The device cannot run it: presenting goes on as usual.
void Disable(const char* reason);
/// The device is going away.
void Shutdown();

void RegisterSwapChain(VkSwapchainKHR swap_chain, VkExtent2D extent, std::span<const VkImage> images);
void UnregisterSwapChain();

/// In place of vkQueuePresentKHR.
VkResult Present(VkQueue queue, const VkPresentInfoKHR& present_info);

} // namespace VulkanLSFG
