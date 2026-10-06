// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vulkan_lsfg.h"
#include "vulkan_device.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/timer.h"

#include "lsfg_bridge.h"

#include <atomic>
#include <string>
#include <vector>

Log_SetChannel(VulkanDevice);

namespace VulkanLSFG {
namespace {

constexpr const char* PIPELINE_CACHE_PATH = "sdmc:/tico/cache/duckstation/lsfg-pipeline-cache.bin";

// The game's own frame rate decides: frames are generated below ~40 fps and
// stop above ~50 fps (a 60 fps game, or the paused menu), with room between
// the two so a rate near the edge does not flip every frame.
constexpr double GENERATE_ABOVE_INTERVAL = 25.0;  // ms
constexpr double PASS_BELOW_INTERVAL = 20.0;      // ms
constexpr u32 MIN_SAMPLES = 8;

std::atomic_bool s_enabled{false};
std::atomic<float> s_flow_scale{0.25f};
std::atomic_bool s_performance_mode{true};

bool s_prepared = false;
std::string s_dll_path;

VkSwapchainKHR s_swap_chain = VK_NULL_HANDLE;
VkExtent2D s_extent = {};
std::vector<VkImage> s_images;

LsfgNxRuntime* s_runtime = nullptr;
bool s_runtime_failed = false; // not tried again until the swapchain or the options change
float s_runtime_flow_scale = 0.0f;
bool s_runtime_performance_mode = false;

Common::Timer::Value s_last_present = 0;
double s_interval_ms = 0.0;
u32 s_samples = 0;
bool s_generating = false;

void DestroyRuntime()
{
  if (!s_runtime)
    return;
  lsfg_nx_destroy(s_runtime);
  s_runtime = nullptr;
}

void ResetRate()
{
  s_last_present = 0;
  s_interval_ms = 0.0;
  s_samples = 0;
  s_generating = false;
}

// One of the game's frames is being presented: follow its rate.
void ObserveFrame()
{
  const Common::Timer::Value now = Common::Timer::GetCurrentValue();
  if (s_last_present != 0)
  {
    const double interval = Common::Timer::ConvertValueToMilliseconds(now - s_last_present);
    if (interval >= 4.0 && interval <= 100.0)
    {
      s_interval_ms = (s_samples == 0) ? interval : (s_interval_ms * 0.875 + interval * 0.125);
      if (s_samples < MIN_SAMPLES)
        s_samples++;
    }
  }
  s_last_present = now;

  if (s_samples < MIN_SAMPLES)
    s_generating = false;
  else if (!s_generating && s_interval_ms >= GENERATE_ABOVE_INTERVAL)
    s_generating = true;
  else if (s_generating && s_interval_ms < PASS_BELOW_INTERVAL)
    s_generating = false;
}

bool EnsureRuntime(VkQueue queue)
{
  const float flow_scale = s_flow_scale.load(std::memory_order_acquire);
  const bool performance_mode = s_performance_mode.load(std::memory_order_acquire);
  if (s_runtime && (flow_scale != s_runtime_flow_scale || performance_mode != s_runtime_performance_mode))
  {
    DestroyRuntime();
    s_runtime_failed = false;
  }
  if (s_runtime)
    return true;
  if (s_runtime_failed)
    return false;

  VulkanDevice& dev = VulkanDevice::GetInstance();
  const LsfgNxCreateInfo info = {
    .instance = dev.GetVulkanInstance(),
    .physical_device = dev.GetVulkanPhysicalDevice(),
    .device = dev.GetVulkanDevice(),
    .queue = queue,
    .queue_family_index = dev.GetPresentQueueFamilyIndex(),
    .get_instance_proc_addr = vkGetInstanceProcAddr,
    .swapchain = s_swap_chain,
    .extent = s_extent,
    .swapchain_images = s_images.data(),
    .swapchain_image_count = static_cast<uint32_t>(s_images.size()),
    .shader_dll_path = s_dll_path.c_str(),
    .pipeline_cache_path = PIPELINE_CACHE_PATH,
    .flow_scale = flow_scale,
    .performance_mode = performance_mode,
  };
  s_runtime = lsfg_nx_create(&info);
  s_runtime_flow_scale = flow_scale;
  s_runtime_performance_mode = performance_mode;
  if (!s_runtime)
  {
    ERROR_LOG("LSFG: cannot start frame generation with {} (a Lossless Scaling DLL it can read?)", s_dll_path);
    s_runtime_failed = true;
    return false;
  }

  INFO_LOG("LSFG: frame generation started ({}x{}, flow scale {}, {} mode)", s_extent.width, s_extent.height,
           flow_scale, performance_mode ? "performance" : "quality");
  return true;
}

} // namespace

void SetOptions(bool enabled, float flow_scale, bool performance_mode)
{
  s_enabled.store(enabled, std::memory_order_release);
  s_flow_scale.store((flow_scale == 0.5f) ? 0.5f : 0.25f, std::memory_order_release);
  s_performance_mode.store(performance_mode, std::memory_order_release);
}

bool IsRequested()
{
  return s_enabled.load(std::memory_order_acquire);
}

bool Prepare()
{
  s_prepared = false;
  s_dll_path.clear();
  for (const char* path : {DLL_PATH, DOLPHIN_DLL_PATH})
  {
    if (FileSystem::FileExists(path))
    {
      s_dll_path = path;
      break;
    }
  }
  if (s_dll_path.empty())
  {
    INFO_LOG("LSFG: no Lossless.dll in {}, frame generation is unavailable", DLL_PATH);
    return false;
  }

  INFO_LOG("LSFG: using {}", s_dll_path);
  s_prepared = true;
  return true;
}

bool IsPrepared()
{
  return s_prepared;
}

void Disable(const char* reason)
{
  if (s_prepared)
    WARNING_LOG("LSFG: frame generation is unavailable: {}", reason);
  DestroyRuntime();
  s_prepared = false;
}

void Shutdown()
{
  UnregisterSwapChain();
  s_prepared = false;
}

void RegisterSwapChain(VkSwapchainKHR swap_chain, VkExtent2D extent, std::span<const VkImage> images)
{
  UnregisterSwapChain();
  if (!s_prepared)
    return;
  if (images.size() < 3)
  {
    Disable("the swapchain has fewer than three images");
    return;
  }

  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (dev.GetGraphicsQueueFamilyIndex() != dev.GetPresentQueueFamilyIndex())
  {
    Disable("separate graphics and present queues");
    return;
  }

  s_swap_chain = swap_chain;
  s_extent = extent;
  s_images.assign(images.begin(), images.end());
}

void UnregisterSwapChain()
{
  DestroyRuntime();
  s_runtime_failed = false;
  s_swap_chain = VK_NULL_HANDLE;
  s_extent = {};
  s_images.clear();
  ResetRate();
}

VkResult Present(VkQueue queue, const VkPresentInfoKHR& present_info)
{
  const bool ours = (s_prepared && s_swap_chain != VK_NULL_HANDLE && present_info.swapchainCount == 1 &&
                     present_info.pSwapchains[0] == s_swap_chain);
  if (!ours || !s_enabled.load(std::memory_order_acquire))
  {
    if (s_runtime)
    {
      // switched off: its images and pipelines go until it is wanted again
      DestroyRuntime();
      s_runtime_failed = false;
      ResetRate();
    }
    return vkQueuePresentKHR(queue, &present_info);
  }

  ObserveFrame();
  if (!s_generating || !EnsureRuntime(queue))
    return vkQueuePresentKHR(queue, &present_info);

  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  if (!lsfg_nx_present(s_runtime, queue, &present_info, &result))
  {
    // not consumed: this frame goes out as usual
    ERROR_LOG("LSFG: the swapchain was not accepted, frame generation stops");
    DestroyRuntime();
    s_runtime_failed = true;
    return vkQueuePresentKHR(queue, &present_info);
  }

  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
  {
    // out of date: DuckStation makes the swapchain again, and frame
    // generation starts over on it; anything else stops it
    DestroyRuntime();
    if (result != VK_ERROR_OUT_OF_DATE_KHR)
    {
      ERROR_LOG("LSFG: presenting failed ({}), frame generation stops", static_cast<int>(result));
      s_runtime_failed = true;
    }
  }
  return result;
}

} // namespace VulkanLSFG
