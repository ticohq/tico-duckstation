#pragma once

namespace TicoConfig
{
constexpr const char* TEST_ROM = "sdmc:/tico/roms/psx/game.cue";
constexpr const char* SYSTEM_PATH = "sdmc:/tico/system/psx";
constexpr const char* SAVES_PATH = "sdmc:/tico/saves/psx";
constexpr const char* STATES_PATH = "sdmc:/tico/states/psx";
} // namespace TicoConfig

namespace Tico::Paths
{
constexpr const char* Root = "sdmc:/tico";
constexpr const char* Cores = "sdmc:/tico/cores";
constexpr const char* Assets = "sdmc:/tico/assets";
constexpr const char* SystemRoot = "sdmc:/tico/system";
constexpr const char* SavesRoot = "sdmc:/tico/saves";
constexpr const char* StatesRoot = "sdmc:/tico/states";
constexpr const char* CoreConfigDir = "sdmc:/tico/config/cores";
constexpr const char* License = "sdmc:/tico/cores/tico-duckstation-license.txt";
constexpr const char* LauncherNro = "sdmc:/switch/tico/tico.nro";
} // namespace Tico::Paths
