find_package(SDL3 CONFIG REQUIRED)

add_library(mediaserver_rhi STATIC
    src/sdl_gpu_device.cpp
)
add_library(MediaServer::Rhi ALIAS mediaserver_rhi)

target_include_directories(mediaserver_rhi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# SDL3 is private on purpose: the seam exists so that nothing above it names a
# backend type, and a PUBLIC link would let one leak in by accident.
target_link_libraries(mediaserver_rhi
    PUBLIC
        MediaServer::Core
    PRIVATE
        SDL3::SDL3
)

mediaserver_set_project_options(mediaserver_rhi)
mediaserver_disable_rtti(mediaserver_rhi)

set_target_properties(mediaserver_rhi PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_rhi_device_tests tests/rhi_device_tests.cpp)
    target_link_libraries(mediaserver_rhi_device_tests
        PRIVATE
            MediaServer::Rhi
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_rhi_device_tests)

    add_test(NAME mediaserver.rhi_device COMMAND mediaserver_rhi_device_tests)
    # The offscreen path needs a GPU but not a display, and the dummy driver
    # keeps that true on a build machine with no desktop session.
    set_tests_properties(mediaserver.rhi_device PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(mediaserver_rhi_device_tests PROPERTIES FOLDER "tests")
endif()
