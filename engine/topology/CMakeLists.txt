add_library(mediaserver_topology STATIC
    src/hardware_map.cpp
    src/hardware_map_model.cpp
    src/hardware_signal.cpp
)
add_library(MediaServer::Topology ALIAS mediaserver_topology)

target_include_directories(mediaserver_topology
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(mediaserver_topology
    PUBLIC MediaServer::Core MediaServer::Doc
)

mediaserver_set_project_options(mediaserver_topology)
mediaserver_disable_rtti(mediaserver_topology)
set_target_properties(mediaserver_topology PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_topology_hardware_map_tests
        tests/hardware_map_model_tests.cpp
    )
    target_link_libraries(mediaserver_topology_hardware_map_tests
        PRIVATE MediaServer::Topology
    )
    mediaserver_set_project_options(mediaserver_topology_hardware_map_tests)
    add_test(
        NAME mediaserver.topology_hardware_map
        COMMAND mediaserver_topology_hardware_map_tests
    )
    set_target_properties(mediaserver_topology_hardware_map_tests
        PROPERTIES FOLDER "tests"
    )
endif()
