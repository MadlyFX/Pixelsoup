add_library(mediaserver_cmd STATIC
    src/command.cpp
    src/director_scheduler.cpp
    src/loopback_transport.cpp
    src/node_command_bus.cpp
    src/reducer.cpp
)
add_library(MediaServer::Cmd ALIAS mediaserver_cmd)

target_include_directories(mediaserver_cmd
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(mediaserver_cmd PUBLIC MediaServer::Doc MediaServer::Core)

mediaserver_set_project_options(mediaserver_cmd)
mediaserver_disable_rtti(mediaserver_cmd)

set_target_properties(mediaserver_cmd PROPERTIES FOLDER "engine")
