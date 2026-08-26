find_package(Threads REQUIRED)

add_executable(media-server
    main.cpp
    managed_process.cpp
    output_window_config.cpp
    playback.cpp
    system_hardware_probe.cpp
)
target_link_libraries(media-server
    PRIVATE
        MediaServer::Net
        MediaServer::Persist
        MediaServer::Assets
        MediaServer::Compositor
        MediaServer::Topology
        Threads::Threads
)

if(WIN32)
    target_link_libraries(media-server PRIVATE dxgi user32)
endif()

if(MEDIASERVER_ENABLE_GUI)
    target_sources(media-server PRIVATE cluster_lifecycle_controller.cpp)
    target_link_libraries(media-server PRIVATE MediaServer::Ui MediaServer::Editor)
    target_compile_definitions(media-server PRIVATE MEDIASERVER_HAS_GUI=1)
endif()

mediaserver_set_project_options(media-server)

set_target_properties(media-server PROPERTIES FOLDER "apps")

if(BUILD_TESTING)
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
            PRIVATE MediaServer::Net MediaServer::Topology MediaServer::Editor
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
