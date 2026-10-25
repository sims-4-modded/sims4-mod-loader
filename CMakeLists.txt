# ******************************************************************************
# Copyright (c) 2026 Sims 4 mods developers
# ******************************************************************************

# The extender of The Sims 4: the DLL that the game loads. It loads the DLL mods from the folder "dlls" in the settings
# folder of the mods (%LOCALAPPDATA%\Sims4Mods). The DLL that goes to the game must bring no other DLL, so every target
# uses only the static C runtime and static libraries.

cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

project(
  ts4_mod_loader
  VERSION 0.1.0
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(TS4_LOADER_BUILD_TESTS "Build the tests of the mod loader" ON)

# The shared code (git submodule).
add_subdirectory(external/common)

find_package(nlohmann_json CONFIG REQUIRED)

# The parts of the extender that do not need the game: the settings, the registry of the mods, the table of a mod, the
# list of the DLL mods, the checksums, the rules of the hooks and the rules and the menus of the menu bar.
add_library(
  ts4_loader_core STATIC
  src/core/HookRules.h
  src/core/HostTable.h
  src/core/LoaderSettings.h
  src/core/ModCatalogue.h
  src/core/ModRegistry.h
  src/core/OverlayMenus.h
  src/core/OverlayRules.h
  src/core/Sha256.h
  src/core/HookRules.cpp
  src/core/HostTable.cpp
  src/core/LoaderSettings.cpp
  src/core/ModCatalogue.cpp
  src/core/ModRegistry.cpp
  src/core/OverlayMenus.cpp
  src/core/Sha256.cpp
)
target_include_directories(ts4_loader_core PUBLIC src)
target_link_libraries(ts4_loader_core PUBLIC ts4_common ts4_mod_core ts4_mod_api nlohmann_json::nlohmann_json)
ts4_set_warnings(ts4_loader_core)

# The load of the DLL mods. Windows only, but it needs no game: the tests load small test mods.
add_library(ts4_loader_dlls STATIC src/dlls/ModDllLoader.h src/dlls/ModDllLoader.cpp)
target_include_directories(ts4_loader_dlls PUBLIC src)
target_compile_definitions(ts4_loader_dlls PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ts4_loader_dlls PUBLIC ts4_loader_core)
ts4_set_warnings(ts4_loader_dlls)

if (TS4_LOADER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif ()
