find_package(SDL3 CONFIG REQUIRED)
find_package(freetype CONFIG REQUIRED)

add_library(mediaserver_gui STATIC
    src/ui_renderer.cpp
    src/text.cpp
    src/input.cpp
    src/layout.cpp
    src/form_layout.cpp
    src/ui_context.cpp
    src/color_picker.cpp
    src/dock.cpp
    src/window.cpp
)
add_library(MediaServer::Gui ALIAS mediaserver_gui)

target_include_directories(mediaserver_gui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Render is PUBLIC because the UI renderer's interface names render::RenderError.
# Shaders (the embedded UI bytecode), FreeType (glyph rasterization), and SDL
# (the window host) are implementation details nothing above this seam sees -
# window.hpp deliberately names neither an SDL window nor an SDL event.
target_link_libraries(mediaserver_gui
    PUBLIC
        MediaServer::Core
        MediaServer::Rhi
        MediaServer::Render
    PRIVATE
        MediaServer::Shaders
        freetype
        SDL3::SDL3
)

mediaserver_set_project_options(mediaserver_gui)
mediaserver_disable_rtti(mediaserver_gui)

set_target_properties(mediaserver_gui PROPERTIES FOLDER "engine")

# A runnable shell for the module. Opening a window and putting a frame on the
# screen is the one claim no headless suite can make, so it is proven by running
# this. It is also the arrangement the operator editor will take, which is what
# the ImGui panels get ported onto from GUI-4 on.
add_executable(mediaserver_gui_preview
    tools/ui_preview.cpp
)
target_link_libraries(mediaserver_gui_preview
    PRIVATE
        MediaServer::Gui
)
mediaserver_set_project_options(mediaserver_gui_preview)
set_target_properties(mediaserver_gui_preview PROPERTIES FOLDER "tools")

if(BUILD_TESTING)
    add_executable(mediaserver_gui_render_tests
        tests/ui_render_tests.cpp
    )
    target_link_libraries(mediaserver_gui_render_tests
        PRIVATE
            MediaServer::Gui
            MediaServer::Rhi
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_gui_render_tests)

    add_test(
        NAME mediaserver.gui_render
        COMMAND mediaserver_gui_render_tests
    )
    # A GPU but no display: the UI draws into an offscreen target and reads it
    # back, exactly as the scene and output suites do.
    set_tests_properties(mediaserver.gui_render PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_gui_render_tests PROPERTIES FOLDER "tests")

    # The widget layer is pure logic - input in, draw list and interaction
    # results out - so it needs no GPU and no display. It drives synthetic input
    # against an empty (fontless) atlas and asserts the interaction outcomes.
    add_executable(mediaserver_gui_widget_tests
        tests/widget_tests.cpp
    )
    target_link_libraries(mediaserver_gui_widget_tests
        PRIVATE
            MediaServer::Gui
    )
    mediaserver_set_project_options(mediaserver_gui_widget_tests)

    add_test(
        NAME mediaserver.gui_widget
        COMMAND mediaserver_gui_widget_tests
    )
    set_target_properties(mediaserver_gui_widget_tests PROPERTIES FOLDER "tests")

    # The layout solver is arithmetic over a tree: no GPU, no display, and no
    # input either. Its suite asserts solved rectangles directly.
    add_executable(mediaserver_gui_layout_tests
        tests/layout_tests.cpp
    )
    target_link_libraries(mediaserver_gui_layout_tests
        PRIVATE
            MediaServer::Gui
    )
    mediaserver_set_project_options(mediaserver_gui_layout_tests)

    add_test(
        NAME mediaserver.gui_layout
        COMMAND mediaserver_gui_layout_tests
    )
    set_target_properties(mediaserver_gui_layout_tests PROPERTIES FOLDER "tests")

    # Rectangle cuts, the form cursor and the scroll region: the layout steps
    # below the solver. Arithmetic plus synthetic wheel input, so no GPU and no
    # display.
    add_executable(mediaserver_gui_form_layout_tests
        tests/form_layout_tests.cpp
    )
    target_link_libraries(mediaserver_gui_form_layout_tests
        PRIVATE
            MediaServer::Gui
    )
    mediaserver_set_project_options(mediaserver_gui_form_layout_tests)

    add_test(
        NAME mediaserver.gui_form_layout
        COMMAND mediaserver_gui_form_layout_tests
    )
    set_target_properties(mediaserver_gui_form_layout_tests PROPERTIES FOLDER "tests")

    # The dock tree is where rearranging goes wrong quietly - a collapse that
    # orphans a node, a drop that loses a panel - so its suite drives the tree
    # operations directly, with no drawing and no pointer.
    add_executable(mediaserver_gui_dock_tests
        tests/dock_tests.cpp
    )
    target_link_libraries(mediaserver_gui_dock_tests
        PRIVATE
            MediaServer::Gui
    )
    mediaserver_set_project_options(mediaserver_gui_dock_tests)

    add_test(
        NAME mediaserver.gui_dock
        COMMAND mediaserver_gui_dock_tests
    )
    set_target_properties(mediaserver_gui_dock_tests PROPERTIES FOLDER "tests")

    # Event translation is the one part of windowing worth testing on its own,
    # and it needs no window: the suite synthesizes SDL events and asserts what
    # the widget layer would have seen. It links SDL only to build the events.
    add_executable(mediaserver_gui_window_tests
        tests/window_tests.cpp
    )
    target_link_libraries(mediaserver_gui_window_tests
        PRIVATE
            MediaServer::Gui
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_gui_window_tests)

    add_test(
        NAME mediaserver.gui_window
        COMMAND mediaserver_gui_window_tests
    )
    set_tests_properties(mediaserver.gui_window PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_gui_window_tests PROPERTIES FOLDER "tests")
endif()
