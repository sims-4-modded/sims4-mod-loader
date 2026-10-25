#!/usr/bin/env python3
# ******************************************************************************
# Copyright (c) 2026 Sims 4 mods developers
# ******************************************************************************
"""Make the export table of the proxy winmm.dll from the export directory of the system winmm.dll.

The script reads the export directory of the system DLL and writes three files in src/game/proxy:

- winmm.def: each export with the name and the ordinal of the system DLL, and the marker export of the extender.
- WinmmExports.h: the name and the ordinal of each export, in the order of the stubs.
- ProxyStubs.asm: one stub for each export. Each stub jumps through the table that Loader.cpp fills.

An export that has no name gets a stub and its ordinal only (NONAME). The extender finds the function of a named
export by its name, and the function of an export with no name by its ordinal.

    python tools/proxy_exports.py generate [--dll C:\\Windows\\System32\\winmm.dll]
    python tools/proxy_exports.py list <dll>
    python tools/proxy_exports.py compare <system dll> <proxy dll>
    python tools/proxy_exports.py --self-test
"""

import argparse
import dataclasses
import os
import struct
import sys
import tempfile
from pathlib import Path
from typing import List, Optional

ROOT = Path(__file__).resolve().parents[1]
PROXY_FOLDER = ROOT / "src" / "game" / "proxy"
LIBRARY = "winmm"
MARKER = "Ts4ModLoaderVersion"
TABLE = "g_winmmFunctions"
HEADER = """/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/
"""


@dataclasses.dataclass(frozen=True)
class Export:
    ordinal: int
    name: Optional[str]
    forwarder: Optional[str] = None


def system_dll() -> Path:
    return Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / f"{LIBRARY}.dll"


def read_exports(data: bytes) -> List[Export]:
    """Read the export directory of a PE file (32-bit or 64-bit) and return its exports in the order of the ordinals."""
    if data[:2] != b"MZ":
        raise ValueError("the file is not a PE file: no MZ header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("the file is not a PE file: no PE header")
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    export_rva, export_size = struct.unpack_from("<II", data, directories)
    table = optional + optional_size
    layout = [struct.unpack_from("<IIII", data, table + i * 40 + 8) for i in range(sections)]

    def offset(rva: int) -> int:
        for virtual_size, virtual_address, raw_size, raw_offset in layout:
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return rva - virtual_address + raw_offset
        raise ValueError(f"the RVA 0x{rva:x} is in no section")

    def text(rva: int) -> str:
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii")

    if export_rva == 0:
        return []
    directory = offset(export_rva)
    base, functions, names, function_table, name_table, ordinal_table = struct.unpack_from("<IIIIII", data,
                                                                                          directory + 16)
    named = {}
    for i in range(names):
        name_rva = struct.unpack_from("<I", data, offset(name_table) + i * 4)[0]
        index = struct.unpack_from("<H", data, offset(ordinal_table) + i * 2)[0]
        named[index] = text(name_rva)
    exports = []
    for index in range(functions):
        rva = struct.unpack_from("<I", data, offset(function_table) + index * 4)[0]
        if rva == 0:
            continue
        forwarder = text(rva) if export_rva <= rva < export_rva + export_size else None
        exports.append(Export(ordinal=base + index, name=named.get(index), forwarder=forwarder))
    return exports


def stub_name(export: Export) -> str:
    return f"Proxy_{export.name}" if export.name else f"Proxy_Ordinal{export.ordinal}"


def check_exports(exports: List[Export]) -> None:
    """Refuse a table that the stubs cannot hold: MASM does not tell upper case from lower case in the names."""
    if not exports:
        raise ValueError("the DLL has no exports")
    stubs = [stub_name(export).lower() for export in exports]
    if len(set(stubs)) != len(stubs):
        raise ValueError("two exports give the same stub name")
    if any(export.name == MARKER for export in exports):
        raise ValueError(f"the system DLL exports {MARKER}")


def make_def(exports: List[Export]) -> str:
    lines = [
        f"; The exports of the proxy {LIBRARY}.dll. tools/proxy_exports.py makes this file from the system DLL: do",
        "; not change it by hand. Each export has the name and the ordinal of the system DLL.",
        f"; {MARKER} tells the install tool and game_builds.py that this {LIBRARY}.dll is the extender.",
        f"LIBRARY {LIBRARY}",
        "EXPORTS",
    ]
    for export in exports:
        if export.name:
            lines.append(f"    {export.name}={stub_name(export)} @{export.ordinal}")
        else:
            lines.append(f"    {stub_name(export)} @{export.ordinal} NONAME")
    lines.append(f"    {MARKER}")
    return "\n".join(lines) + "\n"


def make_header(exports: List[Export]) -> str:
    rows = "\n".join(f"    WinmmExport {{{json_text(export.name)}, {export.ordinal}}}," for export in exports)
    return f"""{HEADER}
// The exports of the system {LIBRARY}.dll, in the order of ProxyStubs.asm. tools/proxy_exports.py makes this file from
// the system DLL: do not change it by hand.
#pragma once

#include <cstdint>

#include <array>

namespace ts4_mod::proxy
{{
/// One export of the system {LIBRARY}.dll: its name, or null for an export that has only an ordinal.
struct WinmmExport
{{
	const char* name;
	std::uint16_t ordinal;
}};

inline constexpr std::array k_WinmmExports {{
{rows}
}};
}} // namespace ts4_mod::proxy
"""


def json_text(name: Optional[str]) -> str:
    return f'"{name}"' if name else "nullptr"


def make_asm(exports: List[Export]) -> str:
    stubs = "\n".join(f"PROXY_STUB {stub_name(export)[len('Proxy_'):]}, {index}" for index, export in
                      enumerate(exports))
    return f"""; ******************************************************************************
; Copyright (c) 2026 Sims 4 mods developers
; ******************************************************************************

; Each export of the proxy jumps to the same function in the system {LIBRARY}.dll.
; Loader.cpp fills {TABLE} in the order of this file (WinmmExports.h).
; tools/proxy_exports.py makes this file from the system DLL: do not change it by hand.

EXTERN {TABLE}:QWORD

PROXY_STUB MACRO name, index
Proxy_&name PROC
    jmp qword ptr [{TABLE} + index * 8]
Proxy_&name ENDP
ENDM

.code
{stubs}

END
"""


def generate(dll: Path, folder: Path) -> int:
    exports = read_exports(dll.read_bytes())
    check_exports(exports)
    folder.mkdir(parents=True, exist_ok=True)
    files = {f"{LIBRARY}.def": make_def(exports), "WinmmExports.h": make_header(exports),
             "ProxyStubs.asm": make_asm(exports)}
    for name, content in files.items():
        (folder / name).write_text(content, encoding="utf-8", newline="\n")
    unnamed = [export.ordinal for export in exports if not export.name]
    print(f"{dll}: {len(exports)} exports, {len(unnamed)} with an ordinal only ({unnamed}); wrote {', '.join(files)}")
    return 0


def compare(system: List[Export], proxy: List[Export]) -> List[str]:
    """Return the differences between the exports of the system DLL and of the proxy. The marker is permitted."""
    errors = []
    by_ordinal = {export.ordinal: export for export in proxy}
    for export in system:
        other = by_ordinal.get(export.ordinal)
        if other is None:
            errors.append(f"ordinal {export.ordinal} ({export.name}) is missing")
        elif other.name != export.name:
            errors.append(f"ordinal {export.ordinal} has the name {other.name}, not {export.name}")
    known = {export.ordinal for export in system}
    for export in proxy:
        if export.ordinal not in known and export.name != MARKER:
            errors.append(f"the proxy has the export {export.name} @{export.ordinal} that the system DLL does not have")
    if not any(export.name == MARKER for export in proxy):
        errors.append(f"the proxy has no {MARKER}")
    return errors


def self_test() -> int:
    """Check the reader, the files and the compare on the system DLL, with the output in a temporary folder."""
    exports = read_exports(system_dll().read_bytes())
    check_exports(exports)
    assert len({export.ordinal for export in exports}) == len(exports)
    assert any(export.name == "timeGetTime" for export in exports)
    with tempfile.TemporaryDirectory() as temporary:
        assert generate(system_dll(), Path(temporary)) == 0
        definition = (Path(temporary) / f"{LIBRARY}.def").read_text(encoding="utf-8")
        assert definition.count(" @") == len(exports) and MARKER in definition
        for export in exports:
            if not export.name:
                assert f"{stub_name(export)} @{export.ordinal} NONAME" in definition
    proxy = exports + [Export(ordinal=max(e.ordinal for e in exports) + 1, name=MARKER)]
    assert compare(exports, proxy) == []
    assert compare(exports, exports) == [f"the proxy has no {MARKER}"]
    wrong = [dataclasses.replace(proxy[0], name="other")] + proxy[1:]
    assert len(compare(exports, wrong)) == 1
    assert len(compare(exports, proxy[1:])) == 1
    try:
        check_exports([Export(1, "Ab"), Export(2, "aB")])
        raise AssertionError("two stub names that differ only in case passed")
    except ValueError:
        pass
    print("self-test: OK")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    make = commands.add_parser("generate", help="write the def, the header and the stubs")
    make.add_argument("--dll", type=Path, default=system_dll())
    make.add_argument("--out", type=Path, default=PROXY_FOLDER)
    show = commands.add_parser("list", help="print the exports of a DLL")
    show.add_argument("dll", type=Path)
    check = commands.add_parser("compare", help="compare the exports of the system DLL and of the proxy")
    check.add_argument("system", type=Path)
    check.add_argument("proxy", type=Path)
    args = parser.parse_args()
    if args.command == "generate":
        return generate(args.dll, args.out)
    if args.command == "list":
        for export in read_exports(args.dll.read_bytes()):
            print(f"{export.ordinal:5} {export.name or '(no name)'}" + (f" -> {export.forwarder}" if export.forwarder
                                                                         else ""))
        return 0
    errors = compare(read_exports(args.system.read_bytes()), read_exports(args.proxy.read_bytes()))
    for error in errors:
        print(error)
    print("the exports are the same" if not errors else f"{len(errors)} differences")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
