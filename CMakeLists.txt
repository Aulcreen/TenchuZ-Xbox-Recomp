# tenchuz - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tenchuz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TENCHUZ_SOURCES
    src/main.cpp
    src/crash_report.cpp
    src/debug_tools.cpp
    src/frame_rate.cpp
    src/frame_stats.cpp
    src/input/pc_input.cpp
    src/profile_fix.cpp
    src/profiler.cpp
    src/sdk_fixes.cpp
    src/ui/audio_volume.cpp
    src/ui/game_menu.cpp
    src/widescreen.cpp
    src/net/system_link.cpp
    src/net/transport_local.cpp
    src/png_writer.cpp
    src/renderer/d3d_trace.cpp
    src/renderer/d3d_trace_hooks.gen.cpp
    src/renderer/draw_path.cpp
    src/renderer/gpu_observer.cpp
    src/renderer/native_gpu.cpp
    src/renderer/native_renderer.cpp
    src/renderer/pm4_decoder.cpp
    src/renderer/shader_cache.cpp
    src/renderer/texture_loader.cpp
)

if(WIN32)
    add_executable(tenchuz WIN32 ${TENCHUZ_SOURCES})
else()
    add_executable(tenchuz ${TENCHUZ_SOURCES})
endif()

rexglue_setup_target(tenchuz GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(tenchuz PRIVATE ws2_32 dbghelp winmm synchronization windowscodecs ole32)
endif()

# Setup.exe: the installer stub. tools/package_release.ps1 appends the game
# files to it (TenchuZ-Setup-<version>.exe), which installs them and copies the
# player's own disc image into game\ (src/setup/setup_main.cpp).
if(WIN32)
    add_executable(tenchuz_setup src/setup/setup_main.cpp)
    set_target_properties(tenchuz_setup PROPERTIES OUTPUT_NAME "Setup")
    target_link_libraries(tenchuz_setup PRIVATE bcrypt cabinet ole32 shell32 uuid)
endif()

# System Link over Steam, when the Steamworks SDK (the zip's sdk folder) is in
# third_party/steamworks. steam_api64.dll is delay-loaded, so the game still
# starts without it (src/net/transport_steam.cpp).
set(TENCHUZ_STEAMWORKS_DIR "${CMAKE_SOURCE_DIR}/third_party/steamworks" CACHE PATH "Steamworks SDK folder")
if(NOT EXISTS "${TENCHUZ_STEAMWORKS_DIR}/public" AND EXISTS "${TENCHUZ_STEAMWORKS_DIR}/sdk/public")
    set(TENCHUZ_STEAMWORKS_DIR "${TENCHUZ_STEAMWORKS_DIR}/sdk")  # the zip's sdk folder itself
endif()
if(WIN32 AND EXISTS "${TENCHUZ_STEAMWORKS_DIR}/public/steam/steam_api.h")
    set(steam_bin "${TENCHUZ_STEAMWORKS_DIR}/redistributable_bin/win64")
    target_sources(tenchuz PRIVATE src/net/transport_steam.cpp)
    target_compile_definitions(tenchuz PRIVATE TENCHUZ_STEAM=1)
    target_include_directories(tenchuz SYSTEM PRIVATE "${TENCHUZ_STEAMWORKS_DIR}/public")
    target_link_libraries(tenchuz PRIVATE "${steam_bin}/steam_api64.lib" delayimp)
    target_link_options(tenchuz PRIVATE "LINKER:/DELAYLOAD:steam_api64.dll")
    add_custom_command(TARGET tenchuz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${steam_bin}/steam_api64.dll" "$<TARGET_FILE_DIR:tenchuz>")
    message(STATUS "tenchuz: Steamworks SDK found, System Link over Steam is on")
else()
    message(STATUS "tenchuz: no Steamworks SDK in ${TENCHUZ_STEAMWORKS_DIR}, System Link is local only")
endif()

# Xenos -> DXBC shader translator and texture layout utilities for the native
# renderer, vendored from the ReXGlue SDK at the commit our SDK binaries were
# built from (BSD-3, see external/xenos_shader/LICENSE). The SDK only ships them
# inside the GPU plugin.
add_library(tenchuz_xenos_shader STATIC
    external/xenos_shader/shader.cpp
    external/xenos_shader/translator.cpp
    external/xenos_shader/translator_disasm.cpp
    external/xenos_shader/dxbc.cpp
    external/xenos_shader/dxbc_translator.cpp
    external/xenos_shader/dxbc_translator_alu.cpp
    external/xenos_shader/dxbc_translator_fetch.cpp
    external/xenos_shader/dxbc_translator_memexport.cpp
    external/xenos_shader/dxbc_translator_om.cpp
    external/xenos_shader/geometry_shader.cpp
    external/xenos_shader/ucode.cpp
    external/xenos_shader/support.cpp
    external/xenos_shader/texture/extent.cpp
    external/xenos_shader/texture/info.cpp
    external/xenos_shader/texture/info_formats.cpp
    external/xenos_shader/texture/util.cpp
    external/xenos_shader/thirdparty/dxbc/DXBCChecksum.cpp
)
target_include_directories(tenchuz_xenos_shader PRIVATE external/xenos_shader)
target_link_libraries(tenchuz_xenos_shader PUBLIC rex::runtime)
target_compile_options(tenchuz_xenos_shader PRIVATE -Wno-unused-variable)
target_link_libraries(tenchuz PRIVATE tenchuz_xenos_shader)
# Compiled texture load compute shaders (DXBC), from the same SDK commit.
target_include_directories(tenchuz PRIVATE external/xenos_shader/shaders)
