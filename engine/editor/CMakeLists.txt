find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(tinyfiledialogs CONFIG REQUIRED)

add_library(mediaserver_editor STATIC
    src/editor_shell.cpp
    src/viewport_panel.cpp
)
add_library(MediaServer::Editor ALIAS mediaserver_editor)

target_include_directories(mediaserver_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Scene is PUBLIC because the editor's interface names scene types (the scene it
# edits, and the intents it returns). Everything below - the renderer it draws
# viewports with, the device, ImGui, and SDL - is an implementation detail no
# caller sees, so it links privately. This is the one module that names ImGui;
# keeping it here is what makes the throwaway shell deletable.
# Gui and Rhi are PUBLIC because the ported viewport panel's interface names
# them: it takes a UiContext to draw into and the engine's own render target to
# draw. That is the point of the port - a viewport is a texture in the widget
# tree - and it is what the throwaway ImGui shell below never had.
target_link_libraries(mediaserver_editor
    PUBLIC
        MediaServer::Core
        MediaServer::Scene
        MediaServer::Gui
        MediaServer::Rhi
    PRIVATE
        MediaServer::Render
        imgui::imgui
        imguizmo::imguizmo
        tinyfiledialogs::tinyfiledialogs
        SDL3::SDL3
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
endif()
