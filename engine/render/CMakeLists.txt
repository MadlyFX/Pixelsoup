# Only the test executable names SDL3 (through the rhi device it drives); the
# render library itself reaches the backend through MediaServer::Rhi and names
# no platform type. Found here anyway so this directory does not depend on
# engine/rhi having been configured first for the imported target to exist.
find_package(SDL3 CONFIG REQUIRED)

add_library(mediaserver_render STATIC
    src/scene_renderer.cpp
    src/output_transform.cpp
    src/scene_mixer.cpp
    src/projection_mesh_warp.cpp
    src/mapped_scene.cpp
    src/gaussian_blur.cpp
    src/compositor_scene_executor.cpp
    src/media_cache.cpp
    src/media_source.cpp
    src/ffmpeg_source.cpp
    src/model_loader.cpp
    src/projection_mesh_loader.cpp
)
add_library(MediaServer::Render ALIAS mediaserver_render)

# FFmpeg decodes real video files behind the MediaSource seam. Private: only
# ffmpeg_source.cpp names it, and nothing above this seam. vcpkg ships a
# FindFFMPEG module exposing these variables.
find_package(FFMPEG REQUIRED)
target_include_directories(mediaserver_render PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(mediaserver_render PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mediaserver_render PRIVATE ${FFMPEG_LIBRARIES})

# Header-only vendored loaders, all private to this library: stb_image for the
# image-sequence path, and cgltf + tiny_obj_loader for model import. Each has its
# single implementation compiled in exactly one translation unit here.
find_path(STB_INCLUDE_DIRS "stb_image.h")
find_path(CGLTF_INCLUDE_DIRS "cgltf.h")
find_path(TINYOBJ_INCLUDE_DIRS "tiny_obj_loader.h")
target_include_directories(mediaserver_render PRIVATE
    ${STB_INCLUDE_DIRS}
    ${CGLTF_INCLUDE_DIRS}
    ${TINYOBJ_INCLUDE_DIRS})

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
        MediaServer::Compositor
        MediaServer::Output
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

    add_executable(mediaserver_render_projection_mesh_warp_tests
        tests/projection_mesh_warp_tests.cpp
    )
    target_link_libraries(mediaserver_render_projection_mesh_warp_tests
        PRIVATE MediaServer::Render MediaServer::Output MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(
        mediaserver_render_projection_mesh_warp_tests
    )
    add_test(
        NAME mediaserver.render_projection_mesh_warp
        COMMAND mediaserver_render_projection_mesh_warp_tests
    )
    set_tests_properties(mediaserver.render_projection_mesh_warp PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_projection_mesh_warp_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_render_scene_mixer_tests
        tests/scene_mixer_tests.cpp
    )
    target_link_libraries(mediaserver_render_scene_mixer_tests
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_scene_mixer_tests)
    add_test(
        NAME mediaserver.render_scene_mixer
        COMMAND mediaserver_render_scene_mixer_tests
    )
    set_tests_properties(mediaserver.render_scene_mixer PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_scene_mixer_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_render_gaussian_blur_tests
        tests/gaussian_blur_tests.cpp
    )
    target_link_libraries(mediaserver_render_gaussian_blur_tests
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_gaussian_blur_tests)
    add_test(
        NAME mediaserver.render_gaussian_blur
        COMMAND mediaserver_render_gaussian_blur_tests
    )
    set_tests_properties(mediaserver.render_gaussian_blur PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_gaussian_blur_tests
        PROPERTIES FOLDER "tests"
    )

    # Pure scene/graph planning: no device or display is opened. Kept separate
    # from the pixel suite so graph isolation remains tested on GPU-less hosts.
    add_executable(mediaserver_render_compositor_plan_tests
        tests/compositor_scene_plan_tests.cpp
    )
    target_link_libraries(mediaserver_render_compositor_plan_tests
        PRIVATE MediaServer::Render MediaServer::Compositor
    )
    mediaserver_set_project_options(mediaserver_render_compositor_plan_tests)
    add_test(
        NAME mediaserver.render_compositor_plan
        COMMAND mediaserver_render_compositor_plan_tests
    )
    set_target_properties(mediaserver_render_compositor_plan_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_render_compositor_scene_tests
        tests/compositor_scene_executor_tests.cpp
    )
    target_link_libraries(mediaserver_render_compositor_scene_tests
        PRIVATE
            MediaServer::Render
            MediaServer::Compositor
            MediaServer::Rhi
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_compositor_scene_tests)
    add_test(
        NAME mediaserver.render_compositor_scene
        COMMAND mediaserver_render_compositor_scene_tests
    )
    set_tests_properties(mediaserver.render_compositor_scene PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_compositor_scene_tests
        PROPERTIES FOLDER "tests"
    )

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

    # A tiny embedded MP4 drives the real FFmpeg path without relying on a
    # machine-local sample or an ffmpeg executable at test time.
    add_executable(mediaserver_render_media_video_tests
        tests/media_video_tests.cpp
    )
    target_link_libraries(mediaserver_render_media_video_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_media_video_tests)
    add_test(
        NAME mediaserver.render_media_video
        COMMAND mediaserver_render_media_video_tests
    )
    set_target_properties(mediaserver_render_media_video_tests PROPERTIES
        FOLDER "tests")

    # Built but intentionally not registered with CTest: this consumes an
    # operator-supplied real clip and enforces performance thresholds only when
    # explicitly requested, preferably from the Release preset.
    add_executable(mediaserver_render_media_video_benchmark
        tests/media_video_benchmark.cpp
    )
    target_link_libraries(mediaserver_render_media_video_benchmark
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_media_video_benchmark)
    set_target_properties(mediaserver_render_media_video_benchmark PROPERTIES
        FOLDER "tests")

    # Model loading is CPU-only too: it parses geometry, material parts, and
    # bounds into a ModelAsset with no GPU.
    add_executable(mediaserver_render_model_loader_tests
        tests/model_loader_tests.cpp
    )
    target_link_libraries(mediaserver_render_model_loader_tests
        PRIVATE
            MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_model_loader_tests)

    add_test(
        NAME mediaserver.render_model_loader
        COMMAND mediaserver_render_model_loader_tests
    )
    set_target_properties(mediaserver_render_model_loader_tests PROPERTIES
        FOLDER "tests")

    add_executable(mediaserver_render_projection_mesh_loader_tests
        tests/projection_mesh_loader_tests.cpp
    )
    target_link_libraries(mediaserver_render_projection_mesh_loader_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(
        mediaserver_render_projection_mesh_loader_tests
    )
    add_test(
        NAME mediaserver.render_projection_mesh_loader
        COMMAND mediaserver_render_projection_mesh_loader_tests
    )
    set_target_properties(mediaserver_render_projection_mesh_loader_tests
        PROPERTIES FOLDER "tests"
    )
endif()
