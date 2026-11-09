find_package(Threads REQUIRED)
find_package(FFMPEG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image_write.h")

# Prediction is a Director-owned service compiled with the same reproducible
# arithmetic contract as scene math. The node path never invokes this service.
add_library(mediaserver_camera_prediction STATIC camera_prediction.cpp)
target_link_libraries(mediaserver_camera_prediction PUBLIC
    MediaServer::Scene MediaServer::Track MediaServer::Net)
mediaserver_set_project_options(mediaserver_camera_prediction)
mediaserver_set_deterministic_float_options(mediaserver_camera_prediction)
set_target_properties(mediaserver_camera_prediction PROPERTIES FOLDER "apps")

# Everything except main.cpp, compiled once and shared by media-server and
# every server test. Its feature definitions are public so a test sees the
# same declarations the library was built with.
add_library(mediaserver_server STATIC
    asset_readiness_provider.cpp
    blender_links.cpp
    clip_clipboard.cpp
    cluster_lifecycle_controller.cpp
    cluster_observability.cpp
    director_clip_authoring.cpp
    director_compositor_authoring.cpp
    director_documents.cpp
    director_editor_intents.cpp
    director_editor_view.cpp
    director_effect_authoring.cpp
    director_feed_map_authoring.cpp
    director_frame_evaluation.cpp
    director_hardware_authoring.cpp
    director_key_authoring.cpp
    director_linked_scenes.cpp
    director_live_data.cpp
    director_operator_edits.cpp
    director_operator_input.cpp
    director_project_load.cpp
    director_scene_authoring.cpp
    director_show_loop.cpp
    director_show_status.cpp
    edit_coalescing_policy.cpp
    ffmpeg_proxy_encoder.cpp
    ffmpeg_waveform_decoder.cpp
    history_labels.cpp
    key_clipboard.cpp
    lifecycle_agent.cpp
    managed_process.cpp
    mask_edit_integration.cpp
    mosaic_layout.cpp
    output_deployment.cpp
    output_window_config.cpp
    plateset_authoring.cpp
    plateset_motion.cpp
    plateset_motion_service.cpp
    playback.cpp
    playback_media.cpp
    playback_runtime.cpp
    process_log.cpp
    project_asset_import.cpp
    project_assets.cpp
    proxy_generation_service.cpp
    proxy_playback.cpp
    render_machine.cpp
    render_machine_panel.cpp
    render_machine_system.cpp
    render_machine_window.cpp
    renderer_lockstep.cpp
    scene_duplicate.cpp
    source_range_edit.cpp
    sync_device_probe.cpp
    system_hardware_probe.cpp
    thumbnail_service.cpp
    timeline_edit_authoring.cpp
    timeline_edit_integration.cpp
    timeline_lane_authoring.cpp
    timeline_marker_authoring.cpp
    timeline_playback_cache.cpp
    tracked_camera_bridge.cpp
    tracker_listener.cpp
    transport_commands.cpp
    waveform_generation_service.cpp
)
target_include_directories(mediaserver_server
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE ${FFMPEG_INCLUDE_DIRS} ${STB_INCLUDE_DIRS}
)
target_link_directories(mediaserver_server PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mediaserver_server
    PUBLIC
        mediaserver_camera_prediction
        MediaServer::Cmd
        MediaServer::Doc
        MediaServer::Net
        MediaServer::Show
        MediaServer::Persist
        MediaServer::Scene
        MediaServer::Assets
        MediaServer::Output
        MediaServer::Compositor
        MediaServer::Sync
        MediaServer::Topology
        # LED processor protocols. Every request runs on the monitor's own
        # thread; the Director only hands it the map and reads what it saw.
        MediaServer::Devices
        # Camera tracking. The library has no I/O at all; this process supplies
        # the datagrams and is the only one that may sample a tracker.
        MediaServer::Track
        MediaServer::Gui
        MediaServer::Render
        MediaServer::Ui
        MediaServer::Editor
        Threads::Threads
        ${FFMPEG_LIBRARIES}
)

if(WIN32)
    # ole32/oleaut32 read the Windows Firewall rules, shell32 asks for the
    # elevation that adds them, and advapi32 holds the sign-in entry.
    target_link_libraries(mediaserver_server
        PUBLIC dxgi user32 ole32 oleaut32 shell32 advapi32)
endif()

# The machine-level Quadro Sync probe and the D3D12 present barrier share the
# optional NVAPI runtime, loaded dynamically.
if(WIN32 AND MEDIASERVER_ENABLE_NVAPI)
    target_sources(mediaserver_server PRIVATE
        nvapi_present_barrier.cpp
        nvapi_sync_device.cpp
    )
    target_compile_definitions(mediaserver_server PUBLIC
        MEDIASERVER_HAS_NVAPI_PRESENT_BARRIER=1
        MEDIASERVER_HAS_NVAPI_RUNTIME=1
    )
endif()

mediaserver_set_project_options(mediaserver_server)
set_target_properties(mediaserver_server PROPERTIES FOLDER "apps")

add_executable(media-server main.cpp)
target_link_libraries(media-server PRIVATE mediaserver_server)
mediaserver_set_project_options(media-server)
set_target_properties(media-server PROPERTIES FOLDER "apps")

if(BUILD_TESTING)
    # mediaserver_<name>_tests from tests/<name>_tests.cpp, registered as
    # mediaserver.server_<name> unless TEST gives another entry name.
    function(mediaserver_server_test name)
        cmake_parse_arguments(PARSE_ARGV 1 arg "" "TEST" "")
        if(NOT arg_TEST)
            set(arg_TEST ${name})
        endif()
        set(target mediaserver_${name}_tests)
        add_executable(${target} tests/${name}_tests.cpp)
        target_link_libraries(${target} PRIVATE mediaserver_server)
        mediaserver_set_project_options(${target})
        set_target_properties(${target} PROPERTIES FOLDER "tests")
        add_test(NAME mediaserver.server_${arg_TEST} COMMAND ${target})
    endfunction()

    mediaserver_server_test(asset_activation)
    mediaserver_server_test(asset_readiness_provider)
    mediaserver_server_test(blender_links)
    mediaserver_server_test(clip_clipboard)
    mediaserver_server_test(cluster_lifecycle_controller)
    mediaserver_server_test(cluster_observability)
    mediaserver_server_test(compositor_authoring)
    mediaserver_server_test(edit_coalescing_policy)
    mediaserver_server_test(feed_map_authoring)
    mediaserver_server_test(ffmpeg_waveform_decoder)
    mediaserver_server_test(hardware_authoring)
    mediaserver_server_test(history_labels)
    mediaserver_server_test(key_clipboard)
    mediaserver_server_test(managed_process)
    mediaserver_server_test(model_source_authoring)
    mediaserver_server_test(output_deployment)
    mediaserver_server_test(output_media_runtime)
    mediaserver_server_test(output_playback_buffer)
    mediaserver_server_test(output_window_config)
    mediaserver_server_test(plateset_authoring)
    mediaserver_server_test(plateset_motion)
    # The disk log every process keeps and the agent serves to Output Monitor:
    # real files in a temporary folder, and this process's own stdout.
    mediaserver_server_test(process_log)
    mediaserver_server_test(project_asset_import)
    mediaserver_server_test(project_assets)
    mediaserver_server_test(proxy_generation_service)
    mediaserver_server_test(proxy_playback)
    # The render machine's stored path - what a launch with no arguments
    # starts from - is plain logic over Preferences, tested with real files
    # and no window, network, or second machine.
    mediaserver_server_test(render_machine)
    # The window's panel is input in, actions out: driven with synthetic
    # pointer and keyboard input against a fontless atlas.
    mediaserver_server_test(render_machine_panel)
    mediaserver_server_test(renderer_lockstep)
    mediaserver_server_test(scene_duplicate)
    mediaserver_server_test(source_replacement_authoring)
    mediaserver_server_test(sync_device_probe)
    mediaserver_server_test(system_hardware_probe)
    # Checks thumbnails by decoding them with stb and FFmpeg directly.
    mediaserver_server_test(thumbnail_service)
    target_include_directories(mediaserver_thumbnail_service_tests
        PRIVATE ${STB_INCLUDE_DIRS})
    target_include_directories(mediaserver_thumbnail_service_tests
        SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    mediaserver_server_test(timeline_edit_authoring)
    mediaserver_server_test(timeline_edit_integration)
    mediaserver_server_test(timeline_lane_authoring)
    mediaserver_server_test(timeline_marker_authoring)
    mediaserver_server_test(timeline_playback_cache)
    # The one place the tracker, the wire, and the scene meet. Each of the
    # three libraries deliberately knows nothing about the other two.
    mediaserver_server_test(tracked_camera_bridge)
    mediaserver_server_test(tracker_listener)
    mediaserver_server_test(transport_command TEST transport_commands)
    mediaserver_server_test(waveform_generation_service)
    # mask_edit_integration.cpp has no tests yet.

    add_executable(mediaserver_proxy_encoder_benchmark
        tests/proxy_encoder_benchmark.cpp)
    target_link_libraries(mediaserver_proxy_encoder_benchmark
        PRIVATE mediaserver_server)
    mediaserver_set_project_options(mediaserver_proxy_encoder_benchmark)
    set_target_properties(mediaserver_proxy_encoder_benchmark
        PROPERTIES FOLDER "benchmarks")

    add_test(
        NAME mediaserver.server_bootstrap
        COMMAND "${CMAKE_COMMAND}"
            "-DSERVER_EXE=$<TARGET_FILE:media-server>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/bootstrap-smoke"
            "-DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/bootstrap_smoke.cmake"
    )
endif()
