find_package(tinyfiledialogs CONFIG REQUIRED)

add_library(mediaserver_editor STATIC
    src/editor_shell.cpp
    src/viewport_panel.cpp
    src/inspector_panel.cpp
    src/number_field.cpp
    src/color_correction_panel.cpp
    src/texture_panel.cpp
    src/library_thumbnail_texture_cache.cpp
    src/timeline_filmstrip_texture_cache.cpp
    src/selection_sets_panel.cpp
    src/masks_panel.cpp
    src/effects_panel.cpp
    src/timeline_panel.cpp
    src/workspace_panels.cpp
    src/mapping_panels.cpp
)
add_library(MediaServer::Editor ALIAS mediaserver_editor)

target_include_directories(mediaserver_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Scene, Output, Topology, Assets, and Compositor are PUBLIC because panel
# interfaces name their authored values and edit intents. Gui and Rhi are
# PUBLIC because the panels name UiContext and the engine texture they
# composite. Render is PUBLIC because the Library thumbnail cache accepts its
# decoded-frame value in the public interface. The native file dialog remains
# a private implementation detail of the live shell.
target_link_libraries(mediaserver_editor
    PUBLIC
        MediaServer::Core
        MediaServer::Scene
        MediaServer::Output
        MediaServer::Topology
        MediaServer::Sync
        MediaServer::Assets
        MediaServer::Compositor
        MediaServer::Gui
        MediaServer::Rhi
        MediaServer::Render
    PRIVATE
        tinyfiledialogs::tinyfiledialogs
)

mediaserver_set_project_options(mediaserver_editor)
mediaserver_disable_rtti(mediaserver_editor)

set_target_properties(mediaserver_editor PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_editor_selection_tests
        tests/selection_tests.cpp
    )
    target_include_directories(mediaserver_editor_selection_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(mediaserver_editor_selection_tests
        PRIVATE
            MediaServer::Doc
    )
    mediaserver_set_project_options(mediaserver_editor_selection_tests)

    add_test(
        NAME mediaserver.editor_selection
        COMMAND mediaserver_editor_selection_tests
    )
    set_target_properties(mediaserver_editor_selection_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_selection_sets_tests
        tests/selection_sets_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_selection_sets_tests
        PRIVATE MediaServer::Editor
    )
    mediaserver_set_project_options(
        mediaserver_editor_selection_sets_tests
    )
    add_test(
        NAME mediaserver.editor_selection_sets
        COMMAND mediaserver_editor_selection_sets_tests
    )
    set_target_properties(
        mediaserver_editor_selection_sets_tests PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_editor_masks_tests
        tests/masks_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_masks_tests
        PRIVATE MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_masks_tests)
    add_test(
        NAME mediaserver.editor_masks
        COMMAND mediaserver_editor_masks_tests
    )
    set_target_properties(
        mediaserver_editor_masks_tests PROPERTIES FOLDER "tests"
    )

    # The ported viewport is interaction and geometry: where a click lands in
    # canvas pixels, what it picks, and what world translation a drag resolves
    # to. None of that needs a GPU or a window, so the suite drives it with a
    # synthetic pointer and a scene built by hand.
    add_executable(mediaserver_editor_viewport_tests
        tests/viewport_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_viewport_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_viewport_tests)

    add_test(
        NAME mediaserver.editor_viewport
        COMMAND mediaserver_editor_viewport_tests
    )
    set_target_properties(mediaserver_editor_viewport_tests PROPERTIES FOLDER "tests")

    # The Inspector is a value editor: which property an operator changed, and
    # to what. The part that would be wrong silently is the scrub - a field that
    # re-based on the lagging document each frame would crawl instead of
    # tracking - so the suite drives a synthetic pointer across it and asserts
    # the values it emits.
    add_executable(mediaserver_editor_inspector_tests
        tests/inspector_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_inspector_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_inspector_tests)

    add_test(
        NAME mediaserver.editor_inspector
        COMMAND mediaserver_editor_inspector_tests
    )
    set_target_properties(mediaserver_editor_inspector_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_color_correction_tests
        tests/color_correction_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_color_correction_tests
        PRIVATE MediaServer::Editor
    )
    mediaserver_set_project_options(
        mediaserver_editor_color_correction_tests
    )
    add_test(
        NAME mediaserver.editor_color_correction
        COMMAND mediaserver_editor_color_correction_tests
    )
    set_target_properties(
        mediaserver_editor_color_correction_tests PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_editor_texture_tests
        tests/texture_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_texture_tests
        PRIVATE MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_texture_tests)
    add_test(
        NAME mediaserver.editor_texture
        COMMAND mediaserver_editor_texture_tests
    )
    set_target_properties(
        mediaserver_editor_texture_tests PROPERTIES FOLDER "tests"
    )

    # The timeline is mostly compound interaction: ruler scrubs, layer-tree
    # twirls, key actions, and a diamond drag that must be authored once on
    # release. Drive it headlessly so those behaviours stay independent of a
    # window, GPU, and the live composed shell.
    add_executable(mediaserver_editor_timeline_tests
        tests/timeline_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_timeline_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_timeline_tests)

    add_test(
        NAME mediaserver.editor_timeline
        COMMAND mediaserver_editor_timeline_tests
    )
    set_target_properties(mediaserver_editor_timeline_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_workspace_tests
        tests/workspace_panels_tests.cpp
    )
    target_link_libraries(mediaserver_editor_workspace_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_workspace_tests)

    add_test(
        NAME mediaserver.editor_workspace
        COMMAND mediaserver_editor_workspace_tests
    )
    set_target_properties(mediaserver_editor_workspace_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_spatial_effect_tests
        tests/spatial_effect_editor_tests.cpp
    )
    target_link_libraries(mediaserver_editor_spatial_effect_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_spatial_effect_tests)

    add_test(
        NAME mediaserver.editor_spatial_effect
        COMMAND mediaserver_editor_spatial_effect_tests
    )
    set_target_properties(
        mediaserver_editor_spatial_effect_tests PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_editor_library_thumbnail_texture_tests
        tests/library_thumbnail_texture_cache_tests.cpp
    )
    target_link_libraries(mediaserver_editor_library_thumbnail_texture_tests
        PRIVATE MediaServer::Editor MediaServer::Rhi
    )
    mediaserver_set_project_options(
        mediaserver_editor_library_thumbnail_texture_tests
    )
    add_test(
        NAME mediaserver.editor_library_thumbnail_texture
        COMMAND mediaserver_editor_library_thumbnail_texture_tests
    )
    set_tests_properties(
        mediaserver.editor_library_thumbnail_texture PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(
        mediaserver_editor_library_thumbnail_texture_tests PROPERTIES
        FOLDER "tests"
    )

    add_executable(mediaserver_editor_timeline_filmstrip_texture_tests
        tests/timeline_filmstrip_texture_cache_tests.cpp
    )
    target_link_libraries(
        mediaserver_editor_timeline_filmstrip_texture_tests
        PRIVATE MediaServer::Editor MediaServer::Rhi
    )
    mediaserver_set_project_options(
        mediaserver_editor_timeline_filmstrip_texture_tests
    )
    add_test(
        NAME mediaserver.editor_timeline_filmstrip_texture
        COMMAND mediaserver_editor_timeline_filmstrip_texture_tests
    )
    set_tests_properties(
        mediaserver.editor_timeline_filmstrip_texture PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(
        mediaserver_editor_timeline_filmstrip_texture_tests PROPERTIES
        FOLDER "tests"
    )

    add_executable(mediaserver_editor_mapping_tests
        tests/mapping_panels_tests.cpp
        src/mapping_panels.cpp
        src/number_field.cpp
    )
    target_include_directories(mediaserver_editor_mapping_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(mediaserver_editor_mapping_tests
        PRIVATE
            MediaServer::Core
            MediaServer::Scene
            MediaServer::Output
            MediaServer::Topology
            MediaServer::Sync
            MediaServer::Gui
    )
    mediaserver_set_project_options(mediaserver_editor_mapping_tests)

    add_test(
        NAME mediaserver.editor_mapping
        COMMAND mediaserver_editor_mapping_tests
    )
    set_target_properties(mediaserver_editor_mapping_tests PROPERTIES FOLDER "tests")
endif()
