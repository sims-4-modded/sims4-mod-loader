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
| `mod_loader/src/game/proxy` (proxy `version.dll`) | A proxy `winmm.dll` (`ts4_mod_loader`, OUTPUT_NAME `winmm`): all 181 exports of the System32 `winmm.dll` with their names and ordinals (ordinal 2 has no name), made by `tools/proxy_exports.py` from the export directory of the system DLL, and the marker export `Ts4ModLoaderVersion`. The start thread waits for the decryption (`core/GameStart.h`), `loader.log`, and `tools/install.py` (only `winmm.dll`). | Stage 3a (`version.dll`): the game never loads it. Stage 3b (branch `loader/winmm-extender`): the game process loads `Game\Bin\WINMM.dll` after the decryption; the stub process does not load it (`docs/research/loader/load.md`). |
| `mod_loader/src/game/loader/Loader.cpp` | `Loader.cpp` (DllMain, the proxy, the start thread) and `Extender.cpp` (the port of the start of Masters of Albion): mods.json (a file with the defaults if there is none), the start-up guard, the DLL mods through `ModDllLoader` and `HostTable`, the hotkeys, the menu bar, the queries `mods.status`, `mods.list`, `mods.settings`, `mods.reload`, `hooks.status`, `overlay.pin`, `overlay.edit`, `overlay.probe`. `TickHook.cpp`: the bootstrap hook on the tick. | Stage 3b, tested to the main menu with the template mod (branch `loader/winmm-extender`). |
| Mod zips, map overrides, custom objects, packs, the merges, `unitymesh` | Not ported. These are Unity data of Masters of Albion. Packages and tuning of The Sims 4 come later, if a mod needs them. | Not ported |
| (new) | The native Python module of the extender for script mods. | Stage 4 |

## Research before stage 3

- How the game loads the extender: a proxy `winmm.dll` (done in stage 3b; `version.dll` failed, `docs/research/loader/load.md`). A
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
| `tickHook` | `true`: one heartbeat line in the log in each 3600 ticks of the main thread. After a restart. The log always tells the thread of the tick in each 600 ticks of the first 6000. | `frameHook` (the hook on `GameManager.Update`). The native tick is the update of the client services (stage 3b, below). |
| `liveReload` | Monitor `mods.json` and read the hotkeys while the game runs. After a restart. | The same. |
| `hotkeys` | `enabled`, `messages`, `messageSeconds`, and the hotkey of each action by its name. | The same. |
| `overlay` | `enabled` (after a restart), `fontScale`, `pinned` (show the menu bar at all times; live). | `enabled` and `fontScale`. `pinned` is new: the extender cannot tell yet when the menu of the game is open. |
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
- `tickHook` stays. Stage 3b found the native tick of the main thread in Ghidra and hooks it by signature (below).
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

## Stage 3b: the proxy winmm.dll and the start of the extender

- **The tick** is the update of all client services of the game (`TickHook.h`, `k_ServiceUpdateSignature`; tree
  `TS4_x64_1.128.90.1030` RVA `0x01637D20`). The main loop calls it one time in each frame, also at the main menu and on
  the load screens, on the main thread. The service that runs the simulation (and the game Python tick) runs inside
  it. The log proves the thread: the tick runs on the thread that loaded `Simulation_x64.dll`.
- **The main thread is the thread of the first tick.** The start thread puts a bootstrap hook (own MinHook) on the
  tick. The first tick makes the hook manager on its thread, hooks the shutdown of the application (the quit hook),
  puts the hooks of the DLL mods on their functions (one unique scan in the named module; the keys that find one
  address join into one hook, `JoinResolvedFunctions`), and removes the bootstrap hook. The hook manager then takes the
  tick, also with the callbacks of the mods on the tick itself.
- **Each tick** runs the frame callbacks of the extender (`FrameCallbacks`): the hotkeys (`HotkeyInput`), the menu bar
  (`Overlay::Tick`, `Publish` each 15 visible frames), the late steps of the start (`StartupSequence`: the menu bar and
  the monitor of mods.json at tick 120, the removal of the marker at tick 720) and the tasks (`MainThreadTasks`).
- **The quit hook** (`k_AppShutdownSignature`, RVA `0x0006E080`) stops the DLL mods (`ShutdownAll`), the menu bar
  (`Shutdown`) and the monitor of mods.json, and removes the marker. Not yet seen in a run.
- **The menu bar** hooks `Present1` and `ResizeBuffers1` too: the game presents with `Present1`. The hotkey of the
  action `toggleOverlay` is `Ctrl+Shift+F4`, the default of Masters of Albion (also `reloadSettings` `Ctrl+Shift+F10`
  and `showSettings` `Ctrl+Shift+F11`). The extender has no messages on the screen yet: `showMessage` writes the log.
- **Imports** of `winmm.dll`: KERNEL32 only; `user32`, `gdi32`, `shell32`, `ole32`, `dwmapi`, `d3dcompiler_47` and
  `api-ms-win-core-synch-l1-2-0` load at the first call.
- **Not ported:** the game messages (`GameMessages`), the probe of the menu of the game (`GameMenuProbe`), the crash
  dump writer, the mod zips, the objects and the maps.
