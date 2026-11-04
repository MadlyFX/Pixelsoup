# ajfx Studio: the scene workspace that authors .ajfx particle effects.
#
# The scene model and its compiler are plain C++ on ajfx alone
# (ajfx_studio_core), so they are tested headless and driven from the command
# line (ajfx-studio-cli). The application previews through the media server's
# own renderer, so an effect looks in Studio exactly as it will on the wall,
# and draws its interface with Dear ImGui; it is built when the host turns on
# AJFX_BUILD_STUDIO_APP (the repository root does, with the GUI on).

add_library(ajfx_studio_core STATIC
    src/json.cpp
    src/controls.cpp
    src/scene.cpp
    src/scene_properties.cpp
    src/scene_io.cpp
    src/timeline.cpp
    src/scene_stats.cpp
    src/library.cpp
    src/compile.cpp
    src/compile_worker.cpp
    src/document_io.cpp
    src/guides.cpp
    src/user_assets.cpp
    src/session.cpp
    src/bake.cpp
    src/stage_pick.cpp
    src/performance.cpp
    src/stage.cpp
    src/hands.cpp
    src/kinds.cpp
    src/starter_scenes.cpp
    src/issues.cpp
    src/delivery.cpp
    src/export_gltf.cpp
)
target_include_directories(ajfx_studio_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(ajfx_studio_core PUBLIC ajfx::ajfx)
ajfx_set_options(ajfx_studio_core)
set_target_properties(ajfx_studio_core PROPERTIES FOLDER "ajfx")

# A scene's performance acted on and replayed from the command line, with no
# window: the hands Studio has, before (and beside) their buttons.
if(TARGET ajfx::rigid)
    add_executable(ajfx-studio-cli src/cli.cpp)
    target_link_libraries(ajfx-studio-cli PRIVATE ajfx_studio_core ajfx::rigid)
    if(TARGET ajfx::d3d12)
        # Delivering a package needs its kernels, compiled with DXC.
        target_link_libraries(ajfx-studio-cli PRIVATE ajfx::d3d12)
        target_compile_definitions(ajfx-studio-cli PRIVATE AJFX_STUDIO_CLI_KERNELS=1)
        ajfx_copy_dxc(ajfx-studio-cli)
    endif()
    ajfx_set_options(ajfx-studio-cli)
    set_target_properties(ajfx-studio-cli PROPERTIES FOLDER "ajfx")
endif()

if(AJFX_BUILD_STUDIO_APP)
    if(PROJECT_IS_TOP_LEVEL OR NOT TARGET ajfx::d3d12)
        message(FATAL_ERROR "ajfx-studio needs the D3D12 backend and the media server renderer; "
                            "build it from the repository root")
    endif()
    find_package(tinyfiledialogs CONFIG REQUIRED)
    find_package(imgui CONFIG REQUIRED)
    find_package(imguizmo CONFIG REQUIRED)

    # The scene's tree, inspector and controls: drawn by the application and,
    # headless of the window, by the UI tests.
    add_library(ajfx_studio_page STATIC
        src/scene_view.cpp
        src/imgui_layer.cpp
        src/stage_panel.cpp
        src/stage_gizmo.cpp
        src/im_controls.cpp
        src/scene_view_imgui.cpp
        src/im_viewport.cpp
                            src/stage_camera.cpp
        )
    target_link_libraries(ajfx_studio_page PUBLIC
        ajfx_studio_core
        MediaServer::Gui
        MediaServer::Editor
        tinyfiledialogs::tinyfiledialogs
        imgui::imgui
        imguizmo::imguizmo
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

        # The Dear ImGui layer and the panels drawn with it, on a context of
        # their own with synthetic pointer events.
        add_executable(ajfx_studio_imgui_tests tests/imgui_tests.cpp)
        target_link_libraries(ajfx_studio_imgui_tests PRIVATE ajfx_studio_page)
        ajfx_set_options(ajfx_studio_imgui_tests)
        add_test(NAME ajfx.studio_imgui COMMAND ajfx_studio_imgui_tests)
        set_target_properties(ajfx_studio_imgui_tests PROPERTIES FOLDER "ajfx/tests")

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
        tests/document_tests.cpp
        tests/compile_tests.cpp
        tests/guides_tests.cpp
        tests/bake_tests.cpp
        tests/stage_pick_tests.cpp
        tests/performance_tests.cpp
        tests/stage_tests.cpp
        tests/hands_tests.cpp
        tests/palette_tests.cpp
        tests/delivery_tests.cpp
    )
    # The starter scenes are the compiler's fixtures; the solid ones run
    # through the physics engine.
    target_link_libraries(ajfx_studio_tests PRIVATE ajfx_studio_core ajfx::rigid)
    if(TARGET ajfx::d3d12)
        # Kernels for every starter scene compile, where DXC is present.
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
