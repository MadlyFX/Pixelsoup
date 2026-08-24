add_library(mediaserver_scene STATIC
    src/canvas.cpp
    src/geometry.cpp
    src/manipulate.cpp
    src/math.cpp
    src/scene.cpp
    src/scene_model.cpp
)
add_library(MediaServer::Scene ALIAS mediaserver_scene)

target_include_directories(mediaserver_scene
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Core and doc, and deliberately nothing else. This library is the document's
# view of a 3D scene; it names no graphics API, no window, and no transport, so
# a headless build and a test both get it for free.
target_link_libraries(mediaserver_scene PUBLIC MediaServer::Core MediaServer::Doc)

mediaserver_set_project_options(mediaserver_scene)
mediaserver_disable_rtti(mediaserver_scene)
# Every node in a cohort evaluates this arithmetic and must reach the same
# bits. See the comment on the function itself.
mediaserver_set_deterministic_float_options(mediaserver_scene)

set_target_properties(mediaserver_scene PROPERTIES FOLDER "engine")

if(BUILD_TESTING)
    # The tests carry the golden digests that pin the arithmetic, so they are
    # built with the same floating-point settings as the library. Built any
    # other way they would be testing a different computation.
    foreach(_scene_suite IN ITEMS math geometry slice model manipulate)
        add_executable(mediaserver_scene_${_scene_suite}_tests
            tests/scene_${_scene_suite}_tests.cpp
        )
        target_link_libraries(mediaserver_scene_${_scene_suite}_tests
            PRIVATE MediaServer::Scene
        )
        mediaserver_set_project_options(mediaserver_scene_${_scene_suite}_tests)
        mediaserver_set_deterministic_float_options(
            mediaserver_scene_${_scene_suite}_tests
        )
        add_test(
            NAME mediaserver.scene_${_scene_suite}
            COMMAND mediaserver_scene_${_scene_suite}_tests
        )
        set_target_properties(mediaserver_scene_${_scene_suite}_tests
            PROPERTIES FOLDER "tests"
        )
    endforeach()
endif()
