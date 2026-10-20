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
    src/frame_stats.cpp
    src/profile_fix.cpp
    src/widescreen.cpp
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
