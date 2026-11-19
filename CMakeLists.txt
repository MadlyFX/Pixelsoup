cmake_minimum_required(VERSION 3.28)

project(
    MediaServer
    VERSION 0.1.0
    DESCRIPTION "Cross-platform virtual production media server (Pixelsoup)"
    # C only for the vendored ufbx FBX reader.
    LANGUAGES C CXX
)

include(CTest)

# Off by default so a headless build needs no video stack. Enabling it also
# requires the vcpkg manifest feature: VCPKG_MANIFEST_FEATURES=gui.
option(MEDIASERVER_ENABLE_GUI "Build the SDL3 window output shell" OFF)
# The Director's editor and every node's renderer need the GUI stack, so a
# headless build covers the engine modules only.
include(CMakeDependentOption)
cmake_dependent_option(MEDIASERVER_BUILD_SERVER
    "Build the Pixelsoup executable" ON "MEDIASERVER_ENABLE_GUI" OFF)
option(MEDIASERVER_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
# The sustained-rate gate runs for ten minutes, so it is registered with CTest
# only on request. Its binary is built either way and takes --seconds.
option(MEDIASERVER_ENABLE_SOAK_TESTS "Register the long sustained-rate soak with CTest" OFF)
option(MEDIASERVER_ENABLE_SANITIZERS "Enable supported address/undefined sanitizers" OFF)
if(WIN32)
    option(
        MEDIASERVER_ENABLE_ADVANCED_RENDERER
        "Build the native D3D12 Advanced renderer backend"
        ON
    )
    # This uses nvapi64.dll dynamically, so turning it on adds no SDK header
    # or import-library requirement to ordinary developer builds. A host with
    # no supported NVIDIA driver still reports physical sync/barrier support
    # as Unsupported.
    option(
        MEDIASERVER_ENABLE_NVAPI
        "Enable optional NVIDIA Quadro Sync and D3D12 present-barrier integration"
        OFF
    )
else()
    option(
        MEDIASERVER_ENABLE_ADVANCED_RENDERER
        "Build the native Advanced renderer backend"
        OFF
    )
    set(MEDIASERVER_ENABLE_NVAPI OFF CACHE BOOL
        "NVAPI is only available on Windows" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Release builds carry symbols. A dump from a stage machine is otherwise a
# list of bare addresses: the optimized package is what runs on a wall, so it
# is the build whose dumps we get. /Z7 keeps debug info in each object rather
# than one compile PDB, so parallel Ninja compiles never contend for it; the
# linker still writes a single pixelsoup.pdb beside the executable. /DEBUG
# turns off reference elimination and COMDAT folding unless asked, so both are
# asked for and the shipped code stays what it was. Debug keeps its default.
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<$<CONFIG:Release>:Embedded>$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>")
endif()

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
# Authored hardware devices and typed links. Runtime discovery and health are
# intentionally separate, so this document projection remains headless and
# deterministic like scene and output.
add_subdirectory(engine/topology)
# Device control protocols for the hardware on the map that is not a render
# machine - LED processors first. Headless: sockets and JSON on a thread of
# their own, with observations kept apart from the authored topology.
add_subdirectory(engine/devices)
# Camera tracking. Protocol parsers, a bounded pose slot, and the per-frame
# pose channel. It names no socket, no scene, and no renderer, which is what
# keeps a tracker read structurally impossible anywhere but the Director.
add_subdirectory(engine/track)
# The output synchronization policy, the sync groups derived from the hardware
# map, and the Program readiness gate for physical timing. Authored policy and
# measured lock state are deliberately separate types so a saved project can
# never claim that outputs are genlocked.
add_subdirectory(engine/sync)
# Persistent imported-media/model metadata. This is a document projection only:
# probing/decoding stays in the renderer/import workflow and the registry stays
# available to headless nodes.
add_subdirectory(engine/assets)
# A headless typed compositor DAG. It evaluates ordinary document nodes into
# an explicit render-pass plan (including offscreen blur requirements and
# deterministic radial transforms) without depending on the GPU renderer.
add_subdirectory(engine/compositor)

# SDL3 carries both the window shell and the first RHI backend, so the option
# now means "this build has SDL3" rather than "this build opens windows": a
# headless node can drive a GPU output target without ever creating one.
if(MEDIASERVER_ENABLE_GUI)
    # Particle effects are played by the ajfx runtime, which is a separate,
    # optional project. A build has it when ajfx Studio added it before adding
    # MediaServer, or when MEDIASERVER_AJFX_SOURCE_DIR names a checkout. Any
    # other build compiles the renderer without it (engine/render), and a
    # scene that places an effect is refused readiness with that reason.
    set(MEDIASERVER_AJFX_SOURCE_DIR "" CACHE PATH
        "Optional ajfx source checkout; empty builds without particle effects")
    if(NOT TARGET ajfx::ajfx AND MEDIASERVER_AJFX_SOURCE_DIR)
        if(NOT EXISTS "${MEDIASERVER_AJFX_SOURCE_DIR}/lib/include/ajfx/effect.hpp")
            message(FATAL_ERROR "MEDIASERVER_AJFX_SOURCE_DIR is not an ajfx checkout: "
                "${MEDIASERVER_AJFX_SOURCE_DIR}. Clear it to build without particle effects.")
        endif()
        block(SCOPE_FOR VARIABLES)
            set(BUILD_TESTING OFF)
            set(AJFX_BUILD_STUDIO_APP OFF)
            set(AJFX_WARNINGS_AS_ERRORS ${MEDIASERVER_WARNINGS_AS_ERRORS})
            add_subdirectory("${MEDIASERVER_AJFX_SOURCE_DIR}" _deps/ajfx EXCLUDE_FROM_ALL)
        endblock()
    endif()

    add_subdirectory(engine/shaders)
    add_subdirectory(engine/rhi)
    # The renderer that draws a scene::Scene onto a GPU target. It sits above
    # rhi and shaders and below ui, which presents what it draws.
    add_subdirectory(engine/render)
    add_subdirectory(apps/bake)
    add_subdirectory(engine/ui)
    # The bespoke operator UI's own draw stack on the RHI: a 2D draw list,
    # FreeType glyph atlas, widgets, and docking. Above render (it names
    # render::RenderError) and shaders.
    add_subdirectory(engine/gui)
    # The bespoke operator editor: engine viewports composed into a docked
    # layer/timeline/Inspector surface on the shared RHI device.
    add_subdirectory(engine/editor)
endif()

if(MEDIASERVER_BUILD_SERVER)
    add_subdirectory(apps/server)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests/unit)

    # Area labels and tests-<area> build targets; see cmake/TestGroups.cmake.
    # The areas follow the code map in AGENTS.md.
    include(TestGroups)
    mediaserver_test_area(spine engine/core engine/doc engine/cmd tests/unit)
    mediaserver_test_area(session engine/wire engine/net engine/show)
    mediaserver_test_area(persist engine/persist)
    mediaserver_test_area(scene engine/scene engine/track engine/assets)
    mediaserver_test_area(compositor engine/compositor)
    mediaserver_test_area(output engine/output engine/topology engine/sync
        engine/devices)
    mediaserver_test_area(render engine/shaders engine/rhi engine/render)
    mediaserver_test_area(editor engine/ui engine/gui engine/editor)
    mediaserver_test_area(server apps/server)
    mediaserver_test_blender_group()
endif()
