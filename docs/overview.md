# What the mod loader is

The mod loader is the script extender of The Sims 4. It is one DLL: `winmm.dll` in `Game\Bin`.

It loads the DLL mods and gives them function callbacks on the native code of the game, settings in `mods.json`,
hotkeys, a menu bar in the game window, tasks on the main thread and inspector queries.

## The parts

- The C interface: `Ts4ModApi.h` in common.
- `mod_core` of common: tasks, frame callbacks, hotkeys, settings, slot swap, start-up guard, folders.
- `mod_sdk` of common: `ModHost`, `BindFields`, `HostLogSink`.
- `src/core`: the host table, the loader settings, the mod registry, the mod catalogue, the overlay rules.
- `src/dlls`: the load and the checks of the DLL mods.
- `src/game/overlay`: the Dear ImGui menu bar on the swap chain.

## How it works

- A callback names a native function by a byte signature.
- The game loads a proxy `winmm.dll`. See [game-load.md](game-load.md).
- The tick is the update of the client services of the game.
- The DLL mods are in `%LOCALAPPDATA%\Sims4Mods\dlls`, not in the game folder.
- The extender writes the SHA-256 of each DLL mod to the log.
- The menu bar uses Direct3D 11 only.

**Not done yet:** the game messages, the probe of the game menu and the crash dump writer.
See [follow-ups.md](follow-ups.md).

**Comes later:** the native Python module for script mods, and packages or tuning if a mod needs them.
