add_library(mediaserver_show STATIC
    src/demo_scene.cpp
    src/demo_feed_map.cpp
    src/demo_edit_scene.cpp
    src/frame_boundary_driver.cpp
    src/frame_root_mailbox.cpp
    src/frame_waiter.cpp
    src/pointer_overlay.cpp
    src/presenter.cpp
    src/show_clock.cpp
    src/switcher_author.cpp
    src/switcher_authority.cpp
    src/switcher_state.cpp
    src/test_pattern.cpp
    src/transport_author.cpp
    src/transport_authority.cpp
    src/transport_state.cpp
)
add_library(MediaServer::Show ALIAS mediaserver_show)

target_include_directories(mediaserver_show
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# Scene and Output are PUBLIC because the presentation seam names their types: a
# node presents either a test pattern, its slice of a 3D scene, or that slice
# put through a feed map, and all three are part of the interface a presenter
# implements.
target_link_libraries(mediaserver_show
    PUBLIC
        MediaServer::Cmd
        MediaServer::Core
        MediaServer::Scene
        MediaServer::Output
)

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

    add_executable(mediaserver_pointer_overlay_tests
        tests/pointer_overlay_tests.cpp
    )
    target_link_libraries(mediaserver_pointer_overlay_tests
        PRIVATE MediaServer::Show
    )
    mediaserver_set_project_options(mediaserver_pointer_overlay_tests)
    add_test(
        NAME mediaserver.show_pointer_overlay
        COMMAND mediaserver_pointer_overlay_tests
    )
    set_target_properties(mediaserver_pointer_overlay_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_video_reference_tests
        tests/video_reference_tests.cpp
    )
    target_link_libraries(mediaserver_video_reference_tests
        PRIVATE MediaServer::Show
    )
    mediaserver_set_project_options(mediaserver_video_reference_tests)
    add_test(
        NAME mediaserver.show_video_reference
        COMMAND mediaserver_video_reference_tests
    )
    set_target_properties(
        mediaserver_video_reference_tests
        PROPERTIES FOLDER "tests"
    )

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

    add_executable(mediaserver_transport_anchor_tests
        tests/transport_anchor_tests.cpp
    )
    target_link_libraries(mediaserver_transport_anchor_tests
        PRIVATE MediaServer::Show
    )
    mediaserver_set_project_options(mediaserver_transport_anchor_tests)
    add_test(
        NAME mediaserver.show_transport_anchor
        COMMAND mediaserver_transport_anchor_tests
    )
    set_target_properties(
        mediaserver_transport_anchor_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_test_pattern_tests tests/test_pattern_tests.cpp)
    target_link_libraries(mediaserver_test_pattern_tests PRIVATE MediaServer::Show)
    mediaserver_set_project_options(mediaserver_test_pattern_tests)
    add_test(NAME mediaserver.show_test_pattern COMMAND mediaserver_test_pattern_tests)
    set_target_properties(mediaserver_test_pattern_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_frame_waiter_tests tests/frame_waiter_tests.cpp)
    target_link_libraries(mediaserver_frame_waiter_tests PRIVATE MediaServer::Show)
    mediaserver_set_project_options(mediaserver_frame_waiter_tests)
    add_test(NAME mediaserver.show_frame_waiter COMMAND mediaserver_frame_waiter_tests)
    set_target_properties(mediaserver_frame_waiter_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_presenter_tests tests/presenter_tests.cpp)
    target_link_libraries(mediaserver_presenter_tests PRIVATE MediaServer::Show)
    mediaserver_set_project_options(mediaserver_presenter_tests)
    add_test(NAME mediaserver.show_presenter COMMAND mediaserver_presenter_tests)
    set_target_properties(mediaserver_presenter_tests PROPERTIES FOLDER "tests")

    add_executable(mediaserver_switcher_tests tests/switcher_tests.cpp)
    target_link_libraries(mediaserver_switcher_tests PRIVATE MediaServer::Show)
    mediaserver_set_project_options(mediaserver_switcher_tests)
    add_test(NAME mediaserver.show_switcher COMMAND mediaserver_switcher_tests)
    set_target_properties(mediaserver_switcher_tests PROPERTIES FOLDER "tests")
endif()
