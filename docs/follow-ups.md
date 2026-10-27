# Open items

- **The quit from the main menu.** A quit from live mode runs the quit hook. A quit from the main menu wrote no quit
  line in 70 seconds. Find out if that quit calls the shutdown function of the application.
- **The probe of the game menu.** It is not ported. The menu bar shows only when the user pins it.
- **Messages on the screen.** The game messages are not joined. `showMessage` and the hotkey messages write only to
  the log.
- **The crash dump writer** is not joined.
- **The file names in `modDlls.disabled`** must match with no difference between upper case and lower case, as in
  Windows. The code still compares the exact text.
- **A scan after a hook.** A mod that scans the game code after the extender hooked a function finds the jump of the
  hook. A mod scans in `Ts4ModInit`, before the hooks go on. A better fix: the extender gives mods the address of a
  function that it found.
- **The native Python module** for script mods. A call from Python comes on the simulation thread. See
  [threads.md](threads.md).
- **Python function callbacks** in a later version of `Ts4ModApi.h`.

## Done

- The DLL mods load in the first tick, on the main thread (2026-10-26).
- With no `overlay.fontScale`, the text size comes from the window height (2026-10-26).
- `Ts4ModApi.h` names the main thread, the simulation thread and the render thread.
