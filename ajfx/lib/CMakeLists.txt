# The ajfx runtime: the effect model, CPU reference, packages, GPU runtime,
# the D3D12 reference backend and the rigid physics solver. Configured from
# ../CMakeLists.txt, which sets the warnings policy and ajfx_set_options.

# The HLSL in shaders/ is embedded in the library, so generating kernels at
# runtime needs no files beside the executable. Rewritten only when it
# changes, and editing a shader re-runs this configure step.
set(_ajfx_embedded "// Generated from ajfx/lib/shaders by ajfx/lib/CMakeLists.txt; do not edit.\n")
string(APPEND _ajfx_embedded "#include \"hlsl_sources.hpp\"\n\nnamespace ajfx::detail {\n")
foreach(_ajfx_shader IN ITEMS random:ajfx_random.hlsli particles:ajfx_particles.hlsli runtime:ajfx_runtime.hlsl
                              sprite:ajfx_sprite.hlsl sort:ajfx_sort.hlsl)
    string(REPLACE ":" ";" _ajfx_pair "${_ajfx_shader}")
    list(GET _ajfx_pair 0 _ajfx_name)
    list(GET _ajfx_pair 1 _ajfx_file)
    set(_ajfx_path "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_ajfx_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ajfx_path}")
    file(READ "${_ajfx_path}" _ajfx_text)
    string(LENGTH "${_ajfx_text}" _ajfx_length)
    string(APPEND _ajfx_embedded "\nextern const std::string_view hlsl_${_ajfx_name};\n")
    string(APPEND _ajfx_embedded "const std::string_view hlsl_${_ajfx_name} =\n")
    # Pieces stay well under MSVC's limit for a single string literal.
    set(_ajfx_at 0)
    while(_ajfx_at LESS _ajfx_length)
        string(SUBSTRING "${_ajfx_text}" ${_ajfx_at} 2000 _ajfx_piece)
        string(APPEND _ajfx_embedded "    R\"ajfx(${_ajfx_piece})ajfx\"\n")
        math(EXPR _ajfx_at "${_ajfx_at} + 2000")
    endwhile()
    string(APPEND _ajfx_embedded "    ;\n")
endforeach()
string(APPEND _ajfx_embedded "\n} // namespace ajfx::detail\n")
set(_ajfx_embedded_path "${CMAKE_CURRENT_BINARY_DIR}/generated/ajfx_hlsl_sources.cpp")
set(_ajfx_previous "")
if(EXISTS "${_ajfx_embedded_path}")
    file(READ "${_ajfx_embedded_path}" _ajfx_previous)
endif()
if(NOT _ajfx_previous STREQUAL _ajfx_embedded)
    file(WRITE "${_ajfx_embedded_path}" "${_ajfx_embedded}")
endif()

add_library(ajfx STATIC
    src/builder.cpp
    src/cache.cpp
    src/effect.cpp
    src/field.cpp
    src/fracture.cpp
    src/mesh.cpp
    src/noise.cpp
    src/gpu/kernels.cpp
    src/gpu/renderer.cpp
    src/gpu/simulation.cpp
    src/gpu/sort.cpp
    src/json.cpp
    src/package.cpp
    src/presets.cpp
    src/rigid_runner.cpp
    src/simulation.cpp
    src/texture.cpp
    src/version.cpp
    "${_ajfx_embedded_path}"
)
add_library(ajfx::ajfx ALIAS ajfx)

target_include_directories(ajfx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu"
)
# RigidRunner steps rigid physics on a thread of its own.
find_package(Threads REQUIRED)
target_link_libraries(ajfx PUBLIC Threads::Threads)
ajfx_set_options(ajfx)
set_target_properties(ajfx PROPERTIES FOLDER "ajfx")

# ajfx::rigid: rigid physics emitters run by Jolt Physics (third_party/jolt),
# built for cross-platform determinism. Core ajfx never depends on it; a host
# links it and hands its solver to the simulations.
option(AJFX_RIGID "Build ajfx::rigid, rigid physics emitters run by Jolt" ON)
if(AJFX_RIGID)
    add_subdirectory(third_party/jolt)
    # Jolt's headers are held to Jolt's warnings, not ours.
    set_target_properties(Jolt PROPERTIES SYSTEM ON)
    add_library(ajfx_rigid STATIC src/rigid/jolt_solver.cpp)
    add_library(ajfx::rigid ALIAS ajfx_rigid)
    target_link_libraries(ajfx_rigid PUBLIC ajfx::ajfx PRIVATE Jolt)
    ajfx_set_options(ajfx_rigid)
    set_target_properties(ajfx_rigid PROPERTIES FOLDER "ajfx")
endif()

# HLSL that a host compiles into its own kernels; exposed as a directory so
# the host's shader compiler can add it to its include path.
set(AJFX_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    CACHE INTERNAL "ajfx HLSL include directory")

# The D3D12 reference backend and the DXC-backed runtime compiler.
if(WIN32)
    option(AJFX_BUILD_D3D12 "Build the D3D12 reference backend" ON)
else()
    set(AJFX_BUILD_D3D12 OFF)
endif()

if(AJFX_BUILD_D3D12)
    add_library(ajfx_d3d12 STATIC
        backends/d3d12/d3d12_device.cpp
        backends/d3d12/dxc_compiler.cpp
    )
    add_library(ajfx::d3d12 ALIAS ajfx_d3d12)
    target_link_libraries(ajfx_d3d12 PUBLIC ajfx::ajfx PRIVATE d3d12 dxgi)
    ajfx_set_options(ajfx_d3d12)
    set_target_properties(ajfx_d3d12 PROPERTIES FOLDER "ajfx")

    # dxcompiler.dll and dxil.dll are loaded at runtime; executables that
    # compile kernels get a copy beside them. Defaults to the newest Windows
    # SDK that ships them.
    set(AJFX_DXC_DIR "" CACHE PATH "Directory holding dxcompiler.dll and dxil.dll")
    set(_ajfx_dxc_dir "${AJFX_DXC_DIR}")
    if(NOT _ajfx_dxc_dir)
        file(GLOB _ajfx_dxc_candidates
            "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxcompiler.dll"
            "C:/Program Files/Windows Kits/10/bin/*/x64/dxcompiler.dll")
        list(SORT _ajfx_dxc_candidates COMPARE NATURAL ORDER DESCENDING)
        foreach(_ajfx_candidate IN LISTS _ajfx_dxc_candidates)
            get_filename_component(_ajfx_candidate_dir "${_ajfx_candidate}" DIRECTORY)
            if(EXISTS "${_ajfx_candidate_dir}/dxil.dll")
                set(_ajfx_dxc_dir "${_ajfx_candidate_dir}")
                break()
            endif()
        endforeach()
    endif()
    if(_ajfx_dxc_dir)
        message(STATUS "ajfx: DXC from ${_ajfx_dxc_dir}")
    else()
        message(STATUS "ajfx: no dxcompiler.dll found; GPU tests will be skipped")
    endif()
    # Cached so ajfx_copy_dxc works when called from another directory: a
    # function sees its caller's variables, not those where it was defined.
    set(AJFX_DXC_RUNTIME_DIR "${_ajfx_dxc_dir}" CACHE INTERNAL "Directory ajfx copies dxcompiler.dll from")

    function(ajfx_copy_dxc target)
        if(AJFX_DXC_RUNTIME_DIR)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AJFX_DXC_RUNTIME_DIR}/dxcompiler.dll" "${AJFX_DXC_RUNTIME_DIR}/dxil.dll"
                    "$<TARGET_FILE_DIR:${target}>")
        endif()
    endfunction()
endif()

# The sample viewer, built by default only when ajfx is the top-level project.
option(AJFX_BUILD_EXAMPLES "Build the ajfx sample viewer" ${PROJECT_IS_TOP_LEVEL})
if(AJFX_BUILD_D3D12 AND AJFX_BUILD_EXAMPLES)
    add_executable(ajfx_viewer examples/viewer/main.cpp)
    target_link_libraries(ajfx_viewer PRIVATE ajfx::d3d12)
    ajfx_set_options(ajfx_viewer)
    ajfx_copy_dxc(ajfx_viewer)
    set_target_properties(ajfx_viewer PROPERTIES FOLDER "ajfx/examples")

    # A windowed media player for .ajfx files (no console).
    add_executable(ajfx_player WIN32 examples/player/main.cpp)
    target_link_libraries(ajfx_player PRIVATE ajfx::d3d12 comctl32 comdlg32 shell32 advapi32)
    ajfx_set_options(ajfx_player)
    ajfx_copy_dxc(ajfx_player)
    set_target_properties(ajfx_player PROPERTIES FOLDER "ajfx/examples")

    # Both play rigid physics effects when ajfx::rigid is built.
    if(AJFX_RIGID)
        foreach(example IN ITEMS ajfx_viewer ajfx_player)
            target_link_libraries(${example} PRIVATE ajfx::rigid)
            target_compile_definitions(${example} PRIVATE AJFX_HAS_RIGID=1)
        endforeach()
    endif()
endif()

if(BUILD_TESTING)
    add_executable(ajfx_random_tests tests/random_tests.cpp)
    target_link_libraries(ajfx_random_tests PRIVATE ajfx::ajfx)
    ajfx_set_options(ajfx_random_tests)
    # The test reads the HLSL twin of random.hpp to prove the two agree.
    target_compile_definitions(ajfx_random_tests PRIVATE
        AJFX_SHADER_INCLUDE_DIR="${AJFX_SHADER_INCLUDE_DIR}")
    add_test(NAME ajfx.random COMMAND ajfx_random_tests)
    set_target_properties(ajfx_random_tests PROPERTIES FOLDER "ajfx/tests")

    # Cell fracture cuts a model at bake time; the suite is pure geometry.
    add_executable(ajfx_fracture_tests tests/fracture_tests.cpp)
    target_link_libraries(ajfx_fracture_tests PRIVATE ajfx::ajfx)
    ajfx_set_options(ajfx_fracture_tests)
    add_test(NAME ajfx.fracture COMMAND ajfx_fracture_tests)
    set_target_properties(ajfx_fracture_tests PROPERTIES FOLDER "ajfx/tests")

    foreach(suite IN ITEMS effect package simulation)
        add_executable(ajfx_${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(ajfx_${suite}_tests PRIVATE ajfx::ajfx)
        ajfx_set_options(ajfx_${suite}_tests)
        add_test(NAME ajfx.${suite} COMMAND ajfx_${suite}_tests)
        set_target_properties(ajfx_${suite}_tests PROPERTIES FOLDER "ajfx/tests")
    endforeach()

    if(AJFX_BUILD_D3D12)
        # Kernel generation runs anywhere; compiling and running need DXC and
        # a D3D12 device, and report "skipped" (77) without them. The d3d12
        # suite runs the backend under the debug layer.
        foreach(suite IN ITEMS kernels gpu render d3d12)
            add_executable(ajfx_${suite}_tests tests/${suite}_tests.cpp)
            target_link_libraries(ajfx_${suite}_tests PRIVATE ajfx::d3d12)
            ajfx_set_options(ajfx_${suite}_tests)
            ajfx_copy_dxc(ajfx_${suite}_tests)
            add_test(NAME ajfx.${suite} COMMAND ajfx_${suite}_tests)
            set_tests_properties(ajfx.${suite} PROPERTIES SKIP_RETURN_CODE 77)
            set_target_properties(ajfx_${suite}_tests PROPERTIES FOLDER "ajfx/tests")
        endforeach()
    endif()

    if(AJFX_RIGID)
        add_executable(ajfx_rigid_tests tests/rigid_tests.cpp)
        target_link_libraries(ajfx_rigid_tests PRIVATE ajfx::rigid)
        ajfx_set_options(ajfx_rigid_tests)
        add_test(NAME ajfx.rigid COMMAND ajfx_rigid_tests)
        set_target_properties(ajfx_rigid_tests PROPERTIES FOLDER "ajfx/tests")
        # Saved state, rigid and particle caches, and parameter timing.
        add_executable(ajfx_cache_tests tests/cache_tests.cpp)
        target_link_libraries(ajfx_cache_tests PRIVATE ajfx::rigid)
        ajfx_set_options(ajfx_cache_tests)
        add_test(NAME ajfx.cache COMMAND ajfx_cache_tests)
        set_target_properties(ajfx_cache_tests PROPERTIES FOLDER "ajfx/tests")
        # Particles born of contacts and moving bodies, scoped init and
        # appearance, and heat.
        add_executable(ajfx_contact_spawn_tests tests/contact_spawn_tests.cpp)
        target_link_libraries(ajfx_contact_spawn_tests PRIVATE ajfx::rigid)
        ajfx_set_options(ajfx_contact_spawn_tests)
        add_test(NAME ajfx.contact_spawn COMMAND ajfx_contact_spawn_tests)
        set_target_properties(ajfx_contact_spawn_tests PROPERTIES FOLDER "ajfx/tests")
        # Step cost, checkpoint size and a state digest; run by hand.
        add_executable(ajfx_rigid_bench tests/rigid_bench.cpp)
        target_link_libraries(ajfx_rigid_bench PRIVATE ajfx::rigid)
        ajfx_set_options(ajfx_rigid_bench)
        set_target_properties(ajfx_rigid_bench PROPERTIES FOLDER "ajfx/tests")
        # Particles meeting another emitter's bodies through the grid, and
        # wakes.
        add_executable(ajfx_body_grid_tests tests/body_grid_tests.cpp)
        target_link_libraries(ajfx_body_grid_tests PRIVATE ajfx::rigid)
        ajfx_set_options(ajfx_body_grid_tests)
        add_test(NAME ajfx.body_grid COMMAND ajfx_body_grid_tests)
        set_target_properties(ajfx_body_grid_tests PROPERTIES FOLDER "ajfx/tests")
        # The grid's cost against meeting every body in turn; run by hand.
        add_executable(ajfx_body_grid_bench tests/body_grid_bench.cpp)
        target_link_libraries(ajfx_body_grid_bench PRIVATE ajfx::rigid)
        ajfx_set_options(ajfx_body_grid_bench)
        set_target_properties(ajfx_body_grid_bench PROPERTIES FOLDER "ajfx/tests")
        if(AJFX_BUILD_D3D12)
            add_executable(ajfx_rigid_gpu_tests tests/rigid_gpu_tests.cpp)
            target_link_libraries(ajfx_rigid_gpu_tests PRIVATE ajfx::rigid ajfx::d3d12)
            ajfx_set_options(ajfx_rigid_gpu_tests)
            ajfx_copy_dxc(ajfx_rigid_gpu_tests)
            add_test(NAME ajfx.rigid_gpu COMMAND ajfx_rigid_gpu_tests)
            set_tests_properties(ajfx.rigid_gpu PROPERTIES SKIP_RETURN_CODE 77)
            set_target_properties(ajfx_rigid_gpu_tests PROPERTIES FOLDER "ajfx/tests")
        endif()
    endif()

    if(TARGET ajfx_viewer)
        # End to end: export a package with kernels, then play it from the
        # package alone and capture a frame.
        add_test(NAME ajfx.viewer_export
            COMMAND ajfx_viewer --preset smoke --export "${CMAKE_CURRENT_BINARY_DIR}/viewer-smoke.ajfx")
        add_test(NAME ajfx.viewer_capture
            COMMAND ajfx_viewer --package "${CMAKE_CURRENT_BINARY_DIR}/viewer-smoke.ajfx" --seconds 3
                    --capture "${CMAKE_CURRENT_BINARY_DIR}/viewer-smoke.png")
        set_tests_properties(ajfx.viewer_export PROPERTIES FIXTURES_SETUP ajfx_viewer_package)
        # A preset's generated mesh travels in the package it exports.
        add_test(NAME ajfx.viewer_debris
            COMMAND ajfx_viewer --preset debris --export "${CMAKE_CURRENT_BINARY_DIR}/viewer-debris.ajfx")
        add_test(NAME ajfx.viewer_debris_capture
            COMMAND ajfx_viewer --package "${CMAKE_CURRENT_BINARY_DIR}/viewer-debris.ajfx" --seconds 2
                    --capture "${CMAKE_CURRENT_BINARY_DIR}/viewer-debris.png")
        set_tests_properties(ajfx.viewer_debris PROPERTIES FIXTURES_SETUP ajfx_debris_package)
        if(AJFX_RIGID)
            # Rigid physics through the viewer: the package carries the chip
            # mesh its hulls wrap, and plays from the package alone.
            add_test(NAME ajfx.viewer_rubble
                COMMAND ajfx_viewer --preset rubble --export "${CMAKE_CURRENT_BINARY_DIR}/viewer-rubble.ajfx")
            add_test(NAME ajfx.viewer_rubble_capture
                COMMAND ajfx_viewer --package "${CMAKE_CURRENT_BINARY_DIR}/viewer-rubble.ajfx" --seconds 4
                        --capture "${CMAKE_CURRENT_BINARY_DIR}/viewer-rubble.png")
            set_tests_properties(ajfx.viewer_rubble PROPERTIES FIXTURES_SETUP ajfx_rubble_package)
            set_tests_properties(ajfx.viewer_rubble_capture PROPERTIES
                FIXTURES_REQUIRED ajfx_rubble_package
                PASS_REGULAR_EXPRESSION "using the package's precompiled kernels")
        endif()
        set_tests_properties(ajfx.viewer_debris_capture PROPERTIES
            FIXTURES_REQUIRED ajfx_debris_package
            PASS_REGULAR_EXPRESSION "using the package's precompiled kernels")
        set_tests_properties(ajfx.viewer_capture PROPERTIES
            FIXTURES_REQUIRED ajfx_viewer_package
            PASS_REGULAR_EXPRESSION "using the package's precompiled kernels")
        # The player's open-and-frame path, offscreen.
        add_test(NAME ajfx.player_screenshot
            COMMAND ajfx_player "${CMAKE_CURRENT_BINARY_DIR}/viewer-smoke.ajfx"
                    --screenshot "${CMAKE_CURRENT_BINARY_DIR}/player-smoke.png" --seconds 3)
        set_tests_properties(ajfx.player_screenshot PROPERTIES
            FIXTURES_REQUIRED ajfx_viewer_package
            PASS_REGULAR_EXPRESSION "using the package's precompiled kernels")
    endif()
endif()
