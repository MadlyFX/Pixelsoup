cmake_minimum_required(VERSION 3.28)

project(
    MediaServer
    VERSION 0.1.0
    DESCRIPTION "Frame-scheduled distributed media server"
    LANGUAGES CXX
)

include(CTest)

option(MEDIASERVER_BUILD_SERVER "Build the media-server executable" ON)
# Off by default so a headless build needs no video stack. Enabling it also
# requires the vcpkg manifest feature: VCPKG_MANIFEST_FEATURES=gui.
option(MEDIASERVER_ENABLE_GUI "Build the SDL3 window output shell" OFF)
option(MEDIASERVER_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(MEDIASERVER_ENABLE_SANITIZERS "Enable supported address/undefined sanitizers" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)

add_subdirectory(engine/core)
add_subdirectory(engine/doc)
add_subdirectory(engine/cmd)
add_subdirectory(engine/wire)
add_subdirectory(engine/persist)
add_subdirectory(engine/show)
add_subdirectory(engine/net)

if(MEDIASERVER_ENABLE_GUI)
    add_subdirectory(engine/ui)
endif()

if(MEDIASERVER_BUILD_SERVER)
    add_subdirectory(apps/server)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests/unit)
endif()
