find_package(SDL3 CONFIG REQUIRED)
find_package(freetype CONFIG REQUIRED)

add_library(mediaserver_gui STATIC
    src/ui_renderer.cpp
    src/text.cpp
)
add_library(MediaServer::Gui ALIAS mediaserver_gui)

target_include_directories(mediaserver_gui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Render is PUBLIC because the UI renderer's interface names render::RenderError.
# Shaders (the embedded UI bytecode) and FreeType (glyph rasterization) are
# implementation details nothing above this seam sees.
target_link_libraries(mediaserver_gui
    PUBLIC
        MediaServer::Core
        MediaServer::Rhi
        MediaServer::Render
    PRIVATE
        MediaServer::Shaders
        freetype
)

mediaserver_set_project_options(mediaserver_gui)
mediaserver_disable_rtti(mediaserver_gui)

set_target_properties(mediaserver_gui PROPERTIES FOLDER "engine")

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
endif()
