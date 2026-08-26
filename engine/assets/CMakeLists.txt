add_library(mediaserver_assets STATIC
    src/content_identity.cpp
    src/content_resolver.cpp
    src/asset_transfer.cpp
    src/asset_registry.cpp
    src/asset_model.cpp
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
    add_executable(mediaserver_assets_transfer_tests
        tests/asset_transfer_tests.cpp
    )
    target_link_libraries(mediaserver_assets_transfer_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_transfer_tests)
    add_test(
        NAME mediaserver.assets_transfer
        COMMAND mediaserver_assets_transfer_tests
    )
    set_target_properties(mediaserver_assets_transfer_tests
        PROPERTIES FOLDER "tests"
    )

    add_executable(mediaserver_assets_content_tests
        tests/content_identity_tests.cpp
    )
    target_link_libraries(mediaserver_assets_content_tests
        PRIVATE MediaServer::Assets
    )
    mediaserver_set_project_options(mediaserver_assets_content_tests)
    add_test(
        NAME mediaserver.assets_content
        COMMAND mediaserver_assets_content_tests
    )
    set_target_properties(mediaserver_assets_content_tests
        PROPERTIES FOLDER "tests"
    )

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
endif()
