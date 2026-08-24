# Only the test executable names SDL3 (through the rhi device it drives); the
# render library itself reaches the backend through MediaServer::Rhi and names
# no platform type. Found here anyway so this directory does not depend on
# engine/rhi having been configured first for the imported target to exist.
find_package(SDL3 CONFIG REQUIRED)

add_library(mediaserver_render STATIC
    src/scene_renderer.cpp
    src/output_transform.cpp
    src/media_cache.cpp
    src/media_source.cpp
)
add_library(MediaServer::Render ALIAS mediaserver_render)

# stb_image, header-only, for the spike-grade image-sequence loader. Private:
# only media_source.cpp's implementation names it, and nothing above this seam.
find_path(STB_INCLUDE_DIRS "stb_image.h")
target_include_directories(mediaserver_render PRIVATE ${STB_INCLUDE_DIRS})

target_include_directories(mediaserver_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Scene in, one rendered target out. Scene and Rhi are PUBLIC because both
# appear in this library's own interface - `render()` takes a `scene::Scene`
# and an `rhi::CommandList`. Shaders is PRIVATE: the bytecode it embeds is an
# implementation detail nothing above this seam names.
target_link_libraries(mediaserver_render
    PUBLIC
        MediaServer::Core
        MediaServer::Scene
        MediaServer::Rhi
    PRIVATE
        MediaServer::Shaders
)

mediaserver_set_project_options(mediaserver_render)
mediaserver_disable_rtti(mediaserver_render)

set_target_properties(mediaserver_render PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_render_scene_tests
        tests/scene_renderer_tests.cpp
    )
    target_link_libraries(mediaserver_render_scene_tests
        PRIVATE
            MediaServer::Render
            MediaServer::Rhi
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_scene_tests)

    add_test(
        NAME mediaserver.render_scene
        COMMAND mediaserver_render_scene_tests
    )
    # The renderer needs a GPU but not a display: it draws into an offscreen
    # target and reads it back. The dummy video driver keeps that runnable on a
    # build machine with no desktop session, exactly as the rhi and ui suites do.
    set_tests_properties(mediaserver.render_scene PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_scene_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_render_output_tests
        tests/output_transform_tests.cpp
    )
    target_link_libraries(mediaserver_render_output_tests
        PRIVATE
            MediaServer::Render
            MediaServer::Rhi
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_output_tests)

    add_test(
        NAME mediaserver.render_output
        COMMAND mediaserver_render_output_tests
    )
    # Same as the scene suite: a GPU but no display. The transform draws into an
    # offscreen target and reads it back.
    set_tests_properties(mediaserver.render_output PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_output_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_render_media_tests
        tests/media_render_tests.cpp
    )
    target_link_libraries(mediaserver_render_media_tests
        PRIVATE
            MediaServer::Render
            MediaServer::Rhi
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_media_tests)

    add_test(
        NAME mediaserver.render_media
        COMMAND mediaserver_render_media_tests
    )
    set_tests_properties(mediaserver.render_media PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_media_tests PROPERTIES FOLDER "tests")

    # The decode seam is CPU-only, so this suite needs no GPU and no display: it
    # decodes image sequences through the MediaSource and checks frame-exactness.
    add_executable(mediaserver_render_media_source_tests
        tests/media_source_tests.cpp
    )
    target_link_libraries(mediaserver_render_media_source_tests
        PRIVATE
            MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_media_source_tests)

    add_test(
        NAME mediaserver.render_media_source
        COMMAND mediaserver_render_media_source_tests
    )
    set_target_properties(mediaserver_render_media_source_tests PROPERTIES
        FOLDER "tests")
endif()
