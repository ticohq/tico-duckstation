<picture>
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">
<img src="https://i.imgur.com/8qsV6MH.png" width="200">
</picture>

*Part of the Tico ecosystem* - https://www.ticoverse.com

# Tico DuckStation

Tico DuckStation is a Nintendo Switch core for Tico based on the GPL-licensed era of DuckStation.

This fork exists so the PlayStation core can be redistributed as part of the Tico ecosystem while keeping the Tico overlay, launcher integration, file layout, save/state behavior, and RetroAchievements flow consistent with the other Tico cores.

The Switch build also includes the deko3D renderer work from RSDuck's Switch port work, adapted here for the current Tico DuckStation integration.

## Features

- Tico overlay with clock, battery, avatar, game title formatting, quick menu, settings, save states, disc swap, and RetroAchievements notifications
- Tico file layout under `sdmc:/tico/system/psx`, `sdmc:/tico/saves/psx`, `sdmc:/tico/states/psx`, and `sdmc:/tico/config/cores/duckstation.jsonc`
- DuckStation settings are generated internally from the Tico JSON config at runtime
- deko3D renderer for Nintendo Switch
- Switch input mapping with analog/digital handling and vibration support
- Chainload back to the Tico launcher on exit

## Build

From this directory:

```sh
./build-switch.sh nro
```

To build only the core library:

```sh
./build-switch.sh core
```

The NRO is generated at:

```text
build-switch/tico-duckstation.nro
```

## Credits

- **DuckStation** - original PlayStation emulator project by Stenzek and contributors
- **RSDuck** - Switch/deko3D renderer work and UAM shader compilation support used by the Switch port
- **Tico** - frontend integration, overlay, runtime configuration, Switch packaging, and ecosystem behavior
- **RetroAchievements/rcheevos** - achievement runtime and service integration

## License

This repository is based on the GPL-licensed era of DuckStation and keeps the project redistributable under GPL terms.

DuckStation, deko3D Switch work, Tico integration code, and third-party dependencies remain credited to their respective authors and license holders.
