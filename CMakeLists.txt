# tenchuz - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tenchuz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# macOS: the SDK's packages ask for the Vulkan loader and MoltenVK next to
# their own libraries, and the 0.10.0.24 package ships neither. Homebrew's
# (brew install vulkan-loader molten-vk), or conda-forge's that
# tools/macos/build.sh passes as TENCHUZ_VULKAN_LOADER and TENCHUZ_MOLTENVK,
# get staged into the app instead.
if(APPLE)
    find_library(TENCHUZ_VULKAN_LOADER NAMES vulkan.1 vulkan PATHS /opt/homebrew/lib /usr/local/lib)
    find_library(TENCHUZ_MOLTENVK NAMES MoltenVK PATHS /opt/homebrew/lib /usr/local/lib)
    if(TENCHUZ_VULKAN_LOADER AND TENCHUZ_MOLTENVK)
        add_library(rex::vulkan-loader SHARED IMPORTED)
        set_target_properties(rex::vulkan-loader PROPERTIES IMPORTED_LOCATION "${TENCHUZ_VULKAN_LOADER}")
        add_library(rex::moltenvk SHARED IMPORTED)
        set_target_properties(rex::moltenvk PROPERTIES IMPORTED_LOCATION "${TENCHUZ_MOLTENVK}")
    else()
        message(WARNING "tenchuz: no Vulkan loader or MoltenVK (brew install vulkan-loader molten-vk, or run tools/macos/build.sh)")
    endif()
endif()

include(generated/rexglue.cmake)

if(APPLE AND TENCHUZ_MOLTENVK)
    set(REXGLUE_MOLTENVK_ICD "${CMAKE_SOURCE_DIR}/tools/macos/MoltenVK_icd.json")
endif()

# Sources
set(TENCHUZ_SOURCES
    src/main.cpp
    src/crash_report.cpp
    src/frame_rate.cpp
    src/frame_stats.cpp
    src/input/pc_input.cpp
    src/profile_fix.cpp
    src/ui/game_menu.cpp
    src/widescreen.cpp
    src/net/system_link.cpp
    src/net/transport_local.cpp
    src/renderer/d3d_trace.cpp
    src/renderer/d3d_trace_hooks.gen.cpp
    src/renderer/gpu_observer.cpp
    src/renderer/pm4_decoder.cpp
)
if(WIN32)
    # The native D3D12 renderer and the Windows-only tools.
    list(APPEND TENCHUZ_SOURCES
        src/debug_tools.cpp
        src/profiler.cpp
        src/sdk_fixes.cpp
        src/ui/audio_volume.cpp
        src/png_writer.cpp
        src/renderer/draw_path.cpp
        src/renderer/native_gpu.cpp
        src/renderer/native_renderer.cpp
        src/renderer/shader_cache.cpp
        src/renderer/texture_loader.cpp
    )
else()
    # macOS draws with the Xenos GPU plugin (Vulkan on MoltenVK).
    list(APPEND TENCHUZ_SOURCES src/platform_stubs.cpp)
endif()

if(WIN32)
    add_executable(tenchuz WIN32 ${TENCHUZ_SOURCES})
elseif(APPLE)
    # Tenchu Z.app: the SDK stages its libraries, the GPU plugin and Vulkan
    # next to the executable, in Contents/MacOS.
    add_executable(tenchuz MACOSX_BUNDLE ${TENCHUZ_SOURCES})
    set_target_properties(tenchuz PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/tools/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Tenchu Z"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.aulcreen.tenchuz"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")
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
    target_sources(tenchuz PRIVATE src/net/transport_steam.cpp src/input/steam_input.cpp)
    target_compile_definitions(tenchuz PRIVATE TENCHUZ_STEAM=1)
    target_include_directories(tenchuz SYSTEM PRIVATE "${TENCHUZ_STEAMWORKS_DIR}/public")
    target_link_libraries(tenchuz PRIVATE "${steam_bin}/steam_api64.lib" delayimp)
    target_link_options(tenchuz PRIVATE "LINKER:/DELAYLOAD:steam_api64.dll")
    add_custom_command(TARGET tenchuz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${steam_bin}/steam_api64.dll" "$<TARGET_FILE_DIR:tenchuz>")
    message(STATUS "tenchuz: Steamworks SDK found, System Link over Steam is on")
elseif(APPLE AND EXISTS "${TENCHUZ_STEAMWORKS_DIR}/public/steam/steam_api.h")
    # Weakly linked, so the game still starts without the library.
    set(steam_lib "${TENCHUZ_STEAMWORKS_DIR}/redistributable_bin/osx/libsteam_api.dylib")
    target_sources(tenchuz PRIVATE src/net/transport_steam.cpp src/input/steam_input.cpp)
    target_compile_definitions(tenchuz PRIVATE TENCHUZ_STEAM=1)
    target_include_directories(tenchuz SYSTEM PRIVATE "${TENCHUZ_STEAMWORKS_DIR}/public")
    target_link_options(tenchuz PRIVATE "LINKER:-weak_library,${steam_lib}")
    add_custom_command(TARGET tenchuz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${steam_lib}" "$<TARGET_FILE_DIR:tenchuz>")
    message(STATUS "tenchuz: Steamworks SDK found, System Link over Steam is on")
else()
    message(STATUS "tenchuz: no Steamworks SDK in ${TENCHUZ_STEAMWORKS_DIR}, System Link is local only")
endif()

# Steam Input's action manifest and a configuration per controller type
# (src/input/steam_input.h): next to the exe, in the app bundle's Resources on
# macOS.
if(EXISTS "${TENCHUZ_STEAMWORKS_DIR}/public/steam/steam_api.h" AND (WIN32 OR APPLE))
    set(steam_input_dir "${CMAKE_CURRENT_BINARY_DIR}/steam_input")
    configure_file(src/input/steam_input/tenchuz_input_manifest.vdf "${steam_input_dir}/tenchuz_input_manifest.vdf"
        COPYONLY)
    # The controller types tenchuz_input_manifest.vdf lists.
    foreach(type xboxone xbox360 ps3 ps4 ps5 switch_pro generic neptune android apple)
        set(TENCHUZ_CONTROLLER_TYPE "controller_${type}")
        configure_file(src/input/steam_input/controller.vdf.in "${steam_input_dir}/controller_${type}.vdf" @ONLY)
    endforeach()
    if(APPLE)
        set(steam_input_dest "$<TARGET_BUNDLE_CONTENT_DIR:tenchuz>/Resources/steam_input")
    else()
        set(steam_input_dest "$<TARGET_FILE_DIR:tenchuz>/steam_input")
    endif()
    add_custom_command(TARGET tenchuz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${steam_input_dir}" "${steam_input_dest}")
endif()

# Xenos -> DXBC shader translator and texture layout utilities for the native
# renderer, vendored from the ReXGlue SDK at the commit our SDK binaries were
# built from (BSD-3, see external/xenos_shader/LICENSE). The SDK only ships them
# inside the GPU plugin.
if(WIN32)
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
endif()
