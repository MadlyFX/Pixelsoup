# Only the test executable names SDL3 (through the rhi device it drives); the
# render library itself reaches the backend through MediaServer::Rhi and names
# no platform type. Found here anyway so this directory does not depend on
# engine/rhi having been configured first for the imported target to exist.
find_package(SDL3 CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_library(mediaserver_bc7enc STATIC ../../third_party/bc7enc/bc7enc.cpp)
target_include_directories(mediaserver_bc7enc SYSTEM PUBLIC ../../third_party/bc7enc)
set_target_properties(mediaserver_bc7enc PROPERTIES FOLDER "third_party")

add_library(mediaserver_render STATIC
    src/render_conformance.cpp
    src/bake_texture_decode.cpp
    src/baked_lighting.cpp
    src/probe_cache.cpp
    src/scene_renderer.cpp
    src/geometry_visibility.cpp
    src/output_transform.cpp
    src/scene_mixer.cpp
    src/projection_mesh_warp.cpp
    src/mapped_scene.cpp
    src/gaussian_blur.cpp
    src/icvfx.cpp
    src/icvfx_stage.cpp
    src/spatial_blur.cpp
    src/layer_keyer.cpp
    src/mask_raster.cpp
    src/layer_matte.cpp
    src/compositor_scene_executor.cpp
    src/media_cache.cpp
    src/media_source.cpp
    src/ffmpeg_source.cpp
    src/model_mips.cpp
    src/model_compression.cpp
    src/model_package.cpp
    src/model_ingest_queue.cpp
    src/texture_streaming.cpp
    src/model_cache.cpp
    src/model_loader.cpp
    src/projection_mesh_loader.cpp
    src/hybrid_scene_renderer.cpp
    src/environment_cache.cpp
    src/display_transform.cpp
    src/display_lut_cache.cpp
    src/environment_prefilter.cpp
    src/render_graph.cpp
    src/render_graph_executor.cpp
    src/graph_transient_pool.cpp
    src/scene_bridge.cpp
    src/bloom.cpp
    src/bloom_chain.cpp
    src/froxel_grid.cpp
    src/volumetric_fog.cpp
    src/shadow_atlas.cpp
    src/slice_apron.cpp
    src/temporal_history.cpp
    src/ambient_occlusion.cpp
    src/spatial_antialias.cpp
    src/plate_depth_of_field.cpp
    src/object_motion_blur.cpp
    src/temporal_resolve.cpp
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
# image-sequence path, cgltf + tiny_obj_loader for model import, and the glTF
# reference tangent-space generator. Each header-only loader has its single
# implementation compiled in exactly one translation unit here.
find_path(STB_INCLUDE_DIRS "stb_image.h")
find_path(CGLTF_INCLUDE_DIRS "cgltf.h")
find_path(TINYOBJ_INCLUDE_DIRS "tiny_obj_loader.h")
find_package(mikktspace CONFIG REQUIRED)
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
        Threads::Threads
        mikktspace::mikktspace
        mediaserver_bc7enc
)

mediaserver_set_project_options(mediaserver_render)
mediaserver_disable_rtti(mediaserver_render)

set_target_properties(mediaserver_render PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_render_conformance_tests tests/render_conformance_tests.cpp)
    target_link_libraries(mediaserver_render_conformance_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_conformance_tests)
    add_test(NAME mediaserver.render_conformance COMMAND mediaserver_render_conformance_tests)
    set_tests_properties(mediaserver.render_conformance PROPERTIES SKIP_RETURN_CODE 77)
    set_target_properties(mediaserver_render_conformance_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_plate_dof_tests tests/plate_depth_of_field_tests.cpp)
    target_link_libraries(mediaserver_render_plate_dof_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_plate_dof_tests)
    add_test(NAME mediaserver.render_plate_dof COMMAND mediaserver_render_plate_dof_tests)
    set_tests_properties(mediaserver.render_plate_dof PROPERTIES SKIP_RETURN_CODE 77)
    set_target_properties(mediaserver_render_plate_dof_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_object_motion_tests tests/object_motion_blur_tests.cpp)
    target_link_libraries(mediaserver_render_object_motion_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_object_motion_tests)
    add_test(NAME mediaserver.render_object_motion COMMAND mediaserver_render_object_motion_tests)
    set_target_properties(mediaserver_render_object_motion_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_shadow_stability_tests tests/shadow_stability_tests.cpp)
    target_link_libraries(mediaserver_render_shadow_stability_tests PRIVATE MediaServer::Render MediaServer::Shaders)
    mediaserver_set_project_options(mediaserver_render_shadow_stability_tests)
    add_test(NAME mediaserver.render_shadow_stability COMMAND mediaserver_render_shadow_stability_tests)
    set_target_properties(mediaserver_render_shadow_stability_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_baked_lighting_tests tests/baked_lighting_tests.cpp)
    target_link_libraries(mediaserver_render_baked_lighting_tests PRIVATE MediaServer::Render MediaServer::Net)
    mediaserver_set_project_options(mediaserver_render_baked_lighting_tests)
    add_test(NAME mediaserver.render_baked_lighting COMMAND mediaserver_render_baked_lighting_tests)
    set_target_properties(mediaserver_render_baked_lighting_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_material_ingest_tests tests/material_ingest_tests.cpp)
    target_link_libraries(mediaserver_render_material_ingest_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_material_ingest_tests)
    add_test(NAME mediaserver.render_material_ingest COMMAND mediaserver_render_material_ingest_tests)
    set_target_properties(mediaserver_render_material_ingest_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_material_sampling_tests tests/material_sampling_tests.cpp)
    target_link_libraries(mediaserver_render_material_sampling_tests PRIVATE MediaServer::Render MediaServer::Shaders)
    mediaserver_set_project_options(mediaserver_render_material_sampling_tests)
    add_test(NAME mediaserver.render_material_sampling COMMAND mediaserver_render_material_sampling_tests)
    set_target_properties(mediaserver_render_material_sampling_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_render_texture_streaming_tests tests/texture_streaming_tests.cpp)
    target_link_libraries(mediaserver_render_texture_streaming_tests PRIVATE MediaServer::Render MediaServer::Shaders)
    mediaserver_set_project_options(mediaserver_render_texture_streaming_tests)
    add_test(NAME mediaserver.render_texture_streaming COMMAND mediaserver_render_texture_streaming_tests)
    set_tests_properties(mediaserver.render_texture_streaming PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    set_target_properties(mediaserver_render_texture_streaming_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_geometry_tests tests/geometry_visibility_tests.cpp)
    target_link_libraries(mediaserver_render_geometry_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_geometry_tests)
    add_test(NAME mediaserver.render_geometry COMMAND mediaserver_render_geometry_tests)
    set_target_properties(mediaserver_render_geometry_tests PROPERTIES FOLDER "tests")

    # CPU-only contracts for the advanced renderer boundary. This covers graph
    # hazards, stable scene records, temporal holds/resets, slice aprons, and
    # fail-closed capability selection without opening a GPU or a file.
    # Image-based lighting prefilter: analytic diffuse integral, cross-instance
    # bit equality, and construction-time refusal.
    add_executable(mediaserver_render_environment_prefilter_tests
        tests/environment_prefilter_tests.cpp
    )
    target_link_libraries(mediaserver_render_environment_prefilter_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(
        mediaserver_render_environment_prefilter_tests)
    add_test(
        NAME mediaserver.render_environment_prefilter
        COMMAND mediaserver_render_environment_prefilter_tests
    )
    set_tests_properties(mediaserver.render_environment_prefilter PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_environment_prefilter_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_render_temporal_stability_tests tests/temporal_stability_tests.cpp)
    target_link_libraries(mediaserver_render_temporal_stability_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_temporal_stability_tests)
    add_test(NAME mediaserver.render_temporal_stability COMMAND mediaserver_render_temporal_stability_tests)
    set_target_properties(mediaserver_render_temporal_stability_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_render_integration_tests
        tests/renderer_integration_tests.cpp
    )
    target_link_libraries(mediaserver_render_integration_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_integration_tests)
    add_test(
        NAME mediaserver.render_integration
        COMMAND mediaserver_render_integration_tests
    )
    set_target_properties(mediaserver_render_integration_tests PROPERTIES
        FOLDER "tests"
    )

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

    # The headless half of this suite runs everywhere; the composite half
    # needs a device and says so rather than silently passing.
    add_executable(mediaserver_render_icvfx_tests
        tests/icvfx_tests.cpp
    )
    target_link_libraries(mediaserver_render_icvfx_tests
        PRIVATE MediaServer::Render MediaServer::Rhi MediaServer::Output
                SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_icvfx_tests)
    add_test(
        NAME mediaserver.render_icvfx
        COMMAND mediaserver_render_icvfx_tests
    )
    set_tests_properties(mediaserver.render_icvfx PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_icvfx_tests
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

    add_executable(mediaserver_render_spatial_blur_tests
        tests/spatial_blur_tests.cpp
    )
    target_link_libraries(mediaserver_render_spatial_blur_tests
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_spatial_blur_tests)
    add_test(
        NAME mediaserver.render_spatial_blur
        COMMAND mediaserver_render_spatial_blur_tests
    )
    set_tests_properties(mediaserver.render_spatial_blur PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_spatial_blur_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_render_layer_keyer_tests
        tests/layer_keyer_tests.cpp
    )
    target_link_libraries(mediaserver_render_layer_keyer_tests
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_layer_keyer_tests)
    add_test(
        NAME mediaserver.render_layer_keyer
        COMMAND mediaserver_render_layer_keyer_tests
    )
    set_tests_properties(mediaserver.render_layer_keyer PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_layer_keyer_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_render_mask_raster_tests
        tests/mask_raster_tests.cpp
    )
    target_link_libraries(mediaserver_render_mask_raster_tests
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_mask_raster_tests)
    add_test(
        NAME mediaserver.render_mask_raster
        COMMAND mediaserver_render_mask_raster_tests
    )
    set_tests_properties(mediaserver.render_mask_raster PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_mask_raster_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_render_layer_matte_tests
        tests/layer_matte_tests.cpp
    )
    target_link_libraries(mediaserver_render_layer_matte_tests
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_layer_matte_tests)
    add_test(
        NAME mediaserver.render_layer_matte
        COMMAND mediaserver_render_layer_matte_tests
    )
    set_tests_properties(mediaserver.render_layer_matte PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_layer_matte_tests
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

    # Compile-time registry enforcement plus CPU-only identity/depth contract
    # checks for every first-class scene effect.
    add_executable(mediaserver_render_effect_execution_tests
        tests/effect_execution_tests.cpp
    )
    target_link_libraries(mediaserver_render_effect_execution_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_effect_execution_tests)
    add_test(
        NAME mediaserver.render_effect_execution
        COMMAND mediaserver_render_effect_execution_tests
    )
    set_target_properties(mediaserver_render_effect_execution_tests
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

    # Encoded-image memory decoding is pure CPU work and owns no filesystem or
    # device. Keep it isolated so thumbnail payload validation stays runnable
    # on build machines without a display.
    add_executable(mediaserver_render_image_decode_tests
        tests/image_decode_tests.cpp
    )
    target_link_libraries(mediaserver_render_image_decode_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_image_decode_tests)
    add_test(
        NAME mediaserver.render_image_decode
        COMMAND mediaserver_render_image_decode_tests
    )
    set_target_properties(mediaserver_render_image_decode_tests PROPERTIES
        FOLDER "tests"
    )

    # The display transform bakes on the CPU and is checked there: the values
    # a table holds are arithmetic, and a GPU is only how they get sampled.
    add_executable(mediaserver_render_display_transform_tests
        tests/display_transform_tests.cpp
    )
    target_link_libraries(mediaserver_render_display_transform_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(
        mediaserver_render_display_transform_tests)
    add_test(
        NAME mediaserver.render_display_transform
        COMMAND mediaserver_render_display_transform_tests
    )
    set_target_properties(mediaserver_render_display_transform_tests PROPERTIES
        FOLDER "tests"
    )

    # The bloom chain's geometry is pure CPU for the same reason the shadow
    # atlas allocator is: what matters is that two nodes partition the canvas
    # identically, and that is settled before a pixel is filtered.
    add_executable(mediaserver_render_bloom_chain_tests
        tests/bloom_chain_tests.cpp
    )
    target_link_libraries(mediaserver_render_bloom_chain_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_bloom_chain_tests)
    add_test(
        NAME mediaserver.render_bloom_chain
        COMMAND mediaserver_render_bloom_chain_tests
    )
    set_target_properties(mediaserver_render_bloom_chain_tests PROPERTIES
        FOLDER "tests"
    )

    # The froxel grid, for the same reason again: a volume cut differently by
    # two nodes lights a shared world point twice over, and that is decided on
    # the CPU before anything is integrated.
    add_executable(mediaserver_render_froxel_grid_tests
        tests/froxel_grid_tests.cpp
    )
    target_link_libraries(mediaserver_render_froxel_grid_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_froxel_grid_tests)
    add_test(
        NAME mediaserver.render_froxel_grid
        COMMAND mediaserver_render_froxel_grid_tests
    )
    set_target_properties(mediaserver_render_froxel_grid_tests PROPERTIES
        FOLDER "tests"
    )

    # The shadow atlas allocator is pure CPU on purpose: the property that
    # matters is that two nodes reach the same assignment, and that is decided
    # long before anything touches a GPU.
    add_executable(mediaserver_render_shadow_atlas_tests
        tests/shadow_atlas_tests.cpp
    )
    target_link_libraries(mediaserver_render_shadow_atlas_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_shadow_atlas_tests)
    add_test(
        NAME mediaserver.render_shadow_atlas
        COMMAND mediaserver_render_shadow_atlas_tests
    )
    set_target_properties(mediaserver_render_shadow_atlas_tests PROPERTIES
        FOLDER "tests"
    )

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

    add_executable(mediaserver_render_model_cache_tests
        tests/model_cache_tests.cpp
    )
    target_link_libraries(mediaserver_render_model_cache_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_model_cache_tests)
    add_test(
        NAME mediaserver.render_model_cache
        COMMAND mediaserver_render_model_cache_tests
    )
    set_target_properties(mediaserver_render_model_cache_tests PROPERTIES
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

    # CPU-only ownership and scheduling contract for mapped projection meshes:
    # imported bytes stay on bounded background work, failures are explicit-
    # refresh-only, and unchanged procedural arcs retain one allocation.
    add_executable(mediaserver_render_mapped_scene_tests
        tests/mapped_scene_tests.cpp
    )
    target_link_libraries(mediaserver_render_mapped_scene_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_mapped_scene_tests)
    add_test(
        NAME mediaserver.render_mapped_scene
        COMMAND mediaserver_render_mapped_scene_tests
    )
    set_target_properties(mediaserver_render_mapped_scene_tests
        PROPERTIES FOLDER "tests"
    )
endif()
