# What the mod loader is

The mod loader is the script extender of The Sims 4. It is one DLL: `winmm.dll` in `Game\Bin`.

It loads the DLL mods and gives them function callbacks on the native code of the game, settings in `mods.json`,
hotkeys, a menu bar in the game window, tasks on the main thread and inspector queries.

## From the Masters of Albion loader

The extender is a mirror of the logic of the Masters of Albion mod loader. The logic is the same. The parts that
touch the game are different.

**Stays** (with new names):

- The C interface: `Ts4ModApi.h` in common.
- `mod_core` of common: tasks, frame callbacks, hotkeys, settings, slot swap, start-up guard, folders.
- `mod_sdk` of common: `ModHost`, `BindFields`, `HostLogSink`.
- `src/core`: the host table, the loader settings, the mod registry, the mod catalogue, the overlay rules.
- `src/dlls`: the load and the checks of the DLL mods.
- `src/game/overlay`: the Dear ImGui menu bar on the swap chain.

**Changes:**

- A callback names a native function by a byte signature, not a managed method.
- A proxy `winmm.dll`, not a proxy `version.dll`. See [game-load.md](game-load.md).
- The tick is the update of the client services of the game, not `GameManager.Update`.
- The DLL mods are in `%LOCALAPPDATA%\Sims4Mods\dlls`, not in the game folder.
- The extender writes the SHA-256 of each DLL mod to the log. This is new.
- Direct3D 11 only. Direct3D 12 is not ported.

**Goes:** IL2CPP, managed objects, `UnityMath.h`, the mod zips, map overrides, custom objects, packs, the merges and
`unitymesh`. These are Unity parts of Masters of Albion.

**Not ported yet:** the game messages, the probe of the game menu and the crash dump writer.
See [follow-ups.md](follow-ups.md).

**Comes later:** the native Python module for script mods, and packages or tuning if a mod needs them.
