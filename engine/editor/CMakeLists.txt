find_package(tinyfiledialogs CONFIG REQUIRED)

add_library(mediaserver_editor STATIC
    src/editor_shell.cpp
    src/workspace_layout.cpp
    src/viewport_panel.cpp
    src/model_surface_cache.cpp
    src/stage_placement.cpp
    src/measurement.cpp
    src/transform_tool.cpp
    src/light_guides.cpp
    src/inspector_panel.cpp
    src/number_field.cpp
    src/color_correction_panel.cpp
    src/texture_panel.cpp
    src/library_thumbnail_texture_cache.cpp
    src/timeline_filmstrip_texture_cache.cpp
    src/live_data_panel.cpp
    src/selection_sets_panel.cpp
    src/library_panel.cpp
    src/masks_panel.cpp
    src/effects_panel.cpp
    src/timeline_panel.cpp
    src/timeline/timeline_rows.cpp
    src/timeline/timeline_row_cells.cpp
    src/timeline/timeline_graph_view.cpp
    src/timeline/timeline_toolbar.cpp
    src/timeline/timeline_actions.cpp
    src/timeline/timeline_key_tools.cpp
    src/timeline/timeline_reorder.cpp
    src/timeline/timeline_clip_gesture.cpp
    src/timeline/timeline_key_selection.cpp
    src/timeline/timeline_ruler.cpp
    src/timeline/timeline_navigator.cpp
    src/timeline/timeline_keyboard.cpp
    src/cues_panel.cpp
    src/operator_widgets.cpp
    src/workspace_panels.cpp
    src/scene_settings_panel.cpp
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

# Renders the operator panels with a representative show into an image at any
# interface scale - how the panels' look is checked at 100% and at 4K's 150%
# or 200% without a running Director or a populated project.
add_executable(mediaserver_editor_ui_snapshot
    tools/editor_ui_snapshot.cpp
)
target_link_libraries(mediaserver_editor_ui_snapshot
    PRIVATE
        MediaServer::Editor
)
mediaserver_set_project_options(mediaserver_editor_ui_snapshot)
set_target_properties(mediaserver_editor_ui_snapshot PROPERTIES FOLDER "tools")

if(BUILD_TESTING)
    find_package(SDL3 CONFIG REQUIRED)
    add_executable(mediaserver_editor_shell_workflow_tests
        tests/editor_shell_workflow_tests.cpp
    )
    target_link_libraries(mediaserver_editor_shell_workflow_tests
        PRIVATE MediaServer::Editor MediaServer::Persist SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_editor_shell_workflow_tests)
    add_test(NAME mediaserver.editor_shell_workflow
        COMMAND mediaserver_editor_shell_workflow_tests)
    set_tests_properties(mediaserver.editor_shell_workflow PROPERTIES
        TIMEOUT 30 RUN_SERIAL TRUE)
    set_target_properties(mediaserver_editor_shell_workflow_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_shell_layer_drop_tests
        tests/editor_shell_layer_drop_tests.cpp
    )
    target_link_libraries(mediaserver_editor_shell_layer_drop_tests
        PRIVATE MediaServer::Editor SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_editor_shell_layer_drop_tests)
    add_test(NAME mediaserver.editor_shell_layer_drop
        COMMAND mediaserver_editor_shell_layer_drop_tests)
    set_tests_properties(mediaserver.editor_shell_layer_drop PROPERTIES
        TIMEOUT 30 RUN_SERIAL TRUE)
    set_target_properties(mediaserver_editor_shell_layer_drop_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_shell_source_replacement_tests
        tests/editor_shell_source_replacement_tests.cpp)
    target_link_libraries(mediaserver_editor_shell_source_replacement_tests
        PRIVATE MediaServer::Editor SDL3::SDL3)
    mediaserver_set_project_options(mediaserver_editor_shell_source_replacement_tests)
    add_test(NAME mediaserver.editor_shell_source_replacement
        COMMAND mediaserver_editor_shell_source_replacement_tests)
    set_tests_properties(mediaserver.editor_shell_source_replacement PROPERTIES
        TIMEOUT 60 RUN_SERIAL TRUE)
    set_target_properties(mediaserver_editor_shell_source_replacement_tests PROPERTIES FOLDER "tests")

    # Library assets dropped on Stage rest on the surface under the pointer.
    add_executable(mediaserver_editor_shell_stage_drop_tests
        tests/editor_shell_stage_drop_tests.cpp)
    target_link_libraries(mediaserver_editor_shell_stage_drop_tests
        PRIVATE MediaServer::Editor SDL3::SDL3)
    mediaserver_set_project_options(mediaserver_editor_shell_stage_drop_tests)
    add_test(NAME mediaserver.editor_shell_stage_drop
        COMMAND mediaserver_editor_shell_stage_drop_tests)
    set_tests_properties(mediaserver.editor_shell_stage_drop PROPERTIES
        TIMEOUT 60 RUN_SERIAL TRUE)
    set_target_properties(mediaserver_editor_shell_stage_drop_tests PROPERTIES FOLDER "tests")

    # Stage views: orthographic keys and four views, picking in each.
    add_executable(mediaserver_editor_shell_stage_views_tests
        tests/editor_shell_stage_views_tests.cpp)
    target_link_libraries(mediaserver_editor_shell_stage_views_tests
        PRIVATE MediaServer::Editor SDL3::SDL3)
    mediaserver_set_project_options(mediaserver_editor_shell_stage_views_tests)
    add_test(NAME mediaserver.editor_shell_stage_views
        COMMAND mediaserver_editor_shell_stage_views_tests)
    set_tests_properties(mediaserver.editor_shell_stage_views PROPERTIES
        TIMEOUT 60 RUN_SERIAL TRUE)
    set_target_properties(mediaserver_editor_shell_stage_views_tests PROPERTIES FOLDER "tests")

    # The Stage's Blender transform keys through the real shell and window.
    add_executable(mediaserver_editor_shell_transform_tests
        tests/editor_shell_transform_tests.cpp
    )
    target_link_libraries(mediaserver_editor_shell_transform_tests
        PRIVATE MediaServer::Editor SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_editor_shell_transform_tests)
    add_test(NAME mediaserver.editor_shell_transform
        COMMAND mediaserver_editor_shell_transform_tests)
    set_tests_properties(mediaserver.editor_shell_transform PROPERTIES
        TIMEOUT 30 RUN_SERIAL TRUE)
    set_target_properties(mediaserver_editor_shell_transform_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_workspace_layout_tests
        tests/workspace_layout_tests.cpp
    )
    target_link_libraries(mediaserver_editor_workspace_layout_tests
        PRIVATE MediaServer::Editor
    )
    mediaserver_set_project_options(
        mediaserver_editor_workspace_layout_tests
    )
    add_test(
        NAME mediaserver.editor_workspace_layout
        COMMAND mediaserver_editor_workspace_layout_tests
    )
    set_target_properties(
        mediaserver_editor_workspace_layout_tests PROPERTIES FOLDER "tests"
    )

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
    # The transform tools' arithmetic: snapping, pivot orbits, yaw/pitch/roll
    # recovery and the pointer geometry the gizmos and G/R/S keys share.
    add_executable(mediaserver_editor_transform_tool_tests
        tests/transform_tool_tests.cpp
    )
    target_link_libraries(mediaserver_editor_transform_tool_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_transform_tool_tests)
    add_test(
        NAME mediaserver.editor_transform_tool
        COMMAND mediaserver_editor_transform_tool_tests
    )
    set_target_properties(mediaserver_editor_transform_tool_tests PROPERTIES FOLDER "tests")

    # Where a Library asset dropped on Stage lands, and the background
    # triangle hierarchies that let Stage hit imported models exactly.
    add_executable(mediaserver_editor_stage_placement_tests
        tests/stage_placement_tests.cpp
    )
    target_link_libraries(mediaserver_editor_stage_placement_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_stage_placement_tests)
    add_test(
        NAME mediaserver.editor_stage_placement
        COMMAND mediaserver_editor_stage_placement_tests
    )
    set_target_properties(mediaserver_editor_stage_placement_tests PROPERTIES FOLDER "tests")

    # The Measure tool: readings, clicks, snapping and the Stage interaction.
    add_executable(mediaserver_editor_measurement_tests
        tests/measurement_tests.cpp
    )
    target_link_libraries(mediaserver_editor_measurement_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_measurement_tests)
    add_test(
        NAME mediaserver.editor_measurement
        COMMAND mediaserver_editor_measurement_tests
    )
    set_target_properties(mediaserver_editor_measurement_tests PROPERTIES FOLDER "tests")

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

    add_executable(mediaserver_editor_color_sampling_tests
        tests/color_sampling_tests.cpp
    )
    target_link_libraries(mediaserver_editor_color_sampling_tests
        PRIVATE MediaServer::Editor
    )
    mediaserver_set_project_options(
        mediaserver_editor_color_sampling_tests
    )
    add_test(
        NAME mediaserver.editor_color_sampling
        COMMAND mediaserver_editor_color_sampling_tests
    )
    set_target_properties(
        mediaserver_editor_color_sampling_tests PROPERTIES FOLDER "tests"
    )

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

    # Which of several overlapping Timeline pointer targets owns a point.
    add_executable(mediaserver_editor_timeline_hits_tests
        tests/timeline_hits_tests.cpp
    )
    target_link_libraries(mediaserver_editor_timeline_hits_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_timeline_hits_tests)
    add_test(
        NAME mediaserver.editor_timeline_hits
        COMMAND mediaserver_editor_timeline_hits_tests
    )
    set_target_properties(mediaserver_editor_timeline_hits_tests PROPERTIES FOLDER "tests")

    # The Timeline's row model on its own: the tree build_rows lists and the
    # summaries of closed parents.
    add_executable(mediaserver_editor_timeline_rows_tests
        tests/timeline_rows_tests.cpp
    )
    target_link_libraries(mediaserver_editor_timeline_rows_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_timeline_rows_tests)
    add_test(
        NAME mediaserver.editor_timeline_rows
        COMMAND mediaserver_editor_timeline_rows_tests
    )
    set_target_properties(mediaserver_editor_timeline_rows_tests PROPERTIES FOLDER "tests")

    # The Timeline's curve view on its own: channel colours, the value axis
    # and curves sampled from the scene's evaluator.
    add_executable(mediaserver_editor_timeline_graph_tests
        tests/timeline_graph_tests.cpp
    )
    target_link_libraries(mediaserver_editor_timeline_graph_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_timeline_graph_tests)
    add_test(
        NAME mediaserver.editor_timeline_graph
        COMMAND mediaserver_editor_timeline_graph_tests
    )
    set_target_properties(mediaserver_editor_timeline_graph_tests PROPERTIES FOLDER "tests")

    # Not a test: times TimelinePanel::draw() on a large expanded scene,
    # using the timeline tests' harness. Run it before and after a change to
    # the panel's per-frame work.
    add_executable(mediaserver_editor_timeline_bench
        tests/timeline_panel_tests.cpp
    )
    target_compile_definitions(mediaserver_editor_timeline_bench
        PRIVATE MEDIASERVER_TIMELINE_BENCH=1
    )
    target_link_libraries(mediaserver_editor_timeline_bench
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_timeline_bench)
    set_target_properties(mediaserver_editor_timeline_bench PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_cues_tests
        tests/cues_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_cues_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_cues_tests)
    add_test(
        NAME mediaserver.editor_cues
        COMMAND mediaserver_editor_cues_tests
    )
    set_target_properties(mediaserver_editor_cues_tests PROPERTIES FOLDER "tests")

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

    add_executable(mediaserver_editor_library_tests
        tests/library_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_library_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_library_tests)
    add_test(
        NAME mediaserver.editor_library
        COMMAND mediaserver_editor_library_tests
    )
    set_target_properties(mediaserver_editor_library_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_editor_live_data_tests
        tests/live_data_panel_tests.cpp
    )
    target_link_libraries(mediaserver_editor_live_data_tests
        PRIVATE
            MediaServer::Editor
    )
    mediaserver_set_project_options(mediaserver_editor_live_data_tests)

    add_test(
        NAME mediaserver.editor_live_data
        COMMAND mediaserver_editor_live_data_tests
    )
    set_target_properties(mediaserver_editor_live_data_tests
        PROPERTIES FOLDER "tests"
    )

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
        src/operator_widgets.cpp
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
            MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_editor_mapping_tests)

    add_test(
        NAME mediaserver.editor_mapping
        COMMAND mediaserver_editor_mapping_tests
    )
    set_target_properties(mediaserver_editor_mapping_tests PROPERTIES FOLDER "tests")
endif()
