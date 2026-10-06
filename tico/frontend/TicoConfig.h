/// @file TicoConfig.h
/// @brief Where tico keeps what the DuckStation module reads and writes.
#pragma once

#include <string>

namespace TicoConfig
{
/// DuckStation's own files: BIOS images, game settings, the game database cache.
constexpr const char *SystemPath = "sdmc:/tico/system/duckstation";
/// The overlay's save states (slots 1-6 are <game>.state0 .. .state5).
constexpr const char *StatesPath = "sdmc:/tico/states/psx";
} // namespace TicoConfig

// Emulator-agnostic paths used by the generic Tico layer (Tico::ChainloadLauncher).
namespace Tico { namespace Paths {
constexpr const char *Root = "sdmc:/tico";
constexpr const char *LauncherNro = "sdmc:/switch/tico.nro";
constexpr const char *LauncherNroFallback = "sdmc:/switch/tico/tico.nro";
}} // namespace Tico::Paths
