# Decisions

These rules came from decisions of the merger (2026-10-25 and 2026-10-26). A reader must know them.

- **A proxy `winmm.dll`.** The game never loads a proxy `version.dll` from `Game\Bin`. The user approved
  `Game\Bin\winmm.dll` as the only file of ours in the game folder.
- **The DLL mods are not in the game folder.** They are in `%LOCALAPPDATA%\Sims4Mods\dlls`. `TS4_MODS_SETTINGS`
  moves only `mods.json` until a need for more occurs.
- **The DLL mods load in the first tick**, on the main thread, as `Ts4ModApi.h` tells. The exe is then decrypted.
- **`tickHook` stays**, for the heartbeat line in the log.
- **`mods.disabled` (by id) stays**, next to `modDlls.disabled` (by file name).
- **Two signatures on one address** give one hook. The join fails if the `parameterCount` values are different.
- **Callbacks get integers only**: up to 4 integer or pointer parameters and an integer result. A function with a
  float parameter or result needs an assembly thunk, later, if a mod needs it.
- **The menu bar has its own hooks** on the swap chain, not the hooks of the hook manager.
- **The bootstrap hook is a second hook owner** until the first ticks end. This is permitted, as for the menu bar.
- **The menu bar shows only when the user pins it**, with the action `toggleOverlay` (`Ctrl+Shift+F4`).
- **The decryption checks.** The start thread waits for an entropy below 7.5. Before a scan, common checks a limit of
  7.2. The code of 1.128.90.1030 has 6.48 in memory and 8.0 on disk.
