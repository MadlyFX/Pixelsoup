find_package(SDL3 CONFIG REQUIRED)

add_library(mediaserver_ui STATIC
    src/gpu_output_target.cpp
    src/sdl_presenter.cpp
)
add_library(MediaServer::Ui ALIAS mediaserver_ui)

target_include_directories(mediaserver_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(mediaserver_ui
    PUBLIC
        MediaServer::Show
        MediaServer::Rhi
    PRIVATE
        # The scene renderer is an implementation detail of the presenter's 3D
        # path: nothing above ui names it, so it links privately.
        MediaServer::Render
        MediaServer::Shaders
        SDL3::SDL3
)

mediaserver_set_project_options(mediaserver_ui)
mediaserver_disable_rtti(mediaserver_ui)

set_target_properties(mediaserver_ui PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_sdl_presenter_tests
        tests/sdl_presenter_tests.cpp
    )
    target_link_libraries(mediaserver_sdl_presenter_tests
        PRIVATE
            MediaServer::Ui
            MediaServer::Render
            MediaServer::Rhi
            SDL3::SDL3
    )
    mediaserver_set_project_options(mediaserver_sdl_presenter_tests)

    add_test(
        NAME mediaserver.ui_sdl_presenter
        COMMAND mediaserver_sdl_presenter_tests
    )
    # The dummy video driver keeps this runnable on a build machine with no
    # display, and keeps the readback free of window-manager scaling.
    set_tests_properties(mediaserver.ui_sdl_presenter PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
    )
    set_target_properties(
        mediaserver_sdl_presenter_tests
        PROPERTIES FOLDER "tests"
    )

    if(WIN32 AND MEDIASERVER_ENABLE_ADVANCED_RENDERER)
        add_executable(mediaserver_native_scene_presenter_tests
            tests/native_scene_presenter_tests.cpp
        )
        target_link_libraries(mediaserver_native_scene_presenter_tests
            PRIVATE
                MediaServer::Ui
                MediaServer::Render
                MediaServer::Rhi
                SDL3::SDL3
        )
        mediaserver_set_project_options(
            mediaserver_native_scene_presenter_tests
        )
        add_test(
            NAME mediaserver.ui_native_scene_presenter
            COMMAND mediaserver_native_scene_presenter_tests
        )
        # It checks which window holds keyboard focus, and only one window on
        # the desktop can; a window another test opens alongside takes it.
        set_tests_properties(mediaserver.ui_native_scene_presenter
            PROPERTIES RUN_SERIAL TRUE
        )
        set_target_properties(mediaserver_native_scene_presenter_tests
            PROPERTIES FOLDER "tests"
        )
    endif()
endif()
