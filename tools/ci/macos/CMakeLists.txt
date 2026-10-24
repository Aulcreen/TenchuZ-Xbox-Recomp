# Compiles the game's own sources for macOS against the prebuilt SDK, without
# the recompiled game (.github/workflows/macos-check.yml). tools/macos/build.sh
# makes the real build.
cmake_minimum_required(VERSION 3.25)
project(tenchuz_macos_check LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# Nothing is linked, so the Vulkan libraries the SDK package names can be
# missing.
add_library(rex::vulkan-loader SHARED IMPORTED)
set_target_properties(rex::vulkan-loader PROPERTIES IMPORTED_LOCATION "/usr/lib/libvulkan.1.dylib")
add_library(rex::moltenvk SHARED IMPORTED)
set_target_properties(rex::moltenvk PROPERTIES IMPORTED_LOCATION "/usr/lib/libMoltenVK.dylib")
find_package(rexglue CONFIG REQUIRED)

# The non-Windows sources of CMakeLists.txt (no Steamworks SDK here).
add_library(tenchuz_check OBJECT
    ${root}/src/main.cpp
    ${root}/src/crash_report.cpp
    ${root}/src/frame_rate.cpp
    ${root}/src/frame_stats.cpp
    ${root}/src/input/pc_input.cpp
    ${root}/src/profile_fix.cpp
    ${root}/src/ui/game_menu.cpp
    ${root}/src/widescreen.cpp
    ${root}/src/net/system_link.cpp
    ${root}/src/net/transport_local.cpp
    ${root}/src/renderer/d3d_trace.cpp
    ${root}/src/renderer/d3d_trace_hooks.gen.cpp
    ${root}/src/renderer/gpu_observer.cpp
    ${root}/src/renderer/pm4_decoder.cpp
    ${root}/src/platform_stubs.cpp
)
target_include_directories(tenchuz_check PRIVATE ${root} ${root}/src)
target_link_libraries(tenchuz_check PRIVATE rex::runtime)
