/// @file DuckShaders.h
/// @brief RetroArch slang presets over DuckStation's picture, on deko3D and
/// Vulkan alike.
#pragma once

#include "DuckSlangChain.h"

namespace DuckShaders
{
/// Adds the Shaders category to Settings (with the overlay).
void Init();
/// Loads the preset the settings name when it changes (outside a frame).
void Update();
/// The GPU device goes away: the chain frees what it owns, and builds it again
/// on the next device.
void ReleaseGPU();
/// Removes the category; the chain stays for the overlay's next device.
void Shutdown();
/// The session ends: the chain goes too.
void Destroy();

DuckSlangChain::Result Apply(GPUTexture* source, const GSVector4i source_rect, GPUTexture* target,
                             const GSVector4i draw_rect, float aspect, double fps);
} // namespace DuckShaders
