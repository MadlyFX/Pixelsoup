# ajfx Studio: the block editor that authors .ajfx particle effects.
#
# The page model, block catalog, compiler, recipes and coach are plain C++ on
# ajfx alone (ajfx_studio_core), so they are tested headless. The application
# previews through the media server's own renderer, so an effect looks in
# Studio exactly as it will on the wall, and draws its interface with the
# media server's UI toolkit.

add_library(ajfx_studio_core STATIC
    src/json.cpp
    src/document.cpp
    src/catalog.cpp
    src/raw_modules.cpp
    src/library.cpp
    src/compile.cpp
    src/document_io.cpp
    src/recipes.cpp
    src/coach.cpp
    src/guides.cpp
    src/user_assets.cpp
    src/session.cpp
)
target_include_directories(ajfx_studio_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_link_libraries(ajfx_studio_core PUBLIC ajfx::ajfx)
mediaserver_set_project_options(ajfx_studio_core)
set_target_properties(ajfx_studio_core PROPERTIES FOLDER "apps")

if(TARGET ajfx::d3d12)
    find_package(tinyfiledialogs CONFIG REQUIRED)

    add_executable(ajfx-studio
        src/main.cpp
        src/preview.cpp
        src/package_builder.cpp
        src/studio_app.cpp
        src/page_view.cpp
        src/widgets.cpp
        src/stage_overlay.cpp
        src/palette.cpp
    )
    target_link_libraries(ajfx-studio PRIVATE
        ajfx_studio_core
        ajfx::d3d12
        ajfx::rigid
        MediaServer::Render
        MediaServer::Gui
        MediaServer::Editor
        tinyfiledialogs::tinyfiledialogs
    )
    mediaserver_set_project_options(ajfx-studio)
    # Kernels are compiled as an artist edits, so DXC travels with Studio.
    ajfx_copy_dxc(ajfx-studio)
    set_target_properties(ajfx-studio PROPERTIES FOLDER "apps")
endif()

if(BUILD_TESTING)
    add_executable(ajfx_studio_tests tests/studio_tests.cpp)
    # The rock pile recipe is run through the physics engine.
    target_link_libraries(ajfx_studio_tests PRIVATE ajfx_studio_core ajfx::rigid)
    if(TARGET ajfx::d3d12)
        # Kernels for every recipe compile, where DXC is present.
        target_link_libraries(ajfx_studio_tests PRIVATE ajfx::d3d12)
        target_compile_definitions(ajfx_studio_tests PRIVATE AJFX_STUDIO_TEST_KERNELS=1)
        ajfx_copy_dxc(ajfx_studio_tests)
    endif()
    mediaserver_set_project_options(ajfx_studio_tests)
    add_test(NAME ajfx.studio COMMAND ajfx_studio_tests)
    set_target_properties(ajfx_studio_tests PROPERTIES FOLDER "tests")
endif()
