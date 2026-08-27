add_library(mediaserver_persist STATIC
    src/command_log.cpp
    src/durable_director.cpp
    src/project_file.cpp
    src/settings.cpp
    src/release_ledger.cpp
    src/sealed_frame_log.cpp
)
add_library(MediaServer::Persist ALIAS mediaserver_persist)

target_include_directories(mediaserver_persist
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(mediaserver_persist
    PUBLIC
        MediaServer::Cmd
        MediaServer::Core
        MediaServer::Wire
)

mediaserver_set_project_options(mediaserver_persist)
mediaserver_disable_rtti(mediaserver_persist)

set_target_properties(mediaserver_persist PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(mediaserver_persist_tests
        tests/sealed_frame_log_tests.cpp
    )
    target_link_libraries(mediaserver_persist_tests PRIVATE MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_persist_tests)

    add_test(NAME mediaserver.persist COMMAND mediaserver_persist_tests)
    set_target_properties(mediaserver_persist_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_command_log_tests
        tests/command_log_tests.cpp
    )
    target_link_libraries(mediaserver_command_log_tests
        PRIVATE
            MediaServer::Persist
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_command_log_tests)

    add_test(NAME mediaserver.command_log COMMAND mediaserver_command_log_tests)
    set_target_properties(mediaserver_command_log_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_durable_director_tests
        tests/durable_director_tests.cpp
    )
    target_link_libraries(mediaserver_durable_director_tests PRIVATE MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_durable_director_tests)

    add_test(NAME mediaserver.durable_director COMMAND mediaserver_durable_director_tests)
    set_target_properties(mediaserver_durable_director_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_release_ledger_tests
        tests/release_ledger_tests.cpp
    )
    target_link_libraries(mediaserver_release_ledger_tests PRIVATE MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_release_ledger_tests)

    add_test(NAME mediaserver.release_ledger COMMAND mediaserver_release_ledger_tests)
    set_target_properties(mediaserver_release_ledger_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_durable_release_tests
        tests/durable_release_tests.cpp
    )
    target_link_libraries(mediaserver_durable_release_tests PRIVATE MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_durable_release_tests)

    add_test(NAME mediaserver.durable_release COMMAND mediaserver_durable_release_tests)
    set_target_properties(mediaserver_durable_release_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_project_file_tests
        tests/project_file_tests.cpp
    )
    target_link_libraries(mediaserver_project_file_tests PRIVATE MediaServer::Persist)
    mediaserver_set_project_options(mediaserver_project_file_tests)

    add_test(NAME mediaserver.project_file COMMAND mediaserver_project_file_tests)
    set_target_properties(mediaserver_project_file_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_settings_tests
        tests/settings_tests.cpp
    )
    target_link_libraries(mediaserver_settings_tests
        PRIVATE MediaServer::Persist
    )
    mediaserver_set_project_options(mediaserver_settings_tests)

    add_test(
        NAME mediaserver.settings
        COMMAND mediaserver_settings_tests
    )
    set_target_properties(mediaserver_settings_tests
        PROPERTIES FOLDER "tests"
    )

endif()
