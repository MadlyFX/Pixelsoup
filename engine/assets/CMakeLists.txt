add_library(mediaserver_assets STATIC
    src/asset_path.cpp
    src/content_fingerprint.cpp
    src/derivative.cpp
    src/derivative_cache.cpp
    src/proxy_frame_map.cpp
    src/proxy_policy.cpp
    src/waveform.cpp
    src/asset_registry.cpp
    src/asset_model.cpp
    src/library_edit.cpp
    src/plateset.cpp
)
add_library(MediaServer::Assets ALIAS mediaserver_assets)

target_include_directories(mediaserver_assets
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(mediaserver_assets
    PUBLIC MediaServer::Core MediaServer::Doc
)

mediaserver_set_project_options(mediaserver_assets)
mediaserver_disable_rtti(mediaserver_assets)
set_target_properties(mediaserver_assets PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    add_executable(mediaserver_assets_model_tests
        tests/asset_model_tests.cpp
    )
    target_link_libraries(mediaserver_assets_model_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_model_tests)
    add_test(
        NAME mediaserver.assets_model
        COMMAND mediaserver_assets_model_tests
    )
    set_target_properties(mediaserver_assets_model_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_plateset_tests
        tests/plateset_tests.cpp
    )
    target_link_libraries(mediaserver_assets_plateset_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_plateset_tests)
    add_test(
        NAME mediaserver.assets_plateset
        COMMAND mediaserver_assets_plateset_tests
    )
    set_target_properties(mediaserver_assets_plateset_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_path_tests
        tests/asset_path_tests.cpp
    )
    target_link_libraries(mediaserver_assets_path_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_path_tests)
    add_test(
        NAME mediaserver.assets_path
        COMMAND mediaserver_assets_path_tests
    )
    set_target_properties(mediaserver_assets_path_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_content_fingerprint_tests
        tests/content_fingerprint_tests.cpp
    )
    target_link_libraries(mediaserver_assets_content_fingerprint_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(
        mediaserver_assets_content_fingerprint_tests
    )
    add_test(
        NAME mediaserver.assets_content_fingerprint
        COMMAND mediaserver_assets_content_fingerprint_tests
    )
    set_target_properties(mediaserver_assets_content_fingerprint_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_derivative_tests
        tests/derivative_tests.cpp
    )
    target_link_libraries(mediaserver_assets_derivative_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_derivative_tests)
    add_test(
        NAME mediaserver.assets_derivative
        COMMAND mediaserver_assets_derivative_tests
    )
    set_target_properties(mediaserver_assets_derivative_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_derivative_cache_tests
        tests/derivative_cache_tests.cpp
    )
    target_link_libraries(mediaserver_assets_derivative_cache_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_derivative_cache_tests)
    add_test(
        NAME mediaserver.assets_derivative_cache
        COMMAND mediaserver_assets_derivative_cache_tests
    )
    set_target_properties(mediaserver_assets_derivative_cache_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_proxy_frame_map_tests
        tests/proxy_frame_map_tests.cpp
    )
    target_link_libraries(mediaserver_assets_proxy_frame_map_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_proxy_frame_map_tests)
    add_test(
        NAME mediaserver.assets_proxy_frame_map
        COMMAND mediaserver_assets_proxy_frame_map_tests
    )
    set_target_properties(mediaserver_assets_proxy_frame_map_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_proxy_policy_tests
        tests/proxy_policy_tests.cpp
    )
    target_link_libraries(mediaserver_assets_proxy_policy_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_proxy_policy_tests)
    add_test(
        NAME mediaserver.assets_proxy_policy
        COMMAND mediaserver_assets_proxy_policy_tests
    )
    set_target_properties(mediaserver_assets_proxy_policy_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_waveform_tests
        tests/waveform_tests.cpp
    )
    target_link_libraries(mediaserver_assets_waveform_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_waveform_tests)
    add_test(
        NAME mediaserver.assets_waveform
        COMMAND mediaserver_assets_waveform_tests
    )
    set_target_properties(mediaserver_assets_waveform_tests
        PROPERTIES FOLDER "tests"
    )
endif()
