# The test window of the menu bar. It links the menu bar as the extender DLL does, and Direct3D 11 for its own window.
add_executable(ts4_overlay_app OverlayApp.cpp)
target_link_libraries(ts4_overlay_app PRIVATE ts4_loader_overlay d3d11)
ts4_set_warnings(ts4_overlay_app)
