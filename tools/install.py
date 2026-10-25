#!/usr/bin/env python3
# ******************************************************************************
# Copyright (c) 2026 Sims 4 mods developers
# ******************************************************************************
"""Put the extender (winmm.dll) in the game, remove it, show its state, start the game, and add DLL mods.

The extender is one file in the game folder: Game\\Bin\\winmm.dll. The user approved only this file name. The game
folder is read only for everything else: the script writes and removes no other file there, and it refuses every other
name (for example version.dll, which the game never loads from Game\\Bin). The script refuses to replace or remove a
winmm.dll that is not the extender. It knows the extender by the export name Ts4ModLoaderVersion in the file, and it keeps the SHA-256 of
each file that it installed in %LOCALAPPDATA%\\Sims4Mods\\install-state.json. It refuses to change a file while
TS4_x64.exe runs, because Windows locks a loaded DLL.

The DLL mods go into %LOCALAPPDATA%\\Sims4Mods\\dlls. The script backs up a DLL that it did not put there before it
replaces or removes it (%LOCALAPPDATA%\\Sims4Mods\\backups).

    python tools/install.py status
    python tools/install.py install [--dll build/x64-release/winmm.dll]
    python tools/install.py uninstall
    python tools/install.py launch                 (the EA app must run)
    python tools/install.py mods list
    python tools/install.py mods add <dll>         (copies the DLL to the folder of the DLL mods)
    python tools/install.py mods remove <file name>
    python tools/install.py --self-test
"""

import argparse
import dataclasses
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Optional

GAME = Path(r"F:\Origin Games\The Sims 4")
LOADER_NAME = "winmm.dll"
TARGET_IN_GAME = Path("Game") / "Bin" / LOADER_NAME
GAME_EXE_IN_GAME = Path("Game") / "Bin" / "TS4_x64.exe"
GAME_EXE_NAME = "TS4_x64.exe"
EA_APP_NAME = "EADesktop.exe"
DEFAULT_DLL = Path(__file__).resolve().parents[1] / "build" / "x64-release" / LOADER_NAME
LOADER_MARKER = b"Ts4ModLoaderVersion"
# A DLL mod exports these functions, so their names are in the file.
MOD_MARKERS = (b"Ts4ModApiVersion", b"Ts4ModGetInfo", b"Ts4ModInit", b"Ts4ModShutdown")
GAME_BUILDS = Path(r"D:\sims_4\projects\ts4_research\tools\game_builds.py")
LOG_LINES = 25


def local_app_data() -> Path:
    value = os.environ.get("LOCALAPPDATA")
    if not value:
        sys.exit("LOCALAPPDATA is not set")
    return Path(value)


def documents() -> Path:
    """Return the Documents folder of the user. The game keeps its user folder there."""
    try:
        import ctypes
        from ctypes import wintypes

        buffer = ctypes.create_unicode_buffer(wintypes.MAX_PATH)
        # 5 is CSIDL_PERSONAL, the Documents folder, also when the user moved it.
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buffer) == 0:
            return Path(buffer.value)
    except (OSError, AttributeError, ImportError):
        pass
    return Path.home() / "Documents"


@dataclasses.dataclass
class Places:
    """The folders that the script uses. The self-test gives a fake game and fake folders."""

    game: Path
    settings: Path
    user: Path
    game_builds: Optional[Path]
    running: Callable[[str], bool]

    @property
    def target(self) -> Path:
        return self.game / TARGET_IN_GAME

    @property
    def state_file(self) -> Path:
        return self.settings / "install-state.json"

    @property
    def dlls(self) -> Path:
        return self.settings / "dlls"

    @property
    def backups(self) -> Path:
        return self.settings / "backups"

    @property
    def log(self) -> Path:
        return self.settings / "logs" / "loader.log"


def process_runs(name: str) -> bool:
    """Return True if a process with this image name runs."""
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH"], capture_output=True, text=True,
                             timeout=30)
    except (OSError, subprocess.SubprocessError):
        return False
    return name.lower() in out.stdout.lower()


def real_places() -> Places:
    return Places(game=GAME, settings=local_app_data() / "Sims4Mods",
                  user=documents() / "Electronic Arts" / "The Sims 4", game_builds=GAME_BUILDS, running=process_runs)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def is_loader(path: Path) -> bool:
    data = path.read_bytes()
    return data[:2] == b"MZ" and LOADER_MARKER in data


def is_mod_dll(path: Path) -> bool:
    data = path.read_bytes()
    return data[:2] == b"MZ" and all(marker in data for marker in MOD_MARKERS)


def load_state(places: Places) -> dict:
    try:
        data = json.loads(places.state_file.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_state(places: Places, data: dict) -> None:
    places.state_file.parent.mkdir(parents=True, exist_ok=True)
    places.state_file.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def now_text() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# --- The extender in the game folder ----------------------------------------------------------------------------------


def checked_target(places: Places, target: Optional[Path] = None) -> Path:
    """Return the one path in the game folder that the script writes. Refuse every other path."""
    target = places.target if target is None else target
    if not (places.game / GAME_EXE_IN_GAME).is_file():
        sys.exit(f"{places.game} has no {GAME_EXE_IN_GAME}")
    if os.path.normcase(os.path.abspath(target)) != os.path.normcase(os.path.abspath(places.target)):
        sys.exit(f"{target} is not {places.target}; the script writes no other file in the game folder")
    if target.is_symlink() or target.parent.is_symlink():
        sys.exit(f"{target} is a link; the script writes only a plain file")
    return target


def refuse_while_game_runs(places: Places) -> None:
    if places.running(GAME_EXE_NAME):
        sys.exit(f"{GAME_EXE_NAME} runs: stop the game at the main menu first, then run the command again")


def is_ours(places: Places, path: Path) -> bool:
    """Return True if the winmm.dll is the extender: the marker is in the file, or the script installed this file."""
    if not path.is_file():
        return False
    recorded = load_state(places).get("loader", {}).get("sha256")
    return is_loader(path) or recorded == sha256(path)


def install(places: Places, dll: Path, target: Optional[Path] = None) -> int:
    target = checked_target(places, target)
    refuse_while_game_runs(places)
    if dll.name.lower() != LOADER_NAME:
        sys.exit(f"{dll} is not named {LOADER_NAME}; the user approved only {LOADER_NAME} in the game folder")
    if not dll.is_file() or not is_loader(dll):
        sys.exit(f"{dll} is not a build of the extender: it has no export {LOADER_MARKER.decode()}")
    if target.exists() and not is_ours(places, target):
        sys.exit(f"{target} exists and is not the extender; the script will not replace it")
    shutil.copyfile(dll, target)
    digest = sha256(target)
    data = load_state(places)
    data["loader"] = {"path": str(target), "sha256": digest, "source": str(dll), "installedAt": now_text()}
    save_state(places, data)
    print(f"installed {dll} -> {target} sha256 {digest}")
    return 0


def uninstall(places: Places) -> int:
    target = checked_target(places)
    refuse_while_game_runs(places)
    if not target.exists():
        print(f"nothing to remove: {target} does not exist")
        return 0
    if not is_ours(places, target):
        sys.exit(f"{target} is not the extender; the script will not remove it")
    target.unlink()
    data = load_state(places)
    data.pop("loader", None)
    save_state(places, data)
    print(f"removed {target}")
    return 0


def check_game_version(places: Places) -> int:
    """Run game_builds.py check --quick. Show a warning if the game changed since the last record."""
    if places.game_builds is None or not places.game_builds.is_file():
        print("game version: game_builds.py is not there; no check")
        return 0
    try:
        out = subprocess.run([sys.executable, "-I", str(places.game_builds), "check", "--quick"], capture_output=True,
                             text=True, timeout=300, cwd=str(places.game_builds.parent.parent))
    except (OSError, subprocess.SubprocessError) as error:
        print(f"game version: WARNING: the check did not run: {error}")
        return 1
    for line in (out.stdout + out.stderr).splitlines():
        print(f"game version: {line}")
    if out.returncode == 1:
        print("game version: WARNING: THE GAME CHANGED since the last record. Record it (game_builds.py record) and "
              "check the extender before a test.")
    elif out.returncode == 2:
        print("game version: WARNING: the packs changed since the last record.")
    return out.returncode


def read_options(places: Places) -> dict:
    """Return the values of the two lines of Options.ini that turn mods on."""
    values = {}
    try:
        for line in (places.user / "Options.ini").read_text(encoding="utf-8", errors="replace").splitlines():
            key, _, value = line.partition("=")
            if key.strip().lower() in ("modsdisabled", "scriptmodsenabled"):
                values[key.strip().lower()] = value.strip()
    except OSError:
        pass
    return values


def show_options(places: Places) -> bool:
    values = read_options(places)
    good = values.get("modsdisabled") == "0" and values.get("scriptmodsenabled") == "1"
    text = ", ".join(f"{key} = {value}" for key, value in sorted(values.items())) or "no mod lines"
    print(f"Options.ini: {text}" + ("" if good else " (WARNING: mods are off; only the user changes Options.ini)"))
    return good


def show_log(places: Places, lines: int = LOG_LINES) -> None:
    if not places.log.is_file():
        print(f"loader.log: {places.log} does not exist")
        return
    text = places.log.read_text(encoding="utf-8", errors="replace").splitlines()
    print(f"loader.log: the last {min(lines, len(text))} of {len(text)} lines of {places.log}")
    for line in text[-lines:]:
        print(f"  {line}")


def status(places: Places) -> int:
    target = places.target
    result = 0
    if not target.exists():
        print(f"extender: not installed ({target} does not exist)")
    elif is_ours(places, target):
        digest = sha256(target)
        recorded = load_state(places).get("loader", {})
        same = "as installed" if recorded.get("sha256") == digest else "NOT the file that the script installed"
        print(f"extender: installed, ours: {target} sha256 {digest} ({same}"
              + (f", {recorded.get('installedAt')})" if recorded else ")"))
    else:
        print(f"extender: FOREIGN: {target} is not the extender; the script will not touch it. sha256 {sha256(target)}")
        result = 1
    print(f"game: {GAME_EXE_NAME} {'runs' if places.running(GAME_EXE_NAME) else 'does not run'}, "
          f"{EA_APP_NAME} {'runs' if places.running(EA_APP_NAME) else 'does not run'}")
    check_game_version(places)
    show_options(places)
    mods_list(places)
    show_log(places)
    return result


def launch(places: Places, start: Optional[Callable[[Path], int]] = None) -> int:
    """Start the game as the research found: TS4_x64.exe in Game\\Bin, with the EA app open."""
    exe = places.game / GAME_EXE_IN_GAME
    if not exe.is_file():
        sys.exit(f"{exe} does not exist")
    if places.running(GAME_EXE_NAME):
        sys.exit(f"{GAME_EXE_NAME} runs already")
    if not places.running(EA_APP_NAME):
        sys.exit(f"{EA_APP_NAME} does not run: start the EA app and log in first")
    if not places.target.exists():
        print("extender: WARNING: not installed; the game starts with no extender")
    elif not is_ours(places, places.target):
        print(f"extender: WARNING: {places.target} is not the extender")
    else:
        print(f"extender: installed, sha256 {sha256(places.target)}")
    check_game_version(places)
    show_options(places)
    if start is None:
        def start(path: Path) -> int:
            flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
            process = subprocess.Popen([str(path)], cwd=str(path.parent), creationflags=flags, close_fds=True,
                                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
            return process.pid
    pid = start(exe)
    print(f"started {exe} (stub process {pid}); the EA app starts the game process. "
          "Stop the game only at the main menu.")
    return 0


# --- DLL mods ---------------------------------------------------------------------------------------------------------


def dll_folder(places: Places) -> Path:
    folder = places.dlls
    if folder.is_symlink() or (folder.exists() and not folder.is_dir()):
        sys.exit(f"{folder} is not a plain folder")
    return folder


def inside(folder: Path, path: Path) -> Path:
    """Return the path if it is directly in the folder. Stop the script if it is not."""
    resolved = path.resolve()
    if resolved.parent != folder.resolve():
        sys.exit(f"{path} is not in {folder}; the script writes nothing else")
    return resolved


def is_our_mod(places: Places, path: Path) -> bool:
    return path.is_file() and load_state(places).get("dlls", {}).get(path.name.lower()) == sha256(path)


def back_up(places: Places, path: Path) -> Path:
    """Copy a DLL that is not ours to the backup folder before the script replaces or removes it."""
    stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M%S-%f")
    target = places.backups / f"{stamp}-{path.name}"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, target)
    print(f"backed up {path} (not ours) -> {target} sha256 {sha256(target)}")
    return target


def mods_list(places: Places) -> int:
    folder = dll_folder(places)
    if not folder.exists():
        print(f"DLL mods: no folder {folder}")
        return 0
    dlls = sorted(path for path in folder.iterdir() if path.suffix.lower() == ".dll")
    if not dlls:
        print(f"DLL mods: {folder} has no DLL")
    for path in dlls:
        kind = "DLL mod" if is_mod_dll(path) else "NOT A DLL MOD"
        owner = "ours" if is_our_mod(places, path) else "not ours"
        print(f"DLL mods: {path.name}: {kind}, {path.stat().st_size} bytes, sha256 {sha256(path)}, {owner}")
    return 0


def mods_add(places: Places, source: Path) -> int:
    refuse_while_game_runs(places)
    folder = dll_folder(places)
    if not source.is_file():
        sys.exit(f"{source} is not a file")
    if source.suffix.lower() != ".dll" or not is_mod_dll(source):
        sys.exit(f"{source} is not a DLL mod: it must export {', '.join(m.decode() for m in MOD_MARKERS)}")
    if folder.exists() and source.resolve().parent == folder.resolve():
        sys.exit(f"{source} is already in {folder}")
    folder.mkdir(parents=True, exist_ok=True)
    target = inside(folder, folder / source.name)
    if target.exists() and not is_our_mod(places, target):
        back_up(places, target)
    shutil.copy2(source, target)
    data = load_state(places)
    data.setdefault("dlls", {})[target.name.lower()] = sha256(target)
    save_state(places, data)
    print(f"added {source} -> {target} sha256 {sha256(target)}")
    return 0


def mods_remove(places: Places, name: str) -> int:
    refuse_while_game_runs(places)
    folder = dll_folder(places)
    if not folder.exists():
        print("nothing to remove")
        return 0
    matches = [path for path in sorted(folder.iterdir())
               if path.suffix.lower() == ".dll" and path.name.lower() == name.lower()]
    if not matches:
        print(f"no DLL {name} in {folder}")
        return 0
    data = load_state(places)
    for path in matches:
        target = inside(folder, path)
        if not is_our_mod(places, target):
            back_up(places, target)
        target.unlink()
        data.get("dlls", {}).pop(target.name.lower(), None)
        print(f"removed {target}")
    save_state(places, data)
    return 0


# --- Self-test --------------------------------------------------------------------------------------------------------


def self_test() -> int:
    """Check all commands on a fake game folder in a temporary folder. The real game and folders stay as they are."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        game = root / "The Sims 4"
        (game / "Game" / "Bin").mkdir(parents=True)
        (game / GAME_EXE_IN_GAME).write_bytes(b"MZ exe")
        (game / "Game" / "Bin" / "Simulation_x64.dll").write_bytes(b"MZ game dll")
        user = root / "user"
        user.mkdir()
        (user / "Options.ini").write_text("[options]\nmodsdisabled = 0\nscriptmodsenabled = 1\n", encoding="utf-8")
        runs = set()
        places = Places(game=game, settings=root / "Sims4Mods", user=user, game_builds=None,
                        running=lambda name: name in runs)
        game_files = {path: path.read_bytes() for path in game.rglob("*") if path.is_file()}

        def refused(call) -> bool:
            try:
                call()
            except SystemExit:
                return True
            return False

        def fake_dll(path: Path, *markers: bytes) -> Path:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"MZ" + b"\0".join(markers) + b"\0" + path.name.encode())
            return path

        # The extender.
        loader = fake_dll(root / LOADER_NAME, LOADER_MARKER)
        assert install(places, loader) == 0 and is_loader(places.target)
        assert load_state(places)["loader"]["sha256"] == sha256(places.target)
        assert status(places) == 0
        # A new build replaces our file.
        loader.write_bytes(loader.read_bytes() + b"new build")
        assert install(places, loader) == 0 and places.target.read_bytes() == loader.read_bytes()
        assert uninstall(places) == 0 and not places.target.exists()
        assert "loader" not in load_state(places)
        assert uninstall(places) == 0, "a second uninstall does nothing"
        # A winmm.dll that is not ours stays.
        places.target.write_bytes(b"MZ other winmm.dll")
        assert refused(lambda: install(places, loader)), "a foreign winmm.dll was replaced"
        assert refused(lambda: uninstall(places)), "a foreign winmm.dll was removed"
        assert status(places) == 1
        assert places.target.read_bytes() == b"MZ other winmm.dll"
        places.target.unlink()
        # Only the extender goes into the game folder, and only to Game\Bin\winmm.dll.
        assert refused(lambda: install(places, fake_dll(root / "plain" / LOADER_NAME))), "not the extender went in"
        assert refused(lambda: install(places, loader, game / "Game" / "Bin" / "version.dll")), "version.dll was used"
        assert refused(lambda: install(places, loader, game / "Game" / "Bin" / "dxgi.dll")), "another name was used"
        assert refused(lambda: install(places, loader, game / LOADER_NAME)), "another path was used"
        # A build of the extender with another file name does not go in.
        assert refused(lambda: install(places, fake_dll(root / "version.dll", LOADER_MARKER))), "version.dll went in"
        assert not (game / "Game" / "Bin" / "version.dll").exists()
        assert refused(lambda: checked_target(dataclasses.replace(places, game=root / "nothing"))), "no game, no install"
        # Nothing changes while the game runs.
        runs.add(GAME_EXE_NAME)
        assert refused(lambda: install(places, loader)), "install while the game runs"
        assert not places.target.exists()
        runs.discard(GAME_EXE_NAME)
        assert install(places, loader) == 0
        runs.add(GAME_EXE_NAME)
        assert refused(lambda: uninstall(places)), "uninstall while the game runs"
        assert places.target.exists()
        runs.discard(GAME_EXE_NAME)
        # Without the record, the marker still tells that the file is ours.
        places.state_file.unlink()
        assert uninstall(places) == 0 and not places.target.exists()
        # No other file of the game folder changed.
        assert {path: path.read_bytes() for path in game.rglob("*") if path.is_file()} == game_files

        # The start of the game.
        started = []
        assert refused(lambda: launch(places, started.append)), "launch with no EA app"
        runs.add(EA_APP_NAME)
        runs.add(GAME_EXE_NAME)
        assert refused(lambda: launch(places, started.append)), "launch while the game runs"
        runs.discard(GAME_EXE_NAME)
        assert launch(places, lambda path: started.append(path) or 1) == 0
        assert started == [game / GAME_EXE_IN_GAME]
        (user / "Options.ini").write_text("modsdisabled = 1\nscriptmodsenabled = 0\n", encoding="utf-8")
        assert not show_options(places)

        # The DLL mods.
        mod = fake_dll(root / "camera.dll", *MOD_MARKERS)
        assert mods_add(places, mod) == 0
        installed = places.dlls / "camera.dll"
        assert installed.exists() and is_our_mod(places, installed)
        # A DLL that changed after the add is not ours: it goes to the backup before the replacement.
        installed.write_bytes(installed.read_bytes() + b"changed")
        assert mods_add(places, mod) == 0
        assert len(list(places.backups.iterdir())) == 1
        assert is_our_mod(places, installed)
        assert refused(lambda: mods_add(places, fake_dll(root / "not_mod.dll", b"Ts4ModInit"))), "not a DLL mod"
        text = root / "mod.txt"
        text.write_bytes(b"MZ" + b"".join(MOD_MARKERS))
        assert refused(lambda: mods_add(places, text)), "a file that is not a DLL was added"
        (places.dlls / "other.dll").write_bytes(b"MZ user")
        assert mods_remove(places, "OTHER.dll") == 0 and not (places.dlls / "other.dll").exists()
        assert len(list(places.backups.iterdir())) == 2, "a DLL that is not ours goes to the backup before the removal"
        runs.add(GAME_EXE_NAME)
        assert refused(lambda: mods_add(places, mod)), "add while the game runs"
        assert refused(lambda: mods_remove(places, "camera.dll")), "remove while the game runs"
        runs.discard(GAME_EXE_NAME)
        assert mods_remove(places, "camera.dll") == 0 and not installed.exists()
        assert len(list(places.backups.iterdir())) == 2, "our own DLL goes with no backup"
        assert refused(lambda: inside(places.dlls, places.dlls / ".." / "install-state.json")), "'..' was accepted"
        assert mods_list(places) == 0
        # The log.
        places.log.parent.mkdir(parents=True)
        places.log.write_text("".join(f"line {i}\n" for i in range(40)), encoding="utf-8")
        show_log(places, 3)
    print("self-test: OK")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["status", "install", "uninstall", "launch", "mods"])
    parser.add_argument("action", nargs="?", choices=["list", "add", "remove"], help="the action of the mods command")
    parser.add_argument("target", nargs="?", help="mods add: the DLL; mods remove: the file name")
    parser.add_argument("--dll", type=Path, default=DEFAULT_DLL, help="install: the winmm.dll of the build")
    args = parser.parse_args()
    places = real_places()
    if args.command == "mods":
        if args.action is None or (args.action != "list" and not args.target):
            sys.exit("use: mods list | mods add <dll> | mods remove <file name>")
        if args.action == "list":
            return mods_list(places)
        if args.action == "add":
            return mods_add(places, Path(args.target))
        return mods_remove(places, args.target)
    if args.command == "status":
        return status(places)
    if args.command == "install":
        return install(places, args.dll)
    if args.command == "uninstall":
        return uninstall(places)
    return launch(places)


if __name__ == "__main__":
    sys.exit(main())
