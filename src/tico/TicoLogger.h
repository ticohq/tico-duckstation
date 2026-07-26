// SPDX-FileCopyrightText: 2026 Tico
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

namespace Tico
{
using LogCallback = std::function<void(const std::string&)>;
}

class Logger
{
public:
  enum class Level
  {
    Debug,
    Info,
    Warning,
    Error,
  };

  static Logger& Instance()
  {
    static Logger instance;
    return instance;
  }

  void Log(Level level, const char* category, const char* fmt, ...)
  {
    if (!fmt)
      return;

    std::printf("[%s][%s] ", LevelName(level), category ? category : "LOG");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
  }

private:
  static const char* LevelName(Level level)
  {
    switch (level)
    {
      case Level::Debug:
        return "DEBUG";
      case Level::Info:
        return "INFO";
      case Level::Warning:
        return "WARN";
      case Level::Error:
        return "ERROR";
      default:
        return "LOG";
    }
  }
};

#define LOG_DEBUG(cat, ...) Logger::Instance().Log(Logger::Level::Debug, cat, __VA_ARGS__)
#define LOG_INFO(cat, ...) Logger::Instance().Log(Logger::Level::Info, cat, __VA_ARGS__)
#define LOG_WARN(cat, ...) Logger::Instance().Log(Logger::Level::Warning, cat, __VA_ARGS__)
#define LOG_ERROR(cat, ...) Logger::Instance().Log(Logger::Level::Error, cat, __VA_ARGS__)
