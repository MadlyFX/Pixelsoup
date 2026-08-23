find_package(flatbuffers CONFIG REQUIRED)

set(MEDIASERVER_WIRE_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/schema/protocol.fbs")
set(MEDIASERVER_WIRE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MEDIASERVER_WIRE_GENERATED_HEADER
    "${MEDIASERVER_WIRE_GENERATED_DIR}/protocol_generated.h")

add_custom_command(
    OUTPUT "${MEDIASERVER_WIRE_GENERATED_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MEDIASERVER_WIRE_GENERATED_DIR}"
    COMMAND "$<TARGET_FILE:flatbuffers::flatc>"
        --cpp
        --scoped-enums
        --warnings-as-errors
        -o "${MEDIASERVER_WIRE_GENERATED_DIR}"
        "${MEDIASERVER_WIRE_SCHEMA}"
    DEPENDS flatbuffers::flatc "${MEDIASERVER_WIRE_SCHEMA}"
    VERBATIM
)

add_library(mediaserver_wire STATIC
    src/protocol_codec.cpp
    "${MEDIASERVER_WIRE_GENERATED_HEADER}"
)
add_library(MediaServer::Wire ALIAS mediaserver_wire)

target_include_directories(mediaserver_wire
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        "${MEDIASERVER_WIRE_GENERATED_DIR}"
)
target_link_libraries(mediaserver_wire
    PUBLIC MediaServer::Cmd
    PRIVATE flatbuffers::flatbuffers
)

mediaserver_set_project_options(mediaserver_wire)
mediaserver_disable_rtti(mediaserver_wire)

set_target_properties(mediaserver_wire PROPERTIES FOLDER "engine")
