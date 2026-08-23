add_library(mediaserver_doc STATIC
    src/document.cpp
)
add_library(MediaServer::Doc ALIAS mediaserver_doc)

target_include_directories(mediaserver_doc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(mediaserver_doc PUBLIC MediaServer::Core)

mediaserver_set_project_options(mediaserver_doc)
mediaserver_disable_rtti(mediaserver_doc)

set_target_properties(mediaserver_doc PROPERTIES FOLDER "engine")

