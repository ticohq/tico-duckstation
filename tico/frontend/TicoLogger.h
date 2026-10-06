/// @file TicoLogger.h
/// @brief The overlay's log macros, written through DuckStation's log (the
/// debug build's sdmc:/tico/debug/duckstation.txt).
#pragma once

#include "common/log.h"

#include <functional>
#include <string>

namespace Tico
{
using LogCallback = std::function<void(const std::string &)>;
}

#define LOG_DEBUG(cat, ...) Log::Writef(cat, __func__, LOGLEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(cat, ...) Log::Writef(cat, __func__, LOGLEVEL_INFO, __VA_ARGS__)
#define LOG_WARN(cat, ...) Log::Writef(cat, __func__, LOGLEVEL_WARNING, __VA_ARGS__)
#define LOG_ERROR(cat, ...) Log::Writef(cat, __func__, LOGLEVEL_ERROR, __VA_ARGS__)
