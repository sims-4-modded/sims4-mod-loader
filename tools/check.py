#!/usr/bin/env python3
# ******************************************************************************
# Copyright (c) 2026 Sims 4 mods developers
# ******************************************************************************
"""Do the format check and the lint check of CI on this computer, before a push.

    python tools/check.py               do the format check and the lint check
    python tools/check.py format        clang-format on the C++ files, ruff on the Python files
    python tools/check.py tidy          clang-tidy on the C++ files in compile_commands.json
    python tools/check.py tidy --build-dir build/x64-release --jobs 8

The lint check needs a configured build (tools\\preset.bat x64-release configure). The checks need the tool
versions in tools/requirements-check.txt:

    python -m pip install -r tools/requirements-check.txt

The script stops with an error when a tool has a different version, because each version formats and lints in a
different way. Set CLANG_FORMAT, CLANG_TIDY or RUFF to use a tool that is not on the PATH.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REQUIREMENTS = ROOT / "tools" / "requirements-check.txt"
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
# The C++ files of the project are in these folders. The other folders hold code that is not ours.
OWN_FOLDERS = ("src", "tests", "imgui_config")
PYTHON_FOLDERS = ("tools",)


def pinned_versions() -> dict[str, str]:
    """Return the version of each tool in the requirements file."""
    versions = {}
    for line in REQUIREMENTS.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if "==" in line:
            name, version = line.split("==", 1)
            versions[name.strip()] = version.strip()
    return versions


def find_tool(name: str, variable: str) -> str:
    """Return the path of a tool from the environment variable, the Python scripts folder or the PATH."""
    if os.environ.get(variable):
        return os.environ[variable]
    scripts = Path(sys.executable).parent
    for folder in (scripts, scripts / "Scripts"):
        for candidate in (folder / f"{name}.exe", folder / name):
            if candidate.is_file():
                return str(candidate)
    found = shutil.which(name)
    if not found:
        sys.exit(f"{name} is not installed. Do: python -m pip install -r {REQUIREMENTS.relative_to(ROOT)}")
    return found


def check_version(tool: str, name: str, pinned: str, same_major: bool = False) -> None:
    """Stop if the tool does not have the pinned version (or the pinned major version)."""
    out = subprocess.run([tool, "--version"], capture_output=True, text=True, check=False).stdout
    match = re.search(r"(\d+\.\d+\.\d+)", out)
    version = match.group(1) if match else "unknown"
    ok = version.split(".")[0] == pinned.split(".")[0] if same_major else version == pinned
    if not ok:
        sys.exit(f"{name} {version} is at {tool}; the checks need {name} {pinned}. Set the path or install the pin.")
    print(f"{name} {version}: {tool}")


def tracked_files(folders: tuple[str, ...], suffixes: set[str]) -> list[Path]:
    """Return the files that git tracks in the folders, with one of the suffixes."""
    out = subprocess.run(
        ["git", "ls-files", "-z", "--", *folders], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout
    return [ROOT / name for name in out.split("\0") if name and Path(name).suffix.lower() in suffixes]


def check_format() -> int:
    """Do the format check: clang-format on the C++ files, ruff format and ruff check on the Python files."""
    versions = pinned_versions()
    clang_format = find_tool("clang-format", "CLANG_FORMAT")
    check_version(clang_format, "clang-format", versions["clang-format"])
    ruff = find_tool("ruff", "RUFF")
    check_version(ruff, "ruff", versions["ruff"])
    failures = 0
    files = [str(path) for path in tracked_files(OWN_FOLDERS, CPP_SUFFIXES)]
    print(f"clang-format: {len(files)} files")
    if subprocess.run([clang_format, "--dry-run", "--Werror", *files], cwd=ROOT, check=False).returncode != 0:
        print("clang-format: FAILED. Do a whole-file clang-format -i on the files above.")
        failures += 1
    for command in (["format", "--check"], ["check"]):
        if subprocess.run([ruff, *command, *PYTHON_FOLDERS], cwd=ROOT, check=False).returncode != 0:
            print(f"ruff {command[0]}: FAILED")
            failures += 1
    return 1 if failures else 0


def tidy_sources(build_dir: Path) -> list[Path]:
    """Return the C++ files of the project in compile_commands.json, one time each."""
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        sys.exit(f"{database} is not there. Configure first: tools\\preset.bat x64-release configure")
    own = [(ROOT / folder).resolve() for folder in OWN_FOLDERS]
    sources = []
    for entry in json.loads(database.read_text(encoding="utf-8")):
        path = (Path(entry["directory"]) / entry["file"]).resolve()
        if path.suffix.lower() in CPP_SUFFIXES and any(path.is_relative_to(folder) for folder in own):
            if path not in sources:
                sources.append(path)
    return sorted(sources)


def check_tidy(build_dir: Path, jobs: int) -> int:
    """Do the lint check: clang-tidy with the warnings as errors on each C++ file of the project."""
    clang_tidy = find_tool("clang-tidy", "CLANG_TIDY")
    check_version(clang_tidy, "clang-tidy", pinned_versions()["clang-tidy"], same_major=True)
    sources = tidy_sources(build_dir)
    print(f"clang-tidy: {len(sources)} files, {jobs} jobs")

    def run(path: Path) -> tuple[Path, int, str]:
        command = [clang_tidy, "-p", str(build_dir), "--quiet", "--warnings-as-errors=*", str(path)]
        out = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        # clang-tidy writes the number of warnings that it did not show to stderr. Keep only the findings.
        return path, out.returncode, out.stdout

    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, code, out in pool.map(run, sources):
            if code != 0:
                failed.append(path)
                print(out.rstrip() or f"{path}: clang-tidy stopped with the code {code}")
    if failed:
        print(f"clang-tidy: FAILED in {len(failed)} files")
        return 1
    print("clang-tidy: no findings")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("check", nargs="?", choices=("all", "format", "tidy"), default="all")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "x64-release")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args(argv)
    # Show each line at once, so that the lines of the script and the lines of the tools stay in order.
    sys.stdout.reconfigure(line_buffering=True)
    build_dir = args.build_dir.resolve()
    result = 0
    if args.check in ("all", "format"):
        result |= check_format()
    if args.check in ("all", "tidy"):
        result |= check_tidy(build_dir, args.jobs)
    return result


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
