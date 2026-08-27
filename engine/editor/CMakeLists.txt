find_package(tinyfiledialogs CONFIG REQUIRED)

add_library(mediaserver_editor STATIC
    src/editor_shell.cpp
    src/viewport_panel.cpp
    src/inspector_panel.cpp
    src/number_field.cpp
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
# composite. Render and the native file dialog remain private implementation
# details of the live shell.
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
    PRIVATE
        MediaServer::Render
        tinyfiledialogs::tinyfiledialogs
)

mediaserver_set_project_options(mediaserver_editor)
mediaserver_disable_rtti(mediaserver_editor)

set_target_properties(mediaserver_editor PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
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
