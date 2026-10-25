# How the game loads the extender

Build: The Sims 4 1.128.90.1030. `TS4_x64.exe` SHA-256 `916b852b187b9c41b9c2d77af55e0d5ed6aaf06909661397d7aa98f2e4db78ab`.
Research of 2026-10-25, static only (the game did not run). Scripts and output: `D:\sims_4\experiments\loader-research`,
`D:\sims_4\runs\2026-10-25-loader-research`.

## The real import table of the exe

The activation wrapper encrypts `.text` and `.data` of `TS4_x64.exe`, but not `.rdata` (entropy 5.19). The original
import descriptors are intact in `.rdata` at RVA `0x27e6140`: 38 DLLs. The PE data directory points only at the
import of `Core/Activation64.dll` by the `.ooa` stub. The stub resolves the real table after it decrypts the exe,
before the WinMain of the game.

DLLs of that table that are not KnownDLLs (42 entries in `\KnownDlls`), so Windows searches `Game\Bin` first:

| DLL | Functions that the exe uses | Exports of the system DLL |
|---|---|---|
| `VERSION.dll` | `VerQueryValueA`, `GetFileVersionInfoA`, `GetFileVersionInfoSizeA` | 17 (2 forward to KERNEL32) |
| `WINMM.dll` | `timeBeginPeriod`, `timeEndPeriod`, `timeGetTime` | 181 (one by ordinal only) |
| `d3d11.dll` | `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain` | 51 |
| `dxgi.dll` | `CreateDXGIFactory1` | 19 |
| `DSOUND.dll`, `HID.DLL`, `dbghelp.dll`, `WININET.dll`, `IPHLPAPI.DLL` | a few each | |

- `Simulation_x64.dll` is not in the table: the exe loads it with `LoadLibraryW`. It imports `WINMM.dll`
  (`timeGetTime`) and `python37_x64.dll`.
- `python37_x64.dll` imports `VERSION.dll`.
- `Core\Activation64.dll` imports `WINHTTP.dll`.
- No module of the game calls `SetDefaultDllDirectories`, `SetDllDirectory`, `AddDllDirectory` or uses
  `LOAD_LIBRARY_SEARCH`, and `DependentLoadFlags` is 0.
- No ASLR: `TS4_x64.exe` has the fixed base `0x140000000`, `Simulation_x64.dll` `0xF000000`, `python37_x64.dll`
  `0x1E000000`. The code still uses signatures, never addresses.

## The choice: a proxy `version.dll`, then `winmm.dll`

The game never loads a proxy `version.dll` (run of stage 3a below). The extender is now a proxy `winmm.dll` (stage 3b
below), which the game process loads from `Game\Bin` after the decryption.

1. **`version.dll`** (the first choice; the run with the proxy below shows that the game never loads it). The decrypted exe and `python37_x64.dll` import it, so it loads before the D3D11
   device and before Python. It has 17 exports by name. The Masters of Albion loader uses the same mechanism, so the
   proxy stubs port directly.
2. `winmm.dll`: the same early time, but 181 exports.
3. `dxgi.dll` or `d3d11.dll`: direct access to the swap chain, but each export must forward, and they can conflict
   with the EA overlay (`igo64.dll`) and ReShade-style tools.
4. Not `winhttp.dll`: it loads with the activation DLL, before the decryption, in the network path of the DRM.

## The game run of 2026-10-25

One start of `Game\Bin\TS4_x64.exe` with the EA app open, to the main menu, no save. Output:
`D:\sims_4\runs\2026-10-25-loader-research`.

- **Two processes.** The first `TS4_x64.exe` loads `Core\Activation64.dll` and its network DLLs, then stops after
  approximately 0.2 seconds (exit code 100010). The EA app then starts the real game process. In the first process,
  system DLLs load `version.dll` before the decryption. A proxy in `Game\Bin` will thus also load in that process. The
  extender must do nothing there: it forwards the exports, and it starts only when the decrypted exe is ready.
- **The load order in the game process** (141 modules): `TS4_x64.exe`, `Core\Activation64.dll`, ... `version.dll`
  (index 42, System32 now), `d3d11.dll` (65), `dxgi.dll` (66), `WINMM.dll` (70), the EA overlay `IGO64.dll` (79), the
  graphics driver (98), `Simulation_x64.dll` (115, `Game\Bin`), `python37_x64.dll` (116, `Game\Bin`),
  `DiscordHook64.dll` (139). The real import table of the exe points into `version.dll` (3 entries) and `WINMM.dll`
  (3). Thus `version.dll` loads after the decryption and before the D3D11 device and Python. This supports the proxy
  `version.dll`.
- **No fixed base in practice.** `Simulation_x64.dll` loaded at `0x38FC0000`, not at its base `0xF000000`.
- **Decryption.** In memory, `.text` has an entropy of 6.48 (8.0 on disk) and `.data` 4.09 (8.0 on disk). The dump
  `D:\sims_4\dump\TS4_1.128.90.1030\TS4_x64.image.bin` passes the checks of the Ghidra import.
- **Other swap-chain hooks.** The EA overlay (`IGO64.dll`) and the Discord overlay (`DiscordHook64.dll`) hook the swap
  chain too. The `Present` hook of the menu bar must work with them.
- **The window.** Title `The Sims™ 4`, class `Canvas-<hex>`; the class changes at each start. Find the window by the
  process and the title, not by the class.
- 159 threads at the main menu.

## The game run with the proxy (2026-10-25, stage 3a)

One start with `install.py install` and `install.py launch`, to the main menu, no save. Output:
`D:\sims_4\runs\2026-10-25-loader-proxy`. The proxy `version.dll` (extender 0.1.0, SHA-256
`93dbb71b5b6175d927eea216ca3f1c7586fb1d65c9ea8382957dcebcd797157e`) was in `Game\Bin`.

- **The game never loaded the proxy.** No process wrote `loader.log`: not the stub, not the game process. The game
  process had `C:\WINDOWS\system32\version.dll` loaded, and no `version.dll` from `Game\Bin`.
- **Why.** In both processes a system DLL loads `version.dll` before the activation wrapper resolves the real import
  table of the exe. The Windows loader then uses the module that is already there (it matches by name):
  - Game process: `version.dll` (a lower-case System32 path) loads right after `amsi.dll`, `USERENV.dll` and the
    Defender AMSI provider `MpOav.dll`. `MpOav.dll` does not import it, so it probably loads it with `LoadLibraryEx`
    and a System32-only search (not proved with a debugger).
  - Stub process (research run): `VERSION.dll` loads from System32 right after `ieframe.dll`. COM loads `ieframe.dll`
    with the altered search path (its own folder, then System32), so its imports come from System32, not `Game\Bin`.
- So the import tables gave a false search order for `version.dll`: the real import table does not load it; it finds
  it already loaded.
- The game ran normally with the file in place: main menu, no new exception log.
- After the run, `install.py uninstall` removed the file, because it does nothing.

### What follows for the choice of the DLL

- `version.dll` cannot load the extender on this computer while AMSI (Defender) and COM load it first. Another
  antivirus can change this, so it is not reliable either way.
- The next candidate must load first through the exe or a game DLL, never through a system DLL before the
  decryption. From the module list of the game process: `WINMM.dll` (index 70, after the decryption; the exe and
  `Simulation_x64.dll` import it), `dxgi.dll` and `d3d11.dll` (65, 66), `DSOUND.dll`, `HID.DLL`, `dbghelp.dll`.
  Each one needs a run that shows its path and the module that loaded it first, and the user's approval for a new
  file in `Game\Bin` (CLAUDE.md allows only `version.dll`).
- A cheap check before that run: start the game with no proxy and record, for each candidate, which module loaded it
  first and from which folder (a DLL load event trace, or `ts4_dump.py watch` extended with the loading module).

## Risks and open points

- The early load and the search of `Game\Bin` come from the import tables. A game run must prove them: the time that
  each module loads and its path (`ts4_dump.py watch`), then a proxy in `Game\Bin` (needs `install.py` and the
  user's approval).
- The exe exports `GetDenuvoTicketLocation` and `GetDenuvoTimeTicketRequest`. The wrapper can have more checks.
- The EA app keeps a CRC map of the shipped files (`C:\ProgramData\EA Desktop\InstallData\The Sims 4\...\map.eacrc`).
  It has no `version.dll`. Files that are not in the map stayed through earlier updates and repairs, but a repair or
  a new install can remove our DLL. `install.py status` must show it.
- The EA app has updates on (`autoUpdateEnabled="1"`, `treatUpdatesAsMandatory="1"`) and a pending update of itself.
  An update of the game changes the version (skill `ts4-game-builds`).

## The game runs with the proxy winmm.dll (2026-10-25, stage 3b)

The user approved `Game\Bin\winmm.dll`. The proxy forwards all 181 exports of the System32 `winmm.dll` (ordinal 2 has
no name), made by `tools/proxy_exports.py` from the export directory of the system DLL; a test compares the names and
the ordinals of the build with the system DLL. Three starts to the main menu, no save. Output:
`D:\sims_4\runs\2026-10-25-loader-winmm\run1`, `run2`, `run3`.

### Run 1: the load (passive proxy)

- **The game process loads our DLL.** `proxy: Windows loaded F:\Origin Games\The Sims 4\Game\Bin\WINMM.dll 602 ms after
  the start of the process 33016 (TS4_x64.exe); .text entropy 6.481 now`. The module list at the main menu has both
  `Game\Bin\WINMM.dll` (ours) and `C:\WINDOWS\system32\winmm.dll` (the real one, loaded by the proxy).
- **The load comes after the decryption.** The first sample of the `.text` entropy in our start thread is already 6.481
  (encrypted: 8.0). So the exe loads `WINMM.dll` when the activation wrapper resolves the real import table.
- **The stub process does not load it.** No line of the stub process (the first `TS4_x64.exe`) is in `loader.log`.
- `Simulation_x64.dll` and `python37_x64.dll` loaded approximately 4.9 s after the start of the process.

### The tick of the main thread (tree `TS4_x64_1.128.90.1030`)

| RVA | Proposed name | What |
|---|---|---|
| `0x0006A290` | `ClientMain` | Makes the `AppInstance`, initializes it, runs the main loop, then the shutdown. |
| `0x0006E4C0` | `AppInstance::Run` | The main loop: window messages, the game clock (`DAT_02921820`), the frame, then `0x01637D20`. |
| `0x01637D20` | `ClientBase::ServiceManager::UpdateServices` | Calls `vfunc_9` (Update) of each enabled client service with the clock. 4 integer parameters: the manager, the clock, two flags. **The tick of the extender.** |
| `0x0006E080` | `AppInstance::Shutdown` | The first step of the shutdown after the main loop. 1 parameter. **The quit hook of the extender.** |
| `0x000E66B0` | `ClientHostedGameService::vfunc_9` | A service Update: calls `0x01767D90`, which calls vfunc 3 of the simulation server object that `Simulation_x64.dll` `dllInit` returns. Game Python `c_api_server_tick` (`simulation/areaserver.py`) is index 3 of the callbacks that `Simulation_x64` `0x0006F850` binds. |
| `0x01765940` | `HostedAreaServer::LoadSimulation` | Loads `Simulation_x64.dll` and gets `dllInit`, `dllShutdown`, ... |

The update of the services runs one time in each frame of the main loop, and also from `AppInstance::vfunc_1`
(flags 1, 0) and the shutdown (flags 1, 0). The signatures (`src/game/loader/TickHook.h`) come from `ghcat.py sig`.

**The thread proof (Python-free).** The extender records, with the DLL notification of Windows
(`LdrRegisterDllNotification`), the thread that loads `Simulation_x64.dll`; that thread runs `HostedAreaServer`, which
calls `dllInit`, and the interpreter of the game Python starts there. The tick logs its thread every 600 calls. Run 2
and run 3: `tick: call 600 on thread 45008; Simulation_x64.dll loaded on thread 45008 at tick count 36028328 (the same
thread)`, every line from call 1 to call 6000. The static chain above (service update, then the hosted game service,
then the simulation server tick) shows that the game Python tick runs inside this call on the same thread.
Approximately 180 ticks in each second at the main menu.

### The start of the extender on the main thread

The hook manager of common must start on the main thread, which the extender does not know before the first tick.
Thus the start thread puts a one-time bootstrap hook (own MinHook) on the service update. Its first call, after the
original function, makes the hook manager on that thread, hooks the shutdown, puts the hooks of the DLL mods on their
functions (it waits until `Simulation_x64.dll` loads if a mod needs it), and then removes itself. The hook manager then
puts its own hook on the same function, and runs the work of each tick from it. Run 3:
`tick: the first tick runs on thread 48384`, `hook: installed on the update of the client services`,
`tick: the hook manager runs the tick from now on (after 1 ticks of the bootstrap hook)`.

### The swap chain: the game presents with Present1

Run 2 (hooks on `Present` and `ResizeBuffers` only): no `Present` call came through the hooks, and the menu bar did not
show. A read of the game memory at the main menu showed that `DiscordHook64.dll` hooks `Present`, `ResizeBuffers`,
`Present1`, `ResizeBuffers1` and three more functions of the `IDXGISwapChain` vtable in `dxgi.dll` (it puts its jump on
top of ours, and its trampoline calls our jump). The game presents with `IDXGISwapChain1::Present1`. With hooks on
`Present1` (slot 22) and `ResizeBuffers1` (slot 39) too, run 3 logged
`menu bar: the first Present came through the hook on thread 33668` (the render thread), and the menu bar showed with
the hotkey. The EA overlay (`IGO64.dll`) is loaded at the install of the menu bar; `DiscordHook64.dll` loads later and
hooks on top of ours (one warning line for each function); no error.

### Open points

- The quit hook was not seen in a run: each run stopped the process at the main menu, and `WM_CLOSE` does not quit the
  game from the main menu.
- The DLL mods start on the start thread (as in Masters of Albion), not on the main thread.
