cmake_minimum_required(VERSION 3.28)

project(
    MediaServer
    VERSION 0.1.0
    DESCRIPTION "Cross-platform virtual production media server"
    LANGUAGES CXX
)

include(CTest)

option(MEDIASERVER_BUILD_SERVER "Build the media-server executable" ON)
# Off by default so a headless build needs no video stack. Enabling it also
# requires the vcpkg manifest feature: VCPKG_MANIFEST_FEATURES=gui.
option(MEDIASERVER_ENABLE_GUI "Build the SDL3 window output shell" OFF)
option(MEDIASERVER_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
# The sustained-rate gate runs for ten minutes, so it is registered with CTest
# only on request. Its binary is built either way and takes --seconds.
option(MEDIASERVER_ENABLE_SOAK_TESTS "Register the long sustained-rate soak with CTest" OFF)
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
# The scene layer is the document's view of 3D and needs no graphics stack, so
# a headless node builds and tests it like anything else. The renderer that
# draws it lives behind MEDIASERVER_ENABLE_GUI below.
add_subdirectory(engine/scene)
# The feed map is the document's view of the output stage - which canvas region
# each node outputs and how its edges blend. Like the scene it is pure document
# and geometry with no graphics stack, so it too builds and tests headless. The
# GPU transform that applies it lives in engine/render below.
add_subdirectory(engine/output)

# SDL3 carries both the window shell and the first RHI backend, so the option
# now means "this build has SDL3" rather than "this build opens windows": a
# headless node can drive a GPU output target without ever creating one.
if(MEDIASERVER_ENABLE_GUI)
    add_subdirectory(engine/shaders)
    add_subdirectory(engine/rhi)
    # The renderer that draws a scene::Scene onto a GPU target. It sits above
    # rhi and shaders and below ui, which presents what it draws.
    add_subdirectory(engine/render)
    add_subdirectory(engine/ui)
    # The Dear ImGui operator editor: a throwaway surface for placing scene
    # elements, isolated here so the bespoke UI can replace it later.
    add_subdirectory(engine/editor)
endif()

if(MEDIASERVER_BUILD_SERVER)
    add_subdirectory(apps/server)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests/unit)
endif()
