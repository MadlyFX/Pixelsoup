# Device control protocols: talking to the hardware on the Hardware Map that
# is not a render machine. Today that is NovaStar COEX LED processors over
# their HTTP API. It is headless - sockets and JSON, no window, no GPU - and
# keeps every device round trip on its own thread, never the control thread.
find_package(asio CONFIG REQUIRED)

add_library(mediaserver_devices STATIC
    src/http_client.cpp
    src/json.cpp
    src/novastar_coex.cpp
    src/processor_monitor.cpp
)
add_library(MediaServer::Devices ALIAS mediaserver_devices)

target_include_directories(mediaserver_devices
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(mediaserver_devices
    PUBLIC
        MediaServer::Core
        MediaServer::Doc
        MediaServer::Topology
    PRIVATE
        MediaServer::Net
        asio::asio
)

if(WIN32)
    target_compile_definitions(mediaserver_devices PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(mediaserver_devices PRIVATE ws2_32)
endif()

mediaserver_set_project_options(mediaserver_devices)
mediaserver_disable_rtti(mediaserver_devices)
set_target_properties(mediaserver_devices PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(mediaserver_devices_tests
        tests/devices_tests.cpp
    )
    target_link_libraries(mediaserver_devices_tests
        PRIVATE
            MediaServer::Devices
            asio::asio
            Threads::Threads
    )
    if(WIN32)
        target_compile_definitions(mediaserver_devices_tests
            PRIVATE _WIN32_WINNT=0x0A00
        )
    endif()
    mediaserver_set_project_options(mediaserver_devices_tests)
    add_test(NAME mediaserver.devices COMMAND mediaserver_devices_tests)
    set_target_properties(mediaserver_devices_tests PROPERTIES FOLDER "tests")
endif()
