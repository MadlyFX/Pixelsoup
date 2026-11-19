# Only the test executable names SDL3 (through the rhi device it drives); the
# render library itself reaches the backend through MediaServer::Rhi and names
# no platform type. Found here anyway so this directory does not depend on
# engine/rhi having been configured first for the imported target to exist.
find_package(SDL3 CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenEXR CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)
add_library(mediaserver_bc7enc STATIC ../../third_party/bc7enc/bc7enc.cpp)
target_include_directories(mediaserver_bc7enc SYSTEM PUBLIC ../../third_party/bc7enc)
set_target_properties(mediaserver_bc7enc PROPERTIES FOLDER "third_party")

# Autodesk FBX reader for model import. Compiled as C, without this project's
# warnings, and private to the render library.
add_library(mediaserver_ufbx STATIC ../../third_party/ufbx/ufbx.c)
target_include_directories(mediaserver_ufbx SYSTEM PUBLIC ../../third_party/ufbx)
set_target_properties(mediaserver_ufbx PROPERTIES FOLDER "third_party")

add_library(mediaserver_render STATIC
    src/render_conformance.cpp
    src/bake_texture_decode.cpp
    src/baked_lighting.cpp
    src/probe_cache.cpp
    src/ies_profile.cpp
    src/ies_cache.cpp
    src/ltc_asset.cpp
    src/scene_renderer.cpp
    src/scene_renderer_environment.cpp
    src/scene_renderer_gather.cpp
    src/scene_renderer_lighting.cpp
    src/scene_renderer_passes.cpp
    src/scene_renderer_render.cpp
    src/scene_renderer_residency.cpp
    src/scene_renderer_support.cpp
    src/geometry_arena.cpp
    src/geometry_residency.cpp
    src/geometry_visibility.cpp
    src/output_transform.cpp
    src/scene_mixer.cpp
    src/inspection_view.cpp
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
    src/exr_decode.cpp
    src/ffmpeg_source.cpp
    src/d3d12_video_halve.cpp
    src/model_mips.cpp
    src/model_compression.cpp
    src/model_package.cpp
    src/model_ingest_queue.cpp
    src/linked_scene_ingest.cpp
    src/director_link_service.cpp
    src/blender_discovery.cpp
    src/linked_shot.cpp
    src/linked_stage_context.cpp
    src/linked_publication.cpp
    src/texture_streaming.cpp
    src/model_cache.cpp
    src/model_loader.cpp
    src/model_loader_fbx.cpp
    src/model_report.cpp
    src/model_animation.cpp
    src/projection_mesh_loader.cpp
    src/hybrid_scene_renderer.cpp
    src/environment_cache.cpp
    src/display_transform.cpp
    src/display_lut_cache.cpp
    src/environment_prefilter.cpp
    src/frame_graph.cpp
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
        MediaServer::Assets
        MediaServer::Net
        MediaServer::Shaders
        Threads::Threads
        mikktspace::mikktspace
        mediaserver_bc7enc
        mediaserver_ufbx
        OpenEXR::OpenEXR
        OpenColorIO::OpenColorIO
)

# Particle effects need the ajfx runtime, which only some builds have (see the
# root CMakeLists.txt). With it the renderer hosts ajfx on this RHI; without
# it the same renderer entry points count every placed effect as unsupported.
# MEDIASERVER_HAS_AJFX is PUBLIC because the renderer's own header differs.
if(TARGET ajfx::ajfx)
    target_sources(mediaserver_render PRIVATE
        src/scene_renderer_particles.cpp
        src/particle_device.cpp
        src/particle_package_cache.cpp
        src/particle_pass.cpp
    )
    target_compile_definitions(mediaserver_render PUBLIC MEDIASERVER_HAS_AJFX=1)
    target_link_libraries(mediaserver_render
        PUBLIC
            # The particle device implements ajfx's GPU interface, so ajfx's
            # headers are part of this library's own.
            ajfx::ajfx
        PRIVATE
            # Rigid physics emitters, stepped off the frame path by ParticlePass.
            ajfx::rigid
    )
else()
    target_sources(mediaserver_render PRIVATE
        src/scene_renderer_particles_unavailable.cpp
    )
endif()

mediaserver_set_project_options(mediaserver_render)
mediaserver_set_deterministic_float_options(mediaserver_render)
mediaserver_disable_rtti(mediaserver_render)

set_target_properties(mediaserver_render PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_linked_environment_tests tests/linked_environment_tests.cpp)
    target_link_libraries(mediaserver_linked_environment_tests PRIVATE MediaServer::Render MediaServer::Show MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_linked_environment_tests)
    add_executable(mediaserver_linked_publication_tests tests/linked_publication_tests.cpp)
    target_link_libraries(mediaserver_linked_publication_tests PRIVATE MediaServer::Render MediaServer::Show MediaServer::Persist MediaServer::Assets)
    mediaserver_set_project_options(mediaserver_linked_publication_tests)
    add_test(NAME mediaserver.linked_publication COMMAND mediaserver_linked_publication_tests
        "${PROJECT_SOURCE_DIR}/tests/fixtures/blender-scene-link" "${CMAKE_CURRENT_BINARY_DIR}/publication-test")
    set_tests_properties(mediaserver.linked_publication PROPERTIES TIMEOUT 90)
    add_executable(mediaserver_linked_qualification_tests tests/linked_qualification_tests.cpp)
    target_link_libraries(mediaserver_linked_qualification_tests PRIVATE MediaServer::Render MediaServer::Net
        MediaServer::Show MediaServer::Persist MediaServer::Assets MediaServer::Cmd)
    mediaserver_set_project_options(mediaserver_linked_qualification_tests)
    set_target_properties(mediaserver_linked_qualification_tests PROPERTIES FOLDER "tests")
    # --gpu runs every scenario of the plain run and then renders the published
    # revision in the Advanced D3D12 renderer, so it is the one entry where
    # that renderer exists.
    if(WIN32)
        set(_linked_qualification_mode --gpu)
    else()
        set(_linked_qualification_mode)
    endif()
    add_test(NAME mediaserver.linked_qualification COMMAND mediaserver_linked_qualification_tests
        "${PROJECT_SOURCE_DIR}/tests/fixtures/blender-scene-link" "${CMAKE_CURRENT_BINARY_DIR}/qualification-evidence"
        ${_linked_qualification_mode})
    set_tests_properties(mediaserver.linked_qualification PROPERTIES TIMEOUT 300)
    add_executable(mediaserver_blender_publisher_host tests/blender_publisher_host.cpp)
    target_link_libraries(mediaserver_blender_publisher_host PRIVATE MediaServer::Render MediaServer::Cmd MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_blender_publisher_host)
    set_target_properties(mediaserver_blender_publisher_host PROPERTIES FOLDER "tests")

    add_executable(mediaserver_linked_look_tests tests/linked_look_tests.cpp)
    add_executable(mediaserver_linked_animation_stage_tests tests/linked_animation_stage_tests.cpp)
    target_link_libraries(mediaserver_linked_animation_stage_tests PRIVATE MediaServer::Render MediaServer::Show MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_linked_animation_stage_tests)
    target_link_libraries(mediaserver_linked_look_tests PRIVATE MediaServer::Render MediaServer::Show MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_linked_look_tests)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME mediaserver.animation_stage_fixture COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/blender/test_animation_stage.py" --fixture "${CMAKE_CURRENT_BINARY_DIR}/animation-fixture")
        set_tests_properties(mediaserver.animation_stage_fixture PROPERTIES FIXTURES_SETUP animation_stage)
        add_test(NAME mediaserver.animation_stage_unit COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/blender/test_animation_stage.py")
        add_test(NAME mediaserver.linked_animation_stage COMMAND mediaserver_linked_animation_stage_tests
            "${CMAKE_CURRENT_BINARY_DIR}/animation-fixture" "${CMAKE_CURRENT_BINARY_DIR}/animation-evidence")
        add_test(NAME mediaserver.linked_animation_stage_gpu COMMAND mediaserver_linked_animation_stage_tests
            "${CMAKE_CURRENT_BINARY_DIR}/animation-fixture" "${CMAKE_CURRENT_BINARY_DIR}/animation-gpu-evidence" --gpu)
        # Each pair ingests one fixture manifest, which keeps its package cache beside
        # it. Two processes filling a cold cache collide, so a pair runs one at a time.
        set_tests_properties(mediaserver.linked_animation_stage mediaserver.linked_animation_stage_gpu PROPERTIES FIXTURES_REQUIRED animation_stage RESOURCE_LOCK animation-fixture TIMEOUT 120)
        add_test(NAME mediaserver.look_fixture COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/blender/test_materials_lighting.py" --fixture "${CMAKE_CURRENT_BINARY_DIR}/look-fixture")
        set_tests_properties(mediaserver.look_fixture PROPERTIES FIXTURES_SETUP look)
        add_test(NAME mediaserver.look_unit COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/blender/test_materials_lighting.py")
        add_test(NAME mediaserver.linked_look COMMAND mediaserver_linked_look_tests
            "${CMAKE_CURRENT_BINARY_DIR}/look-fixture" "${CMAKE_CURRENT_BINARY_DIR}/look-evidence")
        add_test(NAME mediaserver.linked_look_gpu COMMAND mediaserver_linked_look_tests
            "${CMAKE_CURRENT_BINARY_DIR}/look-fixture" "${CMAKE_CURRENT_BINARY_DIR}/look-gpu-evidence" --gpu)
        set_tests_properties(mediaserver.linked_look mediaserver.linked_look_gpu PROPERTIES FIXTURES_REQUIRED look RESOURCE_LOCK look-fixture TIMEOUT 120)
        add_test(NAME mediaserver.environment_fixture COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/blender/test_environment_scale.py" --fixture "${CMAKE_CURRENT_BINARY_DIR}/environment-fixture")
        set_tests_properties(mediaserver.environment_fixture PROPERTIES FIXTURES_SETUP environment)
        add_test(NAME mediaserver.environment_unit COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/blender/test_environment_scale.py")
        add_test(NAME mediaserver.linked_environment COMMAND mediaserver_linked_environment_tests
            "${CMAKE_CURRENT_BINARY_DIR}/environment-fixture" "${CMAKE_CURRENT_BINARY_DIR}/environment-evidence")
        add_test(NAME mediaserver.linked_environment_gpu COMMAND mediaserver_linked_environment_tests
            "${CMAKE_CURRENT_BINARY_DIR}/environment-fixture" "${CMAKE_CURRENT_BINARY_DIR}/environment-gpu-evidence" --gpu)
        set_tests_properties(mediaserver.linked_environment mediaserver.linked_environment_gpu PROPERTIES FIXTURES_REQUIRED environment RESOURCE_LOCK environment-fixture TIMEOUT 120)
        add_test(NAME mediaserver.blender_publisher_unit
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/blender/test_publisher_core.py")
        # The rest of the publisher that runs without Blender: discovery records,
        # image sizing, large-mesh splitting and tangent export.
        foreach(_publisher_suite IN ITEMS discovery image_resize large_meshes shading_export)
            add_test(NAME mediaserver.blender_${_publisher_suite}_unit COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/blender/test_${_publisher_suite}.py")
        endforeach()
    endif()
    add_executable(mediaserver_director_link_service_tests tests/director_link_service_tests.cpp)
    target_link_libraries(mediaserver_director_link_service_tests PRIVATE MediaServer::Render MediaServer::Net MediaServer::Assets MediaServer::Cmd)
    mediaserver_set_project_options(mediaserver_director_link_service_tests)
    add_test(NAME mediaserver.director_link_service COMMAND mediaserver_director_link_service_tests "${PROJECT_SOURCE_DIR}/tests/fixtures/blender-scene-link")
    set_tests_properties(mediaserver.director_link_service PROPERTIES TIMEOUT 120)
    add_executable(mediaserver_linked_scene_ingest_tests tests/linked_scene_ingest_tests.cpp)
    target_link_libraries(mediaserver_linked_scene_ingest_tests PRIVATE MediaServer::Render MediaServer::Assets MediaServer::Cmd)
    mediaserver_set_project_options(mediaserver_linked_scene_ingest_tests)
    add_test(NAME mediaserver.linked_scene_ingest COMMAND mediaserver_linked_scene_ingest_tests "${PROJECT_SOURCE_DIR}/tests/fixtures/blender-scene-link")
    add_executable(mediaserver_render_model_animation_tests tests/model_animation_tests.cpp)
    target_link_libraries(mediaserver_render_model_animation_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_model_animation_tests)
    mediaserver_set_deterministic_float_options(mediaserver_render_model_animation_tests)
    add_test(NAME mediaserver.model_animation COMMAND mediaserver_render_model_animation_tests)
    add_test(NAME mediaserver.model_animation_gpu COMMAND mediaserver_render_model_animation_tests --gpu)
    set_tests_properties(mediaserver.model_animation_gpu PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(mediaserver_frame_timing_tests tests/frame_timing_tests.cpp)
    target_link_libraries(mediaserver_frame_timing_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_frame_timing_tests)
    add_test(NAME mediaserver.frame_timing COMMAND mediaserver_frame_timing_tests)
    set_tests_properties(mediaserver.frame_timing PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(mediaserver_facility_color_tests tests/facility_color_tests.cpp)
    target_link_libraries(mediaserver_facility_color_tests PRIVATE MediaServer::Render MediaServer::Topology OpenEXR::OpenEXR OpenColorIO::OpenColorIO SDL3::SDL3)
    mediaserver_set_project_options(mediaserver_facility_color_tests)
    add_test(NAME mediaserver.facility_color COMMAND mediaserver_facility_color_tests)
    add_test(NAME mediaserver.facility_color_gpu COMMAND mediaserver_facility_color_tests --gpu)
    set_tests_properties(mediaserver.facility_color_gpu PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(mediaserver_physical_sky_tests tests/physical_sky_tests.cpp)
    target_link_libraries(mediaserver_physical_sky_tests PRIVATE MediaServer::Render MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_physical_sky_tests)
    mediaserver_set_deterministic_float_options(mediaserver_physical_sky_tests)
    add_test(NAME mediaserver.physical_sky_model COMMAND mediaserver_physical_sky_tests)
    add_test(NAME mediaserver.physical_sky_gpu COMMAND mediaserver_physical_sky_tests --gpu)
    set_tests_properties(mediaserver.physical_sky_gpu PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(mediaserver_render_reflection_tests tests/reflection_tests.cpp)
    target_link_libraries(mediaserver_render_reflection_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_reflection_tests)
    add_test(NAME mediaserver.render_reflections COMMAND mediaserver_render_reflection_tests)
    set_tests_properties(mediaserver.render_reflections PROPERTIES SKIP_RETURN_CODE 77)
    set_target_properties(mediaserver_render_reflection_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_render_area_lighting_tests tests/area_lighting_tests.cpp)
    target_link_libraries(mediaserver_render_area_lighting_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_area_lighting_tests)
    add_test(NAME mediaserver.render_area_lighting COMMAND mediaserver_render_area_lighting_tests)
    set_tests_properties(mediaserver.render_area_lighting PROPERTIES SKIP_RETURN_CODE 77)
    set_target_properties(mediaserver_render_area_lighting_tests PROPERTIES FOLDER "tests")
    add_executable(mediaserver_render_ies_lighting_tests tests/ies_lighting_tests.cpp)
    target_link_libraries(mediaserver_render_ies_lighting_tests PRIVATE MediaServer::Render)
    mediaserver_set_project_options(mediaserver_render_ies_lighting_tests)
    add_test(NAME mediaserver.render_ies_lighting COMMAND mediaserver_render_ies_lighting_tests)
    set_tests_properties(mediaserver.render_ies_lighting PROPERTIES SKIP_RETURN_CODE 77)
    set_target_properties(mediaserver_render_ies_lighting_tests PROPERTIES FOLDER "tests")
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
    set_tests_properties(mediaserver.render_shadow_stability PROPERTIES SKIP_RETURN_CODE 77)
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

    add_executable(mediaserver_render_inspection_view_tests
        tests/inspection_view_tests.cpp
    )
    target_link_libraries(mediaserver_render_inspection_view_tests
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_inspection_view_tests)
    add_test(
        NAME mediaserver.render_inspection_view
        COMMAND mediaserver_render_inspection_view_tests
    )
    set_tests_properties(mediaserver.render_inspection_view PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_render_inspection_view_tests
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
    # Real projection meshes from a four-surface curved LED wall, loaded the
    # way a node loads them.
    target_compile_definitions(mediaserver_render_icvfx_tests PRIVATE
        MEDIASERVER_FOUR_LED_WALL_FIXTURE="${PROJECT_SOURCE_DIR}/tests/fixtures/four-led-wall")
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

    # ajfx particles on this RHI: the same kernels on ajfx's own D3D12 device
    # and on the RHI must leave identical particle records. Compiles the
    # kernels with ajfx's runtime compiler, so it needs dxcompiler.dll beside
    # it; the media server itself only ever loads precompiled kernels.
    if(TARGET ajfx::d3d12)
        add_executable(mediaserver_render_particle_device_tests
            tests/particle_device_tests.cpp
        )
        target_link_libraries(mediaserver_render_particle_device_tests
            PRIVATE MediaServer::Render ajfx::d3d12
        )
        mediaserver_set_project_options(mediaserver_render_particle_device_tests)
        ajfx_copy_dxc(mediaserver_render_particle_device_tests)
        add_test(
            NAME mediaserver.render_particle_device
            COMMAND mediaserver_render_particle_device_tests
        )
        set_tests_properties(mediaserver.render_particle_device PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
        )
        set_target_properties(mediaserver_render_particle_device_tests PROPERTIES
            FOLDER "tests"
        )

        # The particle pass: drawn where the camera says, tested against the
        # scene's depth, and held rather than drawn while a seek catches up.
        add_executable(mediaserver_render_particle_pass_tests
            tests/particle_pass_tests.cpp
        )
        target_link_libraries(mediaserver_render_particle_pass_tests
            PRIVATE MediaServer::Render ajfx::d3d12 ajfx::rigid
        )
        mediaserver_set_project_options(mediaserver_render_particle_pass_tests)
        ajfx_copy_dxc(mediaserver_render_particle_pass_tests)
        add_test(
            NAME mediaserver.render_particle_pass
            COMMAND mediaserver_render_particle_pass_tests
        )
        set_tests_properties(mediaserver.render_particle_pass PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
        )
        set_target_properties(mediaserver_render_particle_pass_tests PROPERTIES
            FOLDER "tests"
        )

        # Effects placed in a scene: loaded off the frame path, held from
        # readiness until drawn at the exact frame, alike on every slice.
        add_executable(mediaserver_render_scene_particle_tests
            tests/scene_particle_tests.cpp
        )
        target_link_libraries(mediaserver_render_scene_particle_tests
            PRIVATE MediaServer::Render ajfx::d3d12
        )
        mediaserver_set_project_options(mediaserver_render_scene_particle_tests)
        ajfx_copy_dxc(mediaserver_render_scene_particle_tests)
        add_test(
            NAME mediaserver.render_scene_particles
            COMMAND mediaserver_render_scene_particle_tests
        )
        set_tests_properties(mediaserver.render_scene_particles PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
        )
        set_target_properties(mediaserver_render_scene_particle_tests PROPERTIES
            FOLDER "tests"
        )
    endif()

    # A build without ajfx: started effects are refused readiness with the
    # reason, and nothing else about the renderer changes.
    if(NOT TARGET ajfx::ajfx)
        add_executable(mediaserver_render_scene_particle_unavailable_tests
            tests/scene_particle_unavailable_tests.cpp
        )
        target_link_libraries(mediaserver_render_scene_particle_unavailable_tests
            PRIVATE MediaServer::Render
        )
        mediaserver_set_project_options(mediaserver_render_scene_particle_unavailable_tests)
        add_test(
            NAME mediaserver.render_scene_particles_unavailable
            COMMAND mediaserver_render_scene_particle_unavailable_tests
        )
        set_tests_properties(mediaserver.render_scene_particles_unavailable PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
        )
        set_target_properties(mediaserver_render_scene_particle_unavailable_tests PROPERTIES
            FOLDER "tests"
        )
    endif()

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
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
                ${FFMPEG_LIBRARIES}
    )
    target_include_directories(mediaserver_render_media_video_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(mediaserver_render_media_video_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
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

    # Built but not registered with CTest: streams a ~30M-triangle scene of
    # distinct parts into the geometry arena and times frames on D3D12.
    add_executable(mediaserver_render_geometry_benchmark
        tests/geometry_benchmark.cpp
    )
    target_link_libraries(mediaserver_render_geometry_benchmark
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_geometry_benchmark)
    set_target_properties(mediaserver_render_geometry_benchmark PROPERTIES
        FOLDER "tests")

    # Built but not registered with CTest: reports the device-local memory a
    # scene renderer, hybrid renderer or compositor executor holds.
    add_executable(mediaserver_render_vram_benchmark
        tests/vram_benchmark.cpp
    )
    target_link_libraries(mediaserver_render_vram_benchmark
        PRIVATE MediaServer::Render MediaServer::Rhi SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_render_vram_benchmark)
    set_target_properties(mediaserver_render_vram_benchmark PROPERTIES
        FOLDER "tests")

    # Model loading is CPU-only too: it parses geometry, material parts, and
    # bounds into a ModelAsset with no GPU.
    add_executable(mediaserver_blender_scene_reference_tests
        tests/blender_scene_reference_tests.cpp)
    target_link_libraries(mediaserver_blender_scene_reference_tests
        PRIVATE MediaServer::Render MediaServer::Persist MediaServer::Show)
    mediaserver_set_project_options(mediaserver_blender_scene_reference_tests)
    add_test(NAME mediaserver.blender_scene_reference
        COMMAND mediaserver_blender_scene_reference_tests
            "${PROJECT_SOURCE_DIR}/tests/fixtures/blender-scene-link"
            "${CMAKE_CURRENT_BINARY_DIR}/blender-scene-reference")
    set_target_properties(mediaserver_blender_scene_reference_tests PROPERTIES FOLDER "tests")

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

    add_executable(mediaserver_render_model_report_tests
        tests/model_report_tests.cpp
    )
    target_link_libraries(mediaserver_render_model_report_tests
        PRIVATE MediaServer::Render
    )
    mediaserver_set_project_options(mediaserver_render_model_report_tests)
    add_test(
        NAME mediaserver.render_model_report
        COMMAND mediaserver_render_model_report_tests
    )
    set_target_properties(mediaserver_render_model_report_tests PROPERTIES
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
