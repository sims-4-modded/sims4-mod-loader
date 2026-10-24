find_package(GTest CONFIG REQUIRED)

add_executable(
  ts4_loader_tests
  test_HookRules.cpp
  test_LoaderSettings.cpp
  test_OverlayRules.cpp
  test_Sha256.cpp
)
target_link_libraries(ts4_loader_tests PRIVATE ts4_loader_core GTest::gtest_main)
target_compile_definitions(ts4_loader_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
ts4_set_warnings(ts4_loader_tests)

include(GoogleTest)
gtest_discover_tests(ts4_loader_tests DISCOVERY_MODE PRE_TEST)
