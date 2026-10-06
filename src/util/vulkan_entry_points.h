// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

// On the Switch, Mesa's NVK is linked statically and exports the vk*
// functions under these very names, so the pointers live in a namespace there
// (their symbols are mangled) and calls reach them through the using-directive.
#if defined(__SWITCH__)
#define VULKAN_ENTRY_POINTS_BEGIN namespace DuckVulkan {
#define VULKAN_ENTRY_POINTS_END }
#elif defined(__cplusplus)
#define VULKAN_ENTRY_POINTS_BEGIN extern "C" {
#define VULKAN_ENTRY_POINTS_END }
#else
#define VULKAN_ENTRY_POINTS_BEGIN
#define VULKAN_ENTRY_POINTS_END
#endif

VULKAN_ENTRY_POINTS_BEGIN
#define VULKAN_MODULE_ENTRY_POINT(name, required) extern PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) extern PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) extern PFN_##name name;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
VULKAN_ENTRY_POINTS_END

#if defined(__SWITCH__)
using namespace DuckVulkan;
#endif
