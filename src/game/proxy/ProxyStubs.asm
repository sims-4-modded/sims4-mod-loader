; ******************************************************************************
; Copyright (c) 2026 Sims 4 mods developers
; ******************************************************************************

; Each export of the proxy jumps to the same function in the system version.dll.
; Loader.cpp fills g_versionFunctions in the order of this file (VersionExports.h).

EXTERN g_versionFunctions:QWORD

PROXY_STUB MACRO name, index
Proxy_&name PROC
    jmp qword ptr [g_versionFunctions + index * 8]
Proxy_&name ENDP
ENDM

.code
PROXY_STUB GetFileVersionInfoA, 0
PROXY_STUB GetFileVersionInfoByHandle, 1
PROXY_STUB GetFileVersionInfoExA, 2
PROXY_STUB GetFileVersionInfoExW, 3
PROXY_STUB GetFileVersionInfoSizeA, 4
PROXY_STUB GetFileVersionInfoSizeExA, 5
PROXY_STUB GetFileVersionInfoSizeExW, 6
PROXY_STUB GetFileVersionInfoSizeW, 7
PROXY_STUB GetFileVersionInfoW, 8
PROXY_STUB VerFindFileA, 9
PROXY_STUB VerFindFileW, 10
PROXY_STUB VerInstallFileA, 11
PROXY_STUB VerInstallFileW, 12
PROXY_STUB VerLanguageNameA, 13
PROXY_STUB VerLanguageNameW, 14
PROXY_STUB VerQueryValueA, 15
PROXY_STUB VerQueryValueW, 16

END
