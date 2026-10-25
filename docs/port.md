# The port of the Masters of Albion mod loader

The mod loader of The Sims 4 is a mirror of the logic of the Masters of Albion mod loader
(`D:\masters_of_albion\projects\mod_loader` and its `common`). The logic is the same; the parts that touch the game
are different. This file gives each part and its state.

## What stays, what changes, what goes

| Masters of Albion part | The Sims 4 part | State |
|---|---|---|
| `common/src/mod_api/ModApi.h` | `common/src/mod_api/Ts4ModApi.h`: the same interface. Native function callbacks (named byte signatures) replace the managed method callbacks. Python function callbacks come later. | Stage 1, done |
| `common/src/mod_core` (tasks, frame callbacks, hotkeys, settings fields and file, slot swap, start-up guard, crash dump rules, folders) | The same, plus the signature parser and matcher. `UnityMath.h` goes. | Stage 1, done |
| `common/src/mod_sdk` (`ModHost`, `BindFields`, `HostLogSink`) | The same, for `Ts4ModApi.h`. | Stage 1, done |
| `common/src/mod_game` (IL2CPP, managed objects, method hooks, hotkey input, game messages, crash dumps) | Hooks by signature (MinHook), the hotkey input, the game messages, crash dumps, and the Python 3.7 C API of the game. IL2CPP and managed objects go. | Stage 2 |
| `mod_loader/src/core` (host table, loader settings, mod registry, mod catalogue, hook rules, overlay rules) | The same (`ts4_loader_core`), with the changes in "Stage 2: the core" below. | Stage 2, done (branch `loader/core`) |
| `mod_loader/src/dlls/ModDllLoader` (the load and the checks of the mod DLLs) | The same (`ts4_loader_dlls`): the exports, the interface version, the ids, the guard after an access violation, the SHA-256 checksum of each DLL, the list of DLLs for the start-up guard. | Stage 2, done (branch `loader/core`) |
| `mod_loader/src/game/overlay` (DXGI `Present` hook, Dear ImGui menu bar) | The same on the D3D11 swap chain of the game (`ts4_loader_overlay`, `src/game/overlay`; the menus from the registry in `core/OverlayMenus`). D3D12 is not ported. The window is found by the process and the title. | Stage 3, done (branch `loader/overlay`); the game test comes with the extender |
| `mod_loader/src/game/proxy` (proxy `version.dll`) | The same proxy `version.dll` (`docs/research/loader/load.md`). | Stage 3; a game run must prove it |
| `mod_loader/src/game/loader/Loader.cpp` | The start of the extender: after the activation wrapper decrypts the exe, find the signatures, start the hooks, load the mods. | Stage 3 |
| Mod zips, map overrides, custom objects, packs, the merges, `unitymesh` | Not ported. These are Unity data of Masters of Albion. Packages and tuning of The Sims 4 come later, if a mod needs them. | Not ported |
| (new) | The native Python module of the extender for script mods. | Stage 4 |

## Research before stage 3

- How the game loads the extender: a proxy `version.dll` (static research done, `docs/research/loader/load.md`). A
  game run must show the load time and path of each module.
- A memory dump of the decrypted `TS4_x64.exe` for Ghidra. The dumper is ready
  (`D:\sims_4\experiments\loader-research\ts4_dump.py`); it needs one game start, and the EA app must run.
- Which thread draws (the `Present` hook) and which thread runs the simulation and the game Python.

## Stage 2: the core

### The folder of the DLL mods

The DLL mods are in `%LOCALAPPDATA%\Sims4Mods\dlls`: the folder `dlls` in the settings folder of `MakeModFolders`
(common `mod_core/Folders.h`), next to `mods.json` and `logs` (`mods::ModDllFolder`, `mods::k_DllFolderName`). The
environment variable `TS4_MODS_SETTINGS` moves `mods.json`; the folder of the DLL mods stays in the settings folder.
Masters of Albion loads its mod DLLs from `<game>\mods`. This port does not, because the game folder
`F:\Origin Games\The Sims 4` is read only: the only file of ours there is the loader DLL (`docs/research/loader/load.md`).

### The keys of mods.json

The extender owns the top-level keys `tickHook`, `liveReload`, `hotkeys`, `overlay`, `mods` and `modDlls`
(`loader::k_LoaderKeys`). A mod cannot register a settings section with one of these names.

| Key | Values | Masters of Albion |
|---|---|---|
| `tickHook` | `true`: one heartbeat line in the log in each 3600 ticks of the main thread. After a restart. | `frameHook` (the hook on `GameManager.Update`). The native tick is a hook by signature in stage 3. |
| `liveReload` | Monitor `mods.json` and read the hotkeys while the game runs. After a restart. | The same. |
| `hotkeys` | `enabled`, `messages`, `messageSeconds`, and the hotkey of each action by its name. | The same. |
| `overlay` | `enabled` (after a restart), `fontScale`. | The same. |
| `mods` | `disabled`: the ids of the DLL mods that are off. The extender reads the id (`Ts4ModGetInfo`), then unloads the DLL before `Ts4ModInit`. After a restart. | `mods` was the list of the mod zips (`disabled`, `map`). The zips are not ported, so `mods` names the DLL mods by id. |
| `modDlls` | `disabled`: the file names of the DLLs that do not load. The start-up guard adds a DLL here after a start that did not complete. After a restart. | The same. |

The keys `objects` and `maps` and the key `mods.map` are not ported.

### What changed

- **Function callbacks** (`core/HookRules.h`): a callback names a native function by its module (`TS4_x64.exe` or
  `Simulation_x64.dll`) and the signature text of its first bytes, in place of a managed method. The key of a
  function is the module and the normal form of the signature (`Signature::ToString`), so two texts of one signature
  name one function. All callbacks on one function must have the same `parameterCount` (0 to 4), because the function
  has one hook. Masters of Albion compared the parameter types of the method.
- **SHA-256** (`core/Sha256.h`): Masters of Albion has no checksum of a mod DLL. This port has a small SHA-256
  (FIPS 180-4, tested with the NIST vectors) in the core. The DLL loader reads the checksum of each DLL before the
  load, also of a DLL that is off; the log and the list of the DLL mods (`mods::DescribeModDll`) give it.
- **The mod catalogue** (`core/ModCatalogue.h`): only the parts for DLL mods. It has the record of each DLL (file,
  checksum, state, the reason that it is off, id, name, version), the switches by file name and by id, and the rules
  of the start-up guard for the DLLs (the feature `dll:<file>`, and `modDlls.disabled` after a start that did not
  complete). Masters of Albion has these rules in `Loader.cpp`; here they are in the core, with tests.
- **The DLL loader** owns each DLL handle (RAII). When the loader goes out of scope, it calls `Ts4ModShutdown`,
  removes the registrations and unloads the DLLs. A DLL whose code can still be in use (an access violation) stays in
  memory, as in Masters of Albion.
- **Not ported:** `Pack*`, `KnackMerge`, `CarriableMerge`, `TypedMerge`, `TemplatePlan`, `ResourceLoads`,
  `MapOverrideSet`, `unitymesh` and the mod zips (and their tests). `src/game` comes in stage 3.

### Decisions of the merger for stage 3

- The extender compares the file names in `modDlls.disabled` with no difference between upper case and lower case,
  because Windows does the same. (Masters of Albion compares the exact text.) Change it in stage 3.
- Two signatures can match one address. After the scan, the extender joins the callbacks of all keys that resolve to
  one address into one hook, and refuses the join if the `parameterCount` values are different.
- `tickHook` stays. Stage 3 finds the native tick of the main thread in Ghidra and hooks it by signature.
- `mods.disabled` (by mod id) stays. `TS4_MODS_SETTINGS` moves only `mods.json`; the folder of the DLL mods stays in
  the settings folder until a need for more occurs.
- The limit of `CheckDecrypted` in common (7.2 bits for each byte) is correct for 1.128.90.1030: the `.text` of the
  exe has an entropy of 6.48 in memory after the decryption and 8.0 on disk (`docs/research/loader/load.md`).
- The hook manager of common calls callbacks only on the main thread. The extender makes it on the main thread of
  the game, or gives it `mainThreadId`. Callbacks read up to 4 integer or pointer parameters and an integer result;
  a function with a float parameter or result needs an assembly thunk, later, if a mod needs it.
- The overlay hooks `Present` and `ResizeBuffers` with its own MinHook hooks, not with the hook manager: the game can
  call `Present` on a thread that is not the main thread, and the hooks must work with the hooks of the EA overlay and
  the Discord overlay. The slots of `IDXGISwapChain` are fixed by the COM interface, not by the game, so they are
  permitted in code.
- The menu bar shows only when the user pins it. The extender adds the action `toggleOverlay` with a default hotkey.
