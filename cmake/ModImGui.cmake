# ******************************************************************************
# Copyright (c) 2026 Sims 4 mods developers
# ******************************************************************************

# The menu bar of the mods uses Dear ImGui. The vcpkg library of ImGui uses the DLL C runtime, but the extender uses the
# static C runtime and must bring no other DLL into the game folder. Thus this file compiles the ImGui sources into the
# static library ts4_imgui, from the same archive as the vcpkg port (vcpkg/ports/imgui/portfile.cmake, feature
# docking-experimental). The menu bar uses only the Win32 and the Direct3D 11 backends.

if (TARGET ts4_imgui)
  return()
endif ()

set(TS4_IMGUI_TAG "1.93.0-docking")
set(TS4_IMGUI_SHA512
    "00ade7a3e2c2e34465f1bb9fafd78733d3e659617b0857cf1b3c7f04cfc1d8566d1fd6a8697946f0de7c808cce1c4d0a4b2195c7bc20144cfd8b1e9b1cfba621"
)
set(_ts4_imgui_archive_name "ocornut-imgui-v${TS4_IMGUI_TAG}.tar.gz")
set(_ts4_imgui_deps "${CMAKE_BINARY_DIR}/_deps/ts4_imgui")

# Use the archive in the download cache of vcpkg. If it is not there, download it one time into that cache, or into the
# build folder when the cache is not set.
if (DEFINED ENV{VCPKG_DOWNLOADS})
  set(_ts4_imgui_archive "$ENV{VCPKG_DOWNLOADS}/${_ts4_imgui_archive_name}")
else ()
  set(_ts4_imgui_archive "${_ts4_imgui_deps}/${_ts4_imgui_archive_name}")
endif ()
if (NOT EXISTS "${_ts4_imgui_archive}")
  message(STATUS "Download the Dear ImGui sources ${TS4_IMGUI_TAG}")
  file(
    DOWNLOAD "https://github.com/ocornut/imgui/archive/v${TS4_IMGUI_TAG}.tar.gz" "${_ts4_imgui_archive}"
    EXPECTED_HASH SHA512=${TS4_IMGUI_SHA512}
    STATUS _ts4_imgui_status
  )
  list(GET _ts4_imgui_status 0 _ts4_imgui_code)
  if (NOT _ts4_imgui_code EQUAL 0)
    file(REMOVE "${_ts4_imgui_archive}")
    message(FATAL_ERROR "The download of the Dear ImGui sources failed: ${_ts4_imgui_status}")
  endif ()
endif ()
file(SHA512 "${_ts4_imgui_archive}" _ts4_imgui_hash)
if (NOT _ts4_imgui_hash STREQUAL TS4_IMGUI_SHA512)
  message(FATAL_ERROR "${_ts4_imgui_archive} is not the archive of Dear ImGui ${TS4_IMGUI_TAG}")
endif ()

set(TS4_IMGUI_DIR "${_ts4_imgui_deps}/imgui-${TS4_IMGUI_TAG}")
if (NOT EXISTS "${TS4_IMGUI_DIR}/imgui.cpp")
  file(ARCHIVE_EXTRACT INPUT "${_ts4_imgui_archive}" DESTINATION "${_ts4_imgui_deps}")
endif ()

add_library(
  ts4_imgui STATIC
  ${TS4_IMGUI_DIR}/imgui.cpp
  ${TS4_IMGUI_DIR}/imgui_draw.cpp
  ${TS4_IMGUI_DIR}/imgui_tables.cpp
  ${TS4_IMGUI_DIR}/imgui_widgets.cpp
  ${TS4_IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${TS4_IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
set_target_properties(ts4_imgui PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The include folders come first, so that no other ImGui header can take their place.
target_include_directories(ts4_imgui BEFORE PUBLIC ${TS4_IMGUI_DIR} ${TS4_IMGUI_DIR}/backends)
# The options of ImGui are in one header, for the ImGui sources and for the extender.
target_include_directories(ts4_imgui PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../imgui_config)
target_compile_definitions(ts4_imgui PUBLIC IMGUI_USER_CONFIG="ModImGuiConfig.h")
# The ImGui sources are not ours. The compiler gives no warnings for them.
target_compile_options(ts4_imgui PRIVATE /W0 /utf-8)
# The Direct3D 11 backend compiles its shaders with d3dcompiler_47.dll. The Win32 backend uses dwmapi.dll.
target_link_libraries(ts4_imgui PUBLIC d3dcompiler dwmapi)
