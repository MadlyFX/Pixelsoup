add_library(mediaserver_track STATIC
    src/freed.cpp
    src/natnet.cpp
    src/opentrackio.cpp
    src/pose_channel.cpp
    src/pose_slot.cpp
    src/tracked_pose.cpp
)
add_library(MediaServer::Track ALIAS mediaserver_track)

target_include_directories(mediaserver_track
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Core, and deliberately nothing else. This library names no socket, no scene,
# no document, and no renderer. That is the structural reason a render node
# cannot read its own tracker: there is nothing here that could open one, and
# nothing here that knows what a node is.
target_link_libraries(mediaserver_track PUBLIC MediaServer::Core)

mediaserver_set_project_options(mediaserver_track)
mediaserver_disable_rtti(mediaserver_track)
# A pose crosses the wire and is then evaluated on every node. The parsers must
# therefore produce identical bits from identical bytes on every machine, which
# is the same requirement the scene layer has.
mediaserver_set_deterministic_float_options(mediaserver_track)

set_target_properties(mediaserver_track PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    foreach(_track_suite IN ITEMS freed natnet opentrackio pose_slot pose_channel)
        add_executable(mediaserver_track_${_track_suite}_tests
            tests/${_track_suite}_tests.cpp
        )
        target_link_libraries(mediaserver_track_${_track_suite}_tests
            PRIVATE MediaServer::Track
        )
        mediaserver_set_project_options(
            mediaserver_track_${_track_suite}_tests
        )
        mediaserver_set_deterministic_float_options(
            mediaserver_track_${_track_suite}_tests
        )
        add_test(
            NAME mediaserver.track_${_track_suite}
            COMMAND mediaserver_track_${_track_suite}_tests
        )
        set_target_properties(mediaserver_track_${_track_suite}_tests
            PROPERTIES FOLDER "tests"
        )
    endforeach()

    find_package(Threads REQUIRED)
    target_link_libraries(mediaserver_track_pose_slot_tests
        PRIVATE Threads::Threads
    )
endif()
