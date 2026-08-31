find_package(Threads REQUIRED)
find_package(FFMPEG REQUIRED)
if(MEDIASERVER_ENABLE_GUI)
    find_path(STB_INCLUDE_DIRS "stb_image_write.h")
endif()

add_executable(media-server
    main.cpp
    asset_readiness_provider.cpp
    clip_clipboard.cpp
    key_clipboard.cpp
    timeline_lane_authoring.cpp
    timeline_edit_authoring.cpp
    edit_coalescing_policy.cpp
    ffmpeg_waveform_decoder.cpp
    ffmpeg_proxy_encoder.cpp
    cluster_observability.cpp
    managed_process.cpp
    output_deployment.cpp
    output_window_config.cpp
    playback.cpp
    proxy_generation_service.cpp
    proxy_playback.cpp
    project_asset_import.cpp
    project_assets.cpp
    sync_device_probe.cpp
    system_hardware_probe.cpp
    waveform_generation_service.cpp
)
target_link_libraries(media-server
    PRIVATE
        MediaServer::Net
        MediaServer::Persist
        MediaServer::Assets
        MediaServer::Compositor
        MediaServer::Sync
        MediaServer::Topology
        Threads::Threads
        ${FFMPEG_LIBRARIES}
)
target_include_directories(media-server PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(media-server PRIVATE ${FFMPEG_LIBRARY_DIRS})

if(WIN32)
    target_link_libraries(media-server PRIVATE dxgi user32)
endif()

if(MEDIASERVER_ENABLE_GUI)
    target_sources(media-server PRIVATE
        cluster_lifecycle_controller.cpp
        mask_edit_integration.cpp
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
endif()

mediaserver_set_project_options(media-server)

set_target_properties(media-server PROPERTIES FOLDER "apps")

if(BUILD_TESTING)
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
        target_link_libraries(mediaserver_thumbnail_service_tests
            PRIVATE MediaServer::Assets MediaServer::Render Threads::Threads
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
