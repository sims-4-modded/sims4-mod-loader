# The port of the Masters of Albion mod loader

The mod loader of The Sims 4 is a mirror of the logic of the Masters of Albion mod loader
(`D:\masters_of_albion\projects\mod_loader` and its `common`). The logic is the same; the parts that touch the game
are different. This file gives each part and its state.

## What stays, what changes, what goes

| Masters of Albion part | The Sims 4 part | State |
|---|---|---|
| `common/src/mod_api/ModApi.h` | `common/src/mod_api/Ts4ModApi.h`: the same interface. Native function callbacks (named byte signatures) replace the managed method callbacks. Python function callbacks come later. | Stage 1, in progress |
| `common/src/mod_core` (tasks, frame callbacks, hotkeys, settings fields and file, slot swap, start-up guard, crash dump rules, folders) | The same, plus the signature parser and matcher. `UnityMath.h` goes. | Stage 1, in progress |
| `common/src/mod_sdk` (`ModHost`, `BindFields`, `HostLogSink`) | The same, for `Ts4ModApi.h`. | Stage 1, in progress |
| `common/src/mod_game` (IL2CPP, managed objects, method hooks, hotkey input, game messages, crash dumps) | Hooks by signature (MinHook), the hotkey input, the game messages, crash dumps, and the Python 3.7 C API of the game. IL2CPP and managed objects go. | Stage 2 |
| `mod_loader/src/core` (host table, loader settings, mod registry, mod catalogue, hook rules, overlay rules) | The same. | Stage 2 |
| `mod_loader/src/dlls/ModDllLoader` (the load and the checks of the mod DLLs) | The same: checksums, interface version, ids, the start-up guard. | Stage 2 |
| `mod_loader/src/game/overlay` (DXGI `Present` hook, Dear ImGui menu bar) | The same on the D3D11 swap chain of the game. | Stage 3 |
| `mod_loader/src/game/proxy` (proxy `version.dll`) | A proxy of the DLL that the research finds (see below). | Stage 3, after the research |
| `mod_loader/src/game/loader/Loader.cpp` | The start of the extender: after the activation wrapper decrypts the exe, find the signatures, start the hooks, load the mods. | Stage 3 |
| Mod zips, map overrides, custom objects, packs, the merges, `unitymesh` | Not ported. These are Unity data of Masters of Albion. Packages and tuning of The Sims 4 come later, if a mod needs them. | Not ported |
| (new) | The native Python module of the extender for script mods. | Stage 4 |

## Research before stage 3

- How the game loads the extender: the DLLs that `TS4_x64.exe` loads by name from `Game\Bin`, and which one a proxy
  can replace. In progress.
- A memory dump of the decrypted `TS4_x64.exe` for Ghidra. In progress.
- Which thread draws (the `Present` hook) and which thread runs the simulation and the game Python.
