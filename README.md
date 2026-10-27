# The Sims 4 mod loader

The script extender of The Sims 4: the DLL that the game loads. It loads the DLL mods and gives them hooks, a menu
bar, settings, hotkeys and inspector queries. The state of the port is in `docs/port.md`.

## Build

Requirements: Visual Studio with the C++ tools, and the vcpkg checkout of the workspace (`<workspace>\vcpkg`, where
the workspace is the folder that holds `projects`).

```bat
git submodule update --init
tools\preset.bat x64-release
```

A clone of a local repository needs `git -c protocol.file.allow=always submodule update --init`.

## Tests

`ctest --preset x64-release` runs the GoogleTest tests. `python tools\install.py --self-test` tests the install tool.
`python tools\check.py` does the format and lint checks of CI (the tool versions are in `tools\requirements-check.txt`).

## How to install

The install tool puts `winmm.dll` in `Game\Bin` of the game, and the DLL mods in `%LOCALAPPDATA%\Sims4Mods\dlls`. It
finds the game folder from `TS4_GAME` or from the registry. Stop the game first; only the user starts the game.

```bat
python tools\install.py status
python tools\install.py install
python tools\install.py mods add <dll>
python tools\install.py mods list
python tools\install.py mods remove <file>
python tools\install.py uninstall
python tools\install.py --self-test
```
