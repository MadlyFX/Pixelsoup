add_library(mediaserver_sync STATIC
    src/sync_policy.cpp
    src/sync_policy_model.cpp
    src/sync_group.cpp
    src/sync_readiness.cpp
    src/sync_status.cpp
)
add_library(MediaServer::Sync ALIAS mediaserver_sync)

target_include_directories(mediaserver_sync
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(mediaserver_sync
    PUBLIC MediaServer::Core MediaServer::Doc MediaServer::Topology
)

mediaserver_set_project_options(mediaserver_sync)
mediaserver_disable_rtti(mediaserver_sync)
set_target_properties(mediaserver_sync PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_sync_policy_tests
        tests/sync_policy_tests.cpp
    )
    target_link_libraries(mediaserver_sync_policy_tests
        PRIVATE MediaServer::Sync
    )
    mediaserver_set_project_options(mediaserver_sync_policy_tests)
    add_test(NAME mediaserver.sync_policy COMMAND mediaserver_sync_policy_tests)
    set_target_properties(mediaserver_sync_policy_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_sync_topology_tests
        tests/sync_topology_tests.cpp
    )
    target_link_libraries(mediaserver_sync_topology_tests
        PRIVATE MediaServer::Sync
    )
    mediaserver_set_project_options(mediaserver_sync_topology_tests)
    add_test(
        NAME mediaserver.sync_topology
        COMMAND mediaserver_sync_topology_tests
    )
    set_target_properties(mediaserver_sync_topology_tests
        PROPERTIES FOLDER "tests"
    )
endif()
