# ajfx Studio: the scene workspace that authors .ajfx particle effects.
#
# The scene model, its lowering to the block compiler's layers, and the
# compiler itself are plain C++ on ajfx alone (ajfx_studio_core), so they are
# tested headless. The application
# previews through the media server's own renderer, so an effect looks in
# Studio exactly as it will on the wall, and draws its interface with the
# media server's UI toolkit; it is built when the host turns on
# AJFX_BUILD_STUDIO_APP (the repository root does, with the GUI on).

add_library(ajfx_studio_core STATIC
    src/json.cpp
    src/document.cpp
    src/scene.cpp
    src/scene_properties.cpp
    src/scene_io.cpp
    src/timeline.cpp
    src/scene_stats.cpp
    src/catalog.cpp
    src/catalog_birth.cpp
    src/catalog_motion.cpp
    src/catalog_look.cpp
    src/catalog_contact.cpp
    src/catalog_rigid.cpp
    src/catalog_expert.cpp
    src/raw_modules.cpp
    src/library.cpp
    src/compile.cpp
    src/compile_worker.cpp
    src/layer_builder.cpp
    src/rigid_builder.cpp
    src/solid_world.cpp
    src/document_io.cpp
    src/guides.cpp
    src/user_assets.cpp
    src/session.cpp
    src/bake.cpp
    src/stage_pick.cpp
    src/performance.cpp
    src/stage.cpp
)
target_include_directories(ajfx_studio_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(ajfx_studio_core PUBLIC ajfx::ajfx)
ajfx_set_options(ajfx_studio_core)
set_target_properties(ajfx_studio_core PROPERTIES FOLDER "ajfx")

if(AJFX_BUILD_STUDIO_APP)
    if(PROJECT_IS_TOP_LEVEL OR NOT TARGET ajfx::d3d12)
        message(FATAL_ERROR "ajfx-studio needs the D3D12 backend and the media server renderer; "
                            "build it from the repository root")
    endif()
    find_package(tinyfiledialogs CONFIG REQUIRED)

    # The scene's tree, inspector and controls: drawn by the application and,
    # headless of the window, by the UI tests.
    add_library(ajfx_studio_page STATIC
        src/widgets.cpp
        src/scene_view.cpp
                            src/stage_camera.cpp
        )
    target_link_libraries(ajfx_studio_page PUBLIC
        ajfx_studio_core
        MediaServer::Gui
        MediaServer::Editor
        tinyfiledialogs::tinyfiledialogs
    )
    ajfx_set_options(ajfx_studio_page)
    set_target_properties(ajfx_studio_page PROPERTIES FOLDER "ajfx")

    add_executable(ajfx-studio
        src/main.cpp
        src/preview.cpp
            src/package_builder.cpp
        src/studio_app.cpp
        src/stage_overlay.cpp
        )
    target_link_libraries(ajfx-studio PRIVATE
        ajfx_studio_core
        ajfx_studio_page
        ajfx::d3d12
        ajfx::rigid
        MediaServer::Render
        MediaServer::Gui
        MediaServer::Editor
        tinyfiledialogs::tinyfiledialogs
    )
    ajfx_set_options(ajfx-studio)
    # Kernels are compiled as an artist edits, so DXC travels with Studio.
    ajfx_copy_dxc(ajfx-studio)
    set_target_properties(ajfx-studio PROPERTIES FOLDER "ajfx")

    if(BUILD_TESTING)
        # Studio's own controls and the scene workspace against synthetic
        # input: they need the media server's UI toolkit, so they are built
        # with the application.
        add_executable(ajfx_studio_ui_tests tests/studio_ui_tests.cpp)
        target_link_libraries(ajfx_studio_ui_tests PRIVATE ajfx_studio_page)
        ajfx_set_options(ajfx_studio_ui_tests)
        add_test(NAME ajfx.studio_ui COMMAND ajfx_studio_ui_tests)
        set_target_properties(ajfx_studio_ui_tests PROPERTIES FOLDER "ajfx/tests")

        # The preview's builder, with the kernel compiler and CPU reference:
        # builds keep pace while the CPU run of an earlier one finishes.
        add_executable(ajfx_studio_builder_tests tests/package_builder_tests.cpp src/package_builder.cpp)
        target_link_libraries(ajfx_studio_builder_tests PRIVATE
            ajfx_studio_core
            ajfx::d3d12
            ajfx::rigid
            MediaServer::Render
        )
        ajfx_set_options(ajfx_studio_builder_tests)
        ajfx_copy_dxc(ajfx_studio_builder_tests)
        add_test(NAME ajfx.studio_builder COMMAND ajfx_studio_builder_tests)
        set_target_properties(ajfx_studio_builder_tests PROPERTIES FOLDER "ajfx/tests")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(ajfx_studio_tests
        tests/studio_tests.cpp
        tests/scene_tests.cpp
        tests/stream_object_tests.cpp
        tests/catalog_tests.cpp
        tests/document_tests.cpp
        tests/compile_tests.cpp
        tests/rigid_objects_tests.cpp
        tests/rigid_break_tests.cpp
        tests/guides_tests.cpp
        tests/bake_tests.cpp
        tests/world_tests.cpp
        tests/solid_world_tests.cpp
        tests/stage_pick_tests.cpp
        tests/performance_tests.cpp
        tests/stage_tests.cpp
        tests/fixtures/recipes.cpp
        tests/fixtures/blast.cpp
    )
    # The old page recipes are kept as compiler fixtures; the rock pile is run
    # through the physics engine.
    target_include_directories(ajfx_studio_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
    target_link_libraries(ajfx_studio_tests PRIVATE ajfx_studio_core ajfx::rigid)
    if(TARGET ajfx::d3d12)
        # Kernels for every recipe compile, where DXC is present.
        target_link_libraries(ajfx_studio_tests PRIVATE ajfx::d3d12)
        target_compile_definitions(ajfx_studio_tests PRIVATE AJFX_STUDIO_TEST_KERNELS=1)
        ajfx_copy_dxc(ajfx_studio_tests)
    endif()
    ajfx_set_options(ajfx_studio_tests)
    add_test(NAME ajfx.studio COMMAND ajfx_studio_tests)
    set_target_properties(ajfx_studio_tests PROPERTIES FOLDER "ajfx/tests")

    # Times what the application does on its interface thread each frame and
    # per edit. Not a test: run it to see where a frame goes.
    add_executable(ajfx_studio_frame_bench tests/frame_bench.cpp)
    target_link_libraries(ajfx_studio_frame_bench PRIVATE ajfx_studio_core)
    ajfx_set_options(ajfx_studio_frame_bench)
    set_target_properties(ajfx_studio_frame_bench PROPERTIES FOLDER "ajfx/tests")
endif()
