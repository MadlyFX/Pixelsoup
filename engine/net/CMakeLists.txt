# vcpkg's standalone-Asio config does not export a package version file. The
# manifest override and compile sentinel below enforce the exact version.
find_package(asio CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)

set(MEDIASERVER_NET_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/schema/session_protocol.fbs")
set(MEDIASERVER_NET_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MEDIASERVER_NET_GENERATED_HEADER
    "${MEDIASERVER_NET_GENERATED_DIR}/session_protocol_generated.h")

add_custom_command(
    OUTPUT "${MEDIASERVER_NET_GENERATED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MEDIASERVER_NET_GENERATED_DIR}"
    COMMAND "$<TARGET_FILE:flatbuffers::flatc>"
        --cpp
        --scoped-enums
        --warnings-as-errors
        -o "${MEDIASERVER_NET_GENERATED_DIR}"
        "${MEDIASERVER_NET_SCHEMA}"
    DEPENDS flatbuffers::flatc "${MEDIASERVER_NET_SCHEMA}"
    VERBATIM
)

add_library(mediaserver_net STATIC
    src/asio_dependency.cpp
    src/director_frame_barrier.cpp
    src/director_session_service.cpp
    src/director_session_hub.cpp
    src/durable_director_frame_release_authority.cpp
    src/frame_commit_sink.cpp
    src/heartbeat_schedule.cpp
    src/node_session_service.cpp
    src/resume_session.cpp
    src/session_codec.cpp
    src/tcp_framed_stream.cpp
    src/unresolved_cohort_recovery.cpp
    "${MEDIASERVER_NET_GENERATED_HEADER}"
)
add_library(MediaServer::Net ALIAS mediaserver_net)

target_include_directories(mediaserver_net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        "${MEDIASERVER_NET_GENERATED_DIR}"
)

if(WIN32)
    target_compile_definitions(mediaserver_net PUBLIC _WIN32_WINNT=0x0A00)
endif()

target_link_libraries(mediaserver_net
    PUBLIC
        MediaServer::Cmd
        MediaServer::Core
        MediaServer::Persist
        MediaServer::Show
        MediaServer::Wire
        asio::asio
    PRIVATE
        flatbuffers::flatbuffers
)

mediaserver_set_project_options(mediaserver_net)
mediaserver_disable_rtti(mediaserver_net)

set_target_properties(mediaserver_net PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(mediaserver_net_tests
        tests/tcp_framed_stream_tests.cpp
    )
    target_link_libraries(mediaserver_net_tests PRIVATE MediaServer::Net)
    mediaserver_set_project_options(mediaserver_net_tests)

    add_test(NAME mediaserver.net COMMAND mediaserver_net_tests)
    set_target_properties(mediaserver_net_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_unresolved_cohort_recovery_tests
        tests/unresolved_cohort_recovery_tests.cpp
    )
    target_link_libraries(
        mediaserver_unresolved_cohort_recovery_tests
        PRIVATE MediaServer::Net
    )
    mediaserver_set_project_options(
        mediaserver_unresolved_cohort_recovery_tests)

    add_test(
        NAME mediaserver.unresolved_cohort_recovery
        COMMAND mediaserver_unresolved_cohort_recovery_tests
    )
    set_target_properties(
        mediaserver_unresolved_cohort_recovery_tests
        PROPERTIES FOLDER "tests")

    add_executable(mediaserver_session_codec_tests
        tests/session_codec_tests.cpp
    )
    target_include_directories(mediaserver_session_codec_tests
        PRIVATE "${MEDIASERVER_NET_GENERATED_DIR}"
    )
    target_link_libraries(mediaserver_session_codec_tests PRIVATE MediaServer::Net)
    mediaserver_set_project_options(mediaserver_session_codec_tests)

    add_test(NAME mediaserver.net_session_codec COMMAND mediaserver_session_codec_tests)
    add_test(
        NAME mediaserver.net_schema_conform
        COMMAND "$<TARGET_FILE:flatbuffers::flatc>"
            --conform
            "${CMAKE_CURRENT_SOURCE_DIR}/schema/v1/session_protocol.fbs"
            "${MEDIASERVER_NET_SCHEMA}"
    )
    set_target_properties(mediaserver_session_codec_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_resume_session_tests
        tests/resume_session_tests.cpp
    )
    target_link_libraries(mediaserver_resume_session_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_resume_session_tests)

    add_test(NAME mediaserver.net_resume_session COMMAND mediaserver_resume_session_tests)
    set_target_properties(mediaserver_resume_session_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_frame_acknowledgement_tests
        tests/frame_acknowledgement_tests.cpp
    )
    target_link_libraries(mediaserver_frame_acknowledgement_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_frame_acknowledgement_tests)

    add_test(
        NAME mediaserver.net_frame_acknowledgements
        COMMAND mediaserver_frame_acknowledgement_tests
    )
    set_target_properties(
        mediaserver_frame_acknowledgement_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_commit_permit_tests
        tests/commit_permit_tests.cpp
    )
    target_link_libraries(mediaserver_commit_permit_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_commit_permit_tests)

    add_test(
        NAME mediaserver.net_commit_permit
        COMMAND mediaserver_commit_permit_tests
    )
    set_target_properties(
        mediaserver_commit_permit_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_durable_director_frame_release_authority_tests
        tests/durable_director_frame_release_authority_tests.cpp
    )
    target_link_libraries(
        mediaserver_durable_director_frame_release_authority_tests
        PRIVATE MediaServer::Net
    )
    mediaserver_set_project_options(
        mediaserver_durable_director_frame_release_authority_tests
    )

    add_test(
        NAME mediaserver.net_durable_release_authority
        COMMAND mediaserver_durable_director_frame_release_authority_tests
    )
    set_target_properties(
        mediaserver_durable_director_frame_release_authority_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_director_session_hub_tests
        tests/director_session_hub_tests.cpp
    )
    target_link_libraries(mediaserver_director_session_hub_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_director_session_hub_tests)

    add_test(
        NAME mediaserver.net_director_session_hub
        COMMAND mediaserver_director_session_hub_tests
    )
    set_target_properties(
        mediaserver_director_session_hub_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_director_frame_barrier_tests
        tests/director_frame_barrier_tests.cpp
    )
    target_link_libraries(mediaserver_director_frame_barrier_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_director_frame_barrier_tests)

    add_test(
        NAME mediaserver.net_director_frame_barrier
        COMMAND mediaserver_director_frame_barrier_tests
    )
    set_target_properties(
        mediaserver_director_frame_barrier_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_director_session_service_tests
        tests/director_session_service_tests.cpp
    )
    target_link_libraries(mediaserver_director_session_service_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_director_session_service_tests)

    add_test(
        NAME mediaserver.net_director_session_service
        COMMAND mediaserver_director_session_service_tests
    )
    set_target_properties(
        mediaserver_director_session_service_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_heartbeat_schedule_tests
        tests/heartbeat_schedule_tests.cpp
    )
    target_link_libraries(mediaserver_heartbeat_schedule_tests
        PRIVATE MediaServer::Net
    )
    mediaserver_set_project_options(mediaserver_heartbeat_schedule_tests)

    add_test(
        NAME mediaserver.net_heartbeat_schedule
        COMMAND mediaserver_heartbeat_schedule_tests
    )
    set_target_properties(
        mediaserver_heartbeat_schedule_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_show_playback_tests
        tests/show_playback_tests.cpp
    )
    target_link_libraries(mediaserver_show_playback_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_show_playback_tests)

    add_test(
        NAME mediaserver.show_playback
        COMMAND mediaserver_show_playback_tests
    )
    set_target_properties(
        mediaserver_show_playback_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_node_session_service_tests
        tests/node_session_service_tests.cpp
    )
    target_link_libraries(mediaserver_node_session_service_tests
        PRIVATE
            MediaServer::Net
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_node_session_service_tests)

    add_test(
        NAME mediaserver.net_node_session_service
        COMMAND mediaserver_node_session_service_tests
    )
    set_target_properties(
        mediaserver_node_session_service_tests
        PROPERTIES FOLDER "tests"
    )
endif()
