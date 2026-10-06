/// @file TicoLogger.h
/// @brief The overlay's log macros, written through DuckStation's log (the
/// debug build's sdmc:/tico/debug/duckstation.txt).
#pragma once

#include "common/log.h"

#include "fmt/printf.h"

#include <functional>
#include <string>

namespace Tico
{
using LogCallback = std::function<void(const std::string &)>;
}

#define LOG_DEBUG(cat, ...) Log::FastWrite(cat, __func__, LOGLEVEL_DEBUG, std::string_view(fmt::sprintf(__VA_ARGS__)))
#define LOG_INFO(cat, ...) Log::FastWrite(cat, __func__, LOGLEVEL_INFO, std::string_view(fmt::sprintf(__VA_ARGS__)))
#define LOG_WARN(cat, ...) Log::FastWrite(cat, __func__, LOGLEVEL_WARNING, std::string_view(fmt::sprintf(__VA_ARGS__)))
#define LOG_ERROR(cat, ...) Log::FastWrite(cat, __func__, LOGLEVEL_ERROR, std::string_view(fmt::sprintf(__VA_ARGS__)))
