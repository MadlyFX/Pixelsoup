add_library(mediaserver_show STATIC
    src/frame_boundary_driver.cpp
    src/frame_root_mailbox.cpp
    src/presenter.cpp
    src/show_clock.cpp
    src/test_pattern.cpp
    src/transport_author.cpp
    src/transport_state.cpp
)
add_library(MediaServer::Show ALIAS mediaserver_show)

target_include_directories(mediaserver_show
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(mediaserver_show PUBLIC MediaServer::Cmd MediaServer::Core)

mediaserver_set_project_options(mediaserver_show)
mediaserver_disable_rtti(mediaserver_show)

set_target_properties(mediaserver_show PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(mediaserver_show_clock_tests tests/show_clock_tests.cpp)
    target_link_libraries(mediaserver_show_clock_tests PRIVATE MediaServer::Show)
    mediaserver_set_project_options(mediaserver_show_clock_tests)
    add_test(NAME mediaserver.show_clock COMMAND mediaserver_show_clock_tests)
    set_target_properties(mediaserver_show_clock_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_frame_root_mailbox_tests
        tests/frame_root_mailbox_tests.cpp
    )
    target_link_libraries(mediaserver_frame_root_mailbox_tests
        PRIVATE
            MediaServer::Show
            Threads::Threads
    )
    mediaserver_set_project_options(mediaserver_frame_root_mailbox_tests)
    add_test(
        NAME mediaserver.show_frame_root_mailbox
        COMMAND mediaserver_frame_root_mailbox_tests
    )
    set_target_properties(
        mediaserver_frame_root_mailbox_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_frame_boundary_driver_tests
        tests/frame_boundary_driver_tests.cpp
    )
    target_link_libraries(mediaserver_frame_boundary_driver_tests
        PRIVATE MediaServer::Show
    )
    mediaserver_set_project_options(mediaserver_frame_boundary_driver_tests)
    add_test(
        NAME mediaserver.show_frame_boundary_driver
        COMMAND mediaserver_frame_boundary_driver_tests
    )
    set_target_properties(
        mediaserver_frame_boundary_driver_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_test_pattern_tests tests/test_pattern_tests.cpp)
    target_link_libraries(mediaserver_test_pattern_tests PRIVATE MediaServer::Show)
    mediaserver_set_project_options(mediaserver_test_pattern_tests)
    add_test(NAME mediaserver.show_test_pattern COMMAND mediaserver_test_pattern_tests)
    set_target_properties(mediaserver_test_pattern_tests PROPERTIES FOLDER "tests")
endif()
