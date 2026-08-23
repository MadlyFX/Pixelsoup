add_executable(media-server
    main.cpp
    playback.cpp
)
target_link_libraries(media-server
    PRIVATE
        MediaServer::Net
        MediaServer::Persist
)

if(MEDIASERVER_ENABLE_GUI)
    target_link_libraries(media-server PRIVATE MediaServer::Ui)
    target_compile_definitions(media-server PRIVATE MEDIASERVER_HAS_GUI=1)
endif()

mediaserver_set_project_options(media-server)

set_target_properties(media-server PROPERTIES FOLDER "apps")

if(BUILD_TESTING)
    add_test(
        NAME mediaserver.server_bootstrap
        COMMAND "${CMAKE_COMMAND}"
            "-DSERVER_EXE=$<TARGET_FILE:media-server>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/bootstrap-smoke"
            "-DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/bootstrap_smoke.cmake"
    )
endif()
