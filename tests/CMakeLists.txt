find_package(GTest CONFIG REQUIRED)

# The small DLL mods that the tests of the DLL loader load. Each one is the same source with another kind.
set(TS4_TEST_MODS_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_mods)
set(_ts4_test_mods
    good
    1
    init_fails
    2
    old
    3
    crash
    4
    same_id
    5
    bad_id
    6
)
set(_ts4_test_mod_targets)
while (_ts4_test_mods)
  list(POP_FRONT _ts4_test_mods _name _kind)
  set(_target ts4_test_mod_${_name})
  add_library(${_target} SHARED mods/TestMod.cpp)
  target_compile_definitions(${_target} PRIVATE TS4_TEST_MOD_KIND=${_kind})
  target_link_libraries(${_target} PRIVATE ts4_mod_sdk)
  set_target_properties(${_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TS4_TEST_MODS_DIR})
  ts4_set_warnings(${_target})
  list(APPEND _ts4_test_mod_targets ${_target})
endwhile ()

add_executable(
  ts4_loader_tests
  test_HookRules.cpp
  test_HostTable.cpp
  test_LoaderSettings.cpp
  test_ModCatalogue.cpp
  test_ModDllLoader.cpp
  test_ModRegistry.cpp
  test_OverlayMenus.cpp
  test_OverlayRules.cpp
  test_Sha256.cpp
)
target_link_libraries(ts4_loader_tests PRIVATE ts4_loader_core ts4_loader_dlls GTest::gtest_main)
target_compile_definitions(ts4_loader_tests PRIVATE TS4_TEST_MODS_DIR="${TS4_TEST_MODS_DIR}" WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(ts4_loader_tests ${_ts4_test_mod_targets})
ts4_set_warnings(ts4_loader_tests)

include(GoogleTest)
gtest_discover_tests(ts4_loader_tests DISCOVERY_MODE PRE_TEST)

# A small Direct3D 11 window that draws the menu bar with a fake registry. It is not a test of CTest, because it needs
# a GPU (or WARP) and a desktop. Start it by hand: ts4_overlay_app --seconds 10.
if (WIN32)
  add_subdirectory(overlay_app)
endif ()
