add_library(mediaserver_output STATIC
    src/feed_map.cpp
    src/feed_map_model.cpp
)
add_library(MediaServer::Output ALIAS mediaserver_output)

target_include_directories(mediaserver_output
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Core, doc, and scene - the last only for its Canvas/CanvasRect vocabulary, so
# a feed map is expressed against the same geometry a scene slice is. It names
# no graphics API, no window, and no transport, so a headless build gets it for
# free just as the scene layer does.
target_link_libraries(mediaserver_output
    PUBLIC MediaServer::Core MediaServer::Doc MediaServer::Scene
)

mediaserver_set_project_options(mediaserver_output)
mediaserver_disable_rtti(mediaserver_output)
# The digest pins the authored parameters bit for bit, and the blend weight is a
# curve two neighbours have to agree on; both are built with the same
# deterministic floating-point settings as the scene layer.
mediaserver_set_deterministic_float_options(mediaserver_output)

set_target_properties(mediaserver_output PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_output_feed_map_model_tests
        tests/feed_map_model_tests.cpp
    )
    target_link_libraries(mediaserver_output_feed_map_model_tests
        PRIVATE MediaServer::Output
    )
    mediaserver_set_project_options(mediaserver_output_feed_map_model_tests)
    # The tests carry golden digests and assert the blend is an exact partition
    # of unity, so they are built with the library's floating-point settings.
    mediaserver_set_deterministic_float_options(
        mediaserver_output_feed_map_model_tests
    )
    add_test(
        NAME mediaserver.output_feed_map_model
        COMMAND mediaserver_output_feed_map_model_tests
    )
    set_target_properties(mediaserver_output_feed_map_model_tests
        PROPERTIES FOLDER "tests"
    )
endif()
