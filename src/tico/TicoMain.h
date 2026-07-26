// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "TicoLogger.h"

#include <cstdint>
#include <string>

namespace Tico
{

struct LaunchInfo
{
  int argc = 0;
  char** argv = nullptr;
  std::string contentPath;
};

enum PadButton : uint64_t
{
  Pad_A = 1ull << 0,
  Pad_B = 1ull << 1,
  Pad_X = 1ull << 2,
  Pad_Y = 1ull << 3,
  Pad_Up = 1ull << 4,
  Pad_Down = 1ull << 5,
  Pad_Left = 1ull << 6,
  Pad_Right = 1ull << 7,
  Pad_L = 1ull << 8,
  Pad_R = 1ull << 9,
  Pad_L2 = 1ull << 10,
  Pad_R2 = 1ull << 11,
  Pad_L3 = 1ull << 12,
  Pad_R3 = 1ull << 13,
  Pad_Start = 1ull << 14,
  Pad_Select = 1ull << 15,
  Pad_Guide = 1ull << 16,
};

struct FrameInput
{
  uint64_t buttons = 0;
  uint64_t pressed = 0;
  uint64_t released = 0;
  int leftStickX = 0;
  int leftStickY = 0;
  int rightStickX = 0;
  int rightStickY = 0;
};

class CoreRuntime
{
public:
  virtual ~CoreRuntime() = default;

  virtual const char* Name() const = 0;
  virtual bool Configure(const LaunchInfo& launch) = 0;
  virtual bool Initialize(const LaunchInfo& launch) = 0;
  virtual bool LoadContent(const std::string& path) = 0;
  virtual void HandleInput(const FrameInput& input) = 0;
  virtual void RunFrame() = 0;
  virtual void RenderFrame() = 0;
  virtual bool ShouldExit() const = 0;
  virtual bool ShouldChainloadLauncher() const { return false; }
  virtual void RequestExit() = 0;
  virtual void Shutdown() = 0;
};

class Main
{
public:
  explicit Main(CoreRuntime& runtime, LogCallback log = {});
  int Run(int argc, char** argv);

private:
  bool InitPlatform();
  void ShutdownPlatform();
  FrameInput PollInput();
  void Log(const char* fmt, ...) const;

  CoreRuntime& runtime_;
  LogCallback log_;
  bool platform_ready_ = false;
  bool sd_mounted_ = false;
  bool romfs_mounted_ = false;
  bool socket_initialized_ = false;
  bool psm_initialized_ = false;
  bool exit_locked_ = false;
  bool stdout_redirected_ = false;
  uint64_t previous_buttons_ = 0;
};

float OverlayModeScale();

} // namespace Tico
