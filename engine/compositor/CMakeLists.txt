add_library(mediaserver_compositor STATIC
    src/compositor_graph.cpp
    src/compositor_model.cpp
)
add_library(MediaServer::Compositor ALIAS mediaserver_compositor)

target_include_directories(mediaserver_compositor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# The graph is a document projection. Scene contributes only deterministic
# transform arithmetic for RadialArray; no renderer or graphics API enters the
# dependency chain, so this remains available in headless builds.
target_link_libraries(mediaserver_compositor
    PUBLIC MediaServer::Core MediaServer::Doc MediaServer::Scene
)

mediaserver_set_project_options(mediaserver_compositor)
mediaserver_disable_rtti(mediaserver_compositor)
mediaserver_set_deterministic_float_options(mediaserver_compositor)
set_target_properties(mediaserver_compositor PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_compositor_model_tests
        tests/compositor_model_tests.cpp
    )
    target_link_libraries(mediaserver_compositor_model_tests
        PRIVATE MediaServer::Compositor
    )
    mediaserver_set_project_options(mediaserver_compositor_model_tests)
    mediaserver_set_deterministic_float_options(
        mediaserver_compositor_model_tests
    )
    add_test(
        NAME mediaserver.compositor_model
        COMMAND mediaserver_compositor_model_tests
    )
    set_target_properties(mediaserver_compositor_model_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_compositor_spatial_effect_tests
        tests/compositor_spatial_effect_tests.cpp
    )
    target_link_libraries(mediaserver_compositor_spatial_effect_tests
        PRIVATE MediaServer::Compositor
    )
    mediaserver_set_project_options(
        mediaserver_compositor_spatial_effect_tests
    )
    mediaserver_set_deterministic_float_options(
        mediaserver_compositor_spatial_effect_tests
    )
    add_test(
        NAME mediaserver.compositor_spatial_effect
        COMMAND mediaserver_compositor_spatial_effect_tests
    )
    set_target_properties(mediaserver_compositor_spatial_effect_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_compositor_keyer_tests
        tests/compositor_keyer_tests.cpp
    )
    target_link_libraries(mediaserver_compositor_keyer_tests
        PRIVATE MediaServer::Compositor
    )
    mediaserver_set_project_options(mediaserver_compositor_keyer_tests)
    mediaserver_set_deterministic_float_options(
        mediaserver_compositor_keyer_tests
    )
    add_test(
        NAME mediaserver.compositor_keyer
        COMMAND mediaserver_compositor_keyer_tests
    )
    set_target_properties(mediaserver_compositor_keyer_tests
        PROPERTIES FOLDER "tests"
    )
endif()
