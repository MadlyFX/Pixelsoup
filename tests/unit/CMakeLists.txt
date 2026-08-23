add_executable(mediaserver_unit_tests
    main.cpp
    command_fingerprint_tests.cpp
    core_tests.cpp
    document_tests.cpp
    director_scheduler_tests.cpp
    loopback_tests.cpp
    node_command_bus_tests.cpp
    wire_codec_tests.cpp
)
target_link_libraries(mediaserver_unit_tests PRIVATE MediaServer::Wire)
target_include_directories(mediaserver_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

mediaserver_set_project_options(mediaserver_unit_tests)

add_test(NAME mediaserver.unit COMMAND mediaserver_unit_tests)
set_target_properties(mediaserver_unit_tests PROPERTIES FOLDER "tests")
