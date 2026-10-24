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

## The choice: a proxy `version.dll`

1. **`version.dll`** (the choice). The decrypted exe and `python37_x64.dll` import it, so it loads before the D3D11
   device and before Python. It has 17 exports by name. The Masters of Albion loader uses the same mechanism, so the
   proxy stubs port directly.
2. `winmm.dll`: the same early time, but 181 exports.
3. `dxgi.dll` or `d3d11.dll`: direct access to the swap chain, but each export must forward, and they can conflict
   with the EA overlay (`igo64.dll`) and ReShade-style tools.
4. Not `winhttp.dll`: it loads with the activation DLL, before the decryption, in the network path of the DRM.

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
