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
    src/profile_fix.cpp
)

if(WIN32)
    add_executable(tenchuz WIN32 ${TENCHUZ_SOURCES})
else()
    add_executable(tenchuz ${TENCHUZ_SOURCES})
endif()

rexglue_setup_target(tenchuz GPU_PLUGINS xenos)
