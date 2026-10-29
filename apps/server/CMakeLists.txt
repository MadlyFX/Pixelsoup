find_package(Threads REQUIRED)
find_package(FFMPEG REQUIRED)
if(MEDIASERVER_ENABLE_GUI)
    find_path(STB_INCLUDE_DIRS "stb_image_write.h")
endif()

# Prediction is a Director-owned service compiled with the same reproducible
# arithmetic contract as scene math. The node path never invokes this service.
add_library(mediaserver_camera_prediction STATIC camera_prediction.cpp)
target_link_libraries(mediaserver_camera_prediction PUBLIC
    MediaServer::Scene MediaServer::Track MediaServer::Net)
mediaserver_set_project_options(mediaserver_camera_prediction)
mediaserver_set_deterministic_float_options(mediaserver_camera_prediction)
set_target_properties(mediaserver_camera_prediction PROPERTIES FOLDER "apps")

add_executable(media-server
    main.cpp
    asset_readiness_provider.cpp
    clip_clipboard.cpp
    key_clipboard.cpp
    timeline_lane_authoring.cpp
    plateset_authoring.cpp
    plateset_motion.cpp
    plateset_motion_service.cpp
    timeline_marker_authoring.cpp
    timeline_edit_authoring.cpp
    edit_coalescing_policy.cpp
    ffmpeg_waveform_decoder.cpp
    ffmpeg_proxy_encoder.cpp
    cluster_observability.cpp
    director_documents.cpp
    director_frame_evaluation.cpp
    director_show_loop.cpp
    director_show_status.cpp
    lifecycle_agent.cpp
    managed_process.cpp
    render_machine.cpp
    render_machine_system.cpp
    output_deployment.cpp
    output_window_config.cpp
    playback.cpp
    playback_media.cpp
    playback_runtime.cpp
    transport_commands.cpp
    proxy_generation_service.cpp
    proxy_playback.cpp
    renderer_lockstep.cpp
    tracked_camera_bridge.cpp
    tracker_listener.cpp
    project_asset_import.cpp
    project_assets.cpp
    sync_device_probe.cpp
    system_hardware_probe.cpp
    timeline_playback_cache.cpp
    waveform_generation_service.cpp
)
target_link_libraries(media-server
    PRIVATE
        mediaserver_camera_prediction
        MediaServer::Net
        MediaServer::Persist
        MediaServer::Assets
        MediaServer::Compositor
        MediaServer::Sync
        MediaServer::Topology
        # Camera tracking. The library has no I/O at all; this process supplies
        # the datagrams and is the only one that may sample a tracker.
        MediaServer::Track
        Threads::Threads
        ${FFMPEG_LIBRARIES}
)
target_include_directories(media-server PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(media-server PRIVATE ${FFMPEG_LIBRARY_DIRS})

if(WIN32)
    # ole32/oleaut32 read the Windows Firewall rules, shell32 asks for the
    # elevation that adds them, and advapi32 holds the sign-in entry.
    target_link_libraries(media-server
        PRIVATE dxgi user32 ole32 oleaut32 shell32 advapi32)
endif()

# The machine-level Quadro Sync probe is useful to the lifecycle agent even
# when it is not currently creating a GUI swapchain. Keep its optional NVAPI
# runtime separate from the D3D12 present-barrier source below.
if(WIN32 AND MEDIASERVER_ENABLE_NVAPI)
    target_sources(media-server PRIVATE nvapi_sync_device.cpp)
    target_compile_definitions(media-server
        PRIVATE MEDIASERVER_HAS_NVAPI_RUNTIME=1)
endif()

if(MEDIASERVER_ENABLE_GUI)
    target_sources(media-server PRIVATE
        blender_links.cpp
        cluster_lifecycle_controller.cpp
        director_clip_authoring.cpp
        director_compositor_authoring.cpp
        director_editor_intents.cpp
        director_editor_view.cpp
        director_effect_authoring.cpp
        director_feed_map_authoring.cpp
        director_hardware_authoring.cpp
        director_key_authoring.cpp
        director_linked_scenes.cpp
        director_live_data.cpp
        director_operator_edits.cpp
        director_operator_input.cpp
        director_project_load.cpp
        director_scene_authoring.cpp
        history_labels.cpp
        render_machine_panel.cpp
        scene_duplicate.cpp
        render_machine_window.cpp
        mask_edit_integration.cpp
        source_range_edit.cpp
        timeline_edit_integration.cpp
        thumbnail_service.cpp
    )
    target_include_directories(media-server PRIVATE ${STB_INCLUDE_DIRS})
    target_link_libraries(media-server PRIVATE
        MediaServer::Render
        MediaServer::Ui
        MediaServer::Editor
    )
    target_compile_definitions(media-server PRIVATE MEDIASERVER_HAS_GUI=1)

    if(WIN32 AND MEDIASERVER_ENABLE_NVAPI)
        target_sources(media-server PRIVATE nvapi_present_barrier.cpp)
        target_compile_definitions(media-server
            PRIVATE MEDIASERVER_HAS_NVAPI_PRESENT_BARRIER=1)
    endif()
endif()

mediaserver_set_project_options(media-server)

set_target_properties(media-server PROPERTIES FOLDER "apps")

if(BUILD_TESTING)
    add_executable(mediaserver_transport_command_tests
        transport_commands.cpp
        tests/transport_command_tests.cpp
    )
    target_include_directories(mediaserver_transport_command_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_transport_command_tests
        PRIVATE MediaServer::Show
    )
    mediaserver_set_project_options(mediaserver_transport_command_tests)
    add_test(
        NAME mediaserver.server_transport_commands
        COMMAND mediaserver_transport_command_tests
    )
    set_target_properties(mediaserver_transport_command_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_output_playback_buffer_tests
        tests/output_playback_buffer_tests.cpp
    )
    target_include_directories(mediaserver_output_playback_buffer_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_output_playback_buffer_tests
        PRIVATE MediaServer::Show
    )
    mediaserver_set_project_options(
        mediaserver_output_playback_buffer_tests
    )
    add_test(
        NAME mediaserver.server_output_playback_buffer
        COMMAND mediaserver_output_playback_buffer_tests
    )
    set_target_properties(mediaserver_output_playback_buffer_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_asset_activation_tests
        tests/asset_activation_tests.cpp
    )
    target_include_directories(mediaserver_asset_activation_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_asset_activation_tests
        PRIVATE MediaServer::Show
    )
    mediaserver_set_project_options(
        mediaserver_asset_activation_tests
    )
    add_test(
        NAME mediaserver.server_asset_activation
        COMMAND mediaserver_asset_activation_tests
    )
    set_target_properties(mediaserver_asset_activation_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_output_media_runtime_tests
        tests/output_media_runtime_tests.cpp
    )
    target_include_directories(mediaserver_output_media_runtime_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_output_media_runtime_tests
        PRIVATE MediaServer::Net
    )
    mediaserver_set_project_options(mediaserver_output_media_runtime_tests)
    add_test(
        NAME mediaserver.server_output_media_runtime
        COMMAND mediaserver_output_media_runtime_tests
    )
    set_target_properties(mediaserver_output_media_runtime_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_tracker_listener_tests
        tracker_listener.cpp
        tests/tracker_listener_tests.cpp
    )
    target_include_directories(mediaserver_tracker_listener_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_tracker_listener_tests
        PRIVATE MediaServer::Net MediaServer::Scene MediaServer::Track
                Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_tracker_listener_tests)
    add_test(
        NAME mediaserver.server_tracker_listener
        COMMAND mediaserver_tracker_listener_tests
    )
    set_target_properties(mediaserver_tracker_listener_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_tracked_camera_bridge_tests
        tracked_camera_bridge.cpp
        tests/tracked_camera_bridge_tests.cpp
    )
    target_include_directories(mediaserver_tracked_camera_bridge_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    # The one place the tracker, the wire, and the scene meet. Each of the
    # three libraries deliberately knows nothing about the other two.
    target_link_libraries(mediaserver_tracked_camera_bridge_tests
        PRIVATE MediaServer::Net MediaServer::Scene MediaServer::Track
                mediaserver_camera_prediction
    )
    mediaserver_set_project_options(mediaserver_tracked_camera_bridge_tests)
    add_test(
        NAME mediaserver.server_tracked_camera_bridge
        COMMAND mediaserver_tracked_camera_bridge_tests
    )
    set_target_properties(mediaserver_tracked_camera_bridge_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_timeline_playback_cache_tests
        timeline_playback_cache.cpp
        tests/timeline_playback_cache_tests.cpp
    )
    target_include_directories(mediaserver_timeline_playback_cache_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_timeline_playback_cache_tests
        PRIVATE MediaServer::Scene
    )
    mediaserver_set_project_options(mediaserver_timeline_playback_cache_tests)
    add_test(
        NAME mediaserver.server_timeline_playback_cache
        COMMAND mediaserver_timeline_playback_cache_tests
    )
    set_target_properties(mediaserver_timeline_playback_cache_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_renderer_lockstep_tests
        renderer_lockstep.cpp
        tests/renderer_lockstep_tests.cpp
    )
    target_include_directories(mediaserver_renderer_lockstep_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_renderer_lockstep_tests
        PRIVATE MediaServer::Net MediaServer::Scene
    )
    mediaserver_set_project_options(mediaserver_renderer_lockstep_tests)
    add_test(
        NAME mediaserver.server_renderer_lockstep
        COMMAND mediaserver_renderer_lockstep_tests
    )
    set_target_properties(mediaserver_renderer_lockstep_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_proxy_encoder_benchmark
        ffmpeg_proxy_encoder.cpp
        proxy_generation_service.cpp
        tests/proxy_encoder_benchmark.cpp
    )
    target_include_directories(mediaserver_proxy_encoder_benchmark
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_directories(mediaserver_proxy_encoder_benchmark
        PRIVATE ${FFMPEG_LIBRARY_DIRS}
    )
    target_link_libraries(mediaserver_proxy_encoder_benchmark
        PRIVATE MediaServer::Assets Threads::Threads ${FFMPEG_LIBRARIES}
    )
    mediaserver_set_project_options(mediaserver_proxy_encoder_benchmark)
    set_target_properties(mediaserver_proxy_encoder_benchmark
        PROPERTIES FOLDER "benchmarks"
    )

    add_executable(mediaserver_clip_clipboard_tests
        clip_clipboard.cpp
        tests/clip_clipboard_tests.cpp
    )
    target_include_directories(mediaserver_clip_clipboard_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_clip_clipboard_tests
        PRIVATE MediaServer::Cmd MediaServer::Scene
    )
    mediaserver_set_project_options(mediaserver_clip_clipboard_tests)
    add_test(
        NAME mediaserver.server_clip_clipboard
        COMMAND mediaserver_clip_clipboard_tests
    )
    set_target_properties(mediaserver_clip_clipboard_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_key_clipboard_tests
        key_clipboard.cpp
        tests/key_clipboard_tests.cpp
    )
    target_include_directories(mediaserver_key_clipboard_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_key_clipboard_tests
        PRIVATE MediaServer::Cmd MediaServer::Scene
    )
    mediaserver_set_project_options(mediaserver_key_clipboard_tests)
    add_test(
        NAME mediaserver.server_key_clipboard
        COMMAND mediaserver_key_clipboard_tests
    )
    set_target_properties(mediaserver_key_clipboard_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_timeline_lane_authoring_tests
        timeline_lane_authoring.cpp
        tests/timeline_lane_authoring_tests.cpp
    )
    target_include_directories(mediaserver_timeline_lane_authoring_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_timeline_lane_authoring_tests
        PRIVATE MediaServer::Cmd MediaServer::Scene
    )
    mediaserver_set_project_options(
        mediaserver_timeline_lane_authoring_tests
    )
    add_test(
        NAME mediaserver.server_timeline_lane_authoring
        COMMAND mediaserver_timeline_lane_authoring_tests
    )
    set_target_properties(mediaserver_timeline_lane_authoring_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_plateset_motion_tests
        plateset_motion.cpp
        tests/plateset_motion_tests.cpp
    )
    target_include_directories(mediaserver_plateset_motion_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_plateset_motion_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_plateset_motion_tests)
    add_test(
        NAME mediaserver.server_plateset_motion
        COMMAND mediaserver_plateset_motion_tests
    )
    set_target_properties(mediaserver_plateset_motion_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_timeline_marker_authoring_tests
        timeline_marker_authoring.cpp
        tests/timeline_marker_authoring_tests.cpp
    )
    target_include_directories(mediaserver_timeline_marker_authoring_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_timeline_marker_authoring_tests
        PRIVATE MediaServer::Scene
    )
    mediaserver_set_project_options(
        mediaserver_timeline_marker_authoring_tests
    )
    add_test(
        NAME mediaserver.server_timeline_marker_authoring
        COMMAND mediaserver_timeline_marker_authoring_tests
    )
    set_target_properties(mediaserver_timeline_marker_authoring_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_timeline_edit_authoring_tests
        timeline_edit_authoring.cpp
        tests/timeline_edit_authoring_tests.cpp
    )
    target_include_directories(mediaserver_timeline_edit_authoring_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_timeline_edit_authoring_tests
        PRIVATE MediaServer::Cmd MediaServer::Scene
    )
    mediaserver_set_project_options(
        mediaserver_timeline_edit_authoring_tests
    )
    add_test(
        NAME mediaserver.server_timeline_edit_authoring
        COMMAND mediaserver_timeline_edit_authoring_tests
    )
    set_target_properties(mediaserver_timeline_edit_authoring_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_edit_coalescing_policy_tests
        edit_coalescing_policy.cpp
        tests/edit_coalescing_policy_tests.cpp
    )
    if(MEDIASERVER_ENABLE_GUI)
        add_executable(mediaserver_history_labels_tests
            history_labels.cpp
            tests/history_labels_tests.cpp)
        target_include_directories(mediaserver_history_labels_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(mediaserver_history_labels_tests PRIVATE MediaServer::Editor MediaServer::Cmd)
        mediaserver_set_project_options(mediaserver_history_labels_tests)
        add_test(NAME mediaserver.server_history_labels COMMAND mediaserver_history_labels_tests)
        set_target_properties(mediaserver_history_labels_tests PROPERTIES FOLDER "tests")
        add_executable(mediaserver_source_replacement_authoring_tests
            tests/source_replacement_authoring_tests.cpp)
        target_include_directories(mediaserver_source_replacement_authoring_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(mediaserver_source_replacement_authoring_tests PRIVATE MediaServer::Editor MediaServer::Cmd MediaServer::Assets)
        mediaserver_set_project_options(mediaserver_source_replacement_authoring_tests)
        add_test(NAME mediaserver.server_source_replacement_authoring COMMAND mediaserver_source_replacement_authoring_tests)
        set_target_properties(mediaserver_source_replacement_authoring_tests PROPERTIES FOLDER "tests")
        add_executable(mediaserver_compositor_authoring_tests
            director_compositor_authoring.cpp
            edit_coalescing_policy.cpp
            tests/compositor_authoring_tests.cpp)
        target_include_directories(mediaserver_compositor_authoring_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(mediaserver_compositor_authoring_tests PRIVATE MediaServer::Editor MediaServer::Cmd)
        mediaserver_set_project_options(mediaserver_compositor_authoring_tests)
        add_test(NAME mediaserver.server_compositor_authoring COMMAND mediaserver_compositor_authoring_tests)
        set_target_properties(mediaserver_compositor_authoring_tests PROPERTIES FOLDER "tests")
        add_executable(mediaserver_plateset_authoring_tests
            plateset_authoring.cpp
            timeline_lane_authoring.cpp
            director_compositor_authoring.cpp
            edit_coalescing_policy.cpp
            tests/plateset_authoring_tests.cpp)
        target_include_directories(mediaserver_plateset_authoring_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(mediaserver_plateset_authoring_tests PRIVATE MediaServer::Editor MediaServer::Cmd MediaServer::Assets MediaServer::Scene)
        mediaserver_set_project_options(mediaserver_plateset_authoring_tests)
        add_test(NAME mediaserver.server_plateset_authoring COMMAND mediaserver_plateset_authoring_tests)
        set_target_properties(mediaserver_plateset_authoring_tests PROPERTIES FOLDER "tests")
        add_executable(mediaserver_feed_map_authoring_tests
            director_feed_map_authoring.cpp
            director_compositor_authoring.cpp
            edit_coalescing_policy.cpp
            tests/feed_map_authoring_tests.cpp)
        target_include_directories(mediaserver_feed_map_authoring_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(mediaserver_feed_map_authoring_tests PRIVATE MediaServer::Editor MediaServer::Cmd MediaServer::Assets)
        mediaserver_set_project_options(mediaserver_feed_map_authoring_tests)
        add_test(NAME mediaserver.server_feed_map_authoring COMMAND mediaserver_feed_map_authoring_tests)
        set_target_properties(mediaserver_feed_map_authoring_tests PROPERTIES FOLDER "tests")

        add_executable(mediaserver_hardware_authoring_tests
            director_hardware_authoring.cpp
            director_feed_map_authoring.cpp
            director_compositor_authoring.cpp
            edit_coalescing_policy.cpp
            tests/hardware_authoring_tests.cpp)
        target_include_directories(mediaserver_hardware_authoring_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(mediaserver_hardware_authoring_tests PRIVATE MediaServer::Editor MediaServer::Cmd MediaServer::Assets MediaServer::Topology MediaServer::Sync)
        mediaserver_set_project_options(mediaserver_hardware_authoring_tests)
        add_test(NAME mediaserver.server_hardware_authoring COMMAND mediaserver_hardware_authoring_tests)
        set_target_properties(mediaserver_hardware_authoring_tests PROPERTIES FOLDER "tests")
    endif()

    add_executable(mediaserver_scene_duplicate_tests
        scene_duplicate.cpp
        tests/scene_duplicate_tests.cpp)
    target_include_directories(mediaserver_scene_duplicate_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(mediaserver_scene_duplicate_tests PRIVATE MediaServer::Scene MediaServer::Doc)
    mediaserver_set_project_options(mediaserver_scene_duplicate_tests)
    add_test(NAME mediaserver.server_scene_duplicate COMMAND mediaserver_scene_duplicate_tests)
    set_target_properties(mediaserver_scene_duplicate_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_model_source_authoring_tests
        edit_coalescing_policy.cpp
        tests/model_source_authoring_tests.cpp)
    target_include_directories(mediaserver_model_source_authoring_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(mediaserver_model_source_authoring_tests PRIVATE MediaServer::Cmd MediaServer::Scene)
    mediaserver_set_project_options(mediaserver_model_source_authoring_tests)
    add_test(NAME mediaserver.server_model_source_authoring COMMAND mediaserver_model_source_authoring_tests)
    set_target_properties(mediaserver_model_source_authoring_tests PROPERTIES FOLDER "tests")
    target_include_directories(mediaserver_edit_coalescing_policy_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_edit_coalescing_policy_tests
        PRIVATE MediaServer::Cmd
    )
    mediaserver_set_project_options(mediaserver_edit_coalescing_policy_tests)
    add_test(
        NAME mediaserver.server_edit_coalescing_policy
        COMMAND mediaserver_edit_coalescing_policy_tests
    )
    set_target_properties(mediaserver_edit_coalescing_policy_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_cluster_observability_tests
        cluster_observability.cpp
        tests/cluster_observability_tests.cpp
    )
    target_include_directories(mediaserver_cluster_observability_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_cluster_observability_tests
        PRIVATE MediaServer::Net
    )
    mediaserver_set_project_options(mediaserver_cluster_observability_tests)
    add_test(
        NAME mediaserver.server_cluster_observability
        COMMAND mediaserver_cluster_observability_tests
    )
    set_target_properties(mediaserver_cluster_observability_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_asset_readiness_provider_tests
        asset_readiness_provider.cpp
        tests/asset_readiness_provider_tests.cpp
    )
    target_include_directories(mediaserver_asset_readiness_provider_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_asset_readiness_provider_tests
        PRIVATE MediaServer::Assets MediaServer::Net
    )
    mediaserver_set_project_options(
        mediaserver_asset_readiness_provider_tests
    )
    add_test(
        NAME mediaserver.server_asset_readiness_provider
        COMMAND mediaserver_asset_readiness_provider_tests
    )
    set_target_properties(mediaserver_asset_readiness_provider_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_project_asset_import_tests
        project_asset_import.cpp
        tests/project_asset_import_tests.cpp
    )
    target_include_directories(mediaserver_project_asset_import_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_project_asset_import_tests
        PRIVATE MediaServer::Assets MediaServer::Scene MediaServer::Output
    )
    mediaserver_set_project_options(mediaserver_project_asset_import_tests)
    add_test(
        NAME mediaserver.server_project_asset_import
        COMMAND mediaserver_project_asset_import_tests
    )
    set_target_properties(mediaserver_project_asset_import_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_project_assets_tests
        project_assets.cpp
        tests/project_assets_tests.cpp
    )
    target_include_directories(mediaserver_project_assets_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_project_assets_tests
        PRIVATE MediaServer::Assets MediaServer::Scene MediaServer::Output
    )
    mediaserver_set_project_options(mediaserver_project_assets_tests)
    add_test(
        NAME mediaserver.server_project_assets
        COMMAND mediaserver_project_assets_tests
    )
    set_target_properties(mediaserver_project_assets_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_proxy_generation_service_tests
        proxy_generation_service.cpp
        tests/proxy_generation_service_tests.cpp
    )
    target_include_directories(mediaserver_proxy_generation_service_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_proxy_generation_service_tests
        PRIVATE MediaServer::Assets Threads::Threads
    )
    mediaserver_set_project_options(
        mediaserver_proxy_generation_service_tests
    )
    add_test(
        NAME mediaserver.server_proxy_generation_service
        COMMAND mediaserver_proxy_generation_service_tests
    )
    set_target_properties(mediaserver_proxy_generation_service_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_waveform_generation_service_tests
        waveform_generation_service.cpp
        tests/waveform_generation_service_tests.cpp
    )
    target_include_directories(mediaserver_waveform_generation_service_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_waveform_generation_service_tests
        PRIVATE MediaServer::Assets Threads::Threads
    )
    mediaserver_set_project_options(
        mediaserver_waveform_generation_service_tests
    )
    add_test(
        NAME mediaserver.server_waveform_generation_service
        COMMAND mediaserver_waveform_generation_service_tests
    )
    set_target_properties(mediaserver_waveform_generation_service_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_ffmpeg_waveform_decoder_tests
        ffmpeg_waveform_decoder.cpp
        tests/ffmpeg_waveform_decoder_tests.cpp
    )
    target_include_directories(mediaserver_ffmpeg_waveform_decoder_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_directories(mediaserver_ffmpeg_waveform_decoder_tests
        PRIVATE ${FFMPEG_LIBRARY_DIRS}
    )
    target_link_libraries(mediaserver_ffmpeg_waveform_decoder_tests
        PRIVATE MediaServer::Assets ${FFMPEG_LIBRARIES}
    )
    mediaserver_set_project_options(
        mediaserver_ffmpeg_waveform_decoder_tests
    )
    add_test(
        NAME mediaserver.server_ffmpeg_waveform_decoder
        COMMAND mediaserver_ffmpeg_waveform_decoder_tests
    )
    set_target_properties(mediaserver_ffmpeg_waveform_decoder_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_proxy_playback_tests
        project_asset_import.cpp
        proxy_generation_service.cpp
        proxy_playback.cpp
        tests/proxy_playback_tests.cpp
    )
    target_include_directories(mediaserver_proxy_playback_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_proxy_playback_tests
        PRIVATE MediaServer::Assets MediaServer::Scene MediaServer::Output
                Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_proxy_playback_tests)
    add_test(
        NAME mediaserver.server_proxy_playback
        COMMAND mediaserver_proxy_playback_tests
    )
    set_target_properties(mediaserver_proxy_playback_tests
        PROPERTIES FOLDER "tests"
    )

    if(MEDIASERVER_ENABLE_GUI)
        add_executable(mediaserver_timeline_edit_integration_tests
            clip_clipboard.cpp
            source_range_edit.cpp
            timeline_edit_authoring.cpp
            timeline_edit_integration.cpp
            tests/timeline_edit_integration_tests.cpp
        )
        target_include_directories(mediaserver_timeline_edit_integration_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(mediaserver_timeline_edit_integration_tests
            PRIVATE MediaServer::Cmd MediaServer::Editor MediaServer::Scene
        )
        mediaserver_set_project_options(
            mediaserver_timeline_edit_integration_tests
        )
        add_test(
            NAME mediaserver.server_timeline_edit_integration
            COMMAND mediaserver_timeline_edit_integration_tests
        )
        set_target_properties(mediaserver_timeline_edit_integration_tests
            PROPERTIES FOLDER "tests"
        )

        # TODO: re-enable once apps/server/tests/mask_edit_integration_tests.cpp
        # is written. The file does not exist yet, which fails the CMake generate
        # step for the whole project.
#        add_executable(mediaserver_mask_edit_integration_tests
#            mask_edit_integration.cpp
#            tests/mask_edit_integration_tests.cpp
#        )
#        target_include_directories(mediaserver_mask_edit_integration_tests
#            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
#        )
#        target_link_libraries(mediaserver_mask_edit_integration_tests
#            PRIVATE MediaServer::Cmd MediaServer::Editor MediaServer::Scene
#        )
#        mediaserver_set_project_options(
#            mediaserver_mask_edit_integration_tests
#        )
#        add_test(
#            NAME mediaserver.server_mask_edit_integration
#            COMMAND mediaserver_mask_edit_integration_tests
#        )
#        set_target_properties(mediaserver_mask_edit_integration_tests
#            PROPERTIES FOLDER "tests"
#        )

        add_executable(mediaserver_thumbnail_service_tests
            thumbnail_service.cpp
            tests/thumbnail_service_tests.cpp
        )
        target_include_directories(mediaserver_thumbnail_service_tests
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
                ${STB_INCLUDE_DIRS}
        )
        target_include_directories(mediaserver_thumbnail_service_tests
            SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS}
        )
        target_link_directories(mediaserver_thumbnail_service_tests
            PRIVATE ${FFMPEG_LIBRARY_DIRS}
        )
        target_link_libraries(mediaserver_thumbnail_service_tests
            PRIVATE MediaServer::Assets MediaServer::Render Threads::Threads
                    ${FFMPEG_LIBRARIES}
        )
        mediaserver_set_project_options(mediaserver_thumbnail_service_tests)
        add_test(
            NAME mediaserver.server_thumbnail_service
            COMMAND mediaserver_thumbnail_service_tests
        )
        set_target_properties(mediaserver_thumbnail_service_tests
            PROPERTIES FOLDER "tests"
        )
    endif()

    add_executable(mediaserver_system_hardware_probe_tests
        system_hardware_probe.cpp
        tests/system_hardware_probe_tests.cpp
    )
    target_include_directories(mediaserver_system_hardware_probe_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_system_hardware_probe_tests
        PRIVATE MediaServer::Net Threads::Threads
    )
    if(WIN32)
        target_link_libraries(mediaserver_system_hardware_probe_tests
            PRIVATE dxgi user32
        )
    endif()
    mediaserver_set_project_options(mediaserver_system_hardware_probe_tests)
    add_test(
        NAME mediaserver.server_system_hardware_probe
        COMMAND mediaserver_system_hardware_probe_tests
    )
    set_target_properties(
        mediaserver_system_hardware_probe_tests PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_sync_device_probe_tests
        sync_device_probe.cpp
        system_hardware_probe.cpp
        tests/sync_device_probe_tests.cpp
    )
    target_include_directories(mediaserver_sync_device_probe_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_sync_device_probe_tests
        PRIVATE MediaServer::Net Threads::Threads
    )
    if(WIN32)
        target_link_libraries(mediaserver_sync_device_probe_tests
            PRIVATE dxgi user32
        )
    endif()
    if(WIN32 AND MEDIASERVER_ENABLE_NVAPI)
        target_sources(mediaserver_sync_device_probe_tests
            PRIVATE nvapi_sync_device.cpp)
        target_compile_definitions(mediaserver_sync_device_probe_tests
            PRIVATE MEDIASERVER_HAS_NVAPI_RUNTIME=1)
    endif()
    mediaserver_set_project_options(mediaserver_sync_device_probe_tests)
    add_test(
        NAME mediaserver.server_sync_device_probe
        COMMAND mediaserver_sync_device_probe_tests
    )
    set_target_properties(
        mediaserver_sync_device_probe_tests PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_managed_process_tests
        managed_process.cpp
        tests/managed_process_tests.cpp
    )
    target_include_directories(mediaserver_managed_process_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_managed_process_tests
        PRIVATE MediaServer::Net
    )
    mediaserver_set_project_options(mediaserver_managed_process_tests)
    add_test(
        NAME mediaserver.server_managed_process
        COMMAND mediaserver_managed_process_tests
    )
    set_target_properties(
        mediaserver_managed_process_tests PROPERTIES FOLDER "tests"
    )

    # The render machine's stored path - what a launch with no arguments
    # starts from - is plain logic over Preferences, tested with real files
    # and no window, network, or second machine.
    add_executable(mediaserver_render_machine_tests
        render_machine.cpp
        tests/render_machine_tests.cpp
    )
    target_include_directories(mediaserver_render_machine_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_render_machine_tests
        PRIVATE MediaServer::Net MediaServer::Persist
    )
    mediaserver_set_project_options(mediaserver_render_machine_tests)
    add_test(
        NAME mediaserver.server_render_machine
        COMMAND mediaserver_render_machine_tests
    )
    set_target_properties(
        mediaserver_render_machine_tests PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_output_window_config_tests
        output_window_config.cpp
        tests/output_window_config_tests.cpp
    )
    target_include_directories(mediaserver_output_window_config_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_output_window_config_tests
        PRIVATE MediaServer::Net MediaServer::Persist MediaServer::Topology
    )
    mediaserver_set_project_options(mediaserver_output_window_config_tests)
    add_test(
        NAME mediaserver.server_output_window_config
        COMMAND mediaserver_output_window_config_tests
    )
    set_target_properties(
        mediaserver_output_window_config_tests PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_output_deployment_tests
        output_deployment.cpp
        output_window_config.cpp
        tests/output_deployment_tests.cpp
    )
    target_include_directories(mediaserver_output_deployment_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(mediaserver_output_deployment_tests
        PRIVATE MediaServer::Net MediaServer::Persist MediaServer::Topology
    )
    mediaserver_set_project_options(mediaserver_output_deployment_tests)
    add_test(
        NAME mediaserver.server_output_deployment
        COMMAND mediaserver_output_deployment_tests
    )
    set_target_properties(
        mediaserver_output_deployment_tests PROPERTIES FOLDER "tests"
    )

    if(MEDIASERVER_ENABLE_GUI)
        add_executable(mediaserver_cluster_lifecycle_controller_tests
            cluster_lifecycle_controller.cpp
            tests/cluster_lifecycle_controller_tests.cpp
        )
        target_include_directories(
            mediaserver_cluster_lifecycle_controller_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(
            mediaserver_cluster_lifecycle_controller_tests
            PRIVATE MediaServer::Net MediaServer::Sync MediaServer::Topology
                    MediaServer::Editor
        )
        mediaserver_set_project_options(
            mediaserver_cluster_lifecycle_controller_tests
        )
        add_test(
            NAME mediaserver.server_cluster_lifecycle_controller
            COMMAND mediaserver_cluster_lifecycle_controller_tests
        )
        set_target_properties(
            mediaserver_cluster_lifecycle_controller_tests
            PROPERTIES FOLDER "tests"
        )

        # The window's panel is input in, actions out: driven with synthetic
        # pointer and keyboard input against a fontless atlas.
        add_executable(mediaserver_render_machine_panel_tests
            render_machine.cpp
            render_machine_panel.cpp
            tests/render_machine_panel_tests.cpp
        )
        target_include_directories(mediaserver_render_machine_panel_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(mediaserver_render_machine_panel_tests
            PRIVATE MediaServer::Net MediaServer::Persist MediaServer::Gui
        )
        mediaserver_set_project_options(mediaserver_render_machine_panel_tests)
        add_test(
            NAME mediaserver.server_render_machine_panel
            COMMAND mediaserver_render_machine_panel_tests
        )
        set_target_properties(mediaserver_render_machine_panel_tests
            PROPERTIES FOLDER "tests"
        )

        add_executable(mediaserver_blender_links_tests
            blender_links.cpp
            tests/blender_links_tests.cpp
        )
        target_include_directories(mediaserver_blender_links_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_link_libraries(mediaserver_blender_links_tests
            PRIVATE MediaServer::Render MediaServer::Net MediaServer::Scene
        )
        mediaserver_set_project_options(mediaserver_blender_links_tests)
        add_test(
            NAME mediaserver.server_blender_links
            COMMAND mediaserver_blender_links_tests
        )
        set_target_properties(mediaserver_blender_links_tests
            PROPERTIES FOLDER "tests"
        )
    endif()

    add_test(
        NAME mediaserver.server_bootstrap
        COMMAND "${CMAKE_COMMAND}"
            "-DSERVER_EXE=$<TARGET_FILE:media-server>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/bootstrap-smoke"
            "-DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            "-DEDITOR_DEMO_SCRIPT=${CMAKE_SOURCE_DIR}/scripts/run-editor-demo.ps1"
            "-DHAS_GUI=$<BOOL:${MEDIASERVER_ENABLE_GUI}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/bootstrap_smoke.cmake"
    )
endif()
