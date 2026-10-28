# Targeted test groups.
#
# Each CTest entry is labelled with the code-map area whose directory
# registers it, and each area gets a tests-<area> target that builds only that
# area's test executables. A change can then be built and tested without
# linking the other test programs:
#
#   cmake --build --preset area-render
#   ctest --preset area-render
#
# Trait labels cut across areas: "slow" marks entries the quick preset skips,
# and "blender" marks the Blender link and publisher checks, which also get a
# tests-blender target. The full suite (the windows-msvc-*-gui test presets)
# ignores labels and is what packaging runs.

# Entries that take more than about 8 s in either Debug or Release
# (out/build/<preset>/Testing/Temporary/CTestCostData.txt records the times).
set(MEDIASERVER_SLOW_TESTS
    mediaserver.linked_qualification
    mediaserver.linked_qualification_gpu
    mediaserver.net_resume_session
    mediaserver.render_compositor_scene
    mediaserver.render_integration
    mediaserver.render_media
    mediaserver.render_output
    mediaserver.render_scene
    mediaserver.rhi_device
    mediaserver.scene_model
    mediaserver.server_bootstrap
    mediaserver.server_render_machine
    mediaserver.show_playback
    mediaserver.ui_sdl_presenter
)

set(MEDIASERVER_BLENDER_TEST_REGEX
    "blender_|\\.linked_|scene_linked_scene|director_link_service|_fixture$|_unit$")
set(MEDIASERVER_BLENDER_TARGET_REGEX
    "blender_|^mediaserver_linked_|scene_linked_scene|director_link_service")

# Targets in a directory and its subdirectories, and likewise for tests.
function(_mediaserver_directory_property out dir property)
    get_property(values DIRECTORY "${dir}" PROPERTY ${property})
    get_property(children DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        _mediaserver_directory_property(child_values "${child}" ${property})
        list(APPEND values ${child_values})
    endforeach()
    set(${out} ${values} PARENT_SCOPE)
endfunction()

# Labels the tests registered under the given source directories with <area>
# and adds a tests-<area> target over their test executables. Directories that
# this configuration does not build (the GUI stack on a headless preset) are
# skipped.
function(mediaserver_test_area area)
    get_property(added DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
    set(executables)
    foreach(relative IN LISTS ARGN)
        set(dir "${PROJECT_SOURCE_DIR}/${relative}")
        if(NOT dir IN_LIST added)
            continue()
        endif()

        _mediaserver_directory_property(tests "${dir}" TESTS)
        foreach(test IN LISTS tests)
            set(labels ${area})
            if(test IN_LIST MEDIASERVER_SLOW_TESTS)
                list(APPEND labels slow)
            endif()
            if(test MATCHES "${MEDIASERVER_BLENDER_TEST_REGEX}")
                list(APPEND labels blender)
            endif()
            set_property(TEST "${test}" DIRECTORY "${dir}" APPEND PROPERTY LABELS ${labels})
        endforeach()

        _mediaserver_directory_property(targets "${dir}" BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(NOT type STREQUAL "EXECUTABLE")
                continue()
            endif()
            # media-server is the bootstrap smoke's subject; benchmarks and
            # tools are not run by CTest.
            if(target MATCHES "_tests$" OR target STREQUAL "media-server"
                    OR (MEDIASERVER_ENABLE_SOAK_TESTS AND target MATCHES "_soak$"))
                list(APPEND executables ${target})
                if(target MATCHES "${MEDIASERVER_BLENDER_TARGET_REGEX}")
                    set_property(GLOBAL APPEND PROPERTY MEDIASERVER_BLENDER_TEST_TARGETS ${target})
                endif()
            endif()
        endforeach()
    endforeach()

    if(executables)
        add_custom_target(tests-${area} DEPENDS ${executables})
        set_target_properties(tests-${area} PROPERTIES FOLDER "test groups")
    endif()
endfunction()

# Call after every mediaserver_test_area.
function(mediaserver_test_blender_group)
    get_property(executables GLOBAL PROPERTY MEDIASERVER_BLENDER_TEST_TARGETS)
    if(executables)
        add_custom_target(tests-blender DEPENDS ${executables})
        set_target_properties(tests-blender PROPERTIES FOLDER "test groups")
    endif()
endfunction()
